#!/usr/bin/env python3
"""Generate a private virtual-repeater identity or configure one inventoried Pine over USB."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.hardware.admin import private_file
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives import serialization


def generate():
    seed = os.urandom(32)
    private = bytearray(hashlib.sha512(seed).digest())
    private[0] &= 248
    private[31] = (private[31] & 63) | 64
    public = Ed25519PrivateKey.from_private_bytes(seed).public_key().public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    return {"private": private.hex(), "public": public.hex()}


def identity(path):
    data = json.loads(private_file(path, 512))
    if set(data) != {"private", "public"} or not re.fullmatch(r"[0-9a-f]{128}", data["private"]) or not re.fullmatch(r"[0-9a-f]{64}", data["public"]):
        raise ValueError("Private peer identity must contain expanded private/public Ed25519 hex keys")
    return data


def configure(args):
    from hardware import connection, serial_command
    data = identity(args.identity)
    address = re.fullmatch(r"([01]):([0-9a-fA-F]{12})", args.peer_address)
    pin = private_file(args.pin_file, 7).decode("ascii").strip()
    if not address or not re.fullmatch(r"[1-9][0-9]{5}", pin):
        raise ValueError("Peer address must be TYPE:HEX12 and protected PIN file must contain six digits")
    commands = [(f'peer key private {data["private"]}', "Peer private key staged"),
                (f'peer key public {data["public"]}', "Peer virtual identity saved"),
                (f'peer configure {args.role} {address[1]} {address[2]} {pin}', "Peer configuration saved")]
    if args.enable:
        commands.append(("peer on", "Peer on saved"))
    with connection(args.port) as link:
        time.sleep(.2)
        link.reset_input_buffer()
        for command, expected in commands:
            reply = serial_command(link, command, 6).strip()
            if not reply.startswith(expected):
                raise RuntimeError("Pine did not confirm peer configuration; inspect peer status before retrying")
    print("Pine peer configuration saved; physical identities/PHY unchanged; reboot only after both radios are configured")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="operation", required=True)
    create = sub.add_parser("generate")
    create.add_argument("--output", type=Path, required=True)
    config = sub.add_parser("configure")
    config.add_argument("--port", required=True)
    config.add_argument("--identity", type=Path, required=True)
    config.add_argument("--pin-file", type=Path, required=True)
    config.add_argument("--peer-address", required=True)
    config.add_argument("--role", choices=("central", "peripheral"), required=True)
    config.add_argument("--enable", action="store_true")
    args = parser.parse_args()
    if args.operation == "generate":
        if args.output.is_absolute() or not args.output.resolve().is_relative_to(Path.cwd()):
            raise ValueError("Create the private peer identity below the current project directory")
        fd = os.open(args.output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "w") as stream:
            json.dump(generate(), stream)
            stream.write("\n")
        print("Private virtual-repeater identity created; keep it out of source and release assets")
    else:
        configure(args)


if __name__ == "__main__":
    main()
