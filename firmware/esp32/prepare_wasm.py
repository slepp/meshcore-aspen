#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Fetch and prepare the same metered WAMR interpreter for host and ESP32."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
import urllib.request

REVISION = "b124f70345d712bead5c0c2393acb2dc583511de"
SHA256 = "c197d6d811c23b3df0446e91abffba1091caf282e026156d19ace8ba2fc83f3e"
ROOT = Path(__file__).resolve().parents[2]
CACHE = ROOT / ".cache/meshcore-wamr"

ESP_THREAD_ID_ORIGINAL = """korp_tid
os_self_thread(void)
{
    /* only allowed if this is a thread, xTaskCreate is not enough look at
     * product_mini for how to use this*/
    return pthread_self();
}"""
ESP_THREAD_ID_PATCHED = """korp_tid
os_self_thread(void)
{
#if WASM_ENABLE_THREAD_MGR != 0
#error "MeshCore FreeRTOS task identity requires WAMR thread manager disabled"
#endif
    return (korp_tid)(uintptr_t)xTaskGetCurrentTaskHandle();
}"""

POOL_REALLOC_ORIGINAL = """                hmu_set_size(hmu_old, tot_size);
                memset((char *)hmu_old + tot_size_old, 0,
                       tot_size - tot_size_old);"""
POOL_REALLOC_PATCHED = """                heap->total_free_size -= tot_size - tot_size_old;
                if (heap->current_size - heap->total_free_size
                    > heap->highmark_size)
                    heap->highmark_size =
                        heap->current_size - heap->total_free_size;
#if GC_STAT_DATA != 0
                heap->total_size_allocated += tot_size - tot_size_old;
#endif
                hmu_set_size(hmu_old, tot_size);
                memset((char *)hmu_old + tot_size_old, 0,
                       tot_size - tot_size_old);"""


def patch_pool_realloc_accounting(source):
    path = source / "core/shared/mem-alloc/ems/ems_alloc.c"
    code = path.read_text()
    if POOL_REALLOC_PATCHED in code:
        if code.count(POOL_REALLOC_PATCHED) != 1 or code.count(POOL_REALLOC_ORIGINAL) != 1:
            raise ValueError("Pinned WAMR pool realloc accounting patch is ambiguous")
        return
    if code.count(POOL_REALLOC_ORIGINAL) != 1:
        raise ValueError("Pinned WAMR pool realloc accounting patch no longer matches")
    path.write_text(code.replace(POOL_REALLOC_ORIGINAL, POOL_REALLOC_PATCHED, 1))


def patch_esp_thread_identity(source):
    path = source / "core/shared/platform/esp-idf/espidf_thread.c"
    code = path.read_text()
    original_count = code.count(ESP_THREAD_ID_ORIGINAL)
    patched_count = code.count(ESP_THREAD_ID_PATCHED)
    if original_count + patched_count != 1:
        raise ValueError("Pinned WAMR ESP32 thread identity patch is ambiguous or no longer matches")
    if ESP_THREAD_ID_ORIGINAL in code:
        path.write_text(code.replace(ESP_THREAD_ID_ORIGINAL, ESP_THREAD_ID_PATCHED, 1))


def reset_cmake_source_cache(build, source):
    cache_file = build / "CMakeCache.txt"
    if not cache_file.exists():
        return
    homes = [line.split("=", 1)[1] for line in cache_file.read_text().splitlines()
             if line.startswith("CMAKE_HOME_DIRECTORY:INTERNAL=")]
    if homes and Path(homes[0]).resolve() != source.resolve():
        cache_file.unlink()
        shutil.rmtree(build / "CMakeFiles", ignore_errors=True)


def prepare(target=None, enabled=True, config_only=False):
    if target:
        target = target.resolve()
        from prepare import check_target
        check_target(ROOT / ".tmp/onchip-upstream", target)
        (target / "examples/kiss_modem/onchip/WasmConfig.h").write_text(
            "#pragma once\n#ifndef ONCHIP_BOT_WASM\n"
            f"#define ONCHIP_BOT_WASM {int(enabled)}\n#endif\n"
            f"#if ONCHIP_BOT_WASM != {int(enabled)}\n"
            '#error "ONCHIP_BOT_WASM does not match prepared dependencies; run bot-prepare with the same flag"\n#endif\n')
    if not enabled:
        if target:
            shutil.rmtree(target / "lib/OnchipWamr", ignore_errors=True)
            (target / "examples/kiss_modem/onchip/BotWasm.cpp").unlink(missing_ok=True)
            shutil.rmtree(target / "examples/kiss_modem/onchip/wasm", ignore_errors=True)
        return
    if config_only:
        return
    if target:
        overlay = target / "examples/kiss_modem/onchip"
        shutil.copy2(ROOT / "firmware/runtime/BotWasm.cpp", overlay / "BotWasm.cpp")
        shutil.copytree(ROOT / "firmware/runtime/wasm/sdk", overlay / "wasm/sdk", dirs_exist_ok=True)
    CACHE.mkdir(parents=True, exist_ok=True)
    archive = CACHE / "wamr.tar.gz"
    if not archive.exists():
        urllib.request.urlretrieve(
            f"https://codeload.github.com/bytecodealliance/wasm-micro-runtime/tar.gz/{REVISION}",
            archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != SHA256:
        raise ValueError("WAMR archive SHA256 mismatch")
    source = CACHE / "source"
    if not source.exists():
        with tarfile.open(archive) as package:
            package.extractall(CACHE, filter="data")
        (CACHE / f"wasm-micro-runtime-{REVISION}").rename(source)
    interpreter = source / "core/iwasm/interpreter/wasm_interp_classic.c"
    code = interpreter.read_text()
    marker = "extern bool meshcore_wamr_tick(struct WASMExecEnv *exec_env);"
    if marker not in code:
        code = code.replace('#include "wasm_interp.h"',
                            '#include "wasm_interp.h"\nstruct WASMExecEnv;\n' + marker, 1)
    old_meter = "#define CHECK_INSTRUCTION_LIMIT()                                 \\\n"
    old_hook = "    if (!meshcore_wamr_tick(exec_env)) goto got_exception; \\\n"
    code = code.replace(old_meter + old_hook, old_meter)
    dispatch = "#define HANDLE_OP(opcode) case opcode:\n"
    prior = ("#define HANDLE_OP(opcode) case opcode: \\\n"
             "    if (!meshcore_wamr_tick(exec_env)) goto got_exception;\n")
    metered = ("#define HANDLE_OP(opcode) case opcode: \\\n"
               "    if (frame && !meshcore_wamr_tick(exec_env)) goto got_exception;\n")
    code = code.replace(prior, metered)
    if metered not in code:
        if code.count(dispatch) != 1:
            raise ValueError("Pinned WAMR instruction dispatch patch no longer matches")
        code = code.replace(dispatch, metered, 1)
    interpreter.write_text(code)
    mapping = source / "core/shared/platform/esp-idf/espidf_memmap.c"
    code = mapping.read_text()
    old = "uint32_t mem_caps = MALLOC_CAP_8BIT;"
    if old in code:
        if code.count(old) != 1:
            raise ValueError("Pinned WAMR linear-memory PSRAM patch no longer matches")
        mapping.write_text(code.replace(old,
            "uint32_t mem_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;", 1))
    patch_esp_thread_identity(source)
    patch_pool_realloc_accounting(source)
    build = CACHE / ("esp-config" if target else "host")
    cmake_source = ROOT / "firmware/runtime/wasm"
    reset_cmake_source_cache(build, cmake_source)
    subprocess.run(["cmake", "-S", str(cmake_source), "-B", str(build),
                    "-DCMAKE_BUILD_TYPE=" + ("Release" if target else "RelWithDebInfo"), f"-DWAMR_ROOT_DIR={source}",
                    "-DWAMR_BUILD_PLATFORM=" + ("esp-idf" if target else "linux"),
                    "-DWAMR_BUILD_TARGET=" + ("XTENSA" if target else "X86_64")], check=True)
    if not target:
        subprocess.run(["cmake", "--build", str(build), "--parallel", "4"], check=True)
        return
    dest = target / "lib/OnchipWamr"
    dest.mkdir(parents=True, exist_ok=True)
    for name in ("core", "build-scripts"):
        shutil.copytree(source / name, dest / name, dirs_exist_ok=True)
    shutil.copy2(source / "LICENSE", dest / "LICENSE")
    flags = ["-D" + value for value in (build / "defines.txt").read_text().split(";") if value]
    flags += ["-I" + str(dest / Path(value).relative_to(source))
              for value in (build / "includes.txt").read_text().split(";") if value]
    files = [str(Path(value).relative_to(source))
             for value in (build / "sources.txt").read_text().split(";") if value]
    for index, name in enumerate(files):
        if name.endswith(".s"):
            assembly = Path(name).with_suffix(".S")
            shutil.copy2(dest / name, dest / assembly)
            files[index] = str(assembly)
    (dest / "library.json").write_text(json.dumps({
        "name": "OnchipWamr", "version": "2.4.1",
        "build": {"srcDir": ".", "includeDir": "core/iwasm/include",
                  "flags": flags, "srcFilter": ["-<*>"] + ["+<" + f + ">" for f in files]}
    }, indent=2) + "\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--target", type=Path)
    parser.add_argument("--enabled", type=int, choices=(0, 1), default=1)
    parser.add_argument("--config-only", action="store_true")
    args = parser.parse_args()
    prepare(args.target, bool(args.enabled), args.config_only)
