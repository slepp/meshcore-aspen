# SPDX-License-Identifier: Apache-2.0
import hashlib
import http.client
import os
import ssl
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
from firmware.esp32 import https_profile as hardware


class HttpsPreparation(unittest.TestCase):
    def test_private_literal_settings_never_execute_or_disclose(self):
        with tempfile.TemporaryDirectory(dir=hardware.ROOT / ".tmp") as directory:
            path = Path(directory) / ".env"
            path.write_text("UNRELATED=private\nONCHIP_BOT_HOME_HOST='home.example' # comment\n")
            path.chmod(0o600)
            self.assertEqual(hardware.settings(path), {"ONCHIP_BOT_HOME_HOST": "home.example"})
            path.write_text("ONCHIP_BOT_HOME_HOST='withheld-unclosed\n")
            with self.assertRaisesRegex(ValueError, "values withheld") as error:
                hardware.settings(path)
            self.assertNotIn("withheld-unclosed", str(error.exception))
            path.write_text("ONCHIP_BOT_HOME_HOST=a\nONCHIP_BOT_HOME_HOST=b\n")
            with self.assertRaisesRegex(ValueError, "Duplicate"):
                hardware.settings(path)

    def test_header_is_sealed_private_and_only_in_memory(self):
        source = hardware.header({"EXAMPLE": 'quote"slash\\??/and\n', "NUMBER": 7})
        fd = hardware.memory_header(source)
        path = Path(f"/proc/{os.getpid()}/fd/{fd}")
        try:
            self.assertEqual(path.read_bytes(), source)
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)
            with self.assertRaises(OSError):
                os.write(fd, b"change")
            self.assertNotIn(b'quote"', source)
            self.assertIn(b"#define NUMBER 7", source)
            length = len('quote"slash\\??/and\n'.encode()) + 1
            result = subprocess.run(
                ["cc", "-std=c11", "-Werror", "-x", "c", "-fsyntax-only", "-include", str(path), "-"],
                input=f'_Static_assert(sizeof(EXAMPLE) == {length}, "native literal");\n',
                capture_output=True, text=True, check=False)
            self.assertEqual(result.returncode, 0, result.stderr)
        finally:
            os.close(fd)
        self.assertFalse(path.exists())
        with self.assertRaises(ValueError):
            hardware.header({"BAD\n#define INJECT": "bad"})
        self.assertIn(b"#define FREQ 910.525", hardware.header({"FREQ": 910.525}))
        for value in (float("nan"), float("inf"), True):
            with self.assertRaises(ValueError):
                hardware.header({"FREQ": value})

    def test_ca_and_credential_bounds(self):
        roots = ssl.create_default_context().get_ca_certs(binary_form=True)
        self.assertTrue(roots)
        with tempfile.TemporaryDirectory(dir=hardware.ROOT / ".tmp") as directory:
            path = Path(directory) / "approved-ca.pem"
            path.write_text(ssl.DER_cert_to_PEM_cert(roots[0]))
            path.chmod(0o600)
            values = {"ONCHIP_BOT_HOME_ADDRESS": "192.0.2.1", "ONCHIP_BOT_HOME_HOST": "home.example",
                      "ONCHIP_BOT_HOME_CA_FILE": str(path), "ONCHIP_BOT_HOME_OPERATIONS": "7",
                      "MESHCORE_BOT_SERVICE_TOKEN": "fixture-" * 4}
            self.assertEqual(hardware.configuration(values)["ONCHIP_BOT_HOME_PORT"], 443)
            for key, value in (("ONCHIP_BOT_HOME_ADDRESS", "127.0.0.1"),
                               ("ONCHIP_BOT_HOME_ADDRESS", "224.0.0.1"),
                               ("ONCHIP_BOT_HOME_PORT", "0"),
                               ("ONCHIP_BOT_HOME_HOST", "host\r\nInjected"),
                               ("ONCHIP_BOT_HOME_OPERATIONS", "8"),
                               ("MESHCORE_BOT_SERVICE_TOKEN", "short")):
                with self.subTest(key=key), self.assertRaises(ValueError):
                    hardware.configuration(values | {key: value})
            path.write_text("invalid certificate")
            with self.assertRaisesRegex(ValueError, "contents withheld"):
                hardware.configuration(values)

    def test_backup_image_checksum_digest_and_layout(self):
        raw = bytearray(b"\xff" * 0x800000)
        struct.pack_into("<HBBII", raw, 0x8000, 0x50AA, 0, 0x10, 0x10000, 0x330000)
        image = bytearray(24)
        image[0], image[1], image[23] = 0xE9, 1, 1
        image += struct.pack("<II", 0x3C000020, 4) + b"test"
        checksum = 0xEF
        for byte in b"test":
            checksum ^= byte
        image += bytes(((len(image) + 16) & ~15) - len(image) - 1) + bytes((checksum,))
        image += hashlib.sha256(image).digest()
        raw[0x10000:0x10000 + len(image)] = image
        with patch.object(hardware, "backup_sha256", return_value=hashlib.sha256(raw).hexdigest()):
            self.assertEqual(hardware.backup_app(raw), image)
            with self.assertRaises(ValueError):
                hardware.backup_app(raw[:-1])
        raw[0x10000 + len(image) - 1] ^= 1
        with patch.object(hardware, "backup_sha256", return_value=hashlib.sha256(raw).hexdigest()):
            with self.assertRaisesRegex(ValueError, "digest mismatch"):
                hardware.backup_app(raw)

    def test_probe_build_never_passes_secret_to_subprocess_environment_or_arguments(self):
        with tempfile.TemporaryDirectory(dir=hardware.ROOT / ".tmp") as directory:
            target = Path(directory) / "onchip-test"
            secret = "fixture-only-" * 4

            def compile_stub(command, **kwargs):
                self.assertNotIn(secret, repr(command))
                self.assertNotIn(secret, repr(kwargs["env"]))
                path = Path(kwargs["env"]["ONCHIP_OPERATOR_HEADER"])
                self.assertEqual(path.read_bytes(), hardware.header({"ONCHIP_BOT_HOME_TOKEN": secret}))

            with patch.object(hardware, "DIRECTORY", Path(directory)), \
                    patch.object(hardware.subprocess, "run", side_effect=compile_stub), \
                    patch.dict(os.environ, {"MESHCORE_BOT_SERVICE_TOKEN": secret, "WIFI_PASSWORD": secret}):
                hardware.compile_image({"ONCHIP_BOT_HOME_TOKEN": secret}, target)

    def test_configured_build_preserves_explicit_wasm_compile_selection(self):
        with tempfile.TemporaryDirectory(dir=hardware.ROOT / ".tmp") as directory:
            target = Path(directory) / "onchip-test"
            for enabled in ("0", "1"):
                with self.subTest(enabled=enabled), \
                        patch.object(hardware, "DIRECTORY", Path(directory)), \
                        patch.object(hardware.subprocess, "run") as compile, \
                        patch.dict(os.environ, {"ONCHIP_BOT_WASM": enabled}):
                    hardware.compile_image({"ONCHIP_BOT_HOME_OPERATIONS": 0}, target)
                args, kwargs = compile.call_args
                self.assertIn("bot-firmware", args[0])
                self.assertEqual(kwargs["env"]["ONCHIP_BOT_WASM"], enabled)
