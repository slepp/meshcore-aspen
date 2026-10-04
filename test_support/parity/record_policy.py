#!/usr/bin/env python3
"""Capture a real policy differential run with stable input fingerprints."""
import argparse
import hashlib
import json
import os
import pathlib
import subprocess

from evidence import portable_text

ROOT = pathlib.Path(__file__).resolve().parents[2]
DATA = ROOT / "testdata/parity"


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def sources():
    directories = subprocess.check_output(
        ["go", "list", "-deps", "-f", "{{if not .Standard}}{{.Dir}}{{end}}", "./internal/policy"],
        cwd=ROOT, text=True).splitlines()
    paths = []
    for directory in directories:
        if directory and pathlib.Path(directory).is_relative_to(ROOT):
            paths.extend(sorted(pathlib.Path(directory).glob("*.go")))
    paths += [ROOT / "go.mod", ROOT / "go.sum"]
    return {str(path.relative_to(ROOT)): sha(path) for path in paths}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--events", type=pathlib.Path, required=True)
    args = parser.parse_args()
    events = args.events.resolve()
    before, native_hash = sources(), sha(events)
    env = dict(os.environ, MESHCORE_POLICY_ORACLE=str(events), TMPDIR=str(ROOT / ".tmp"))
    command = ["go", "test", "-race", "-count=1", "-json", "./internal/policy"]
    result = subprocess.run(command, cwd=ROOT, env=env, capture_output=True, text=True, timeout=180)
    log = events.parent / "policy-test.jsonl"
    log.write_text(portable_text(result.stdout))
    if result.returncode != 0:
        raise SystemExit(result.stdout + result.stderr)
    if before != sources() or native_hash != sha(events):
        raise SystemExit("Policy/native inputs changed during execution; evidence not recorded.")
    records = [json.loads(line) for line in result.stdout.splitlines()]
    if not any(e.get("Action") == "pass" and e.get("Test") == "TestNativeDifferential" for e in records):
        raise SystemExit("Native differential did not execute successfully; evidence not recorded.")
    report = json.loads((DATA / "policy-differential.json").read_text())
    report.update({"source_sha256": before, "native_events_sha256": native_hash,
                   "result_artifact": "testdata/parity/policy-test.jsonl",
                   "result_sha256": sha(log),
                   "command": "MESHCORE_POLICY_ORACLE=" + str(events.relative_to(ROOT)) +
                              " TMPDIR=.tmp " + " ".join(command)})
    (DATA / "policy-test.jsonl").write_bytes(log.read_bytes())
    (DATA / "policy-differential.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    print("Policy differential passed with unchanged implementation/native inputs; evidence recorded.")


if __name__ == "__main__":
    main()
