#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build downloadable command ZIPs from the repository's canonical Lua and Wasm."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]
CATALOG = Path(__file__).with_name("catalog.json")
LEGAL = ("LICENSE", "NOTICE", "LICENSES/MeshCore-MIT.txt")


def digest(content):
    return hashlib.sha256(content).hexdigest()


def json_bytes(value):
    return (json.dumps(value, indent=2, ensure_ascii=False) + "\n").encode()


def readme(entry, version, source):
    lines = [f"# {entry['name']}", "", entry["summary"], "",
             f"Command library {version}; source commit `{source['commit']}`.", "",
             "## Requirements", ""]
    lines += [f"- {name.title()}: {text}." for name, text in entry["compatibility"].items()]
    if entry.get("before_install"):
        lines += ["", "## Before installing", ""] + entry["before_install"]
    lines += ["", "## Use", ""] + [f"- {text}" for text in entry["usage"]]
    lines += ["", "## Install one named source", "",
              "Extract this ZIP and check its files with sha256sum --check SHA256SUMS",
              "from the extracted directory. Run installation commands from a MeshCore Aspen source checkout.",
              "Replace DEVICE, the private password path and the extracted file path.",
              "Read source list first. If updating an installed file with the same name,",
              "export it to a new private file with source export NAME FILE before replacing it.",
              "source install retains other named files and the bundled main commands;",
              "duplicate exports or a source set beyond the device's entry/byte limits reject activation.",
              "An existing custom main source is retained too; inspect its namespace before installing.", ""]
    lua = next(item["name"] for item in entry["files"] if item["runtime"] == "lua")
    connections = [("Aspen", "--web http://DEVICE --password-file /private/node-password")]
    if "willow" in entry["compatibility"]:
        connections.append(("Willow", "--willow-socket /path/to/admin.sock"))
    for host, connection in connections:
        admin = f"python3 tools/admin.py {connection}"
        lines += [f"### {host}", "", "```sh", f"{admin} source list",
                  f"{admin} source install {entry['source_name']} /path/to/extracted/{lua}",
                  f"{admin} source status", "```", ""]
        if "packet" in entry:
            packet = entry["packet"]
            slot = packet["slot"]
            wasm = next(item["name"] for item in entry["files"] if item["runtime"] == "packet-wasm")
            lines += ["Check both slots before replacing a packet program:", "", "```sh",
                      f"{admin} command 'packet api'", f"{admin} packet status 0",
                      f"{admin} packet status 1", f"{admin} packet budget {slot}", "```", "",
                      f"If slot {slot} is occupied, export it with the following command and",
                      "decide whether to replace it. Keep the previous budget/enable settings too.",
                      "If the same sampler is already running there, retain it and skip these writes.", "", "```sh",
                      f"{admin} packet export {slot} /private/packet-before.wasm", "```", "",
                      f"Only after choosing to replace slot {slot}:", "", "```sh",
                      f"{admin} packet install {slot} /path/to/extracted/{wasm}",
                      f"{admin} packet budget {slot} " + " ".join(map(str, packet["budget"])),
                      f"{admin} packet enable {slot} on", f"{admin} packet status {slot}", "```", ""]
    lines += ["## Update or remove", "",
              f"Use the same connection options with source export {entry['source_name']} /private/saved.lua before updating.",
              f"Update with source install {entry['source_name']} /path/to/extracted/{lua}.",
              f"Remove only this Lua file with source remove {entry['source_name']}.",
              "Removing code retains stored notes and native monitor configuration.", ""]
    if "packet" in entry:
        slot = entry["packet"]["slot"]
        lines += [f"Removing Lua alone leaves the sampler running. Use packet enable {slot} off to stop sampling;",
                  f"packet remove {slot} removes its Wasm source. Keep other packet slots unchanged.", ""]
    lines += ["Wait for the source selection to become active before trying its commands.",
              "An uncertain result calls for status readback before another installation.",
              "These operations do not flash firmware or replace role identities.",
              "Activation cancels pending jobs in that Lua runtime; stored notes remain.", "",
              "## Behaviour and settings", ""] + [f"- {text}" for text in entry["notes"]]
    if "guide" in entry:
        guide = f"{source['repository']}/blob/{source['commit']}/{entry['guide']}"
        lines += ["", f"Configuration guide: {guide}."]
    return ("\n".join(lines) + "\n").encode()


def build(output):
    catalog = json.loads(CATALOG.read_text())
    subprocess.run(["make", "-C", str(ROOT / "hosts/willow"), "build/repeater-snr.wasm"], check=True)
    source = {
        "repository": catalog["repository"],
        "commit": subprocess.check_output(["git", "-C", str(ROOT), "rev-parse", "HEAD"], text=True).strip(),
        "working_tree_dirty": bool(subprocess.check_output(
            ["git", "-C", str(ROOT), "status", "--porcelain"], text=True).strip()),
    }
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    downloads = []
    for entry in catalog["entries"]:
        name = f"meshcore-command-{entry['id']}-v{catalog['version']}"
        files = {path: (ROOT / path).read_bytes() for path in LEGAL}
        files["README.md"] = readme(entry, catalog["version"], source)
        for item in entry["files"]:
            path = (ROOT / item["source"]).resolve()
            path.relative_to(ROOT)
            files[item["name"]] = path.read_bytes()
        manifest = {"schema_version": 1, "library_version": catalog["version"], "source": source,
                    "entry": entry,
                    "files": [{"name": path, "bytes": len(content), "sha256": digest(content)}
                              for path, content in sorted(files.items())]}
        files["manifest.json"] = json_bytes(manifest)
        files["SHA256SUMS"] = "".join(f"{digest(content)}  {path}\n"
                                       for path, content in sorted(files.items())).encode()
        archive = output / (name + ".zip")
        with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED) as bundle:
            for path, content in sorted(files.items()):
                info = zipfile.ZipInfo(f"{name}/{path}", date_time=(1980, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = 0o100644 << 16
                bundle.writestr(info, content)
        downloads.append({**entry, "download": {"file": archive.name,
                          "bytes": archive.stat().st_size, "sha256": digest(archive.read_bytes())}})
        print(archive)
    published = {**catalog, "source": source, "entries": downloads}
    (output / "catalog.json").write_bytes(json_bytes(published))
    checksums = [(entry["download"]["file"], entry["download"]["sha256"]) for entry in downloads]
    checksums.append(("catalog.json", digest((output / "catalog.json").read_bytes())))
    (output / "SHA256SUMS").write_text("".join(f"{sha}  {name}\n" for name, sha in checksums))
    return published


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / ".tmp/command-library")
    args = parser.parse_args()
    build(args.output)


if __name__ == "__main__":
    main()
