#!/usr/bin/env python3
"""Stage a verified Lua core for a build-only nRF resource probe."""

import argparse
import hashlib
import pathlib
import shutil
import subprocess
import tarfile

HERE = pathlib.Path(__file__).resolve().parent
SHA256 = "1c4b4068d67061f2a2231ad2b5422e77acea1487ea9890f6320af614f4373dce"
CORE = (
    "lapi lcode lctype ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes "
    "lparser lstate lstring ltable ltm lundump lvm lzio lauxlib"
).split()


def prepare(archive):
    if hashlib.sha256(archive.read_bytes()).hexdigest() != SHA256:
        raise ValueError("Lua 5.5.1 archive SHA-256 mismatch")
    build = HERE / ".build"
    with tarfile.open(archive) as tree:
        tree.extractall(build, filter="data")
    source = build / "lua-5.5.1/src"
    target = build / "upstream/lib/PineLuaProbe"
    target.mkdir(parents=True, exist_ok=True)
    for path in source.glob("*.h"):
        shutil.copyfile(path, target / path.name)
    for unit in CORE:
        shutil.copyfile(source / (unit + ".c"), target / (unit + ".c"))
    config = target / "luaconf.h"
    config.write_text(config.read_text().replace(
        "#include <limits.h>",
        "#include <limits.h>\n"
        "#if !defined(LLONG_MAX) && defined(__LONG_LONG_MAX__)\n"
        "#define LLONG_MAX __LONG_LONG_MAX__\n"
        "#define LLONG_MIN (-LLONG_MAX - 1LL)\n"
        "#define ULLONG_MAX (2ULL * LLONG_MAX + 1ULL)\n"
        "#endif\n",
        1,
    ))
    shutil.copyfile(HERE / "tests/lua_probe.c", target / "PineProbe.c")
    shutil.copyfile(build / "lua-5.5.1/README", target / "LUA-README")
    (target / "library.json").write_text(
        '{"name":"PineLuaProbe","version":"5.5.1","build":{"flags":["-DNRFMAST_LUA_PROBE=1"]}}\n'
    )
    dest = build / "upstream/examples/nrfmast/LuaProbe.cpp"
    dest.write_text(
        "#if NRFMAST_LUA_PROBE\n#include <lua.h>\nextern \"C\" void pineLuaProbe();\n"
        "void pineLuaProbeLink() { pineLuaProbe(); }\n#endif\n"
    )
    subprocess.run(
        ["cc", "-std=c11", "-O2", "-I" + str(source), str(HERE / "tests/lua_probe.c")] +
        [str(source / (unit + ".c")) for unit in CORE] +
        ["-lm", "-o", str(build / "lua-native-probe")],
        check=True,
    )
    subprocess.run([str(build / "lua-native-probe")], check=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=pathlib.Path, required=True)
    prepare(parser.parse_args().archive)
