import json
import pathlib
import sys

source, output = map(pathlib.Path, sys.argv[1:])
rows = [
    row
    for line in source.read_text().splitlines()
    if (row := json.loads(line)).get("scenario") == "snr-score-kernel"
    and row.get("event") == "score"
]
if not rows:
    raise SystemExit("native score corpus contains no score events")
lines = [
    "struct NativeScore { float snr; uint8_t sf; int length; float score; };",
    "static const NativeScore native_scores[] = {",
]
for row in rows:
    lines.append(
        "  {%sf, %d, %d, %sf},"
        % (repr(float(row["snr"])), row["sf"], row["length"], repr(float(row["score"])))
    )
lines.append("};")
output.write_text("\n".join(lines) + "\n")
