# SPDX-License-Identifier: Apache-2.0
import unittest
from tools.hardware import lua_sources


class LuaSourcesTest(unittest.TestCase):
    def exact_envelope(self, limit, entries):
        parts = {f"file{i}": b"function f" + str(i).encode() + b"() end\n" for i in range(entries)}
        last = f"file{entries - 1}"
        base = parts[last]
        parts[last] = base + b"--\n"
        padding = limit - len(lua_sources.encode(parts, limit, entries)) - 8
        while True:
            parts[last] = base + b"--" + b"x" * padding + b"\n"
            source = lua_sources.encode(parts, limit, entries)
            if len(source) == limit:
                return parts, base, source
            self.assertLess(len(source), limit, "Exact source boundary was skipped")
            padding += 1

    def test_exact_envelope_and_one_file_replacement(self):
        for limit, entries in ((4096, 8), (lua_sources.LIMIT, lua_sources.ENTRIES)):
            with self.subTest(limit=limit, entries=entries):
                parts, base, source = self.exact_envelope(limit, entries)
                last = f"file{entries - 1}"
                self.assertEqual(lua_sources.decode(source, limit, entries), parts)
                with self.assertRaises(ValueError):
                    lua_sources.encode(dict(parts, **{last: parts[last] + b" "}), limit, entries)
                with self.assertRaises(ValueError):
                    lua_sources.decode(source + b"x", limit, entries)
                replacement = dict(parts, file3=b"function f3() return 'updated' end\n", **{last: base})
                actual = lua_sources.decode(lua_sources.encode(replacement, limit, entries), limit, entries)
                self.assertEqual(actual, replacement)
                for name in parts.keys() - {"file3", last}:
                    self.assertEqual(actual[name], parts[name])
                del actual["file3"]
                self.assertEqual(lua_sources.decode(lua_sources.encode(actual, limit, entries), limit, entries), actual)
                with self.assertRaises(ValueError):
                    lua_sources.encode(dict(parts, extra=b"function extra() end"), limit, entries)

    def test_advertised_limits(self):
        self.assertEqual(lua_sources.advertised_limits(
            "Sources format=meshcore-sources/1 count=16 envelope=32768 namespace=shared"),
            {"limit": 32768, "entries": 16})
        self.assertEqual(lua_sources.advertised_limits("Sources format=meshcore-sources/1"),
                         {"limit": 4096, "entries": 8})

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
                      {"ok": b"x" * lua_sources.LIMIT},
                      {f"a{i}": b"x" for i in range(lua_sources.ENTRIES + 1)}):
            with self.subTest(parts=list(parts)), self.assertRaises(ValueError):
                lua_sources.encode(parts)
