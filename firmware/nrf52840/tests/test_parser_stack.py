# SPDX-License-Identifier: Apache-2.0
import importlib.util
from contextlib import redirect_stdout
import io
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

HERE = Path(__file__).resolve().parent.parent
SPEC = importlib.util.spec_from_file_location("parser_stack", HERE / "parser_stack.py")
PARSER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PARSER)


class ParserStackTest(unittest.TestCase):
    def test_cli_defaults_to_current_worker_source(self):
        with patch("sys.argv", ["parser_stack.py", "--build", "build",
                                "--lua-config", "luaconf.h"]), \
                patch.object(PARSER, "measure", return_value={}) as measure, \
                redirect_stdout(io.StringIO()):
            PARSER.main()
        worker = measure.call_args.args[2]
        self.assertEqual(worker, HERE.parent / "runtime/BotWorker.cpp")
        self.assertTrue(worker.is_file())

    def check(self, depth=32, initialize=72, missing=False, nrf_words=4096, esp_bytes=16384):
        scratch = HERE / ".build/native"
        scratch.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=scratch) as directory:
            root = Path(directory)
            lua = root / "lib1/OnchipLua"
            vm = root / "examples/nrfmast/onchip"
            lua.mkdir(parents=True)
            vm.mkdir(parents=True)

            def write(path, entries):
                path.write_text("".join(f"unit:1:1:{name}\t{size}\tstatic\n"
                                        for name, size in entries.items()))

            write(lua / "lparser.c.su", {
                "statement": 104, "body": 136, "statlist": 8, "globalstatfunc": 112,
                "block": 40, "forbody": 64, "test_then_block": 24, "initglobal": 88,
                "subexpr": 64, "constructor": 88, "recfield": 96, "suffixedexp": 48,
                "funcargs": 56, "explist": 16, "yindex": 16, "restassign": 64,
                "singlevaraux": 32, "luaY_parser": 208})
            write(lua / "lapi.c.su", {"lua_load": 64, "lua_pcallk": 40})
            write(lua / "lauxlib.c.su", {"luaL_loadbufferx": 24})
            write(lua / "ldo.c.su", {
                "luaD_pcall": 32, "luaD_rawrunprotected": 128, "ccall": 24,
                "luaD_precall": 32, "precallC": 40, "luaD_protectedparser": 88,
                "f_parser": 48})
            write(vm / "BotVm.cpp.su", {
                "int onchip::BotSession::Impl::initialize(lua_State*)": initialize,
                "bool onchip::BotSession::load(const char*)": 88})
            write(vm / "BotWorker.cpp.su", {
                "void onchip::BotWorker::entry(void*)": 8,
                "void onchip::BotWorker::run()": 408})
            config = root / "luaconf.h"
            config.write_text(f"#define LUAI_MAXCCALLS {depth}\n")
            worker = root / "BotWorker.cpp"
            worker.write_text(
                '#ifdef ARDUINO_ARCH_ESP32\n'
                f'if (xTaskCreate(entry, "mesh-command", {esp_bytes}, this, 1, &task_) != pdPASS) {{\n' +
                (f'#elif defined(NRF52_PLATFORM)\n'
                 f'if (xTaskCreate(entry, "mesh-command", {nrf_words}, this, 1, &task_) != pdPASS) {{\n'
                 if nrf_words is not None else '') +
                '#endif\n')
            if missing:
                (lua / "lparser.c.su").write_text("")
            return PARSER.measure(root, config, worker)

    def test_actual_frame_profile(self):
        result = self.check()
        self.assertEqual(result["initialize_frame_bytes"], 72)
        self.assertEqual(result["guarded_parser_cycle_bytes"]["global-function"], 360)
        self.assertLessEqual(result["conservative_parser_bound_bytes"], 16384)
        self.assertEqual(result["conservative_parser_bound_bytes"], 16024)
        self.assertEqual(result["leaf_and_context_reserve_bytes"], 2048)

    def test_nrf_words_not_esp_bytes(self):
        self.assertEqual(self.check(esp_bytes=1)["vm_task_stack_bytes"], 16384)
        with self.assertRaisesRegex(ValueError, "exceeds VM task stack 16000B"):
            self.check(nrf_words=4000, esp_bytes=32768)

    def test_missing_nrf_stack_rejected(self):
        with self.assertRaisesRegex(ValueError, "Expected one nRF52"):
            self.check(nrf_words=None)

    def test_old_manifest_frame_rejected(self):
        with self.assertRaisesRegex(ValueError, "initializer frame"):
            self.check(initialize=8736)

    def test_unsafe_depth_rejected(self):
        with self.assertRaisesRegex(ValueError, "exceeds VM task stack"):
            self.check(depth=40)

    def test_missing_frame_rejected(self):
        with self.assertRaisesRegex(ValueError, "Expected one ARM"):
            self.check(missing=True)
