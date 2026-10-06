# SPDX-License-Identifier: Apache-2.0
import copy
import hashlib
import io
import json
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import release_inputs as inputs


def package_bytes(files):
    output = io.BytesIO()
    with tarfile.open(fileobj=output, mode="w:gz") as archive:
        for name, content in files.items():
            member = tarfile.TarInfo(name)
            member.size = len(content)
            archive.addfile(member, io.BytesIO(content))
    return output.getvalue()


class InputTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.files = {"source.cpp": b"pinned source\n", "include/source.h": b"pinned header\n"}
        data = package_bytes(self.files)
        self.pins = copy.deepcopy(inputs.PINS)
        for name, pin in self.pins.items():
            pin["sha256"] = hashlib.sha256(data).hexdigest()
            archive = self.root / inputs.CACHE / Path(pin["url"]).name
            archive.parent.mkdir(parents=True, exist_ok=True)
            archive.write_bytes(data)
            directory = self.root / pin["path"]
            for path, content in self.files.items():
                target = directory / path
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(content)
            (directory / ".piopm").write_text(json.dumps({
                "type": "library", "name": name, "version": pin["version"],
                "spec": {"name": name, "owner": pin["url"].split("/")[4]},
            }))
        self.archives = copy.deepcopy(inputs.ARCHIVES)
        for name, pin in self.archives.items():
            data = (name + " archive fixture").encode()
            pin["sha256"] = hashlib.sha256(data).hexdigest()
            target = self.root / pin["path"]
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        for name, value in (("PINS", self.pins), ("ARCHIVES", self.archives)):
            context = patch.object(inputs, name, value)
            context.start()
            self.addCleanup(context.stop)

    def command(self, args):
        if args[:2] == ["git", "archive"]:
            destination = Path(next(arg.split("=", 1)[1] for arg in args if arg.startswith("--output=")))
            destination.write_bytes(package_bytes({"tracked.txt": b"tracked source"}))
        elif args[:2] == ["git", "clone"]:
            Path(args[-1]).mkdir(parents=True)
        else:
            self.fail(f"Unexpected command: {args}")

    def stage(self):
        source = self.root / "fresh"
        receipt = inputs.stage_source(self.root, source, self.command,
                                      lambda *args, **kwargs: inputs.load()["upstream"]["commit"])
        for path in (".tmp/onchip-native-worker/lib/OnchipLua", ".cache/meshcore-wamr/source/core",
                     ".cache/meshcore-wamr/source/build-scripts"):
            (source / path).mkdir(parents=True)
            (source / path / "prepared.c").write_bytes(b"prepared source")
        receipt["prepared"] = inputs.prepared_inventory(source, ".tmp/onchip-native-worker/lib/OnchipLua")
        return source, receipt

    def relink(self, source):
        output = self.root / "relink.tar.gz"
        with tarfile.open(output, "w:gz") as archive:
            for path in sorted(source.rglob("*")):
                archive.add(path, arcname=path.relative_to(source), recursive=False)
        return output

    def test_fresh_staging_ignores_extracted_and_compiled_caches(self):
        for path in (".tmp/onchip-lua/lua-5.5.1/src/evil.c",
                     ".cache/meshcore-wamr/source/evil.c", ".cache/meshcore-wamr/host/evil.o",
                     ".tmp/onchip-native-worker/evil.o"):
            target = self.root / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(b"modified cache")
        source, receipt = self.stage()
        self.assertEqual((source / "tracked.txt").read_bytes(), b"tracked source")
        self.assertFalse(list(source.rglob("evil.*")))
        for pin in self.pins.values():
            self.assertFalse((source / pin["path"] / ".piopm").exists())
        inputs.verify_relink(self.relink(source), receipt)
        with self.assertRaises(FileExistsError):
            inputs.stage_source(self.root, source, self.command, lambda *args, **kwargs: "")

    def test_changed_missing_extra_and_symlink_cached_source_rejected(self):
        for name, pin in self.pins.items():
            directory = self.root / pin["path"]
            for condition in ("changed", "missing", "extra", "symlink", "directory-link"):
                with self.subTest(name=name, condition=condition):
                    original = directory / "source.cpp"
                    extra = directory / "extra.cpp"
                    if condition == "changed":
                        original.write_bytes(b"altered")
                    elif condition == "missing":
                        original.unlink()
                    elif condition == "extra":
                        extra.write_bytes(b"altered")
                    elif condition == "symlink":
                        original.unlink()
                        original.symlink_to(directory / "include/source.h")
                    else:
                        extra.symlink_to(directory / "include", target_is_directory=True)
                    with self.assertRaisesRegex(ValueError, "source input differs|not a regular file"):
                        _, data = inputs.package_archive(self.root, name)
                        inputs.assert_inventory(inputs.package_inventory(directory, name, pin),
                                                inputs.archive_inventory(data), name)
                    if original.is_symlink():
                        original.unlink()
                    original.write_bytes(self.files["source.cpp"])
                    extra.unlink(missing_ok=True)

    def test_invalid_generated_metadata_rejected(self):
        pin = self.pins["Crypto"]
        metadata = self.root / pin["path"] / ".piopm"
        for data in (b"broken", b"{}", b'{"name":"Crypto","version":"99.0.0"}'):
            metadata.write_bytes(data)
            with self.assertRaisesRegex(ValueError, "metadata"):
                inputs.package_inventory(metadata.parent, "Crypto", pin)

    def test_wamr_unused_binding_symlink_is_not_a_compiled_input(self):
        source, receipt = self.stage()
        wamr = source / ".cache/meshcore-wamr/source"
        (wamr / "LICENSE").write_bytes(b"license")
        binding = wamr / "language-bindings/python"
        binding.mkdir(parents=True)
        (binding / "LICENSE").symlink_to("../../LICENSE")
        self.assertEqual(inputs.prepared_inventory(source, ".tmp/onchip-native-worker/lib/OnchipLua"),
                         receipt["prepared"])
        inputs.verify_relink(self.relink(source), receipt)
        (wamr / "core/extra.c").symlink_to("../LICENSE")
        with self.assertRaisesRegex(ValueError, "not a regular file"):
            inputs.prepared_inventory(source, ".tmp/onchip-native-worker/lib/OnchipLua")
        with self.assertRaisesRegex(ValueError, "not a regular file"):
            inputs.verify_relink(self.relink(source), receipt)

    def test_cached_archive_tamper_rejected_without_replacement(self):
        name, pin = "Crypto", self.pins["Crypto"]
        path = self.root / inputs.CACHE / Path(pin["url"]).name
        path.write_bytes(b"altered archive")
        with patch.object(inputs.urllib.request, "urlopen") as fetch:
            with self.assertRaisesRegex(ValueError, "SHA256 mismatch"):
                inputs.package_archive(self.root, name)
            fetch.assert_not_called()
        self.assertEqual(path.read_bytes(), b"altered archive")
        path.unlink()
        path.symlink_to(self.root / self.archives["lua"]["path"])
        with self.assertRaisesRegex(ValueError, "not a regular file"):
            inputs.package_archive(self.root, name)

    def test_unsafe_archive_members_rejected(self):
        for name in ("../escape", "/escape", "./source.cpp"):
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, "Unsafe"):
                inputs.archive_inventory(package_bytes({name: b"evil"}))
        for kind in (tarfile.SYMTYPE, tarfile.LNKTYPE, tarfile.FIFOTYPE):
            data = io.BytesIO()
            with tarfile.open(fileobj=data, mode="w:gz") as archive:
                member = tarfile.TarInfo("evil")
                member.type, member.linkname = kind, "source.cpp"
                archive.addfile(member)
            with self.assertRaisesRegex(ValueError, "Unsafe"):
                inputs.archive_inventory(data.getvalue())

    def test_offline_relink_detects_source_archive_and_receipt_tampering(self):
        source, receipt = self.stage()
        output = self.relink(source)
        inputs.verify_relink(output, receipt)
        duplicate = copy.deepcopy(receipt)
        duplicate["packages"]["Crypto"]["files"].append(duplicate["packages"]["Crypto"]["files"][0])
        with self.assertRaisesRegex(ValueError, "Duplicate"):
            inputs.verify_relink(output, duplicate)
        altered = copy.deepcopy(receipt)
        altered["packages"]["Crypto"]["sha256"] = "0" * 64
        with self.assertRaisesRegex(ValueError, "pin mismatch"):
            inputs.verify_relink(output, altered)
        for path in (self.pins["Crypto"]["path"] + "/source.cpp", self.archives["lua"]["path"],
                     ".tmp/onchip-native-worker/lib/OnchipLua/prepared.c"):
            target = source / path
            original = target.read_bytes()
            target.write_bytes(b"altered")
            with self.subTest(path=path), self.assertRaisesRegex(ValueError, "differs|pin mismatch"):
                inputs.verify_relink(self.relink(source), receipt)
            target.write_bytes(original)

    def test_firmware_uses_resolved_library_and_prepared_source_bytes(self):
        source, _ = self.stage()
        for bot in (False, True):
            with self.subTest(bot=bot):
                profile = "public_aspen" if bot else "public_birch"
                work = self.root / profile
                packages, _, prepared = inputs.input_layout(profile, bot=bot)
                for name, pin in packages.items():
                    directory = work / pin["path"]
                    directory.mkdir(parents=True)
                    for path, data in self.files.items():
                        target = directory / path
                        target.parent.mkdir(parents=True, exist_ok=True)
                        target.write_bytes(data)
                    (directory / ".piopm").write_bytes((self.root / self.pins[name]["path"] / ".piopm").read_bytes())
                for path in prepared:
                    (work / path).mkdir(parents=True)
                    (work / path / "prepared.c").write_bytes(b"prepared firmware")
                receipt = inputs.firmware_inputs(source, work, profile, bot=bot)
                inputs.verify_relink(self.relink(work), receipt, profile=profile, bot=bot)
                (work / packages["Crypto"]["path"] / "extra.cpp").write_bytes(b"extra")
                with self.assertRaisesRegex(ValueError, "differs"):
                    inputs.verify_relink(self.relink(work), receipt, profile=profile, bot=bot)


if __name__ == "__main__":
    unittest.main()
