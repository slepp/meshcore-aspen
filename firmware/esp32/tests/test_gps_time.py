# SPDX-License-Identifier: Apache-2.0
from pathlib import Path
import importlib.util
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
REVISION = "d92964352441e53b93e8667b802e04f6e072b39e"
SPEC = importlib.util.spec_from_file_location("gps_time", ROOT / "firmware/shared/gps_time.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def dependency_path(variable, required_files, description):
    value = os.environ.get(variable)
    if not value:
        raise RuntimeError(f"Set {variable} to {description}; see firmware/shared/GPS-CLOCK.md.")
    path = Path(value).expanduser().resolve()
    missing = [name for name in required_files if not (path / name).is_file()]
    if missing:
        raise RuntimeError(f"{variable}={path} is missing {', '.join(missing)}; "
                           f"set {variable} to {description}.")
    return path


class GpsTimeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.upstream = dependency_path(
            "GPS_TEST_UPSTREAM",
            ("src/helpers/sensors/LocationProvider.h", "src/MeshCore.h"),
            f"a MeshCore Git checkout at {REVISION}")
        revision = subprocess.run(
            ["git", "-C", str(cls.upstream), "rev-parse", "HEAD"],
            capture_output=True, text=True)
        if revision.returncode or revision.stdout.strip() != REVISION:
            raise RuntimeError(f"GPS_TEST_UPSTREAM must be checked out at {REVISION}; "
                               "use a separate pinned checkout, not the application repository.")
        clean = subprocess.run(
            ["git", "-C", str(cls.upstream), "diff", "--quiet", REVISION, "--", "src"],
            capture_output=True, text=True)
        if clean.returncode:
            raise RuntimeError("GPS_TEST_UPSTREAM has modified source headers; "
                               f"use an unmodified checkout at {REVISION}.")
        cls.nmea = dependency_path(
            "GPS_TEST_NMEA", ("MicroNMEA.h", "MicroNMEA.cpp"),
            "the MicroNMEA src directory installed by the pinned board's PlatformIO dependencies")

    def test_role_restart_retains_shared_gps(self):
        spec = importlib.util.spec_from_file_location("esp_prepare", ROOT / "firmware/esp32/prepare.py")
        prepare = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(prepare)
        for role in ("simple_repeater", "simple_room_server"):
            original = subprocess.check_output([
                "git", "-C", str(self.upstream), "show",
                f"{REVISION}:examples/{role}/MyMesh.cpp",
            ], text=True)
            transformed = prepare.preserve_shared_gps(original)
            self.assertNotIn("\n  applyGpsPrefs();", transformed)
            self.assertIn("GPS power belongs to the shared modem", transformed)

    def test_nmea_freshness_patch_rejects_missing_or_duplicate_anchors(self):
        original = subprocess.check_output([
            "git", "-C", str(self.upstream), "show",
            f"{REVISION}:src/helpers/sensors/MicroNMEALocationProvider.h",
        ], text=True)
        anchors = (
            "    long time_valid = 0;",
            "            nmea.process(c);",
            "        if (!isValid()) time_valid = 0;",
            "            if (isValid()) {\n                time_valid ++;",
        )
        for anchor in anchors:
            for replacement in ("", anchor + "\n" + anchor):
                with self.subTest(anchor=anchor, duplicate=bool(replacement)):
                    with tempfile.TemporaryDirectory() as directory:
                        sensors = Path(directory) / "src/helpers/sensors"
                        sensors.mkdir(parents=True)
                        (sensors / "MicroNMEALocationProvider.h").write_text(
                            original.replace(anchor, replacement))
                        with self.assertRaisesRegex(ValueError, "NMEA UTC freshness anchor changed"):
                            MODULE.stage_gps_time(directory)

    def test_real_nmea_provider_and_staging(self):
        upstream, nmea = self.upstream, self.nmea
        work = ROOT / ".cache/gps-region-validation"
        work.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=work) as directory:
            target = Path(directory)
            sensors = target / "src/helpers/sensors"
            sensors.mkdir(parents=True)
            original = subprocess.check_output([
                "git", "-C", str(upstream), "show",
                f"{REVISION}:src/helpers/sensors/MicroNMEALocationProvider.h",
            ], text=True)
            (sensors / "MicroNMEALocationProvider.h").write_text(original)
            manager = subprocess.check_output([
                "git", "-C", str(upstream), "show",
                f"{REVISION}:src/helpers/sensors/EnvironmentSensorManager.cpp",
            ], text=True)
            (sensors / "EnvironmentSensorManager.cpp").write_text(manager)
            MODULE.stage_gps_time(target)
            staged = (sensors / "MicroNMEALocationProvider.h").read_text()
            staged_manager = (sensors / "EnvironmentSensorManager.cpp").read_text()
            MODULE.stage_gps_time(target)
            self.assertEqual(staged, (sensors / "MicroNMEALocationProvider.h").read_text())
            self.assertEqual(staged_manager, (sensors / "EnvironmentSensorManager.cpp").read_text())
            previous = ("const uint32_t utc = getTimestamp();\n"
                        "                    if (!meshcore::publishGpsTime(utc)) return;\n"
                        "                    _clock->setCurrentTime(utc);")
            (sensors / "MicroNMEALocationProvider.h").write_text(staged.replace(
                "if (!meshcore::publishGpsTime(getTimestamp())) return;", previous))
            MODULE.stage_gps_time(target)
            self.assertEqual(staged, (sensors / "MicroNMEALocationProvider.h").read_text())
            rak = staged_manager.split("class RAK12500LocationProvider", 1)[1].split(
                "static RAK12500LocationProvider", 1)[0]
            (sensors / "RakGpsProvider.h").write_text("class RAK12500LocationProvider" + rak)
            binary = target / "gps-time"
            subprocess.run([
                "c++", "-std=c++17", "-O1", "-g", "-Wall", "-Wextra",
                "-I" + str(ROOT / "firmware/esp32/tests/gps_seams"),
                "-I" + str(ROOT / "test_support/parity/seams"),
                "-I" + str(nmea), "-I" + str(target / "src"),
                "-I" + str(upstream / "src/helpers/sensors"), "-I" + str(upstream / "src"),
                str(ROOT / "firmware/esp32/tests/gps_time.cpp"),
                str(nmea / "MicroNMEA.cpp"), "-o", str(binary),
            ], check=True, env={**os.environ, "TMPDIR": str(work)})
            subprocess.run([str(binary)], check=True)
            subprocess.run([
                "c++", "-std=c++17", "-O1", "-g", "-Wall", "-Wextra",
                "-I" + str(ROOT / "firmware/esp32/tests/gps_seams"),
                "-I" + str(ROOT / "test_support/parity/seams"),
                "-I" + str(nmea), "-I" + str(target / "src"),
                "-I" + str(upstream / "src/helpers/sensors"), "-I" + str(upstream / "src"),
                str(ROOT / "firmware/esp32/tests/gps_rak.cpp"),
                str(nmea / "MicroNMEA.cpp"), "-o", str(binary),
            ], check=True, env={**os.environ, "TMPDIR": str(work)})
            subprocess.run([str(binary)], check=True)
