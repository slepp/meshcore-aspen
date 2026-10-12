#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build/stage the vendored metered WAMR interpreter for host and ESP32."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess

REVISION = "b124f70345d712bead5c0c2393acb2dc583511de"
ROOT = Path(__file__).resolve().parents[2]
CACHE = ROOT / ".cache/meshcore-wamr"

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
    native = ROOT / "packages/hew-wamr/native"
    source = native / "vendor"
    build = CACHE / ("esp-config" if target else "host")
    cmake_source = native
    reset_cmake_source_cache(build, cmake_source)
    subprocess.run(["cmake", "-S", str(cmake_source), "-B", str(build),
                    "-DCMAKE_BUILD_TYPE=" + ("Release" if target else "RelWithDebInfo"), f"-DWAMR_ROOT_DIR={source}",
                    "-DWAMR_BUILD_PLATFORM=" + ("esp-idf" if target else "linux"),
                    "-DWAMR_BUILD_TARGET=" + ("XTENSA" if target else "X86_64")], check=True)
    if not target:
        subprocess.run(["cmake", "--build", str(build), "--parallel", "2"], check=True)
        return
    dest = target / "lib/OnchipWamr"
    dest.mkdir(parents=True, exist_ok=True)
    for retired in ("core", "build-scripts"):
        shutil.rmtree(dest / retired, ignore_errors=True)
    shutil.rmtree(dest / "vendor", ignore_errors=True)
    shutil.copytree(source, dest / "vendor")
    for name in ("runtime.cpp", "runtime.hpp"):
        shutil.copy2(native / name, dest / name)
    shutil.copy2(source / "LICENSE", dest / "LICENSE")
    flags = ["-D" + value for value in (build / "defines.txt").read_text().split(";") if value]
    flags += ["-I" + str(dest / Path(value).relative_to(native))
              for value in (build / "includes.txt").read_text().split(";") if value]
    files = [str(Path(value).relative_to(native))
             for value in (build / "sources.txt").read_text().split(";") if value]
    for index, name in enumerate(files):
        if name.endswith(".s"):
            assembly = Path(name).with_suffix(".S")
            shutil.copy2(dest / name, dest / assembly)
            files[index] = str(assembly)
    (dest / "library.json").write_text(json.dumps({
        "name": "OnchipWamr", "version": "2.4.1",
        "build": {"srcDir": ".", "includeDir": "vendor/core/iwasm/include",
                  "flags": flags, "srcFilter": ["-<*>"] + ["+<" + f + ">" for f in files]}
    }, indent=2) + "\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--target", type=Path)
    parser.add_argument("--enabled", type=int, choices=(0, 1), default=1)
    parser.add_argument("--config-only", action="store_true")
    args = parser.parse_args()
    prepare(args.target, bool(args.enabled), args.config_only)
