"""Check the real serializer's output and the offline frontend's JavaScript."""
import json
import pathlib
import subprocess
import sys

snapshot = json.loads(pathlib.Path(sys.argv[1]).read_text())
assert snapshot["api_version"] == 1
assert snapshot["firmware_version"] == "1.17.1-slp-birch"
assert snapshot["memory"] == {
    "free_bytes": 0,
    "minimum_bytes": 0,
    "dma_free_bytes": 4294967295,
    "dma_largest_bytes": 123456,
    "dma_minimum_bytes": 0,
}
assert "stream" not in snapshot
assert snapshot["device_name"] == 'Radio "A"\n<script>'
assert snapshot["profile"]["frequency_hz"] == 912525000
assert snapshot["scheduler"]["maximum_credit_ms"] == 36000
assert len(snapshot["history"]["events"]) == 32
assert snapshot["history"]["overwritten"] == 10
assert snapshot["history"]["events"][0]["sequence"] == 42
assert snapshot["history"]["events"][0]["state"] == 4
assert snapshot["history"]["events"][0]["rf_ms"] == 150
assert snapshot["history"]["events"][1]["state"] == 2
assert snapshot["totals"]["tx_rf_ms"] == 225
assert len(snapshot["traffic"]) == 60
for event in snapshot["history"]["events"]:
    if event["direction"] == "rx":
        assert event["rf_ms"] is None
        assert event["estimated_ms"] == 40
        assert event["rssi_dbm"] == -70 and event["snr_db"] == 4.25
    else:
        assert event["rssi_dbm"] is None and event["snr_db"] is None
    assert len(bytes.fromhex(event["preview_hex"])) == 16
    assert event["preview_truncated"] is True
page = pathlib.Path(sys.argv[2]).read_text()
script = page.split("<script>", 1)[1].split("</script>", 1)[0]
subprocess.run(["node", "--check"], input=script, text=True, check=True)
print("Dashboard JSON contract and offline JavaScript syntax passed")
