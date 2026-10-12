#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build a copied source package through Hew's ordinary native manifest."""
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


def leb(value):
    out = bytearray()
    while value > 127:
        out.append((value & 127) | 128)
        value >>= 7
    return bytes(out + bytes([value]))


def read_leb(data, offset):
    value = shift = 0
    while True:
        byte = data[offset]
        offset += 1
        value |= (byte & 127) << shift
        if not byte & 128:
            return value, offset
        shift += 7


def start_module(data):
    sections = []
    offset = 8
    function = None
    while offset < len(data):
        kind = data[offset]
        size, begin = read_leb(data, offset + 1)
        end = begin + size
        if kind == 7:
            count, cursor = read_leb(data, begin)
            for _ in range(count):
                length, cursor = read_leb(data, cursor)
                name = data[cursor:cursor + length]
                cursor += length
                export_kind = data[cursor]
                index, cursor = read_leb(data, cursor + 1)
                if name == b"spin" and export_kind == 0:
                    function = index
        sections.append((kind, data[offset:end]))
        offset = end
    assert function is not None
    payload = leb(function)
    start = b"\x08" + leb(len(payload)) + payload
    result = bytearray(data[:8])
    for kind, section in sections:
        if start and kind > 8:
            result += start
            start = b""
        result += section
    return bytes(result + start)


def main():
    with tempfile.TemporaryDirectory(prefix="hew-wamr-") as directory:
        root = Path(directory)
        package = root / "hew-wamr"
        shutil.copytree(PACKAGE, package, ignore=shutil.ignore_patterns(".hew", "hew.lock", "build", "target", "__pycache__"))
        env = os.environ | {"HEW_HOME": str(root / "cache")}
        compiler = os.environ.get("HEW", "hew")
        def run(args, cwd, timeout=180):
            subprocess.run(args, cwd=cwd, env=env, check=True, timeout=timeout,
                           preexec_fn=bounded if os.name == "posix" else None)
        tests = package / "tests"
        flags = ["--target=wasm32", "-O1", "-nostdlib", "-fno-builtin", "-mno-bulk-memory", "-mno-reference-types", "-mno-multivalue",
                 "-Wl,--no-entry", "-Wl,--strip-all", "-Wl,--initial-memory=65536", "-Wl,-z,stack-size=4096"]
        for name, options in (("fixture", []), ("larger", []), ("import", ["-DIMPORT"]), ("implicit", ["-DIMPLICIT"])):
            maximum = "131072" if name == "larger" else "65536"
            run([os.environ.get("CLANG", "clang"), *flags, "-Wl,--max-memory=" + maximum,
                 *options, "fixture.c", "-o", name + ".wasm"], tests)
        (tests / "start.wasm").write_bytes(start_module((tests / "fixture.wasm").read_bytes()))
        (tests / "bad.wasm").write_bytes(b"\x00asm\x01\x00\x00\x00\x05\xff")
        # (i32, i64) -> (i32, i64), with real multi-value results.
        def section(kind, payload):
            return bytes([kind]) + leb(len(payload)) + payload
        pair = b"\x00asm\x01\x00\x00\x00"
        pair += section(1, b"\x01\x60\x02\x7f\x7e\x02\x7f\x7e")
        pair += section(3, b"\x01\x00")
        pair += section(5, b"\x01\x01\x01\x01")
        pair += section(7, b"\x01\x04pair\x00\x00")
        pair += section(10, b"\x01\x06\x00\x20\x00\x20\x01\x0b")
        (tests / "pair.wasm").write_bytes(pair)

        run([compiler, "install", "--offline"], tests)
        for mode in ([], ["--release"]):
            binary = root / ("test-o2" if mode else "test-o0")
            run([compiler, "build", "main.hew", *mode, "-o", str(binary)], tests)
            run([str(binary)], tests, timeout=15)
        example = package / "examples/consumer"
        shutil.copy2(tests / "fixture.wasm", example / "fixture.wasm")
        run([compiler, "install", "--offline"], example)
        run([compiler, "build", "main.hew", "--release", "-o", str(root / "example")], example)
        run([str(root / "example")], example, timeout=15)
    print("PASS independent hew.wamr package: native O0/O2 and standalone arithmetic example")


if __name__ == "__main__":
    main()
