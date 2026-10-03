#!/usr/bin/env python3
"""Bounded unmodified translation-unit probes, not substitute applications."""
import argparse
import hashlib
import json
import os
import pathlib
import subprocess

from evidence import portable_text


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--upstream", type=pathlib.Path, required=True)
    parser.add_argument("--crypto", type=pathlib.Path, required=True)
    parser.add_argument("--build", type=pathlib.Path, required=True)
    args = parser.parse_args()
    args.upstream, args.crypto, args.build = args.upstream.resolve(), args.crypto.resolve(), args.build.resolve()
    roots = (("meshcore", args.upstream), ("crypto", args.crypto),
             ("dependencies", args.crypto.parent), ("generated", args.build))
    here = pathlib.Path(__file__).resolve().parent
    output = args.build / "portability"
    output.mkdir(parents=True, exist_ok=True)
    include = ["-I" + str(here / "seams"), "-I" + str(args.upstream / "src"),
               "-I" + str(args.crypto), "-include", str(here / "seams/FS.h")]
    # Existing installed source dependencies only; no download or installation.
    for path in sorted(args.crypto.parent.iterdir()):
        if path.is_dir():
            include += ["-I" + str(path), "-I" + str(path / "src")]
    sources = [
        "src/helpers/ClientACL.cpp", "src/helpers/RegionMap.cpp",
        "src/helpers/TransportKeyStore.cpp", "src/helpers/ConfigSerializer.cpp",
        "src/helpers/BaseChatMesh.cpp", "examples/companion_radio/DataStore.cpp",
        "src/helpers/CommonCLI.cpp", "examples/simple_repeater/MyMesh.cpp",
        "examples/simple_room_server/MyMesh.cpp", "examples/companion_radio/MyMesh.cpp",
    ]
    results = []
    for source in sources:
        command = [os.environ.get("CXX", "c++"), "-std=c++17", "-fsyntax-only",
                   "-fmax-errors=5", *include, str(args.upstream / source)]
        try:
            result = subprocess.run(command, capture_output=True, text=True, timeout=30)
            log = result.stdout + result.stderr
            status = "syntax-only-pass" if result.returncode == 0 else "blocked"
        except subprocess.TimeoutExpired:
            log, status = "Thirty-second translation-unit portability budget exhausted.\n", "blocked-timeout"
        artifact = source.replace("/", "_") + ".log"
        log = portable_text(log, roots)
        (output / artifact).write_text(log)
        results.append({"source": source, "status": status,
                        "command": [portable_text(arg, roots) for arg in command],
                        "source_sha256": hashlib.sha256((args.upstream/source).read_bytes()).hexdigest(),
                        "diagnostic": artifact,
                        "diagnostic_sha256": hashlib.sha256(log.encode()).hexdigest(),
                        "application_execution_evidence": False})
        print(f"{status}: {source}")
    report = {
        "budget": "One unchanged translation-unit compile, max five diagnostics and thirty seconds, per surface.",
        "results": results,
        "verdict": "Core/ACL/region executable evidence is independent. Syntax success is not application execution.",
        "remaining_application_boundary": [
            "Actual role MyMesh.cpp includes RTClib hardware headers and concrete target.h globals.",
            "Need an approved full HAL profile for RTC/board/radio/sensor/serial/storage before application execution.",
            "Do not replace handlers; pending command/role rows require actual native application or stock-device captures."
        ],
        "stock_rf_evidence": False
    }
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
