#!/usr/bin/env python3
"""Record the unchanged native Make compiler commands before executing them."""
import json
import os
import shutil
import subprocess
import sys

compiler = shutil.which(sys.argv[1])
if not compiler:
    raise SystemExit("Native compiler unavailable: "+sys.argv[1])
record = json.dumps({"directory": os.getcwd(), "arguments": [compiler]+sys.argv[2:]})+"\n"
fd = os.open(os.environ["HEW_NATIVE_COMPILER_LOG"], os.O_WRONLY | os.O_APPEND | os.O_CREAT, 0o600)
os.write(fd, record.encode())
os.close(fd)
raise SystemExit(subprocess.call([compiler]+sys.argv[2:]))
