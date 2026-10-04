"""Synthetic, private inventory for offline hardware-tool tests."""

import os
from pathlib import Path
import tempfile
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]


def configure_inventory(test):
    directory = tempfile.TemporaryDirectory()
    test.addCleanup(directory.cleanup)
    root = Path(directory.name)
    (root / ".tmp").mkdir()
    path = root / ".tmp/operator-inventory.json"
    path.write_bytes((ROOT / "tools/hardware/operator-inventory.json.example").read_bytes())
    path.chmod(0o600)
    environment = patch.dict(os.environ, {"MESHCORE_OPERATOR_CONFIG": str(path)})
    environment.start()
    test.addCleanup(environment.stop)
    defaults = patch("tools.hardware.inventory.ROOT", root)
    defaults.start()
    test.addCleanup(defaults.stop)
    return path
