"""Prepare a session-only extension; activation remains an explicit CLI reload."""

import argparse
import json
import os
from pathlib import Path
import stat
import sys

from .link import load_config


def install(session, config, state, python):
    root = Path(__file__).resolve().parents[2]
    config = Path(config).resolve()
    load_config(config)
    state = Path(state).absolute()
    state.mkdir(mode=0o700, parents=True, exist_ok=True)
    info = state.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_mode & 0o077 or info.st_uid != os.getuid():
        raise ValueError("State directory must be owner-only (0700)")
    session = Path(session).resolve(strict=True)
    if not (session / "checkpoints").is_dir():
        raise ValueError("Session directory must be a Copilot session workspace")
    launcher = state / "launcher.json"
    document = {"root": str(root), "python": str(Path(python).absolute()),
                "config": str(config), "state": str(state)}
    fd = os.open(launcher, os.O_CREAT | os.O_TRUNC | os.O_WRONLY | os.O_NOFOLLOW, 0o600)
    os.fchmod(fd, 0o600)
    with os.fdopen(fd, "w") as stream:
        json.dump(document, stream)
    extension = session / "extensions" / "meshcore-directed-link"
    extension.mkdir(parents=True, mode=0o700, exist_ok=True)
    source = root / "tools" / "meshcore_link" / "extension.mjs"
    loader = (f"process.env.MESHCORE_LINK_LAUNCHER = {json.dumps(str(launcher))};\n"
              f"await import({json.dumps(source.as_uri())});\n")
    (extension / "extension.mjs").write_text(loader)
    return extension / "extension.mjs"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--session", required=True)
    parser.add_argument("--config", required=True)
    parser.add_argument("--state", required=True)
    parser.add_argument("--python", default=sys.executable)
    args = parser.parse_args()
    os.umask(0o077)
    print(install(args.session, args.config, args.state, args.python))


if __name__ == "__main__":
    main()
