#!/usr/bin/env python3
"""Replay Lua through the native runner and optionally assert its JSONL records."""
import argparse
import json
from pathlib import Path
import subprocess
import sys


def matches(actual, expected):
    for key, value in expected.items():
        if key == "text_contains":
            if not isinstance(value, str) or value not in actual.get("text", ""):
                return False
        elif key not in actual or actual[key] != value:
            return False
    return True


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runner", required=True, type=Path)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--source", type=Path)
    source.add_argument("--bundled", action="store_true")
    replay = parser.add_mutually_exclusive_group()
    replay.add_argument("--replay", type=Path)
    replay.add_argument("--scenario", type=Path)
    args = parser.parse_args(argv)
    try:
        expected = None
        if args.scenario:
            scenario = json.loads(args.scenario.read_text())
            if not isinstance(scenario, dict) or set(scenario) != {"replay", "expect"}:
                raise ValueError("scenario requires exactly replay and expect")
            lines, expected = scenario["replay"], scenario["expect"]
            if not isinstance(lines, list) or not all(isinstance(line, str) and "\n" not in line for line in lines):
                raise ValueError("replay must be an array of single-line strings")
            if not isinstance(expected, list) or not all(
                isinstance(item, dict) and isinstance(item.get("type"), str) for item in expected
            ):
                raise ValueError("expect must be an array of objects with a type")
            text = "\n".join(lines) + "\n"
        elif args.replay:
            text = args.replay.read_text()
        else:
            text = sys.stdin.read()
        command = [str(args.runner.resolve()), "--format", "jsonl"]
        command += ["--source", str(args.source.resolve())] if args.source else ["--bundled"]
        result = subprocess.run(command, input=text, text=True, capture_output=True, timeout=120)
        sys.stdout.write(result.stdout)
        sys.stderr.write(result.stderr)
        if result.returncode:
            return 1
        records = [json.loads(line) for line in result.stdout.splitlines()]
        if not records or any(record.get("format") != "meshcore-bot-replay-v1" for record in records):
            raise ValueError("runner did not produce JSONL v1 records")
        if expected is None:
            return 0
        observed_types = {"reply", "async_reply", "pending", "reject", "error"} | {
            item["type"] for item in expected
        }
        actual = [
            record for record in records
            if record["type"] in observed_types or record.get("ok") is False
        ]
        if len(actual) != len(expected):
            print(f"ASSERT failed: expected {len(expected)} records, got {len(actual)}", file=sys.stderr)
            return 1
        for index, (record, wanted) in enumerate(zip(actual, expected), 1):
            if not matches(record, wanted):
                print(f"ASSERT {index} failed: expected {json.dumps(wanted)}, got {json.dumps(record)}",
                      file=sys.stderr)
                return 1
        print(f"PASS {len(expected)} native replay assertions", file=sys.stderr)
        return 0
    except (OSError, ValueError, TypeError, subprocess.TimeoutExpired) as error:
        print(f"Local replay: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
