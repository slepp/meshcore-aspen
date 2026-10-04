#!/usr/bin/env python3
"""Build and bind the real Go-host native extension to this worktree's inputs."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
BUILD = HERE/"build"
NATIVE = BUILD/"native"
WORKER = BUILD/"native-worker"
SPEC = BUILD/"native-worker.json"
INPUTS = BUILD/"native-worker.inputs"
PIN = "d92964352441e53b93e8667b802e04f6e072b39e"
SUFFIXES = {".c", ".cpp", ".h", ".hpp", ".inc", ".py", ".mk", ".cmake", ".S", ".patch"}


def digest(path):
    with Path(path).open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def inventory(directories):
    result = {}
    for directory in directories:
        for path in directory.rglob("*"):
            if path.is_file() and (path.suffix in SUFFIXES or path.name in ("Makefile", "CMakeLists.txt", ".piopm", "library.json")):
                result[str(path)] = digest(path)
    return result


def local_inputs():
    result = inventory([ROOT/"firmware/runtime", ROOT/"firmware/esp32", ROOT/"firmware/shared",
                        ROOT/"internal/nativebot", ROOT/"test_support/phy_parity",
                        ROOT/"test_support/parity"])
    for path in (ROOT/"Makefile", HERE/"build_worker.py", HERE/"compiler_capture.py"):
        result[str(path)] = digest(path)
    return result


def verify(candidate=None):
    if not SPEC.exists():
        raise ValueError("native worker build record missing; run make native-worker")
    spec = json.loads(SPEC.read_text())
    supplied = Path(candidate).absolute() if candidate else WORKER
    if supplied != WORKER or spec["worker"] != str(WORKER):
        raise ValueError(f"unverified external worker {supplied}; use {WORKER} from make native-worker")
    if local_inputs() != spec["local_inputs"]:
        raise ValueError("native worker source inputs changed; run make native-worker and rebuild hew-host")
    if digest(INPUTS) != spec["inputs_sha256"]:
        raise ValueError("native worker input record changed; rebuild with make native-worker")
    for path, expected in spec["inputs"].items():
        if not Path(path).is_file() or digest(path) != expected:
            raise ValueError(f"native worker input or binary changed: {path}; run make native-worker")
    return spec


def write_identity(spec):
    module = (f'pub fn path() -> string {{ "{WORKER}" }}\n'
              f'pub fn inputs() -> string {{ "{INPUTS}" }}\n'
              f'pub fn digest() -> string {{ "{spec["inputs_sha256"]}" }}\n')
    path = HERE/"worker_identity.hew"
    if not path.exists() or path.read_text() != module:
        path.write_text(module)


def run(command, log, env, cwd=ROOT):
    subprocess.run([str(v) for v in command], cwd=cwd, env=env, stdout=log,
                   stderr=subprocess.STDOUT, check=True)


def ensure():
    try:
        spec = verify()
        write_identity(spec)
        print("Verified native worker", spec["worker_sha256"])
        return
    except (ValueError, OSError, KeyError):
        pass
    if SPEC.exists():
        old = json.loads(SPEC.read_text())
        pinned = [NATIVE/name for name in ("upstream", "deps", "usb-deps", "lua", "wamr/source")]
        for name, expected in old["inputs"].items():
            path = Path(name)
            cached_link_input = path.is_relative_to(NATIVE) and path.suffix in (".o", ".a")
            if (cached_link_input or any(path.is_relative_to(parent) for parent in pinned)) and (not path.is_file() or digest(path) != expected):
                raise ValueError("pinned dependency input changed; restore it before rebuilding: "+name)
    NATIVE.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env.update(TMPDIR=str(BUILD), PYTHONDONTWRITEBYTECODE="1", CMAKE_EXPORT_COMPILE_COMMANDS="ON")
    upstream = NATIVE/"upstream"
    relative = lambda path: str(path.relative_to(ROOT))
    log_path = NATIVE/"build.log"
    log_path.write_text("")
    with log_path.open("a") as log:
        if not upstream.exists():
            run(["git", "clone", "--quiet", "--depth", "1", "--branch", "companion-v1.17.1",
                 "https://github.com/meshcore-dev/MeshCore.git", upstream], log, env)
        revision = subprocess.check_output(["git", "-C", str(upstream), "rev-parse", "HEAD"], text=True).strip()
        if revision != PIN or subprocess.check_output(["git", "-C", str(upstream), "status", "--porcelain"]):
            raise ValueError("native upstream must be pristine at "+PIN)
        shared = [f"MESHCORE_DIR={relative(upstream)}",
                  f"NATIVE_LIB_DIR={relative(NATIVE/'deps')}",
                  f"NATIVE_USB_LIB_DIR={relative(NATIVE/'usb-deps')}"]
        run(["make", "--no-print-directory", "native-test-deps"]+shared, log, env)
        for name, version in (("Crypto", "0.4.0"), ("CayenneLPP", "1.6.1")):
            if json.loads((NATIVE/"deps"/name/".piopm").read_text())["version"] != version:
                raise ValueError("native dependency version mismatch: "+name)
        wasm_spec = importlib.util.spec_from_file_location("hew_prepare_wasm", ROOT/"firmware/esp32/prepare_wasm.py")
        wasm = importlib.util.module_from_spec(wasm_spec)
        wasm_spec.loader.exec_module(wasm)
        wasm.CACHE = NATIVE/"wamr"
        # Existing pinned WAMR fetch, checksum, metering patch and CMake recipe.
        with open(log_path, "a") as wasm_log:
            child = ["python3", "-B", "-c",
                     "import importlib.util,pathlib;"
                     f"s=importlib.util.spec_from_file_location('p',{str(ROOT/'firmware/esp32/prepare_wasm.py')!r});"
                     "m=importlib.util.module_from_spec(s);s.loader.exec_module(m);"
                     f"m.CACHE=pathlib.Path({str(wasm.CACHE)!r});m.prepare()"]
            run(child, wasm_log, env)
        local = local_inputs()
        compiler = {name: subprocess.check_output([name, "--version"], text=True).splitlines()[0]
                    for name in ("cc", "c++", "cmake")}
        dependencies = inventory([upstream, NATIVE/"deps", NATIVE/"usb-deps", wasm.CACHE/"source"])
        fingerprint = hashlib.sha256(json.dumps({"sources": local, "dependencies": dependencies, "compiler": compiler,
                                                 "upstream": PIN}, sort_keys=True).encode()).hexdigest()
        output = NATIVE/("worker-"+fingerprint[:20])
        output.mkdir(exist_ok=True)
        if output.is_symlink() or output.resolve().parent != NATIVE.resolve():
            raise ValueError("native output must be a dedicated direct child of "+str(NATIVE))
        compiler_log = output/"compiler-commands.jsonl"
        env["HEW_NATIVE_COMPILER_LOG"] = str(compiler_log)
        json_include = Path(os.environ.get("BOT_JSON_INCLUDE",
            str(Path.home()/".platformio/packages/framework-arduinoespressif32/tools/sdk/esp32s3/include/json/cJSON")))
        if not (json_include/"cJSON.h").is_file():
            raise ValueError("cJSON header missing; set BOT_JSON_INCLUDE to the SDK cJSON directory")
        overrides = shared + [
            f"UPSTREAM={upstream}", f"ONCHIP_UPSTREAM={relative(upstream)}",
            f"BUILD={output/'mesh'}", f"PHY_BUILD={output/'phy'}", f"BOT_BUILD={output/'bot'}",
            f"BOT_LUA={NATIVE/'lua/lua-5.5.1'}", f"BOT_LUA_ARCHIVE={NATIVE/'lua/lua-5.5.1.tar.gz'}",
            f"BOT_WAMR={wasm.CACHE}", f"CRYPTO={NATIVE/'deps/Crypto'}",
            f"LPP={NATIVE/'deps/CayenneLPP/src'}", f"BASE64={NATIVE/'usb-deps/base64/src'}",
            f"BOT_JSON_INCLUDE={json_include}", "ONCHIP_BOT_WASM=1",
            f"CC=python3 {HERE/'compiler_capture.py'} cc",
            f"CXX=python3 {HERE/'compiler_capture.py'} c++",
            f"TMPDIR={BUILD}"]
        make = ["make", "--no-print-directory", "-C", ROOT/"firmware/esp32"]+overrides
        # Host-only source staging: the same pinned archive and core patches
        # as firmware/esp32's prepare recipe, without generating a device profile.
        mesh = output/"mesh"
        if not mesh.exists():
            archive = output/"upstream.tar"
            run(["git", "-C", upstream, "archive", "--format=tar", "-o", archive, PIN], log, env)
            mesh.mkdir()
            with tarfile.open(archive) as files:
                files.extractall(mesh, filter="data")
            for patch in ("radio-reconfigure.patch", "queued-dispatch.patch"):
                run(["patch", "--forward", "-p1", "-i", ROOT/"firmware/shared"/patch], log, env, cwd=mesh)
            (mesh/"hew-host-prepared").write_text(PIN+"\n")
        if not (mesh/"hew-host-prepared").exists():
            raise ValueError("incomplete host-only source staging: "+str(mesh))
        sys.path.insert(0, str(ROOT/"firmware/esp32"))
        import prepare
        import prepare_bot
        clock = mesh/"examples/kiss_modem/onchip/BuildClock.h"
        clock.parent.mkdir(parents=True, exist_ok=True)
        prepare.stage_packet_pool(mesh, clock.parent)
        mesh_source = mesh/"src/Mesh.cpp"
        text = mesh_source.read_text()
        guard = "                if (2u + hash_size * hash_count > unsigned(len)) break;"
        if guard not in text:
            text = prepare.replace_once(text, "                uint8_t hash_count = path_len & 63;",
                                        "                uint8_t hash_count = path_len & 63;\n"+guard)
            mesh_source.write_text(text)
        epoch = int(subprocess.check_output(["git", "show", "-s", "--format=%ct", "HEAD"], cwd=ROOT))
        baseline = epoch - epoch % 86400
        if not 1715770351 <= baseline <= 4102444800:
            raise ValueError("build clock baseline outside the shared May 2024–January 2100 range")
        clock.write_text("// Build-day clock baseline for offline startup.\n"
                         "#pragma once\n#ifndef ONCHIP_CLOCK_BUILD_EPOCH\n"
                         f"#define ONCHIP_CLOCK_BUILD_EPOCH {baseline}u\n#endif\n"
                         f'#define ONCHIP_NATIVE_REVISION "{PIN[:12]}"\n')
        lua = NATIVE/"lua/lua-5.5.1"
        run(make+[lua/"src/lua.h"], log, env)
        # This shared helper explicitly delegates build-tree verification to
        # its caller. Keep its parser hooks and c_stack_limit=40 unchanged.
        prepare_bot.stage_lua(lua, output/"bot")
        run(make+["bot-lua", "bot-prepare-phy", output/"bot/bot-native-worker"], log, env)
        compiled = output/"bot/bot-native-worker"
        shutil.copyfile(compiled, WORKER)
        WORKER.chmod(0o755)
        inputs = local | inventory([upstream, NATIVE/"deps", NATIVE/"usb-deps", NATIVE/"lua",
                                    wasm.CACHE/"source", output/"mesh", output/"bot/lib"])
        for path in [WORKER, compiler_log, wasm.CACHE/"host/compile_commands.json",
                     wasm.CACHE/"host/CMakeCache.txt",
                     NATIVE/"lua/lua-5.5.1.tar.gz", wasm.CACHE/"wamr.tar.gz", json_include/"cJSON.h"]:
            inputs[str(path)] = digest(path)
        for directory in (output, wasm.CACHE/"host"):
            for path in directory.rglob("*"):
                if path.is_file() and path.suffix in (".o", ".a"):
                    inputs[str(path)] = digest(path)
        for name in ("cc", "c++"):
            path = Path(shutil.which(name)).resolve()
            inputs[str(path)] = digest(path)
        linked = subprocess.check_output(["ldd", str(WORKER)], text=True)
        for line in linked.splitlines():
            for item in line.split():
                if item.startswith("/") and Path(item).is_file():
                    inputs[item] = digest(item)
        for path in inputs:
            if "\n" in path or "\t" in path:
                raise ValueError("native build path contains unsupported newline/tab")
        INPUTS.write_text("".join(value+"\t"+path+"\n" for path, value in sorted(inputs.items())))
        spec = {"format": 1, "base": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
                "worker": str(WORKER), "worker_sha256": digest(WORKER), "source_fingerprint": fingerprint,
                "upstream": PIN, "lua_archive_sha256": digest(NATIVE/"lua/lua-5.5.1.tar.gz"),
                "wamr_revision": wasm.REVISION, "wamr_archive_sha256": wasm.SHA256,
                "compiler": compiler, "compiler_commands": str(compiler_log),
                "inputs_sha256": digest(INPUTS), "inputs": inputs, "local_inputs": local}
        SPEC.write_text(json.dumps(spec, indent=2, sort_keys=True)+"\n")
        write_identity(spec)
    verify()
    print("Built verified native worker", spec["worker_sha256"], "inputs", len(inputs))
    print("Native compiler log:", compiler_log)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--verify", action="store_true")
    parser.add_argument("--worker")
    args = parser.parse_args()
    try:
        if args.verify:
            print(verify(args.worker)["worker"])
        else:
            ensure()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        raise SystemExit(f"Native worker validation/build failed: {error}; inspect {NATIVE/'build.log'}")
