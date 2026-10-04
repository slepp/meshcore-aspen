#!/usr/bin/env python3
"""Record behavioural results and source closure; never infer Go parity."""
import argparse
import hashlib
import json
import pathlib
import re
import subprocess
import os

REFERENCE = "d92964352441e53b93e8667b802e04f6e072b39e"
ROOT = pathlib.Path(__file__).resolve().parents[2]
DATA = ROOT / "testdata/parity"


def portable_text(text, roots=()):
    """Keep repository paths relative and name explicitly configured external roots."""
    replacements = [(ROOT, "")]
    replacements.extend((base, label) for label, base in roots
                        if not base.is_relative_to(ROOT))
    for base, label in sorted(replacements, key=lambda item: len(str(item[0])), reverse=True):
        text = text.replace(str(base) + "/", label + "/" if label else "")
        text = re.sub(re.escape(str(base)) + r"""(?=$|[\s"'])""",
                      lambda _: label or ".", text)
    return text


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def save(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")


def source_closure(upstream, crypto):
    result = {}
    for prefix, root in (("meshcore", upstream), ("crypto", crypto),
                         ("harness", ROOT / "test_support/parity")):
        for path in sorted(root.rglob("*")):
            if not path.is_file() or path.suffix not in (".c", ".cpp", ".h", ".py", ".json") and path.name != "Makefile":
                continue
            if any(part in (".git", ".pio", "__pycache__") for part in path.relative_to(root).parts):
                continue
            result[prefix + "/" + str(path.relative_to(root))] = digest(path)
    return result


def row(identifier, category, path, symbol, line, profile, scenarios=()):
    return {"id": identifier, "category": category, "profile": profile,
            "native": {"commit": REFERENCE, "file": path, "symbol": symbol, "line": line},
            "applicability": "applicable-or-profile-dependent",
            "implementation_status": "awaiting-owner-evidence",
            "evidence_status": "awaiting-native-application-or-stock-evidence",
            "scenario_ids": list(scenarios), "oracle_type": None,
            "source_closure": "native-reference.json", "artifacts": [],
            "deviations": [], "blockers": ["No host/native differential result attached."]}


def inventory(upstream):
    rows = []
    companion = "examples/companion_radio/MyMesh.cpp"
    text = (upstream / companion).read_text()
    lines = text.splitlines()
    defines = re.compile(r"#define\s+((?:CMD_|PUSH_CODE_|RESP_CODE_|RESP_ALLOWED_)\w+)\s+(0x[0-9A-Fa-f]+|\d+)")
    for n, line in enumerate(lines, 1):
        match = defines.match(line)
        if not match:
            continue
        name, number = match.groups()
        category = "command" if name.startswith("CMD_") else "push" if name.startswith("PUSH_") else "response"
        item = row("companion." + name.lower(), category, companion, name, n, "companion")
        item["wire_id"] = int(number, 0)
        item["native_forms"] = []
        # Retain every dispatch branch and its length/subtype/version conditions.
        starts = [i for i, source in enumerate(lines) if not source.startswith("#define")
                  and re.search(r"\b" + re.escape(name) + r"\b", source)]
        for start in starts:
            end = start + 1
            if category == "command" and "cmd_frame[0]" in lines[start]:
                while end < len(lines) and not re.search(r"cmd_frame\[0\].*CMD_", lines[end]):
                    end += 1
                conditions = [{"line": i+1, "condition": s.strip()} for i, s in enumerate(lines[start:end], start)
                              if re.search(r"\bif\s*\(|#if|#else|#endif|app_target_ver", s)]
            else:
                conditions = []
            item["native_forms"].append({"line": start+1, "source": lines[start].strip(),
                                         "conditions": conditions})
        rows.append(item)
    # Versioned receive serializers are outside the command dispatcher.
    for n, line in enumerate(lines, 1):
        if "if (app_target_ver" in line:
            for version in ("condition-true", "condition-false"):
                item = row(f"companion.receive-version.{n}.{version}", "versioned-receive",
                           companion, line.strip(), n, "companion")
                item["variant"] = version
                rows.append(item)
    for profile, path in (
        ("repeater,room", "src/helpers/CommonCLI.cpp"),
        ("repeater", "examples/simple_repeater/MyMesh.cpp"),
        ("room", "examples/simple_room_server/MyMesh.cpp"),
        ("companion", "examples/companion_radio/MyMesh.cpp")):
        lines = (upstream / path).read_text().splitlines()
        for n, line in enumerate(lines, 1):
            matches = list(re.finditer(r'(?:memcmp|strcmp|strncmp)\(\s*(command|config|parts\[\d+\])(?:\s*\+\s*\d+)?\s*,\s*"([^"]*)"', line))
            for match in matches:
                subject, literal = match.groups()
                item = row(f"{profile}.cli.{n}.{literal.strip().replace(' ', '-')}", "role-cli",
                           path, line.strip(), n, profile)
                item["match_subject"] = subject
                item["literal"] = literal
                item["required_cases"] = ["authorized-valid", "invalid-boundary", "denied", "commit-failure-if-stateful"]
                rows.append(item)
    for profile, path in (("repeater,room", "src/helpers/CommonCLI.h"),
                          ("companion", "examples/companion_radio/NodePrefs.h")):
        for n, line in enumerate((upstream / path).read_text().splitlines(), 1):
            match = re.search(r'\bdef\("([^"]+)"', line)
            if match:
                rows.append(row(f"{profile}.preference.{n}.{match[1]}", "persisted-preference",
                                path, line.strip(), n, profile))
    catalog = json.loads((DATA / "behaviours.json").read_text())
    for entry in catalog["behaviours"]:
        item = row(entry["id"], "mesh-behaviour", entry["file"], entry["symbol"],
                   None, entry["profile"], entry.get("scenario_ids", []))
        item.update({k: v for k, v in entry.items() if k not in ("file", "symbol", "profile")})
        rows.append(item)
    return {"schema_version": 1, "reference": REFERENCE,
            "coverage_note": "Defined command/push/response IDs; all textual dispatch forms and persisted keys; behavioural catalog. Source forms are obligations, not evidence of execution.",
            "parked_or_undefined_command_ids": [44,45,46,47,48,49,53],
            "board_profile": "Software baseline; board capacities and feature macros must accompany target captures.",
            "rows": rows}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--upstream", type=pathlib.Path, required=True)
    parser.add_argument("--crypto", type=pathlib.Path, required=True)
    parser.add_argument("--lpp", type=pathlib.Path)
    parser.add_argument("--build", type=pathlib.Path, required=True)
    parser.add_argument("--snapshot", action="store_true",
                        help="Explicitly refresh checked-in reference inventory/vectors.")
    parser.add_argument("--snapshot-abi32", action="store_true")
    parser.add_argument("--abi-flags", default="")
    args = parser.parse_args()
    args.upstream, args.crypto, args.build = args.upstream.resolve(), args.crypto.resolve(), args.build.resolve()
    args.lpp = (args.lpp or args.crypto.parent / "CayenneLPP/src").resolve()
    roots = (("meshcore", args.upstream), ("crypto", args.crypto),
             ("cayennelpp", args.lpp), ("dependencies", args.crypto.parent),
             ("generated", args.build))
    revision = subprocess.check_output(["git", "-C", str(args.upstream), "rev-parse", "HEAD"], text=True).strip()
    if revision != REFERENCE or subprocess.check_output(
            ["git", "-C", str(args.upstream), "status", "--porcelain"], text=True):
        raise SystemExit("Reference is not clean and pinned")
    crypto_manifest = json.loads((args.crypto / "library.json").read_text())
    if crypto_manifest["name"] != "Crypto" or crypto_manifest["version"] != "0.4.0":
        raise SystemExit("Expected real rweather/Crypto 0.4.0")
    events = [json.loads(line) for line in (args.build / "events.jsonl").read_text().splitlines()]
    passed = sorted({event["scenario"] for event in events if event["event"] == "passed"})
    if "crypto-known-answer" not in passed:
        raise SystemExit("Missing independent crypto known-answer evidence")
    if "base-chat-ack-hashes" in passed:
        hashes = [event for event in events if event["event"] == "ack_hash"]
        if len(hashes) != 4:
            raise SystemExit("Missing plain/signed ACK attempt-tail vectors")
        for event in hashes:
            plain = bytes.fromhex(event["plaintext"])
            end = plain.index(0, 9 if event["signed"] else 5)
            key = bytes.fromhex(event["recipient_public_key"] if event["signed"] else event["sender_public_key"])
            if hashlib.sha256(plain[:end] + key).hexdigest()[:8] != event["ack_hash"]:
                raise SystemExit("Native ACK differs from independent hashlib SHA256 vector")
    closure = source_closure(args.upstream, args.crypto)
    closure_hash = hashlib.sha256(json.dumps(closure, sort_keys=True).encode()).hexdigest()
    manifest = {"schema_version": 1, "native_commit": REFERENCE, "native_patches": [],
                "crypto": crypto_manifest, "source_hashes": closure, "closure_sha256": closure_hash,
                "extracted_kernels": json.loads((args.build / "kernels.json").read_text()),
                "crypto_rng_note": "Mesh RNG supplied explicitly; unused Crypto global random-key entrypoints are linker-GC discarded.",
                "build_abi": next(e for e in events if e["event"] == "abi"),
                "source_closure_note": "All source/header inventory hashes; compiled dependency closure separately recorded from compiler depfiles."}
    manifest["compiler"] = subprocess.check_output(
        [os.environ.get("CXX", "c++"), "--version"], text=True).splitlines()[0]
    manifest["binary_sha256"] = digest(args.build / "oracle")
    manifest["build_configuration"] = portable_text((args.build / "build-config").read_text(), roots)
    manifest["profile"] = {"MAX_CONTACTS": 32, "MAX_CLIENTS": 20,
                           "hardware_crypto": False, "host_filesystem": "in-memory-fault-injectable",
                           "defines": "No MCU/ARDUINO macros; FILESYSTEM=MemoryFS", "stock_board": None}
    dependencies = {}
    for depfile in args.build.rglob("*.d"):
        deptext = depfile.read_text().replace("\\\n", " ")
        for token in deptext.split():
            path = pathlib.Path(token)
            if not path.is_absolute():
                path = ROOT / "test_support/parity" / path
            path = path.resolve()
            if path.is_file():
                label = portable_text(str(path), roots)
                for prefix, base in (("meshcore", args.upstream), ("crypto", args.crypto),
                                     ("harness", ROOT / "test_support/parity"), ("generated", args.build)):
                    if path.is_relative_to(base):
                        label = prefix + "/" + str(path.relative_to(base))
                        break
                if pathlib.Path(label).is_absolute():
                    raise SystemExit("Compiled dependency outside declared source roots; refusing absolute provenance path.")
                dependencies[label] = digest(path)
    manifest["compiled_dependencies"] = dependencies
    matrix = inventory(args.upstream)
    owner_reports = json.loads((DATA / "implementation-evidence.json").read_text())["reports"]
    differential = json.loads((DATA / "policy-differential.json").read_text())
    result_path = ROOT / differential["result_artifact"]
    differential_current = differential["native_events_sha256"] == digest(args.build / "events.jsonl")
    differential_current &= all((ROOT / path).is_file() and digest(ROOT / path) == sha
                                for path, sha in differential["source_sha256"].items())
    differential_current &= result_path.is_file() and digest(result_path) == differential["result_sha256"]
    if differential_current:
        records = [json.loads(line) for line in result_path.read_text().splitlines()]
        differential_current = any(e.get("Action") == "pass" and e.get("Test") == differential["test"]
                                   for e in records)
    for item in matrix["rows"]:
        item["native"]["source_sha256"] = digest(args.upstream / item["native"]["file"])
        item["dependency_closure_sha256"] = closure_hash
        scenarios = item["scenario_ids"]
        if scenarios and all(s in passed for s in scenarios):
            item["evidence_status"] = "native-reference-executed-only"
            item["oracle_type"] = "extracted-native-arithmetic" if any(
                s in ("native-delay-kernels", "snr-score-kernel", "repeater-loop-kernel", "native-runtime-airtime-factor")
                for s in scenarios) else "compiled-native"
            item["artifacts"] = [{"path": "native-events.jsonl", "sha256": digest(args.build / "events.jsonl"),
                                  "scenarios": scenarios}]
        if item["id"] == "time.millis-wrap" and "millis-wrap" not in passed:
            item["evidence_status"] = "blocked-abi"
            item["blockers"].append("64-bit Linux long is not the MCU timer ABI; make abi32 is required.")
            abi_record = DATA / "abi32-evidence.json"
            if abi_record.exists():
                abi = json.loads(abi_record.read_text())
                abi_events = DATA / "abi32-events.jsonl"
                abi_reference = DATA / "abi32-reference.json"
                if (abi_events.is_file() and abi_reference.is_file() and
                        digest(abi_events) == abi["events_sha256"] and
                        digest(abi_reference) == abi["reference_sha256"]):
                    reference = json.loads(abi_reference.read_text())
                    if reference["source_hashes"] == closure:
                        item["evidence_status"] = "native-32bit-reference-executed"
                        item["oracle_type"] = "compiled-native-elf32"
                        item["source_closure"] = "abi32-reference.json"
                        item["artifacts"] = [
                            {"path": name, "sha256": digest(DATA / name), "scenarios": ["millis-wrap"]}
                            for name in ("abi32-evidence.json", "abi32-reference.json", "abi32-events.jsonl")]
                        item["blockers"] = ["Host clock ABI demonstrated; consumer and physical target evidence remain separate."]
        for report in owner_reports:
            if item["id"] in report["row_ids"]:
                item["implementation_status"] = report["recorded_status"]
                item["implementation_evidence"] = report
                if "revision_artifact" in report:
                    revision = ROOT / report["revision_artifact"]
                    result = ROOT / report["result_artifact"]
                    current = (revision.is_file() and result.is_file() and
                               digest(revision) == report["revision_artifact_sha256"] and
                               digest(result) == report["result_sha256"])
                    if current:
                        for line in revision.read_text().splitlines():
                            sha, name = line.split(maxsplit=1)
                            source = ROOT / name
                            if args.snapshot and source == DATA / "native-events.jsonl":
                                source = args.build / "events.jsonl"
                            if not source.is_file() or digest(source) != sha:
                                current = False
                                break
                    item["evidence_status"] = report["evidence_status"] if current else "stale-software-evidence"
                    if not current:
                        item["implementation_status"] = "implementation-changed-since-software-evidence"
                    item["artifacts"] = [
                        {"path": path.name, "sha256": report[key], "scenarios": report["tests"]}
                        for path, key in ((revision, "revision_artifact_sha256"), (result, "result_sha256"))]
                    item["blockers"] = report["blockers"]
        if item["id"] in differential["row_ids"]:
            item["differential_evidence"] = "policy-differential.json"
            item["evidence_status"] = ("native-differential-covered-vectors" if differential_current
                                       else "stale-differential-evidence")
            if differential_current:
                item["implementation_status"] = "implemented-covered-vectors-verified"
                item["blockers"] = ["Only listed vectors compared; application/RF gates remain separate."]
    save(args.build / "provenance.json", manifest)
    save(args.build / "matrix.json", matrix)
    save(args.build / "results.json", {"passed": passed, "events_sha256": digest(args.build / "events.jsonl"),
         "source_closure_sha256": closure_hash, "stock_rf_evidence": False,
         "host_differential_evidence": differential_current,
         "host_differential_rows": differential["row_ids"] if differential_current else []})
    if args.snapshot:
        save(DATA / "native-reference.json", manifest)
        save(DATA / "matrix.json", matrix)
        (DATA / "native-events.jsonl").write_bytes((args.build / "events.jsonl").read_bytes())
        report = args.build / "portability/report.json"
        if report.exists():
            save(DATA / "portability.json", json.loads(portable_text(report.read_text(), roots)))
        if differential_current:
            (DATA / "policy-test.jsonl").write_bytes(result_path.read_bytes())
    if args.snapshot_abi32:
        if manifest["build_abi"]["long_bits"] != 32 or "millis-wrap" not in passed:
            raise SystemExit("Cannot publish ABI32 evidence without actual 32-bit wrap execution")
        if (args.build / "oracle").read_bytes()[:5] != b"\x7fELF\x01":
            raise SystemExit("ABI32 reference executable is not ELF32")
        save(DATA / "abi32-reference.json", manifest)
        (DATA / "abi32-events.jsonl").write_bytes((args.build / "events.jsonl").read_bytes())
        save(DATA / "abi32-evidence.json", {
            "status": "passed", "reference_sha256": digest(DATA / "abi32-reference.json"),
            "events_sha256": digest(DATA / "abi32-events.jsonl"),
            "binary_sha256": manifest["binary_sha256"], "elf_class": 32,
            "native_commit": REFERENCE, "compiler": manifest["compiler"],
            "abi_flags": portable_text(args.abi_flags, roots), "abi": manifest["build_abi"],
            "passed_scenarios": passed,
            "scope": "Unmodified upstream 32-bit long clock/queue wrap; not stock target firmware or RF.",
            "restoration": {
                "installed_request": "lib32stdc++-15-dev=15.2.0-16ubuntu1, with normal apt dependencies; no upgrades/removals",
                "rejected_request": "gcc-multilib would remove existing ARM/AArch64 cross compilers",
                "header_selection": "Existing official x86 asm headers via -idirafter; same target as gcc-multilib package asm symlink",
                "floating_point": "SSE float evaluation avoids x87 excess-precision budget rounding; upstream types unchanged"
            }})
    print(f"{len(passed)} native scenarios passed; {len(matrix['rows'])} obligations inventoried; no host/RF parity claim.")


if __name__ == "__main__":
    main()
