import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import prepare_wasm


class CMakeSourceCacheTests(unittest.TestCase):
    def test_source_move_resets_only_cmake_metadata(self):
        root = Path(__file__).resolve().parents[3]
        with tempfile.TemporaryDirectory(dir=root / ".tmp") as directory:
            build = Path(directory)
            cache = build / "CMakeCache.txt"
            metadata = build / "CMakeFiles"
            metadata.mkdir()
            (metadata / "source-selection").write_text("old source\n")
            library = build / "libmeshcore_wamr.a"
            library.write_bytes(b"retained build output")
            source = root / "packages/hew-wamr/native"
            prepare_wasm.reset_cmake_source_cache(build, source)
            self.assertTrue(metadata.is_dir())
            cache.write_text(f"CMAKE_HOME_DIRECTORY:INTERNAL={source}\n")
            prepare_wasm.reset_cmake_source_cache(build, source)
            self.assertTrue(cache.is_file())
            self.assertTrue(metadata.is_dir())
            cache.write_text(f"CMAKE_HOME_DIRECTORY:INTERNAL={root}/firmware/onchip/wasm\n")
            prepare_wasm.reset_cmake_source_cache(build, source)
            self.assertFalse(cache.exists())
            self.assertFalse(metadata.exists())
            self.assertEqual(library.read_bytes(), b"retained build output")

class EspThreadIdentityTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.source = Path(self.directory.name)
        native = Path(__file__).resolve().parents[3] / "packages/hew-wamr/native/vendor"
        text = (native / "core/shared/platform/esp-idf/espidf_thread.c").read_text()
        start = text.index("korp_tid\nos_self_thread(void)")
        end = text.index("\n}", start) + 2
        self.function = text[start:end]

    def compile_adapter(self, thread_manager):
        function = self.function
        source = self.source / "adapter.c"
        source.write_text(
            "#include <assert.h>\n#include <stdint.h>\n#include <stdlib.h>\n"
            "typedef uintptr_t korp_tid;\ntypedef void *TaskHandle_t;\n"
            "static TaskHandle_t current;\n"
            "static TaskHandle_t xTaskGetCurrentTaskHandle(void) { return current; }\n"
            "static korp_tid pthread_self(void) { abort(); }\n" + function +
            "\nint main(void) {\n"
            " int first, second;\n current = &first;\n"
            " korp_tid a = os_self_thread();\n"
            " assert(a == (uintptr_t)&first && os_self_thread() == a);\n"
            " assert((TaskHandle_t)(uintptr_t)a == current);\n"
            " current = &second;\n"
            " assert(os_self_thread() == (uintptr_t)&second && os_self_thread() != a);\n"
            " return 0;\n}\n"
        )
        binary = self.source / "adapter"
        result = subprocess.run(
            [*shlex.split(os.environ.get("CC", "cc")), "-std=c11",
             f"-DWASM_ENABLE_THREAD_MGR={thread_manager}", str(source), "-o", str(binary)],
            capture_output=True, text=True, timeout=20,
        )
        return result, binary

    def test_raw_task_identity_never_calls_pthread_self(self):
        result, binary = self.compile_adapter(0)
        self.assertEqual(result.returncode, 0, result.stderr)
        subprocess.run([str(binary)], check=True, timeout=5)

    def test_guest_thread_manager_configuration_is_rejected(self):
        result, _ = self.compile_adapter(1)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("FreeRTOS task identity requires WAMR thread manager disabled", result.stderr)

    def test_disabled_preparation_does_not_touch_wamr(self):
        with patch.object(prepare_wasm.subprocess, "run") as build:
            prepare_wasm.prepare(enabled=False)
        build.assert_not_called()


if __name__ == "__main__":
    unittest.main()
