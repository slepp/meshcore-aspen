# SPDX-License-Identifier: Apache-2.0
import copy
import configparser
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
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import product_versions as versions
import release_candidate as candidate
import release_native as native
import install_birch as installer


def linux_elf_fixture(glibc="2.43"):
    """ELF64 dynamic/version-needs records, independent of the test host OS."""
    names = ["libcjson.so.1", "libssl.so.3", "libcrypto.so.3", "libc.so.6", "libstdc++.so.6",
             "GLIBC_" + glibc, "GLIBCXX_3.4.30", "CXXABI_1.3.9", "OPENSSL_3.0.0"]
    strings, offsets = bytearray(b"\0"), {}
    for name in names:
        offsets[name] = len(strings)
        strings.extend(name.encode() + b"\0")
    interpreter = b"/lib64/ld-linux-x86-64.so.2\0"
    dynamic = b"".join(struct.pack("<qQ", 1, offsets[name]) for name in names[:5]) + struct.pack("<qQ", 0, 0)
    records = []
    for library, versions in (("libc.so.6", ["GLIBC_" + glibc]),
                              ("libstdc++.so.6", ["GLIBCXX_3.4.30", "CXXABI_1.3.9"]),
                              ("libcrypto.so.3", ["OPENSSL_3.0.0"])):
        auxiliaries = b"".join(struct.pack("<IHHII", 0, 0, 2, offsets[name], 16 if i + 1 < len(versions) else 0)
                               for i, name in enumerate(versions))
        records.append(struct.pack("<HHIII", 1, len(versions), offsets[library], 16,
                                   16 + len(auxiliaries) if library != "libcrypto.so.3" else 0) + auxiliaries)
    body = interpreter + strings + dynamic + b"".join(records)
    string_offset = 120 + len(interpreter)
    dynamic_offset = string_offset + len(strings)
    version_offset = dynamic_offset + len(dynamic)
    section_offset = 120 + len(body)
    ident = b"\x7fELF\x02\x01\x01" + b"\0" * 9
    header = struct.pack("<16sHHIQQQIHHHHHH", ident, 3, 62, 1, 0, 64, section_offset, 0, 64, 56, 1, 64, 4, 0)
    program = struct.pack("<IIQQQQQQ", 3, 4, 120, 0, 0, len(interpreter), len(interpreter), 1)
    sections = b"\0" * 64
    sections += struct.pack("<IIQQQQIIQQ", 0, 3, 0, 0, string_offset, len(strings), 0, 0, 1, 0)
    sections += struct.pack("<IIQQQQIIQQ", 0, 6, 0, 0, dynamic_offset, len(dynamic), 1, 0, 8, 16)
    sections += struct.pack("<IIQQQQIIQQ", 0, 0x6ffffffe, 0, 0, version_offset, len(b"".join(records)), 1, 3, 4, 0)
    return header + program + body + sections


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
        changed["products"]["aspen"]["version"] = "0.1.2"
        before, after = versions.generated(data), versions.generated(changed)
        self.assertEqual(before["internal/buildinfo/identity.go"], after["internal/buildinfo/identity.go"])
        self.assertEqual(before["release/versions.mk"], after["release/versions.mk"])
        self.assertIn('"aspen-0.1.2"', after["firmware/esp32/FirmwareIdentity.h"])
        changed = copy.deepcopy(data)
        changed["products"]["birch"]["version"] = "0.1.1"
        self.assertIn('HostVersion = "birch-0.1.1"', versions.generated(changed)["internal/buildinfo/identity.go"])
        self.assertEqual(changed["compatibility"], data["compatibility"])
        changed = copy.deepcopy(data)
        changed["products"]["pine"]["version"] = "0.1.1"
        self.assertIn('"pine-0.1.1"', versions.generated(changed)["firmware/esp32/FirmwareIdentity.h"])
        self.assertEqual(before["internal/buildinfo/identity.go"], versions.generated(changed)["internal/buildinfo/identity.go"])
        changed = copy.deepcopy(data)
        changed["products"]["willow"]["version"] = "0.0.2"
        self.assertIn('"0.0.2"', versions.generated(changed)["experiments/hew-roles/release_identity.hew"])
        self.assertIn('"0.0.2"', versions.generated(changed)["experiments/hew-roles/release_identity.py"])

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

    def test_public_aspen_includes_unconfigured_native_https(self):
        config = configparser.ConfigParser(interpolation=None)
        config.read(ROOT / "firmware/esp32/platformio.public.ini.example")
        flags = {line.strip() for line in config["env:public_aspen"]["build_flags"].splitlines()}
        self.assertIn("-D ONCHIP_BOT_HTTPS=1", flags)
        self.assertIn("-D KISS_MAX_TCP_CLIENTS=3", flags)
        self.assertIn("-D MESHCORE_PUBLIC_PROVISIONING=1", flags)
        for name in ("ONCHIP_OPERATOR_HEADER", "ONCHIP_BOT_HOME_ADDRESS", "ONCHIP_BOT_HOME_TOKEN",
                     "ONCHIP_BOT_HOME_CA", "ONCHIP_MQTT_URI"):
            self.assertFalse(any(name in flag for flag in flags), name)


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

    def birch_host_metadata(self):
        self.birch_metadata()
        data = versions.load()
        build = self.manifest["build"]
        profile, config = candidate.HOST_PROFILE
        build.update(scope="host_only", profile=profile, config_path=config)
        build.pop("dependencies", None)
        for key in ("platformio", "firmware_cxx", "firmware_cxx_sha256"):
            build["toolchain"].pop(key, None)
        self.manifest["layout"] = {"modem": "external", "queued_phy_version": 1}
        for name in candidate.IMAGES | {"firmware-relink.tar.gz"}:
            (self.directory / name).unlink(missing_ok=True)
        (self.directory / "build-profile.ini").write_bytes((ROOT / config).read_bytes())
        build["config_sha256"] = candidate.digest(self.directory / "build-profile.ini")
        for name, relative in candidate.HOST_MATERIAL.items():
            (self.directory / name).write_bytes((ROOT / relative).read_bytes())
        for name in candidate.HOST_FILES:
            content = linux_elf_fixture("2.36")
            if name == "meshcore-host":
                content += self.manifest["identity"].encode()
            (self.directory / name).write_bytes(content)
            build["native_abi"][name] = candidate.inspect_elf(self.directory / name)
        build["native_linked_libraries"].extend(
            {"soname": name, "sha256": "a" * 64, "bytes": 1}
            for name in ("libc.so.6", "libstdc++.so.6"))
        build["native_environment"] = {
            "profile": native.PROFILE, "image_id": "sha256:" + "a" * 64,
            "dockerfile": native.DOCKERFILE, "dockerfile_sha256": candidate.digest(ROOT / native.DOCKERFILE),
            "source_commit": self.manifest["source"]["commit"],
            "source_date_epoch": build["source_date_epoch"],
            "distribution": {"ID": "debian", "VERSION_ID": "12"},
            "native_abi": build["native_abi"], "native_linked_libraries": build["native_linked_libraries"],
            "go_modules": build["go_modules"], "toolchain": build["toolchain"],
            "source_inputs": {"fixture": "checked separately"},
        }
        (self.directory / "native-relink.tar.gz").write_bytes(b"fixture")
        with tarfile.open(self.directory / "source.tar.gz", "w:gz", format=tarfile.PAX_FORMAT,
                          pax_headers={"comment": self.manifest["source"]["commit"]}) as archive:
            for relative in [*versions.generated(data), "release/products.json", "go.sum", config,
                             native.DOCKERFILE, "tools/release_inputs.py", *candidate.HOST_MATERIAL.values()]:
                content = (ROOT / relative).read_bytes()
                info = tarfile.TarInfo(relative)
                info.size = len(content)
                archive.addfile(info, io.BytesIO(content))
        self.manifest["files"] = [candidate.record(path, candidate.artifact_role(path.name))
                                  for path in sorted(self.directory.iterdir()) if path.name != "manifest.json"]
        self.save()

    def test_host_only_birch_checks_native_material_without_modem(self):
        self.birch_host_metadata()
        with patch.object(candidate, "verify_relink") as checked:
            candidate.verify(self.directory)
        checked.assert_called_once_with(self.directory / "native-relink.tar.gz",
                                        self.manifest["build"]["native_environment"]["source_inputs"])
        self.assertFalse(any((self.directory / name).exists() for name in candidate.IMAGES))

    def test_host_only_requires_native_receipts_and_rejects_firmware_claims(self):
        for change, error in (
            (lambda build: build.pop("native_environment"), "requires verified Debian 12"),
            (lambda build: build["native_environment"].pop("source_inputs"), "requires verified Debian 12"),
            (lambda build: build.update(firmware_source_inputs={}), "must not claim a modem"),
        ):
            with self.subTest(error=error):
                self.birch_host_metadata()
                change(self.manifest["build"])
                self.save()
                with self.assertRaisesRegex(ValueError, error):
                    candidate.verify(self.directory)

    def test_aspen_cannot_select_host_only(self):
        with self.assertRaisesRegex(ValueError, "supported only for Birch"):
            candidate.build("aspen", "refs/heads/main", self.directory, host_only=True)
        self.manifest["build"]["scope"] = "host_only"
        self.save()
        with self.assertRaisesRegex(ValueError, "unsupported product scope"):
            candidate.verify(self.directory)

    def test_birch_installer_preserves_existing_install_and_uses_native_worker(self):
        self.birch_host_metadata()
        prefix = self.directory / "installed"
        config_path = installer.install(self.directory, prefix)
        config = json.loads(config_path.read_text())
        self.assertEqual(config["bot_runtime"], "native_lua")
        self.assertEqual(config["bot_native_worker"], str(prefix / "bin/bot-native-worker"))
        self.assertEqual(config["state_dir"], str(prefix / "state"))
        self.assertEqual((config["phy_authority"], config["phy_tracking"]), ("modem", "follow"))
        self.assertEqual(config_path.stat().st_mode & 0o777, 0o600)
        before = config_path.read_bytes()
        with self.assertRaises(FileExistsError):
            installer.install(self.directory, prefix)
        self.assertEqual(config_path.read_bytes(), before)

    def test_birch_installer_rejects_tamper_before_creating_prefix(self):
        self.birch_host_metadata()
        (self.directory / "bot-native-worker").write_bytes(b"changed")
        prefix = self.directory / "not-created"
        with self.assertRaisesRegex(ValueError, "hash/size mismatch"):
            installer.install(self.directory, prefix)
        self.assertFalse(prefix.exists())

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

    def test_debian12_environment_rejects_wrong_source_and_newer_abi(self):
        self.birch_metadata()
        build = self.manifest["build"]
        receipt = {"profile": native.PROFILE, "image_id": "sha256:" + "a" * 64,
                   "dockerfile": native.DOCKERFILE, "dockerfile_sha256": "a" * 64,
                   "source_commit": self.manifest["source"]["commit"],
                   "source_date_epoch": build["source_date_epoch"],
                   "distribution": {"ID": "debian", "VERSION_ID": "12"},
                   "native_abi": build["native_abi"], "native_linked_libraries": build["native_linked_libraries"],
                   "go_modules": build["go_modules"], "toolchain": {"go": "fixture", "cxx": "fixture"}}
        build["native_environment"] = receipt
        receipt["source_commit"] = "b" * 40
        self.save()
        with self.assertRaisesRegex(ValueError, "environment receipt mismatch"):
            candidate.verify(self.directory)
        receipt["source_commit"] = self.manifest["source"]["commit"]
        build["native_abi"]["bot-native-worker"]["required_symbol_versions"]["GLIBC"] = "2.43"
        self.save()
        with self.assertRaisesRegex(ValueError, "exceeds its Debian 12 baseline"):
            candidate.verify(self.directory)

    def test_native_container_isolated_mounts_and_network(self):
        args = native.container_command("sha256:" + "a" * 64, Path("/fresh-public-source"),
                                        Path("/public-go-sdk"), ["make", "host-build"])
        self.assertIn("type=bind,source=/fresh-public-source,target=/src", args)
        self.assertIn("type=bind,source=/public-go-sdk,target=/opt/go,readonly", args)
        self.assertEqual(args[args.index("--network") + 1], "none")
        self.assertIn("--read-only", args)
        self.assertIn("no-new-privileges", args)
        self.assertFalse(any("docker.sock" in arg or "/dev/tty" in arg for arg in args))
        with self.assertRaises(ValueError):
            native.container_command("image", Path("/src"), Path("/go"), ["make"], network="host")

    def test_refuse_private_files_and_unsafe_names(self):
        for name in ("private-profile.json", "../private-profile.json"):
            with self.subTest(name=name):
                self.manifest["files"].append({"name": name, "role": "fixture", "bytes": 0, "sha256": "a" * 64})
                self.save()
                with self.assertRaises(ValueError):
                    candidate.verify(self.directory)
                self.manifest["files"].pop()

    def test_verify_legacy_manifest_with_build_status(self):
        self.manifest["qualification"] = {"state": "unqualified_candidate", "hardware_tested": False}
        self.save()
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

    def test_new_source_cannot_omit_verified_input_receipts(self):
        paths = [*versions.generated(versions.load()), "release/products.json", "go.sum",
                 self.manifest["build"]["config_path"], "tools/release_inputs.py"]
        with tarfile.open(self.directory / "source.tar.gz", "w:gz", format=tarfile.PAX_FORMAT,
                          pax_headers={"comment": self.manifest["source"]["commit"]}) as archive:
            for path in paths:
                archive.add(ROOT / path, arcname=path)
        self.manifest["files"] = [candidate.record(self.directory / item["name"], item["role"])
                                  for item in self.manifest["files"]]
        self.save()
        with self.assertRaisesRegex(ValueError, "missing verified firmware source inputs"):
            candidate.verify(self.directory)

    def test_go_dependency_inventory_stream(self):
        self.assertEqual(candidate.json_stream('{"Path":"one"}\n{"Path":"two"}\n'),
                         [{"Path": "one"}, {"Path": "two"}])

    def test_loader_package_receipt_ignores_dpkg_diversion_metadata(self):
        path = Path("/lib64/ld-linux-x86-64.so.2")
        output = f"diversion by libc6 from: {path}\ndiversion by libc6 to: /lib64/ld-alt.so\nlibc6:amd64: {path}\n"
        self.assertEqual(candidate.package_owner(output, path), "libc6:amd64")
        self.assertIsNone(candidate.package_owner(output, Path("/different-loader")))

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

    def test_elf_inspection_rejects_truncated_and_wrong_architecture(self):
        path = self.directory / "worker"
        path.write_bytes(linux_elf_fixture())
        self.assertEqual(candidate.inspect_elf(path)["required_symbol_versions"]["GLIBC"], "2.43")
        for content in (linux_elf_fixture()[:55],
                        linux_elf_fixture()[:18] + struct.pack("<H", 183) + linux_elf_fixture()[20:]):
            path.write_bytes(content)
            with self.assertRaises(ValueError):
                candidate.inspect_elf(path)

    def test_refuse_lowered_abi_without_changing_packaged_binary(self):
        self.complete_birch()
        candidate.verify(self.directory)
        original_files = copy.deepcopy(self.manifest["files"])
        self.manifest["build"]["native_abi"]["bot-native-worker"]["required_symbol_versions"]["GLIBC"] = "2.36"
        self.save()
        with self.assertRaisesRegex(ValueError, "ABI receipt differs from packaged ELF"):
            candidate.verify(self.directory)
        self.assertEqual(self.manifest["files"], original_files)

    def complete_birch(self, glibc="2.43", debian=False):
        self.birch_metadata()
        config = candidate.PROFILES["birch"][1]
        (self.directory / "build-profile.ini").write_bytes((ROOT / config).read_bytes())
        self.manifest["build"]["config_sha256"] = candidate.digest(self.directory / "build-profile.ini")
        (self.directory / "firmware.bin").write_bytes(versions.identity("birch", self.manifest["version"]).encode())
        with tarfile.open(self.directory / "source.tar.gz", "w:gz", format=tarfile.PAX_FORMAT,
                          pax_headers={"comment": self.manifest["source"]["commit"]}) as archive:
            names = [*versions.generated(versions.load()), "release/products.json", "go.sum", config]
            if debian:
                names.append(native.DOCKERFILE)
            for name in names:
                archive.add(ROOT / name, arcname=name)
        for name in candidate.HOST_FILES:
            (self.directory / name).write_bytes(linux_elf_fixture(glibc))
            self.manifest["build"]["native_abi"][name] = candidate.inspect_elf(self.directory / name)
        (self.directory / "native-relink.tar.gz").write_bytes(b"fixture")
        self.manifest["build"]["native_linked_libraries"] = [
            {"soname": name, "sha256": "a" * 64, "bytes": 1}
            for name in self.manifest["build"]["native_abi"]["bot-native-worker"]["needed_sonames"]]
        if debian:
            build = self.manifest["build"]
            build["native_environment"] = {
                "profile": native.PROFILE, "image_id": "sha256:" + "a" * 64,
                "dockerfile": native.DOCKERFILE, "dockerfile_sha256": candidate.digest(ROOT / native.DOCKERFILE),
                "source_commit": self.manifest["source"]["commit"], "source_date_epoch": build["source_date_epoch"],
                "distribution": {"ID": "debian", "VERSION_ID": "12"},
                "native_abi": copy.deepcopy(build["native_abi"]),
                "native_linked_libraries": copy.deepcopy(build["native_linked_libraries"]),
                "go_modules": copy.deepcopy(build["go_modules"]), "toolchain": {"go": "fixture", "cxx": "fixture"}}
        self.manifest["files"] = [candidate.record(path, candidate.artifact_role(path.name))
                                  for path in sorted(self.directory.iterdir()) if path.name != "manifest.json"]
        self.save()

    def test_complete_debian12_birch_preserves_product_identity_and_checks_elf(self):
        self.complete_birch("2.34", debian=True)
        candidate.verify(self.directory)
        self.assertEqual(self.manifest["identity"], versions.identity("birch", self.manifest["version"]))
        build = self.manifest["build"]
        for receipts in (build["native_abi"], build["native_environment"]["native_abi"]):
            receipts["bot-native-worker"]["required_symbol_versions"]["GLIBC"] = "2.33"
        self.save()
        with self.assertRaisesRegex(ValueError, "ABI receipt differs from packaged ELF"):
            candidate.verify(self.directory)
