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
            source = root / "firmware/runtime/wasm"
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

class PoolReallocAccountingTests(unittest.TestCase):
    def test_exact_idempotent_patch_and_rejection(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory)
            allocator = source / "core/shared/mem-alloc/ems/ems_alloc.c"
            allocator.parent.mkdir(parents=True)
            original = "prefix\n" + prepare_wasm.POOL_REALLOC_ORIGINAL + "\nsuffix\n"
            allocator.write_text(original)
            prepare_wasm.patch_pool_realloc_accounting(source)
            patched = "prefix\n" + prepare_wasm.POOL_REALLOC_PATCHED + "\nsuffix\n"
            self.assertEqual(allocator.read_text(), patched)
            prepare_wasm.patch_pool_realloc_accounting(source)
            self.assertEqual(allocator.read_text(), patched)
            for code in ("unexpected realloc", original * 2, patched * 2, original + patched):
                allocator.write_text(code)
                with self.assertRaisesRegex(ValueError, "pool realloc accounting patch"):
                    prepare_wasm.patch_pool_realloc_accounting(source)
                self.assertEqual(allocator.read_text(), code)


class EspThreadIdentityTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.source = Path(self.directory.name)
        self.thread = self.source / "core/shared/platform/esp-idf/espidf_thread.c"
        self.thread.parent.mkdir(parents=True)
        self.thread.write_text("/* retained prefix */\n" + prepare_wasm.ESP_THREAD_ID_ORIGINAL +
                               "\n/* retained suffix */\n")

    def test_patch_is_exact_idempotent_and_esp_only(self):
        linux = self.source / "core/shared/platform/linux"
        linux.mkdir(parents=True)
        native = linux / "platform.c"
        native.write_text("return pthread_self();\n")
        prepare_wasm.patch_esp_thread_identity(self.source)
        expected = ("/* retained prefix */\n" + prepare_wasm.ESP_THREAD_ID_PATCHED +
                    "\n/* retained suffix */\n")
        self.assertEqual(self.thread.read_text(), expected)
        prepare_wasm.patch_esp_thread_identity(self.source)
        self.assertEqual(self.thread.read_text(), expected)
        self.assertEqual(native.read_text(), "return pthread_self();\n")

    def test_unexpected_and_duplicate_source_fail_without_changes(self):
        for code in ("return an_unreviewed_thread();",
                     prepare_wasm.ESP_THREAD_ID_ORIGINAL * 2,
                     prepare_wasm.ESP_THREAD_ID_PATCHED * 2,
                     prepare_wasm.ESP_THREAD_ID_ORIGINAL + prepare_wasm.ESP_THREAD_ID_PATCHED,
                     prepare_wasm.ESP_THREAD_ID_PATCHED + prepare_wasm.ESP_THREAD_ID_ORIGINAL):
            with self.subTest(code=code):
                self.thread.write_text(code)
                with self.assertRaisesRegex(ValueError, "thread identity patch"):
                    prepare_wasm.patch_esp_thread_identity(self.source)
                self.assertEqual(self.thread.read_text(), code)

    def compile_adapter(self, thread_manager):
        prepare_wasm.patch_esp_thread_identity(self.source)
        function = self.thread.read_text()
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
        with patch.object(prepare_wasm, "patch_esp_thread_identity") as adapter, \
                patch.object(prepare_wasm.subprocess, "run") as build:
            prepare_wasm.prepare(enabled=False)
        adapter.assert_not_called()
        build.assert_not_called()


if __name__ == "__main__":
    unittest.main()
