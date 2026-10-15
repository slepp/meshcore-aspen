# SPDX-License-Identifier: Apache-2.0
"""Fit Pine in its retained application region with link-time optimization."""
from pathlib import Path

Import("env")


def retain_stack_measurements(environment, node):
    compiler = Path(environment.subst("$CC"))
    environment.Replace(AR=str(compiler.with_name(compiler.name.replace("gcc", "gcc-ar"))),
                        RANLIB=str(compiler.with_name(compiler.name.replace("gcc", "gcc-ranlib"))))
    path = node.srcnode().get_abspath().replace("\\", "/")
    if "/freertos/portable/" in path or "/OnchipLua/" in path or path.endswith(("/BotVm.cpp", "/BotWorker.cpp")):
        return environment.Object(node, CCFLAGS=environment["CCFLAGS"] + ["-fno-lto"])[0]
    return node


# Keep naked RTOS handlers and measured Lua/VM frames outside LTO.
env.AddBuildMiddleware(retain_stack_measurements)
env.Append(LINKFLAGS=["-flto", "-Os"])
