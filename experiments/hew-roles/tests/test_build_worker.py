# SPDX-License-Identifier: Apache-2.0
import importlib.util
import json
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location(
    "willow_build_worker", Path(__file__).resolve().parents[1] / "build_worker.py")
worker = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(worker)


class SourceIdentity(unittest.TestCase):
    def test_git_archive_identity_outside_git_and_inside_another_repository(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            checkout = root / "checkout"
            checkout.mkdir()
            subprocess.run(["git", "init", "-q", str(checkout)], check=True)
            (checkout / "release").mkdir()
            (checkout / "release/source.json").write_bytes(
                (worker.ROOT / "release/source.json").read_bytes())
            (checkout / ".gitattributes").write_bytes(
                (worker.ROOT / ".gitattributes").read_bytes())
            subprocess.run(["git", "-C", str(checkout), "add", "."], check=True)
            subprocess.run([
                "git", "-C", str(checkout), "-c", "user.name=Archive test",
                "-c", "user.email=archive@example.invalid", "-c", "commit.gpgsign=false",
                "commit", "-qm", "Archive source identity",
            ], check=True)
            expected = worker.source_identity(checkout)
            archive = root / "source.tar"
            subprocess.run(["git", "-C", str(checkout), "archive", "--format=tar",
                            "-o", str(archive), "HEAD"], check=True)
            extracted = root / "extracted"
            extracted.mkdir()
            with tarfile.open(archive) as package:
                package.extractall(extracted, filter="data")
            self.assertEqual(worker.source_identity(extracted), expected)
            subprocess.run(["git", "init", "-q", str(root)], check=True)
            self.assertEqual(worker.source_identity(extracted), expected)

    def test_invalid_archive_metadata(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "release").mkdir()
            for value in (
                {"commit": "$Format:%H$", "commit_epoch": "$Format:%ct$"},
                {"commit": "a" * 40, "commit_epoch": "0"},
                {"commit": "a" * 40, "commit_epoch": 1791419594},
                {"commit": "wrong", "commit_epoch": "1791419594"},
            ):
                with self.subTest(value=value):
                    (root / "release/source.json").write_text(json.dumps(value))
                    with self.assertRaises(ValueError):
                        worker.source_identity(root)


if __name__ == "__main__":
    unittest.main()
