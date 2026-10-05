# SPDX-License-Identifier: Apache-2.0
import unittest
from tools.hardware import lua_sources


class LuaSourcesTest(unittest.TestCase):
    def test_symbolic_builtin_keeps_room_for_additive_sources(self):
        parts = {"main": None, "monitor": b"function poll() end\nevents.every(15,'poll')\n"}
        self.assertEqual(lua_sources.decode(lua_sources.encode(parts)), parts)
        self.assertEqual(lua_sources.decode(lua_sources.encode({"main": None})), {"main": None})
        for parts in ({"main": None, "other": None},):
            with self.assertRaises(ValueError):
                lua_sources.encode(parts)
        for source in (b"--@meshcore-sources/1\n--@builtin main\n--@builtin other\n",
                       b"--@meshcore-sources/1\n--@builtin main\n--@source main 1\nx\n"):
            with self.assertRaises(ValueError):
                lua_sources.decode(source)

    def test_roundtrip_preserves_separate_sources(self):
        parts = {"main": b"function original() return 'unchanged' end\n",
                 "monitor": b"function poll() end\nevents.every(15,'poll')\n"}
        self.assertEqual(lua_sources.decode(lua_sources.encode(parts)), parts)
        self.assertEqual(lua_sources.decode(parts["main"]), {"main": parts["main"]})
        updated = dict(parts, monitor=b"function poll() return nil end\n")
        self.assertEqual(lua_sources.decode(lua_sources.encode(updated))["main"], parts["main"])

    def test_invalid_or_over_budget(self):
        for source in (b"", b"--@meshcore-sources/2\n", b"--@meshcore-sources/1\n",
                       b"--@meshcore-sources/1\n--@source a 9\nshort\n",
                       b"--@meshcore-sources/1\n--@source a 1\nx\n--@source a 1\ny\n"):
            with self.subTest(source=source), self.assertRaises(ValueError):
                lua_sources.decode(source)
        for parts in ({}, {"Bad": b"code"}, {"ok": b""}, {"ok": b"\x1bcode"},
                      {"ok": b"x" * 4096}, {f"a{i}": b"x" for i in range(9)}):
            with self.subTest(parts=list(parts)), self.assertRaises(ValueError):
                lua_sources.encode(parts)
