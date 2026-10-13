"""Private configuration inputs must return or refuse without blocking startup."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class ListenerConfiguration(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="willow-listener-config-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.fixture = Path(cls.directory.name)/"reader"
        source = Path(__file__).with_name("listener_config_fixture.c")
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-O2", str(source), "-o", str(cls.fixture), "-lcrypto"],
                       check=True, timeout=30)

    def read(self, path):
        result = subprocess.run([str(self.fixture), str(path)], capture_output=True,
                                check=True, timeout=2)
        return result.stdout

    def test_private_regular_configuration_is_unchanged(self):
        with tempfile.TemporaryDirectory(prefix="willow-config-input-") as name:
            path = Path(name)/"config"
            for size in (1, 4096):
                data = b"x"*size
                path.write_bytes(data)
                path.chmod(0o600)
                self.assertEqual(self.read(path), b"\x01"+data)
                self.assertEqual(path.read_bytes(), data)
            for size, mode in ((0, 0o600), (4097, 0o600), (10, 0o644)):
                path.write_bytes(b"x"*size)
                path.chmod(mode)
                self.assertNotEqual(self.read(path)[0], 1)

    def test_special_files_refuse_without_waiting_or_replacing(self):
        with tempfile.TemporaryDirectory(prefix="willow-config-input-") as name:
            root = Path(name)
            original = root/"regular"
            original.write_bytes(b"port=0\n")
            original.chmod(0o600)
            for kind in ("fifo", "directory", "symlink"):
                with self.subTest(kind=kind):
                    path = root/kind
                    if kind == "fifo":
                        os.mkfifo(path, 0o600)
                    elif kind == "directory":
                        path.mkdir(mode=0o700)
                    else:
                        path.symlink_to(original)
                    self.assertNotEqual(self.read(path)[0], 1)
                    self.assertTrue(path.exists())
                    self.assertEqual(original.read_bytes(), b"port=0\n")


if __name__ == "__main__":
    unittest.main()
