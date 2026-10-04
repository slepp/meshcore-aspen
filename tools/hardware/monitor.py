#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Read-only, finite one-hour observation of the commissioned owner mast."""
from datetime import datetime, timezone
import json
import os
import time

import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.hardware.mast_checks import DIRECTORY, mast_host, active_profile, dashboard, save

DURATION = 3600
INTERVAL = 30
REQUIRED_ROLES = ("repeater", "room", "companion", "management", "command-bot")
ERROR_COUNTERS = ("rx_errors", "tx_rejected", "tx_failed", "tx_unknown")
MEMORY_FIELDS = ("free_bytes", "minimum_bytes", "dma_free_bytes",
                 "dma_largest_bytes", "dma_minimum_bytes")
PHY_FIELDS = ("frequency_hz", "bandwidth_hz", "sf", "cr", "tx_power_dbm")


def utc_now():
    return datetime.now(timezone.utc).isoformat()


def snapshot(state):
    required = ("uptime_ms", "profile", "memory", "totals", "scheduler", "roles")
    if not isinstance(state, dict) or any(name not in state for name in required):
        raise ValueError("Incomplete mast status response")
    if type(state["uptime_ms"]) is not int or not isinstance(state["roles"], list):
        raise ValueError("Malformed mast uptime or role list")
    if not isinstance(state["scheduler"], dict):
        raise ValueError("Malformed mast scheduler status")
    for section, fields in (("profile", PHY_FIELDS), ("memory", MEMORY_FIELDS),
                            ("totals", ERROR_COUNTERS)):
        values = state[section]
        if not isinstance(values, dict) or any(type(values.get(name)) is not int for name in fields):
            raise ValueError("Malformed mast " + section)
    roles = {}
    for role in state["roles"]:
        if not isinstance(role, dict) or not isinstance(role.get("role"), str):
            raise ValueError("Malformed native role status")
        name = role["role"]
        if name in roles:
            raise ValueError("Duplicate native role status")
        roles[name] = {key: role.get(key) for key in ("state", "ready", "public_key", "fault")}
    return {"uptime_ms": state["uptime_ms"], "profile": state["profile"],
            "memory": {key: state["memory"][key] for key in MEMORY_FIELDS},
            "totals": state["totals"], "scheduler": state["scheduler"], "roles": roles}


def field_phy(reading):
    return tuple(reading["profile"][key] for key in PHY_FIELDS) == active_profile()


def problems(reading, baseline, previous):
    errors = []
    if not field_phy(reading) or reading["profile"].get("committed") is not True:
        errors.append("Field PHY changed or is not committed")
    if reading["profile"].get("fault") is not False:
        errors.append("Shared PHY fault or missing fault status")
    for name in REQUIRED_ROLES:
        role = reading["roles"].get(name)
        if not role or role["state"] != "running" or role["ready"] is not True:
            errors.append(name + " is not ready/running")
        elif not role["public_key"]:
            errors.append(name + " has no public identity")
        elif role["public_key"] != baseline["roles"].get(name, {}).get("public_key"):
            errors.append(name + " identity changed")
    for name, role in reading["roles"].items():
        if role["fault"]:
            errors.append(name + ": " + str(role["fault"]))
    if previous:
        if reading["uptime_ms"] < previous["uptime_ms"]:
            errors.append("Mast uptime decreased: restart or counter wrap")
        for name in ERROR_COUNTERS:
            delta = reading["totals"][name] - previous["totals"][name]
            if delta > 0:
                errors.append(name + " increased by " + str(delta))
            elif delta < 0:
                errors.append(name + " decreased: counter reset or wrap")
    return errors


def monitor():
    os.umask(0o077)
    DIRECTORY.mkdir(mode=0o700, exist_ok=True)
    initial = snapshot(dashboard())
    if not field_phy(initial):
        raise ValueError("Refusing field monitor before the authorized 912.525 lab commissioning")
    start = time.monotonic()
    started_at = utc_now()
    name = "monitor-" + str(time.time_ns())
    log = DIRECTORY / (name + ".jsonl")
    descriptor = os.open(log, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    minimum = dict(initial["memory"])
    previous = None
    error_count = unavailable = 0
    final = None
    print(f"Monitoring http://{mast_host()}/api/status for {DURATION}s from {started_at}; log {log}", flush=True)
    with os.fdopen(descriptor, "w") as output:
        for tick in range(DURATION // INTERVAL + 1):
            time.sleep(max(0, start + tick * INTERVAL - time.monotonic()))
            try:
                current = initial if tick == 0 else snapshot(dashboard())
                errors = problems(current, initial, previous)
                for key in minimum:
                    minimum[key] = min(minimum[key], current["memory"][key])
                previous = current
            except (OSError, ValueError) as error:
                current = None
                unavailable += 1
                errors = [type(error).__name__ + ": " + str(error)]
            final = current
            error_count += len(errors)
            record = {"at": utc_now(), "elapsed_seconds": time.monotonic() - start,
                      "reading": current, "errors": errors}
            output.write(json.dumps(record) + "\n")
            output.flush()
            os.fsync(output.fileno())
            if errors:
                print(f"Monitor sample {tick}: " + "; ".join(errors), flush=True)
    elapsed = time.monotonic() - start
    summary = {
        "endpoint": f"http://{mast_host()}/api/status", "started_at": started_at, "ended_at": utc_now(),
        "duration_seconds": elapsed, "required_seconds": DURATION,
        "completed": elapsed >= DURATION, "samples": DURATION // INTERVAL + 1,
        "error_count": error_count, "unavailable_samples": unavailable,
        "start_reading": initial, "end_reading": final, "last_successful_reading": previous,
        "minimum_observed_memory": minimum,
        "memory_delta_bytes": {key: final["memory"][key] - initial["memory"][key]
                               for key in minimum} if final else None,
        "raw_counter_deltas": {key: final["totals"][key] - initial["totals"][key]
                               for key in ERROR_COUNTERS} if final else None,
        "sample_log": str(log),
    }
    save(name + ".json", summary)
    print(f"One-hour monitor finished: errors={error_count}, unavailable={unavailable}; "
          f"summary {DIRECTORY / (name + '.json')}", flush=True)
    return summary


def main():
    result = monitor()
    if not result["completed"] or result["error_count"]:
        sys.exit(1)


if __name__ == "__main__":
    main()
