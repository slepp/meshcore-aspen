# SPDX-License-Identifier: Apache-2.0
import copy
import hashlib
import io
import json
from pathlib import Path
import sys
import struct
import re
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import product_versions as versions
import release_candidate as candidate


class VersionTests(unittest.TestCase):
    def test_tags_and_user_labels(self):
        self.assertEqual(versions.tag("aspen", "0.1.0-rc.1"), "aspen-v0.1.0-rc.1")
        self.assertEqual(versions.tag("birch", "0.1.1"), "birch-v0.1.1")
        self.assertEqual(versions.display("aspen", "0.1.0-rc.1", "1.17.1"),
                         "Aspen 0.1.0 RC1 · based on MeshCore 1.17.1")
        self.assertEqual(versions.display("birch", "0.1.1", "1.17.1"),
                         "Birch 0.1.1 · based on MeshCore 1.17.1")

    def test_independent_versions_preserve_wire_contracts(self):
        data = versions.load()
        changed = copy.deepcopy(data)
        changed["products"]["aspen"]["version"] = "0.1.1"
        before, after = versions.generated(data), versions.generated(changed)
        self.assertEqual(before["internal/buildinfo/identity.go"], after["internal/buildinfo/identity.go"])
        self.assertEqual(before["release/versions.mk"], after["release/versions.mk"])
        self.assertIn('"aspen-0.1.1"', after["firmware/esp32/FirmwareIdentity.h"])
        changed = copy.deepcopy(data)
        changed["products"]["birch"]["version"] = "0.1.1"
        self.assertIn('HostVersion = "birch-0.1.1"', versions.generated(changed)["internal/buildinfo/identity.go"])
        self.assertEqual(changed["compatibility"], data["compatibility"])

    def test_validation_rejects_overlong_and_ambiguous_versions(self):
        for version in ("01.1.0", "0.1", "0.1.0-rc.0", "0.1.0-rc1", "10000.10000.10000"):
            with self.subTest(version=version), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                (root / "release").mkdir()
                data = versions.load()
                data["products"]["aspen"]["version"] = version
                (root / "release/products.json").write_text(json.dumps(data))
                with self.assertRaises(ValueError):
                    versions.load(root)

    def test_generated_identities_are_current(self):
        for path, text in versions.generated(versions.load()).items():
            self.assertEqual((ROOT / path).read_text(), text, path)


class CandidateTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        data = versions.load()
        config = candidate.PROFILES["aspen"][1]
        files = candidate.IMAGES | {"source.tar.gz", "firmware-relink.tar.gz", "build-profile.ini", "LICENSE", "NOTICE",
                                   "THIRD_PARTY.md", "dependency-notices.tar.gz"}
        for name in files:
            (self.directory / name).write_bytes(b"fixture")
        version = data["products"]["aspen"]["version"]
        (self.directory / "firmware.bin").write_bytes(b"ESP fixture " + versions.identity("aspen", version).encode())
        (self.directory / "build-profile.ini").write_bytes((ROOT / config).read_bytes())
        (self.directory / "partitions.bin").write_bytes(struct.pack(
            "<HBBII16sI", 0x50aa, 0, 0x10, 0x10000, 0x330000, b"app0", 0))
        sha = "a" * 40
        with tarfile.open(self.directory / "source.tar.gz", "w:gz", format=tarfile.PAX_FORMAT,
                          pax_headers={"comment": sha}) as archive:
            for path in [*versions.generated(data), "release/products.json", "go.sum", config]:
                content = (ROOT / path).read_bytes()
                info = tarfile.TarInfo(path)
                info.size = len(content)
                archive.addfile(info, io.BytesIO(content))
        self.manifest = {
            "schema_version": 1, "product": "aspen", "version": version,
            "tag": versions.tag("aspen", version), "identity": versions.identity("aspen", version),
            "source": {"repository": data["repository"], "commit": sha, "dirty": False},
            "upstream": data["upstream"], "compatibility": data["compatibility"],
            "layout": {"partitions": candidate.partitions(self.directory / "partitions.bin"),
                       "initial_install_offsets": {"bootloader.bin": 0, "partitions.bin": 0x8000,
                                                   "boot_app0.bin": 0xe000, "firmware.bin": 0x10000}},
            "build": {"profile": "public_aspen", "config_path": config,
                      "config_sha256": hashlib.sha256((ROOT / config).read_bytes()).hexdigest(),
                      "source_date_epoch": 1791180000,
                      "toolchain": {"platformio": "fixture", "python": "3.13", "build_os": "Linux",
                                    "firmware_cxx": "fixture", "firmware_cxx_sha256": "a" * 64},
                      "dependencies": {"resolved_packages": "fixture", "build_metadata": {"fixture": "1"}},
                      "lua_archive_sha256": "1c4b4068d67061f2a2231ad2b5422e77acea1487ea9890f6320af614f4373dce",
                      "wamr_commit": "b124f70345d712bead5c0c2393acb2dc583511de",
                      "go_sum_sha256": hashlib.sha256((ROOT / "go.sum").read_bytes()).hexdigest()},
            "qualification": {"state": "unqualified_candidate", "hardware_tested": False},
            "files": [candidate.record(self.directory / name, candidate.artifact_role(name)) for name in sorted(files)],
        }
        self.save()

    def save(self):
        (self.directory / "manifest.json").write_text(json.dumps(self.manifest))

    def test_complete_candidate_and_tamper_detection(self):
        candidate.verify(self.directory)
        (self.directory / "firmware.bin").write_bytes(b"wrong binary")
        with self.assertRaisesRegex(ValueError, "hash/size mismatch"):
            candidate.verify(self.directory)

    def test_old_images_cannot_be_relabelled(self):
        (self.directory / "firmware.bin").write_bytes(b"1.17.1-slp-aspen")
        self.manifest["files"] = [candidate.record(self.directory / item["name"], item["role"])
                                  for item in self.manifest["files"]]
        self.save()
        with self.assertRaisesRegex(ValueError, "identity mismatch"):
            candidate.verify(self.directory)

    def birch_metadata(self):
        version = versions.load()["products"]["birch"]["version"]
        self.manifest.update(product="birch", version=version, tag=versions.tag("birch", version),
                             identity=versions.identity("birch", version))
        self.manifest["build"].update(profile="public_birch", config_path="release/platformio.birch.ini")
        self.manifest["build"]["toolchain"].update(go="fixture", cxx="fixture", native_cxx_sha256="a" * 64,
                                                   readelf="fixture", readelf_sha256="a" * 64)
        self.manifest["build"].update(go_modules=[{"Path": "fixture"}], native_linked_libraries=[
            {"soname": name, "sha256": "a" * 64, "bytes": 1}
            for name in ("libcjson.so.1", "libssl.so.3", "libcrypto.so.3")])
        self.manifest["build"]["native_abi"] = {name: {
            "architecture": "x86_64", "interpreter": "/lib64/ld-linux-x86-64.so.2",
            "needed_sonames": ["libcjson.so.1", "libssl.so.3", "libcrypto.so.3"],
            "required_symbol_versions": {"GLIBC": "2.36", "GLIBCXX": "3.4.30", "CXXABI": "1.3.9", "OPENSSL": "3.0.0"},
            "distribution_qualified": False,
        } for name in candidate.HOST_FILES}

    def test_refuse_incomplete_birch_bundle(self):
        self.birch_metadata()
        self.save()
        with self.assertRaisesRegex(ValueError, "Incomplete product bundle"):
            candidate.verify(self.directory)

    def test_refuse_missing_and_false_birch_abi_claims(self):
        for change, error in (
            (lambda abi: abi.clear(), "missing host ABI receipts"),
            (lambda abi: abi["bot-native-worker"].update(distribution_qualified=True), "falsely qualified host ABI"),
            (lambda abi: abi["bot-native-worker"]["required_symbol_versions"].clear(), "omits required symbol versions"),
        ):
            with self.subTest(error=error):
                self.birch_metadata()
                change(self.manifest["build"]["native_abi"])
                self.save()
                with self.assertRaisesRegex(ValueError, error):
                    candidate.verify(self.directory)

    def test_refuse_private_files_and_unsafe_names(self):
        for name in ("private-profile.json", "../private-profile.json"):
            with self.subTest(name=name):
                self.manifest["files"].append({"name": name, "role": "fixture", "bytes": 0, "sha256": "a" * 64})
                self.save()
                with self.assertRaises(ValueError):
                    candidate.verify(self.directory)
                self.manifest["files"].pop()

    def test_refuse_false_hardware_qualification(self):
        self.manifest["qualification"]["hardware_tested"] = True
        self.save()
        with self.assertRaisesRegex(ValueError, "qualification requires separate review"):
            candidate.verify(self.directory)

    def test_refuse_wrong_artifact_roles(self):
        for item in self.manifest["files"]:
            item["role"] = "application"
        self.save()
        with self.assertRaisesRegex(ValueError, "artifact role mismatch"):
            candidate.verify(self.directory)

    def test_refuse_missing_provenance_receipts(self):
        self.manifest["build"]["toolchain"] = {"platformio": "fixture"}
        self.save()
        with self.assertRaisesRegex(ValueError, "missing toolchain receipt"):
            candidate.verify(self.directory)

    def test_refuse_wrong_partition_layout(self):
        self.manifest["layout"]["initial_install_offsets"]["firmware.bin"] = 0x8000
        self.save()
        with self.assertRaisesRegex(ValueError, "initial-install offsets mismatch"):
            candidate.verify(self.directory)

    def test_refuse_overlapping_or_invalid_partitions(self):
        cases = (
            [(0, 0x10, 0x10000, 0x330000, b"app0"), (1, 0x82, 0x10000, 0x330000, b"spiffs")],
            [(0, 0x10, 0x10000, 0, b"app0")],
            [(0, 0x10, 0x11000, 0x330000, b"app0")],
            [(0, 0x10, 0x10000, 0x10000, b"app0"), (0, 0x11, 0x20000, 0x10000, b"app0")],
        )
        for rows in cases:
            with self.subTest(rows=rows):
                path = self.directory / "partitions.bin"
                path.write_bytes(b"".join(struct.pack("<HBBII16sI", 0x50aa, *row, 0) for row in rows))
                with self.assertRaises(ValueError):
                    candidate.partitions(path)

    def test_refuse_wrong_archive_commit(self):
        self.manifest["source"]["commit"] = "b" * 40
        self.save()
        with self.assertRaisesRegex(ValueError, "archive does not identify"):
            candidate.verify(self.directory)

    def test_go_dependency_inventory_stream(self):
        self.assertEqual(candidate.json_stream('{"Path":"one"}\n{"Path":"two"}\n'),
                         [{"Path": "one"}, {"Path": "two"}])

    def test_birch_cold_staging_resolves_sntp_header_chain(self):
        dest = self.directory / "examples/kiss_modem"
        dest.mkdir(parents=True)
        candidate.stage_birch_files(self.directory)
        self.assertIn('#include "EspSntpClock.h"', (dest / "main.cpp").read_text())
        pending = ["EspSntpClock.h"]
        seen = set()
        while pending:
            name = pending.pop()
            if name in seen:
                continue
            seen.add(name)
            original = ROOT / "firmware/shared" / name
            self.assertEqual((dest / name).read_bytes(), original.read_bytes())
            for include in re.findall(r'^#include "([^\"]+)"', original.read_text(), re.M):
                if (ROOT / "firmware/shared" / include).is_file():
                    pending.append(include)
        self.assertIn("SntpConfig.h", seen)

    def test_elf_requirements_use_only_needed_versions_and_numeric_order(self):
        text = '''Version definition section '.gnu.version_d' contains entries:
          Name: GLIBC_99.0
        Version needs section '.gnu.version_r' contains entries:
          Name: GLIBC_2.9
          Name: GLIBC_2.43
          Name: GLIBC_2.2.5
          Name: GLIBC_PRIVATE
          Name: GLIBCXX_3.4.9
          Name: GLIBCXX_3.4.30
          Name: CXXABI_1.3.9
          Name: OPENSSL_3.0.0
        '''
        self.assertEqual(candidate.required_symbol_versions(text), {
            "GLIBC": "2.43", "GLIBCXX": "3.4.30", "CXXABI": "1.3.9", "OPENSSL": "3.0.0"})
        self.assertEqual(candidate.required_symbol_versions("No version information found in this file."), {})
