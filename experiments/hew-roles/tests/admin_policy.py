"""Compare native Hew permissions with the repository's actual Go patterns."""
import json
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
GO_SOURCE = Path(os.environ.get("MESHCORE_GO_SOURCE", ROOT.parents[1]))
patterns = {}
for file in ("admin_http.go", "admin_roles.go"):
    source = (GO_SOURCE/"internal/app"/file).read_text()
    for name, pattern in re.findall(r"var (host(?:Admin|Role)(?:Read|Write)) = regexp.MustCompile\(`([^`]+)`\)", source):
        patterns[name] = pattern
assert len(patterns) == 4
commands = []
bot = [
    "help", "help bot", "status", "ver", "board", "job", "stats", "stats sensors",
    "bot status", "bot name", "bot policy", "bot forward from", "bot destination 4",
    "role config bot", "role name bot", "source api modules", "source read 123",
    "source begin 0123456789abcdef 1024 "+"a"*64,
    "source chunk 0123456789abcdef 0 "+"ab"*48,
    "source commit 0123456789abcdef", "source help fixture", "data clear",
    "data export kv caller "+"b"*64, "data restore 0123456789abcdef no-rearm",
    "bot name "+"a"*31, "bot shared on", "bot cancel",
    "role config room", "role name companion", "key export", "bot password secret",
    "bot name "+"a"*32, "source chunk 0123456789abcdef 0 "+"ab"*49,
    "bot destination 5", "source read -1", "source begin 0123456789abcdef 10000 "+"a"*64,
]
role = [
    "help", "help region", "get name", "get owner.info", "get radio", "stats radio",
    "get path.hash.mode", "get flood.advert.interval", "set path.hash.mode 2",
    "set repeat off", "set name "+"a"*31, "set flood.advert.interval 999",
    "region", "region home #home", "region default <null>", "region list denied",
    "region put #north #home", "region get #north", "region def #north,#home|* -",
    "region remove #north", "region save", "clock", "set tx 2", "set radio 912.525",
    "set name "+"a"*32, "set flood.advert.interval 1000", "set path.hash.mode 3",
    "password secret", "reboot",
]
for kind, cases in (("b:", bot), ("r:", role)):
    for command in cases:
        commands.append(kind+command)
        for suffix in ("\n", "\r", "\t", "\x01", ";reboot", " ", "é"):
            commands.append(kind+command+suffix)
oracle = subprocess.run(["go", "run", "./tests/admin_policy_oracle"], cwd=ROOT,
    input=json.dumps({"Patterns": patterns, "Commands": commands}), text=True,
    capture_output=True, check=True, timeout=60, env=os.environ|{"TMPDIR": str(ROOT/"build")})
expected = json.loads(oracle.stdout)
for binary in ("admin-checks", "admin-checks-release"):
    actual = subprocess.run([str(ROOT/"build"/binary), *commands],
        text=True, capture_output=True, check=True, timeout=30).stdout.splitlines()
    assert len(actual) == len(expected), binary
    for command, go, hew in zip(commands, expected, actual):
        assert (hew == "1") == go, (binary, command, go, hew)
print(f"ADMIN_POLICY_DIFFERENTIAL_OK commands={len(commands)} modes=debug,release")
