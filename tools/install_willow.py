#!/usr/bin/env python3
"""Build and upgrade existing Willow user services from an explicit private plan."""
import argparse
import contextlib
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import resource
import stat
import shutil
import subprocess
import sys
import tarfile
import time
import urllib.request

BINARIES = {"host": "hew-host-release", "base": "hew-base-release",
            "dashboard": "hew-dashboard-release", "broker": "hew-broker-release"}
OVERRIDE = "70-willow-current.conf"


def absolute(value):
    path = Path(value)
    if not path.is_absolute() or ".." in path.parts or any(c in str(path) for c in '\n\r%$"\\'):
        raise ValueError("Use absolute paths without systemd escapes or specifiers")
    return path


def load(path):
    info = path.lstat()
    if not stat.S_ISREG(info.st_mode) or info.st_uid != os.geteuid() or info.st_mode & 0o077:
        raise ValueError("Plan and receipt files must be private regular files (mode 0600)")
    return json.loads(path.read_text())


def validate(plan):
    for key in ("source", "install", "workspace"):
        absolute(plan[key])
    if not re.fullmatch(r"[0-9a-f]{40}", plan["revision"]):
        raise ValueError("revision must be a full source commit")
    seen = set()
    for item in plan["services"]:
        unit = item["unit"]
        if not re.fullmatch(r"[A-Za-z0-9_-]+\.service", unit) or unit in seen:
            raise ValueError("Each selected service needs a unique unit name")
        seen.add(unit)
        if type(item.get("selected", True)) is not bool:
            raise ValueError("selected must be a JSON boolean")
        if item["kind"] not in BINARIES:
            raise ValueError("Service kind must be host, base, dashboard or broker")
        absolute(item["current_install"])
        if item.get("selected", True) and item["kind"] in ("host", "base"):
            for key in ("state", "next_state"):
                absolute(item[key])
        if item["kind"] != "host":
            absolute(item["config"])
        if item.get("selected", True) and item["kind"] != "host":
            for key in ("config", "next_config"):
                absolute(item[key])
    if not any(s.get("selected", True) for s in plan["services"]):
        raise ValueError("Select at least one existing service")
    destinations = [Path(plan["install"]), Path(plan["workspace"])]
    for item in plan["services"]:
        destinations += [Path(item[k]) for k in ("next_state", "next_config") if k in item]
    if len(set(destinations)) != len(destinations):
        raise ValueError("Use distinct fresh destination paths")
    for item in plan["services"]:
        for old in (Path(item[k]) for k in ("state", "config", "current_install") if k in item):
            for new in destinations:
                if new == old or new.is_relative_to(old) or old.is_relative_to(new):
                    raise ValueError("New paths must be disjoint from existing installs, state and configuration")
    changed = {s["state"] for s in plan["services"] if s.get("selected", True) and "state" in s}
    for item in plan["services"]:
        if not item.get("selected", True) and "config" in item:
            raw = Path(item["config"]).read_bytes()
            if any(state.encode() in raw for state in changed):
                raise ValueError(item["unit"] + ": configuration references moving state; select this service too")
    return plan


def atomic(path, data):
    temporary = path.with_name(path.name + ".new")
    created = False
    try:
        with temporary.open("xb") as stream:
            created = True
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        fd = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)
    except BaseException:
        if created:
            temporary.unlink(missing_ok=True)
        raise



def run(arguments, **kwargs):
    return subprocess.run(arguments, check=True, timeout=90, **kwargs)


def prop(unit, name):
    return subprocess.check_output(["systemctl", "--user", "show", unit,
                                    "--property=" + name, "--value"], text=True).strip()


def roles(install):
    return Path(install) / "experiments/hew-roles"


def executable(item, install):
    return roles(install) / "build" / BINARIES[item["kind"]]


def owners(items, candidate=False):
    result = {}
    for item in items:
        pid = int(prop(item["unit"], "MainPID"))
        expected = executable(item, item["target_install"] if candidate else item["current_install"])
        if not pid or Path(os.readlink(f"/proc/{pid}/exe")) != expected:
            raise ValueError(item["unit"] + ": running executable differs from the plan")
        argument = item.get("state") if item["kind"] == "host" else item.get("config")
        if argument:
            arguments = Path(f"/proc/{pid}/cmdline").read_bytes().split(b"\0")
            expected_argument = item.get("next_state") if candidate and item["kind"] == "host" else item.get("next_config") if candidate else argument
            if len(arguments) < 2 or os.fsdecode(arguments[1]) != expected_argument:
                raise ValueError(item["unit"] + ": running state/configuration differs from the plan")
        result[item["unit"]] = pid
    return result


def health(url):
    with urllib.request.urlopen(url, timeout=8) as response:
        return json.load(response)


def snapshot(plan):
    if not health(plan["ready_url"]).get("ready"):
        raise ValueError("Willow readiness check failed")
    return {name: value["public_key"] for name, value in health(plan["status_url"]).items()}


def observer_retention(status):
    stats = status.get("observer", {}).get("observer_stats")
    if stats is None:
        return False, 0
    fields = {"pending": bool, "dirty": bool, "saving": bool, "save_error": int, "restored": int}
    durable = all(type(stats.get(key)) is kind for key, kind in fields.items())
    outstanding = max(stats["observed"] - stats["published"] - stats["dropped"],
                      stats.get("queued", 0), int(stats.get("pending", False)))
    if durable and stats["save_error"] != 0:
        raise ValueError("Observer checkpoint is unavailable; repair its save/restore error before stopping")
    if not durable and outstanding > 0:
        raise ValueError("Observer backlog is not persisted by this host; wait for the broker to drain it before upgrading")
    return durable, outstanding


def limited():
    os.sched_setaffinity(0, sorted(os.sched_getaffinity(0))[:2])
    resource.setrlimit(resource.RLIMIT_AS, (4 * 1024**3,) * 2)
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def build(plan, workspace):
    install = Path(plan["install"])
    if install.exists():
        raise ValueError("Installation path already exists; use a new final path")
    compiler = shutil.which("hew")
    if not compiler:
        raise ValueError("Put the current Hew build/bin launcher and matching libraries on PATH")
    archive = workspace / "source.tar"
    with archive.open("xb") as stream:
        run(["git", "-C", plan["source"], "archive", plan["revision"]], stdout=stream)
    install.mkdir(mode=0o700, parents=True)
    with tarfile.open(archive) as source:
        source.extractall(install, filter="data")
    kinds = {s["kind"] for s in plan["services"] if s.get("selected", True)}
    targets = sorted({"build/" + BINARIES[k] for k in kinds})
    with (workspace / "build.log").open("xb") as stream:
        subprocess.run(["make", "-j2", *targets], cwd=roles(install),
                       stdout=stream, stderr=subprocess.STDOUT, check=True,
                       timeout=900, preexec_fn=limited)
    if "host" in kinds:
        run([sys.executable, "-B", "build_worker.py", "--verify"], cwd=roles(install),
            stdout=subprocess.DEVNULL)
    return {"compiler": str(Path(compiler).resolve()),
            "compiler_version": subprocess.check_output([compiler, "--version"], text=True).strip(),
            "source_archive_sha256": hashlib.sha256(archive.read_bytes()).hexdigest(),
            "binaries": {k: hashlib.sha256((roles(install) / "build" / BINARIES[k]).read_bytes()).hexdigest()
                         for k in kinds}}


def resume_build(plan, workspace):
    if (workspace / "receipt.json").exists():
        raise ValueError("Switch receipt exists; inspect current owners, never replay a stop or switch")
    stage = load(workspace / "build.json")
    if stage["plan"] != plan:
        raise ValueError("Build stage belongs to a different plan")
    result = stage["build"]
    kinds = {s["kind"] for s in plan["services"] if s.get("selected", True)}
    if set(result["binaries"]) != kinds:
        raise ValueError("Build stage does not contain the selected production binaries")
    if hashlib.sha256((workspace / "source.tar").read_bytes()).hexdigest() != result["source_archive_sha256"]:
        raise ValueError("Saved source archive changed since the build")
    for kind, expected in result["binaries"].items():
        path = roles(plan["install"]) / "build" / BINARIES[kind]
        if hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            raise ValueError(kind + ": production binary changed since the build")
    if "host" in result["binaries"]:
        run([sys.executable, "-B", "build_worker.py", "--verify"], cwd=roles(plan["install"]),
            stdout=subprocess.DEVNULL)
    return result


def files(root):
    result = {}
    for path in Path(root).rglob("*"):
        if path.is_symlink():
            raise ValueError("State contains a symbolic link")
        if path.is_file() and str(path.relative_to(root)) not in ("service.lock", "base-service.lock", ".lock"):
            result[str(path.relative_to(root))] = path.read_bytes()
    return result


def socket_entries(directory, names):
    return [name for name in names if stat.S_ISSOCK((Path(directory) / name).lstat().st_mode)]


def stage_state(item, source, destination, log):
    destination.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    install = item["target_install"]
    if item["kind"] == "host":
        run([sys.executable, "-B", str(roles(install) / "willow.py"), "rebind",
             "--source", str(source), "--state", str(destination)], stdout=log, stderr=log)
        ignored = {"config", "migration.json"}
    else:
        run([sys.executable, "-B", str(roles(install) / "migrate_base.py"), "stage",
             "--source", str(source), "--destination", str(destination)], stdout=log, stderr=log)
        ignored = {"migration-base.json"}
    before, after = files(source), files(destination)
    if {k: v for k, v in before.items() if k not in ignored} != {k: v for k, v in after.items() if k not in ignored}:
        raise ValueError(item["unit"] + ": stopped state was not retained exactly")


def command(item):
    binary = executable(item, item["target_install"])
    if item["kind"] == "host":
        return [sys.executable, "-B", str(roles(item["target_install"]) / "willow.py"),
                "run", "--release", "--state", item["next_state"]]
    return [str(binary), item["next_config"]]


def override_bytes(item, previous):
    # Keep the selected override's environment and service options; change only launch paths.
    text = previous.decode()
    lines = text.splitlines()
    if "[Service]" not in lines:
        raise ValueError("Stable override needs a [Service] section")
    if sum(line.startswith("ExecStart=") and line != "ExecStart=" for line in lines) != 1:
        raise ValueError("Stable override must contain one ExecStart command and its reset")
    if "ExecStart=" not in lines:
        raise ValueError("Stable override needs an empty ExecStart reset")
    result = []
    for line in lines:
        if line.startswith("ExecStart=") and line != "ExecStart=":
            # systemd accepts double-quoted arguments; '%' is refused in plan paths.
            line = "ExecStart=" + " ".join('"' + arg + '"' for arg in command(item))
        elif line.startswith("WorkingDirectory="):
            line = "WorkingDirectory=" + str(roles(item["target_install"]))
        result.append(line)
    if not any(line.startswith("WorkingDirectory=") for line in result):
        result += ["[Service]", "WorkingDirectory=" + str(roles(item["target_install"]))]
    return ("\n".join(result) + "\n").encode()


def stop(items, receipt, save):
    for item in items:
        receipt["stop_intent"] = item["unit"]
        save()
        run(["systemctl", "--user", "stop", item["unit"]])
        if prop(item["unit"], "MainPID") != "0" or prop(item["unit"], "ActiveState") != "inactive":
            raise ValueError(item["unit"] + ": did not stop")
        receipt["stopped"].append(item["unit"])
        save()
        # A killed parent must not leave its native worker writing the same state.
        for pid in receipt["children"].get(item["unit"], []):
            deadline = time.monotonic() + 10
            while Path(f"/proc/{pid}").exists() and time.monotonic() < deadline:
                time.sleep(.1)
            if Path(f"/proc/{pid}").exists():
                raise ValueError(item["unit"] + ": child process remains; inspect before continuing")


def switch(plan, workspace, items, build_result=None):
    receipt_path = workspace / "receipt.json"
    if receipt_path.exists():
        raise ValueError("Receipt exists; inspect it, never replay an uncertain switch")
    for item in items:
        for key in ("next_state", "next_config"):
            if key in item and Path(item[key]).exists():
                raise ValueError("Destination already exists: " + item[key])
    guards = [s for s in plan["services"] if not s.get("selected", True)]
    untouched = owners(guards)
    pids = owners(items)
    for item in items:
        if "config" in item:
            path = Path(item["config"])
            info = path.lstat()
            if not stat.S_ISREG(info.st_mode) or info.st_uid != os.geteuid() or info.st_mode & 0o077:
                raise ValueError(item["unit"] + ": configuration must be an owned private regular file")
            raw = path.read_bytes()
            if item["kind"] == "base" and json.loads(raw).get("state_dir") != item["state"]:
                raise ValueError(item["unit"] + ": state differs from configuration state_dir")
    keys = snapshot(plan)
    children = {}
    for unit, pid in pids.items():
        children[unit] = sorted({int(p) for t in Path(f"/proc/{pid}/task").iterdir()
                                 for p in (t / "children").read_text().split()})
    unitroot = Path.home() / ".config/systemd/user"
    backups = workspace / "overrides"
    backups.mkdir(mode=0o700, exist_ok=True)
    old_overrides = {}
    for item in items:
        path = unitroot / (item["unit"] + ".d") / OVERRIDE
        old_overrides[item["unit"]] = path.read_bytes()
        (backups / item["unit"]).write_bytes(path.read_bytes())
        override_bytes(item, path.read_bytes())
    receipt = {"plan": plan, "items": items, "build": build_result, "public_keys": keys,
               "before_pids": pids, "children": children, "stopped": [], "stage": "stopping"}
    def save():
        atomic(receipt_path, (json.dumps(receipt, indent=2) + "\n").encode())
    save()
    replaced = []
    try:
        durable, outstanding = observer_retention(health(plan["status_url"])) if any(i["kind"] == "host" for i in items) else (False, 0)
        stop(items, receipt, save)
        if durable and outstanding:
            host = next(i for i in items if i["kind"] == "host")
            checkpoint = Path(host["state"]) / "observer.outbox"
            info = checkpoint.lstat()
            if not stat.S_ISREG(info.st_mode) or info.st_uid != os.geteuid() or info.st_mode & 0o077:
                raise ValueError("Stopped observer checkpoint is missing or not an owned private file; inspect before recovery")
        receipt["stage"] = "staging"
        save()
        frozen = workspace / "frozen"
        frozen.mkdir(mode=0o700)
        replacements = {}
        for item in items:
            if "state" in item:
                replacements[item["state"]] = item["next_state"]
        with contextlib.ExitStack() as locks, (workspace / "stage.log").open("xb") as log:
            for item in items:
                if "state" in item:
                    lock_path = Path(item["state"]) / ("service.lock" if item["kind"] == "host" else "base-service.lock")
                    if lock_path.is_symlink():
                        raise ValueError("State lock is a symbolic link")
                    held = locks.enter_context(lock_path.open("a+b"))
                    fcntl.flock(held, fcntl.LOCK_EX | fcntl.LOCK_NB)
            for item in items:
                if "state" in item:
                    source = frozen / item["unit"]
                    files(item["state"])  # Refuse links before copytree could follow them.
                    shutil.copytree(item["state"], source, ignore=socket_entries)
                    stage_state(item, source, Path(item["next_state"]), log)
                if "config" in item:
                    raw = Path(item["config"]).read_bytes()
                    (workspace / (item["unit"] + ".config.before")).write_bytes(raw)
                    substitutions = {**replacements, item["current_install"]: item["target_install"]}
                    for old, new in sorted(substitutions.items(), key=lambda x: -len(x[0])):
                        raw = raw.replace(old.encode(), new.encode())
                    destination = Path(item["next_config"])
                    destination.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
                    destination.write_bytes(raw)
        receipt["stage"] = "replacing-overrides"
        save()
        for item in items:
            path = unitroot / (item["unit"] + ".d") / OVERRIDE
            if path.read_bytes() != old_overrides[item["unit"]]:
                raise ValueError(item["unit"] + ": override changed during staging")
            replaced.append(item["unit"])
            atomic(path, override_bytes(item, old_overrides[item["unit"]]))
        run(["systemctl", "--user", "daemon-reload"])
    except BaseException:
        if receipt["stage"] == "stopping":
            receipt["stage"] = "stop-failed-inspect-current-owners"
            save()
            raise
        for unit in replaced:
            atomic(unitroot / (unit + ".d") / OVERRIDE, old_overrides[unit])
        if replaced:
            run(["systemctl", "--user", "daemon-reload"])
        receipt["stage"] = "staging-failed-original-owners-restoring"
        save()
        if any(Path(f"/proc/{pid}").exists() for unit in receipt["stopped"] for pid in children[unit]):
            receipt["stage"] = "staging-failed-inspect-remaining-child"
            save()
            raise
        for unit in reversed(receipt["stopped"]):
            run(["systemctl", "--user", "start", unit])
        receipt["stage"] = "staging-failed-original-owners-started"
        save()
        raise
    receipt["stage"] = "starting-candidate"
    save()
    for item in reversed(items):
        run(["systemctl", "--user", "start", item["unit"]])
    owners(items, candidate=True)
    deadline = time.monotonic() + 45
    while True:
        try:
            if snapshot(plan) == keys:
                break
        except (OSError, ValueError):
            pass
        if time.monotonic() >= deadline:
            raise ValueError("Candidate readiness/identity check failed; inspect receipt and current owners before recovery")
        time.sleep(.5)
    if owners(guards) != untouched:
        raise ValueError("An unselected service changed; inspect current owners")
    receipt["untouched_pids"] = untouched
    receipt["stage"] = "live"
    save()
    return receipt


def rollback_plan(receipt, workspace):
    items = []
    for item in receipt["items"]:
        new = dict(item, current_install=item["target_install"], target_install=item["current_install"])
        for key in ("state", "config"):
            if key in item:
                new[key] = item["next_" + key]
                new["next_" + key] = str(workspace / ("states" if key == "state" else "configs") / item["unit"])
        items.append(new)
    return items


def main():
    os.umask(0o077)
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="operation", required=True)
    for verb in ("plan", "apply"):
        command = sub.add_parser(verb)
        command.add_argument("plan", type=Path)
        if verb == "apply":
            command.add_argument("--resume", action="store_true", help="reuse a verified completed build only when no switch was attempted")
    rollback = sub.add_parser("rollback", help="restore old installs using latest stopped candidate state")
    rollback.add_argument("receipt", type=Path)
    rollback.add_argument("--workspace", required=True, type=Path)
    args = parser.parse_args()
    if args.operation == "rollback":
        previous = load(args.receipt)
        if previous["stage"] != "live":
            raise ValueError("Inspect an incomplete switch before selecting recovery installs/state")
        plan = previous["plan"]
        workspace = absolute(str(args.workspace))
        items = rollback_plan(previous, workspace)
    else:
        plan = validate(load(args.plan))
        workspace = Path(plan["workspace"])
        items = [dict(item, target_install=plan["install"]) for item in plan["services"] if item.get("selected", True)]
        if args.operation == "plan":
            print(json.dumps({"revision": plan["revision"], "install": plan["install"],
                              "selected_services": [{k: v for k, v in s.items() if k != "config"} for s in items]}, indent=2))
            return
    lock_path = Path.home() / ".local/state/willow-installer.lock"
    lock_path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    with lock_path.open("a+b") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        if args.operation == "apply" and args.resume:
            result = resume_build(plan, workspace)
        else:
            workspace.mkdir(mode=0o700, parents=True)
            result = None if args.operation == "rollback" else build(plan, workspace)
            if result is not None:
                atomic(workspace / "build.json", (json.dumps({"plan": plan, "build": result}, indent=2) + "\n").encode())
        try:
            switch(plan, workspace, items, result)
        except (OSError, ValueError, subprocess.SubprocessError) as error:
            if args.operation == "apply" and not (workspace / "receipt.json").exists():
                raise ValueError(str(error) + "; build retained. Once preflight is ready, use apply --resume with the same plan") from error
            raise
    print("Willow services are ready; private receipt: " + str(workspace / "receipt.json"))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        raise SystemExit(str(error))
