#!/usr/bin/env python3
"""Send synthetic observer-v1 observations only to the approved internal broker."""
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
from tools.hardware.inventory import value as inventory_value

DIRECTORY = Path.home() / ".config/meshcore-mqtt"


def main():
    config = json.loads((DIRECTORY / "config.json").read_text())["mqtt"]
    port = config["broker_listen"].rsplit(":", 1)[-1]
    if not port.isdecimal() or not 1 <= int(port) <= 65535:
        raise ValueError("Configured broker listener requires a valid TCP port")
    values = {}
    for line in (DIRECTORY / "environment").read_text().splitlines():
        fields = shlex.split(line, comments=True)
        if fields:
            name, value = fields[0].split("=", 1)
            values[name] = value
    env = os.environ.copy()
    env.update(
        MESHCORE_OBSERVER_INTERNAL_TEST="1",
        MESHCORE_OBSERVER_TEST_HOST=inventory_value("service_address"),
        MESHCORE_OBSERVER_TEST_PORT=port,
        MESHCORE_OBSERVER_TEST_USERNAME=values[config["username_env"]],
        MESHCORE_OBSERVER_TEST_PASSWORD=values[config["password_env"]],
        TMPDIR=str(ROOT / ".tmp"),
    )
    subprocess.run(
        ["go", "test", "./internal/observer", "-run", "^TestApprovedInternalBroker$",
         "-count=1", "-v"], cwd=ROOT, env=env, check=True)


if __name__ == "__main__":
    main()
