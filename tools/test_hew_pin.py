"""The Hew revision in CI matches the one documented for Willow."""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SHA = re.compile(r"\b[0-9a-f]{40}\b")


class HewPinTest(unittest.TestCase):
    def test_workflow_and_guide_name_the_same_revision(self):
        workflow = (ROOT / ".github/workflows/check.yml").read_text()
        guide = (ROOT / "hosts/willow/docs/LIBRARIES.md").read_text()
        ref = re.search(r"repository: hew-lang/hew\s+ref: ([0-9a-f]{40})\b", workflow)
        self.assertIsNotNone(ref, "check.yml must pin hew-lang/hew to a full revision")
        section = guide.split("A compatible toolchain is Hew source", 1)[1][:200]
        documented = SHA.search(section)
        self.assertIsNotNone(documented, "LIBRARIES.md must name the compatible Hew revision")
        self.assertEqual(ref.group(1), documented.group(0))


if __name__ == "__main__":
    unittest.main()
