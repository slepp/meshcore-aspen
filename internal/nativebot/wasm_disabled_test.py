# SPDX-License-Identifier: Apache-2.0
import os
import unittest
import worker_test


class WasmDisabledTest(worker_test.WorkerProcessTest):
    def test_disabled_runtime_is_advertised_and_rejected(self):
        process = self.start()
        self.ready(process)
        self.assertEqual(self.admin(process, "source api runtimes"),
                         "Runtimes lua-5.5.1/named-commands-v1")
        for command in ("api", "begin 1234567890abcdef 8 " + "00" * 32, "fetch package " + "00" * 32, "remove"):
            self.assertEqual(self.admin(process, "source wasm " + command),
                             "Error: Wasm runtime unavailable in this build")
        self.assertNotIn("wamr", self.admin(process, "source api package"))
        self.stop(process)


if __name__ == "__main__":
    unittest.main()
