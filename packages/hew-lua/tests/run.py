#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build and run a copied package without the MeshCore checkout or its caches."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

PACKAGE = Path(__file__).resolve().parents[1]


def bounded():
    import resource
    resource.setrlimit(resource.RLIMIT_AS, (4 * 1024**3, 4 * 1024**3))
    if hasattr(os, "sched_getaffinity"):
        os.sched_setaffinity(0, sorted(os.sched_getaffinity(0))[:2])


def main():
    with tempfile.TemporaryDirectory(prefix="hew-lua-") as scratch:
        root = Path(scratch)
        package = root / "hew-lua"
        shutil.copytree(PACKAGE, package, ignore=shutil.ignore_patterns(".hew", "hew.lock", "build", "target", "__pycache__"))
        env = os.environ | {"HEW_HOME": str(root / "cache")}
        compiler = os.environ.get("HEW", "hew")
        def run(args, cwd):
            subprocess.run(args, cwd=cwd, env=env, check=True, timeout=120,
                           preexec_fn=bounded if os.name == "posix" else None)
        run([compiler, "install", "--offline"], package / "tests")
        for mode in ([], ["--release"]):
            binary = root / ("test-o2" if mode else "test-o0")
            run([compiler, "build", "main.hew", *mode, "-o", str(binary)], package / "tests")
            run([str(binary)], root)
        run([compiler, "install", "--offline"], package / "examples/consumer")
        run([compiler, "build", "main.hew", "--release", "-o", str(root / "example")], package / "examples/consumer")
        run([str(root / "example")], root)
    print("PASS independent hew.lua package: O0/O2 and standalone host-operation example")


if __name__ == "__main__":
    main()
