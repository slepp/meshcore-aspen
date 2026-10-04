# SPDX-License-Identifier: Apache-2.0
"""Private device selection for hardware and deployment tools."""

from functools import lru_cache
import json
import os
from pathlib import Path
import re
import stat

ROOT = Path(__file__).resolve().parents[2]


@lru_cache(maxsize=1)
def _read(path):
    try:
        fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
    except OSError as error:
        raise ValueError(
            "Operator inventory unavailable; set MESHCORE_OPERATOR_CONFIG "
            "to your private inventory file"
        ) from error
    info = os.fstat(fd)
    if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077:
        os.close(fd)
        raise ValueError("Operator inventory must be an owner-only regular file (0600)")
    with os.fdopen(fd, "rb") as stream:
        raw = stream.read(16385)
    if len(raw) > 16384:
        raise ValueError("Operator inventory exceeds 16 KiB")
    try:
        document = json.loads(raw)
    except (ValueError, UnicodeError) as error:
        raise ValueError("Operator inventory contains invalid JSON") from error
    if not isinstance(document, dict) or document.get("format") != "meshcore-operator-inventory-v1":
        raise ValueError("Operator inventory requires format meshcore-operator-inventory-v1")
    return document


def value(key):
    path = os.environ.get("MESHCORE_OPERATOR_CONFIG")
    if not path:
        path = str(ROOT / ".tmp/operator-inventory.json")
    document = _read(str(Path(path).absolute()))
    if key not in document or document[key] is None:
        raise ValueError(f"Operator inventory requires {key}")
    item = document[key]
    if key in ("active_profile", "commissioning_profile"):
        if not isinstance(item, list) or len(item) != 5 or any(type(part) is not int for part in item):
            raise ValueError(f"Operator inventory {key} must contain five radio integers")
    elif key in ("peer_roles", "mast_roles"):
        if not isinstance(item, dict) or not item or any(
            not isinstance(identifier, str) or not isinstance(row, list) or len(row) != 2
            or any(not isinstance(part, str) or not part for part in row)
            for identifier, row in item.items()
        ):
            raise ValueError(f"Operator inventory {key} requires named role records")
    elif key == "telemetry_policy":
        if not isinstance(item, dict) or any(
            not isinstance(item.get(field), str) or not item[field]
            for field in ("host", "device", "endpoint")
        ) or type(item.get("interval_seconds")) is not int or type(item.get("enabled")) is not bool:
            raise ValueError("Operator inventory telemetry_policy has invalid fields")
    elif not isinstance(item, str) or not item:
        raise ValueError(f"Operator inventory {key} must be a nonempty string")
    if key.endswith("_mac") and not re.fullmatch(r"(?:[0-9a-f]{2}:){5}[0-9a-f]{2}", item):
        raise ValueError(f"Operator inventory {key} requires a lowercase hardware MAC")
    if key in ("dm_target", "backup_sha256") and not re.fullmatch(r"[0-9a-f]{64}", item):
        raise ValueError(f"Operator inventory {key} requires 64 lowercase hex digits")
    return item
