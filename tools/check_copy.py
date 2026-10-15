#!/usr/bin/env python3
"""Find phrases to review in reader-facing prose and application strings."""
import argparse
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SUFFIXES = {".md", ".html", ".svg", ".json", ".c", ".cpp", ".h", ".hew", ".lua",
            ".go", ".rs", ".py", ".ts", ".js", ".mjs"}
SKIP_PARTS = {".git", ".tmp", ".build", "build", "dist", "node_modules", "vendor",
              "third_party", "tests", "test", "testdata", "test_support", "probes", "fixtures", ".github"}
MAINTAINER_NAMES = {"AGENTS.md", "CONTRIBUTING.md", "COPY_REVIEW.md", "BINARY_RELEASE.md",
                    "LIBRARIES.md", "SUPERVISION.md", "NATIVE_SERVICES.md", "resource-budget.md",
                    "ARCHITECTURE.md", "RELEASING.md", "ROADMAP.md", "THIRD_PARTY.md",
                    "BOT_DEVELOPMENT.md", "check_copy.py"}
PROMPTS = {
    "vague-state": r"\b(?:retained|preserved|authoritative|bounded)\b",
    "qualification": r"\b(?:not (?:proof|evidence) of|does not (?:imply|establish|prove)|"
                     r"(?:is|are) not (?:proof|evidence)|cannot (?:prove|establish))\b",
    "contrast": r"[,;—]\s*not\s+\w|\bnot\s+\w[^.!?\n]{0,60}\bbut\s+",
    "process": r"\b(?:release gate|qualification|qualification open|public-default promotion|"
               r"parked implementation|build.status|source.confirmed|acceptance evidence|"
               r"verified in (?:a |an )?(?:disposable|isolated)|review checkpoint|"
               r"ownership boundar(?:y|ies)|provenance|certification|exhaustive coverage)\b",
}
PATTERNS = {name: re.compile(pattern, re.I) for name, pattern in PROMPTS.items()}
# This is a string scanner, not a language parser. Comments and identifiers are
# omitted; raw HTML, C++ raw strings, Lua long strings and multiline literals count.
TOKENS = re.compile(
    r'//[^\n]*|/\*.*?\*/|--\[\[.*?\]\]|--[^\n]*|\#[^\n]*|'
    r'(?P<literal>R"(?P<delimiter>\w*)\(.*?\)(?P=delimiter)"|'
    r'""".*?"""|\x27\x27\x27.*?\x27\x27\x27|\[\[.*?\]\]|'
    r'"(?:\\.|[^"\\])*"|\x27(?:\\.|[^\x27\\])*\x27|`[^`]*`)', re.S)
ALLOW = re.compile(r"copy-check:\s*allow\s+([a-z-]+)\s+--\s*(\S.*)")


def included(path):
    if path.suffix not in SUFFIXES or set(path.parts) & SKIP_PARTS:
        return False
    if path.parts[:1] == ("release",) or path.parts[:4] == ("packages", "hew-lua", "native", "lua"):
        return False
    name = path.name
    return (name not in MAINTAINER_NAMES and
            not re.search(r"(?:protocol|changelog|history|validation|release|qualification)", name, re.I) and
            not name.startswith(("test_", "test-")) and not name.endswith(("_test.go", ".test.ts", ".test.mjs")))


def targets(root, paths):
    if paths:
        files = []
        for path in paths:
            candidate = root / path
            if not candidate.exists():
                raise ValueError(f"Path does not exist: {candidate}")
            files.extend(candidate.rglob("*") if candidate.is_dir() else [candidate])
    else:
        result = subprocess.run(["git", "-C", str(root), "ls-files", "-z"],
                                check=True, capture_output=True)
        files = [root / name.decode() for name in result.stdout.split(b"\0") if name]
    return sorted({path for path in files if path.is_file() and not path.is_symlink() and
                   included(path.relative_to(root))})


def prose(path, text):
    if path.suffix == ".md":
        text = re.sub(r"<!--.*?-->", lambda match: "\n" * match[0].count("\n"), text, flags=re.S)
        fenced = False
        for number, line in enumerate(text.splitlines(), 1):
            if re.match(r"\s*(```|~~~)", line):
                fenced = not fenced
            elif not fenced:
                # Inline code usually names commands, fields or protocol tokens.
                yield number, re.sub(r"`[^`]*`", "", line)
    elif path.suffix in {".html", ".svg"}:
        # Inline JavaScript can contain errors and dynamically displayed text.
        for script in re.finditer(r"<script\b[^>]*>(.*?)</script>", text, re.S | re.I):
            first = text.count("\n", 0, script.start(1))
            for number, line in prose(Path("inline.js"), script[1]):
                yield first + number, line
        # Scan visible text, labels and tooltips; avoid script/CSS implementation.
        cleaned = re.sub(r"<(script|style)\b[^>]*>.*?</\1>",
                         lambda match: "\n" * match[0].count("\n"), text, flags=re.S | re.I)
        cleaned = re.sub(r"<!--.*?-->", lambda match: "\n" * match[0].count("\n"), cleaned, flags=re.S)
        yield from enumerate(cleaned.splitlines(), 1)
    else:
        for match in TOKENS.finditer(text):
            if match.group("literal") is None:
                continue
            if path.suffix == ".json" and re.match(r"\s*:", text[match.end():]):
                continue  # JSON field names are identifiers.
            first = text.count("\n", 0, match.start()) + 1
            for offset, line in enumerate(match[0].splitlines()):
                yield first + offset, line


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="*", type=Path, help="Files/directories relative to --root")
    parser.add_argument("--root", type=Path, default=ROOT, help="Git checkout to scan")
    parser.add_argument("--inventory", action="store_true", help="List candidate files instead of phrases")
    args = parser.parse_args()
    try:
        root = args.root.resolve()
        files = targets(root, args.paths)
        count = allowed = 0
        for path in files:
            label = path.relative_to(root)
            if args.inventory:
                print(label)
                continue
            text = path.read_text()
            lines = text.splitlines()
            for number, line in prose(path, text):
                for name, pattern in PATTERNS.items():
                    if not pattern.search(line):
                        continue
                    nearby = "\n".join(lines[max(0, number - 2):number])
                    reasons = {item[1]: re.sub(r"\s*(?:-->|\*/)\s*$", "", item[2])
                               for item in ALLOW.finditer(nearby)}
                    if reasons.get(name):
                        print(f"{label}:{number}: allow {name}: {reasons[name]}")
                        allowed += 1
                    else:
                        excerpt = line.strip()
                        if len(excerpt) > 240:
                            start = max(0, pattern.search(excerpt).start() - 90)
                            excerpt = ("…" if start else "") + excerpt[start:start + 240] + "…"
                        print(f"{label}:{number}: {name}: {excerpt}")
                        count += 1
        if not args.inventory:
            print(f"Review prompts: {count}; explained exceptions: {allowed}; candidate files: {len(files)}")
            print("Read each passage in context. Matches are prompts, not errors or word bans.")
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.exit(2, f"Copy check: {error}\n")


if __name__ == "__main__":
    main()
