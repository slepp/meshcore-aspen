#!/usr/bin/env python3
"""Back up or restore the standard per-user installation during maintenance."""

import argparse
from contextlib import contextmanager
import fcntl
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


SERVICE = "meshcore-host.service"
TARGETS = {
    "config": ".config/meshcore-host",
    "state": ".local/state/meshcore-host",
    "host": ".local/bin/meshcore-host",
    "check": ".local/bin/meshcore-check",
    "rf-check": ".local/bin/meshcore-rf-check",
    "unit": ".config/systemd/user/meshcore-host.service",
}

def targets(home, config=None):
    result = {name: home / relative for name, relative in TARGETS.items()}
    path = home / TARGETS["config"] / "config.json"
    settings = json.loads((config or path).read_text())
    state = Path(settings["state_dir"])
    result["state"] = state if state.is_absolute() else path.parent / state
    if settings.get("bot_runtime") == "native_lua":
        worker = Path(settings["bot_native_worker"])
        if not worker.is_absolute():
            raise RuntimeError("Configured native worker must have an absolute path.")
        result["worker"] = worker
    for name, destination in result.items():
        destination = destination.resolve()
        if home not in destination.parents:
            raise RuntimeError(f"Configured {name} is outside this per-user installation.")
        result[name] = destination
    for name, destination in result.items():
        for other, candidate in result.items():
            if name == "config" and other == "state" and destination in candidate.parents:
                continue
            if name != other and (destination == candidate or destination in candidate.parents):
                raise RuntimeError(f"Configured {name} overlaps {other}; use a separate installation path.")
    return result


def check_directory(directory, installed):
    for target in installed.values():
        if directory == target or directory in target.parents or target in directory.parents:
            raise RuntimeError("Backup directory must not overlap the installed files or state.")


@contextmanager
def state_lock(state):
    with (state / ".lock").open("a+b") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise RuntimeError(f"Another host still owns {state}; stop it before maintenance.") from error
        yield


def service(*arguments):
    subprocess.run(["systemctl", "--user", *arguments, SERVICE], check=True)


def copy(source, destination):
    if source.is_dir():
        shutil.copytree(source, destination)
    else:
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)


def backup(directory, home):
    installed = targets(home)
    check_directory(directory, installed)
    for name in ("config", "state", "host", "unit"):
        if not installed[name].exists():
            raise RuntimeError(f"Existing {name} installation not found; nothing was stopped.")
    if "worker" in installed and not installed["worker"].is_file():
        raise RuntimeError("Configured native worker not found; nothing was stopped.")
    directory.mkdir(mode=0o700, parents=True, exist_ok=False)
    service("stop")
    saved = []
    with state_lock(installed["state"]):
        for name, source in installed.items():
            if source.exists():
                copy(source, directory / name)
                saved.append(name)
        (directory / "manifest.json").write_text(json.dumps({"version": 2, "saved": saved}) + "\n")
    print("Host backup complete. Service is stopped for maintenance.")


def restore(directory, home):
    manifest = json.loads((directory / "manifest.json").read_text())
    installed = (targets(home, directory / "config/config.json") if manifest.get("version") == 2
                 else {name: home / relative for name, relative in TARGETS.items()})
    check_directory(directory, installed)
    saved = manifest.get("saved")
    if (
        manifest.get("version") not in (1, 2)
        or not isinstance(saved, list)
        or any(not isinstance(name, str) or name not in installed for name in saved)
        or len(set(saved)) != len(saved)
        or not {"config", "state", "host", "unit"}.issubset(saved)
        or ("worker" in installed and "worker" not in saved)
        or any(not (directory / name).exists() for name in saved)
    ):
        raise RuntimeError("Invalid or incomplete host backup; nothing was stopped.")
    service("stop")
    previous = Path(tempfile.mkdtemp(prefix="replaced-", dir=directory))
    installed["state"].mkdir(mode=0o700, parents=True, exist_ok=True)
    with state_lock(installed["state"]):
        for name in sorted(saved, key=lambda name: name != "state"):
            destination = installed[name]
            if destination.exists() or destination.is_symlink():
                # Restore complete state, never overlay newer identity envelopes.
                shutil.move(str(destination), previous / name)
            copy(directory / name, destination)
    subprocess.run(["systemctl", "--user", "daemon-reload"], check=True)
    print("Host restored; replaced files retained privately beside the backup.")
    print("Restore matching modem firmware if necessary, then start meshcore-host.service.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("backup", "restore"))
    parser.add_argument("--directory", required=True)
    args = parser.parse_args()
    os.umask(0o077)
    home = Path.home().resolve()
    directory = Path(args.directory).resolve()
    try:
        if args.action == "backup":
            backup(directory, home)
        else:
            restore(directory, home)
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"meshcore-host {args.action}: {error}\nService may remain stopped; no automatic restart was attempted.\n")


if __name__ == "__main__":
    main()
