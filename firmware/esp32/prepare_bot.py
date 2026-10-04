#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Prepare the pinned source-only Lua interpreter in a disposable build tree."""
import argparse
import json
from pathlib import Path
import shutil
from prepare import check_target, replace_once

CORE = """lapi lcode lctype ldebug ldo ldump lfunc lgc llex lmem lobject
lopcodes lparser lstate lstring ltable ltm lundump lvm lzio lauxlib""".split()


def generate(lua: Path, target: Path):
    root = Path(__file__).resolve().parents[2]
    check_target(root / ".tmp/onchip-upstream", target)
    stage_lua(lua, target)


def stage_lua(lua: Path, target: Path, *, c_stack_limit: int = 40):
    """Stage the common parser hooks after the caller verifies its build tree."""
    dest = target / "lib/OnchipLua"
    dest.mkdir(parents=True, exist_ok=True)
    for path in (lua / "src").glob("*.h"):
        shutil.copy2(path, dest / path.name)
    for stem in CORE:
        shutil.copy2(lua / "src" / (stem + ".c"), dest / (stem + ".c"))
    lexer = dest / "llex.c"
    text = lexer.read_text()
    text = replace_once(
        text, "#define next(ls)\t(ls->current = zgetc(ls->z))",
        "extern void onchip_lua_parser_step(lua_State *L);\n"
        "#define next(ls) (onchip_lua_parser_step((ls)->L), "
        "(ls)->current = zgetc((ls)->z))")
    lexer.write_text(text)
    config = dest / "luaconf.h"
    configuration = replace_once(config.read_text(), "#include <limits.h>",
        "#include <limits.h>\n"
        "/* Older ESP32 newlib hides C99 limits from C++ translation units. */\n"
        "#if !defined(LLONG_MAX) && defined(__LONG_LONG_MAX__)\n"
        "#define LLONG_MAX __LONG_LONG_MAX__\n"
        "#define LLONG_MIN (-LLONG_MAX - 1LL)\n"
        "#define ULLONG_MAX (2ULL * LLONG_MAX + 1ULL)\n"
        "#endif\n")
    config.write_text(configuration +
                      f"\n#undef LUAI_MAXCCALLS\n#define LUAI_MAXCCALLS {c_stack_limit}\n")
    (dest / "library.json").write_text(json.dumps({
        "name": "OnchipLua", "version": "5.5.1",
        "build": {"srcDir": ".", "includeDir": ".",
                  "srcFilter": ["+<" + stem + ".c>" for stem in CORE]}
    }, indent=2) + "\n")
    shutil.copy2(lua / "README", dest / "LUA-README")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--lua", required=True, type=Path)
    parser.add_argument("--target", required=True, type=Path)
    args = parser.parse_args()
    generate(args.lua, args.target)
