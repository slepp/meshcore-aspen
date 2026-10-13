#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build Willow against Debian 12 libraries with current Hew and a matched runtime archive."""

import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess

from release_inputs import PINS, stage_source

BINARIES = {
    "host": "service_main.hew",
    "base": "base_service.hew",
    "dashboard": "dashboard_main.hew",
    "broker": "broker_main.hew",
}


def prepare(root, destination, image, runtime, *, prepared=False, curl_source=None):
    root, destination, runtime = map(Path, (root, destination, runtime))
    if not runtime.is_file():
        raise ValueError("Select the matching Debian 12 libhew.a")
    destination.parent.mkdir(parents=True, exist_ok=True)

    def command(args, **kwargs):
        subprocess.run(
            list(map(str, args)), cwd=kwargs.pop("cwd", root), check=True, **kwargs
        )

    def output(args, **kwargs):
        return subprocess.check_output(
            list(map(str, args)), cwd=kwargs.pop("cwd", root), text=True, **kwargs
        ).strip()

    if not prepared:
        stage_source(root, destination, command, output)
        native = destination / "hosts/willow/build/native"
        native.mkdir(parents=True)
        command(
            [
                "git",
                "clone",
                "--no-hardlinks",
                destination / ".tmp/onchip-upstream",
                native / "upstream",
            ]
        )
        for name, pin in PINS.items():
            shutil.copytree(
                destination / pin["path"],
                native / ("usb-deps" if name == "base64" else "deps") / name,
            )

    # The container receives public build inputs and a read-only matched runtime.
    # It cannot read the operator's home, credentials or private node state.
    def inside(args):
        command(
            [
                "docker",
                "run",
                "--rm",
                "--pull",
                "never",
                "--network",
                "none",
                "--read-only",
                "--cap-drop",
                "ALL",
                "--security-opt",
                "no-new-privileges",
                "--cpus",
                "2",
                "--memory",
                "4g",
                "--memory-swap",
                "4g",
                "--user",
                f"{os.getuid()}:{os.getgid()}",
                "--mount",
                f"type=bind,source={destination.resolve()},target=/src",
                "--mount",
                f"type=bind,source={runtime.resolve()},target=/runtime/libhew.a,readonly",
                "--workdir",
                "/src/hosts/willow",
                "--env",
                "HOME=/src/.tmp/home",
                "--env",
                "TMPDIR=/src/.tmp",
                "--env",
                "PYTHONDONTWRITEBYTECODE=1",
                "--env",
                "BOT_JSON_INCLUDE=/usr/include/cjson",
                "--env",
                "PKG_CONFIG_PATH=/src/hosts/willow/build/curl/install/lib/pkgconfig",
                "--entrypoint",
                args[0],
                image,
                *args[1:],
            ],
            timeout=900,
        )

    if not prepared:
        inside(["python3", "-B", "tools/build_worker.py"])
    else:
        inside(["python3", "-B", "tools/build_worker.py", "--verify"])
    from build_willow_curl import prepare as prepare_curl

    curl = prepare_curl(destination, inside, curl_source)
    inside(["python3", "-B", "tools/build_worker.py", "--portable"])
    # Use the Makefile's C recipes and link arguments. Hew code generation stays
    # with the current in-PATH compiler; no container compiler pin is needed.
    recipes = output(
        ["make", "-B", "-n", "release"], cwd=destination / "hosts/willow"
    ).splitlines()
    bridges = set()
    for line in recipes:
        args = shlex.split(line)
        if (
            "-c" in args
            and "-o" in args
            and any(arg.startswith("native/") and arg.endswith(".c") for arg in args)
        ):
            bridges.add(args[args.index("-o") + 1])
    inside(["make", "-B", "-j2", *sorted(bridges)])
    compiler = shutil.which(os.environ.get("HEW", "hew"))
    if not compiler:
        raise ValueError("Put current Hew and its matching standard library on PATH")
    relink = []
    for kind, source in BINARIES.items():
        binary = f"hew-{kind}-release"
        obj = f"build/{binary}.o"
        command(
            [compiler, "build", f"src/{source}", "--release", "--emit-obj", "-o", obj],
            cwd=destination / "hosts/willow",
            timeout=180,
        )
        recipe = next(
            shlex.split(line)
            for line in recipes
            if len(shlex.split(line)) > 1
            and shlex.split(line)[1] == "build"
            and f"build/{binary}" in shlex.split(line)
        )
        libraries = [recipe[i + 1] for i, a in enumerate(recipe) if a == "--link-lib"]
        libraries = [a.replace(str(destination), "/src") for a in libraries]
        link = [
            "cc",
            obj,
            "/runtime/libhew.a",
            "-o",
            f"build/{binary}",
            "-Wl,--gc-sections",
            "-Wl,--strip-all",
            "-lpthread",
            "-lm",
            "-ldl",
            "-lrt",
            "-L/src/hosts/willow/build/curl/install/lib",
            "-Wl,-rpath,$ORIGIN/lib",
            *libraries,
            "/runtime/libhew.a",
        ]
        inside(link)
        relink.append({"directory": "hosts/willow", "arguments": link})
    (destination / "hosts/willow/build/relink-commands.json").write_text(
        json.dumps(relink, indent=2) + "\n"
    )
    shutil.copy2(runtime, destination / "hosts/willow/build/libhew.a")
    inside(
        [
            "python3",
            "-c",
            """
import json
import os
from pathlib import Path
import shutil
import sys

sys.path.insert(0, '/src/tools')
from release_candidate import native_libraries

root = Path('/src/hosts/willow/build')
libraries = native_libraries(root / 'native-worker', os.environ)
(root / 'linked-libraries.json').write_text(json.dumps(libraries, indent=2) + chr(10))
material = root / 'dependency-notices'
packages = {p['package'].split(':')[0] for p in libraries if p.get('package')}
packages.update(('libsodium23', 'zlib1g'))
for package in sorted(packages):
    source = Path('/usr/share/doc') / package / 'copyright'
    if source.is_file():
        target = material / package / 'copyright'
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
shutil.copytree('/usr/share/common-licenses', material / 'common-licenses', dirs_exist_ok=True)
shutil.copytree('/usr/include/cjson', root / 'relink-headers/cjson', dirs_exist_ok=True)
""",
        ]
    )
    (destination / "hosts/willow/build/baseline.json").write_text(
        json.dumps(
            {
                "distribution": "Debian 12",
                "curl": curl,
                "container_image": image,
                "hew_runtime_sha256": hashlib.sha256(runtime.read_bytes()).hexdigest(),
                "compiler": subprocess.check_output(
                    [compiler, "--version"], text=True
                ).strip(),
            },
            indent=2,
        )
        + "\n"
    )
    return destination
