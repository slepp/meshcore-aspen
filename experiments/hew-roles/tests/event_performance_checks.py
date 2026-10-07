"""Deterministic accepted-reference noise envelope cases."""
import unittest
from unittest.mock import patch

from event_performance import noise_envelope


def sample(mean, rates, field):
    total = 0
    rows = [{"elapsed": 0, "processes": {"host": {field: total}}}]
    for index, rate in enumerate(rates, 1):
        total += rate
        rows.append({"elapsed": index, "processes": {"host": {field: total}}})
    return {"cpu_percent_one_core": mean,
            "processes": {"host": {"messages_per_second": mean}}, "samples": rows}


class NoiseEnvelope(unittest.TestCase):
    def test_reference_drift(self):
        with patch("event_performance.os.sysconf", return_value=100):
            envelope = noise_envelope(sample(2, [2, 2], "cpu_ticks"),
                                      sample(3, [3, 3], "cpu_ticks"), "cpu_ticks")
        self.assertEqual(envelope["observed_drift"], 1)
        self.assertEqual(envelope["limit"], 4)

    def test_reference_sampling_noise(self):
        envelope = noise_envelope(sample(10, [0, 20], "messages"),
                                  sample(10, [0, 20], "messages"), "messages")
        self.assertEqual(envelope["observed_drift"], 0)
        self.assertGreater(envelope["two_standard_errors"], 10)
        self.assertEqual(envelope["limit"], 10 + envelope["two_standard_errors"])

    def test_native_process_without_actor_counters(self):
        first = sample(10, [10, 10], "messages")
        second = sample(10, [10, 10], "messages")
        for value in (first, second):
            for row in value["samples"]:
                row["processes"]["native"] = {"cpu_ticks": 10}
        self.assertEqual(noise_envelope(first, second, "messages")["limit"], 10)

    def test_invalid_counters(self):
        first = sample(10, [10, 10], "messages")
        second = sample(10, [10, -1], "messages")
        with self.assertRaisesRegex(ValueError, "backwards"):
            noise_envelope(first, second, "messages")
        second = sample(10, [10, 10], "messages")
        second["samples"][1]["elapsed"] = 0
        with self.assertRaisesRegex(ValueError, "monotonic"):
            noise_envelope(first, second, "messages")


if __name__ == "__main__":
    unittest.main()
