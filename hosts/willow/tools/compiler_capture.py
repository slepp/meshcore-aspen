#!/usr/bin/env python3
"""Record native Make compiler commands and their source dependencies."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

compiler = shutil.which(sys.argv[1])
if not compiler:
    raise SystemExit("Native compiler unavailable: "+sys.argv[1])
arguments = [compiler]+sys.argv[2:]
log = Path(os.environ["HEW_NATIVE_COMPILER_LOG"])


def run(command):
    record = json.dumps({"directory": os.getcwd(), "arguments": command})+"\n"
    fd = os.open(log, os.O_WRONLY | os.O_APPEND | os.O_CREAT, 0o600)
    os.write(fd, record.encode())
    os.close(fd)
    subprocess.run(command, check=True)


sources = [arg for arg in arguments[1:] if not arg.startswith("-") and arg.endswith((".c", ".cc", ".cpp", ".cxx", ".S"))]
if sources:
    # A multi-source link with -MD/-MF overwrites earlier translation units.
    # Ask the same compiler for each unit's full dependency rule separately.
    flags = []
    skip = False
    for arg in arguments[1:]:
        if skip:
            skip = False
        elif arg == "-o":
            skip = True
        elif arg not in sources and not arg.endswith((".o", ".a", ".so")) and not arg.startswith(("-l", "-Wl,")):
            flags.append(arg)
    dependencies = log.parent/"dependencies"
    dependencies.mkdir(exist_ok=True)
    for source in sources:
        key = hashlib.sha256(json.dumps(arguments+[source]).encode()).hexdigest()
        run([compiler, *flags, source, "-M", "-MF", str(dependencies/(key+".d")), "-MT", "native"])
run(arguments)
