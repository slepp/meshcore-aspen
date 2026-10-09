# SPDX-License-Identifier: Apache-2.0
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class PacketProgramTests(unittest.TestCase):
    def test_durable_control_and_async_loader(self):
        with tempfile.TemporaryDirectory(dir=ROOT / ".tmp") as directory:
            binary = Path(directory) / "packet-programs"
            subprocess.run(
                [os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                 "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-pthread",
                 "-I", str(ROOT / "firmware/shared"), "-I", str(ROOT / "firmware/runtime"),
                 str(ROOT / "firmware/esp32/tests/packet_programs.cpp"),
                 str(ROOT / "firmware/runtime/PacketPrograms.cpp"),
                 str(ROOT / "firmware/runtime/PacketProgramWorker.cpp"),
                 "-lcrypto", "-o", str(binary)], check=True,
            )
            subprocess.run([str(binary)], check=True)
