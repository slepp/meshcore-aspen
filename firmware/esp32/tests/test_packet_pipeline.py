# SPDX-License-Identifier: Apache-2.0
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class PacketPipelineTests(unittest.TestCase):
    def test_transactions_budgets_and_registration(self):
        with tempfile.TemporaryDirectory(dir=ROOT / ".tmp") as directory:
            binary = Path(directory) / "packet-pipeline"
            subprocess.run(
                [os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra",
                 "-Werror", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                 "-I", str(ROOT / "firmware/shared"),
                 str(ROOT / "firmware/esp32/tests/packet_pipeline.cpp"),
                 "-o", str(binary)], check=True,
            )
            subprocess.run([str(binary)], check=True)
