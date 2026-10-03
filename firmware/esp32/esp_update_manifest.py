#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Sign an ESP32S3 app-only field update with the existing operator seed."""
import argparse
import hashlib
import json
import re
import struct

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.hardware.rf import read_key


def canonical(target, size, digest):
    if not re.fullmatch(r"[a-z0-9][a-z0-9-]{0,62}", target):
        raise ValueError("Target must be 1..63 lowercase letters, digits or hyphens")
    if not 0 < size <= 0x330000 or not re.fullmatch(r"[0-9a-f]{64}", digest):
        raise ValueError("Invalid application size or SHA256")
    return f"meshcore-esp-update-v1\nesp32s3\n{target}\n{size}\n{digest}\n".encode("ascii")


def sign(image, target, seed):
    if len(image) < 24 or image[0] != 0xe9 or struct.unpack_from("<H", image, 12)[0] != 9:
        raise ValueError("Expected an ESP32S3 application image")
    if not 0 < image[1] <= 16 or image[23] not in (0, 1):
        raise ValueError("Invalid ESP application image header")
    offset, checksum = 24, 0xef
    for _ in range(image[1]):
        if offset + 8 > len(image):
            raise ValueError("Truncated ESP application segment header")
        size = struct.unpack_from("<I", image, offset + 4)[0]
        offset += 8
        if offset + size > len(image):
            raise ValueError("Truncated ESP application segment")
        for value in image[offset:offset + size]:
            checksum ^= value
        offset += size
    end = (offset + 16) & ~15
    if end > len(image) or image[end - 1] != checksum:
        raise ValueError("ESP application checksum does not match")
    expected_size = end + (32 if image[23] else 0)
    if expected_size != len(image):
        raise ValueError("ESP application has extra or missing trailing bytes")
    if image[23] and hashlib.sha256(image[:end]).digest() != image[end:]:
        raise ValueError("ESP application appended SHA256 does not match")
    digest = hashlib.sha256(image).hexdigest()
    message = canonical(target, len(image), digest)
    if b"meshcore-esp-target-v1:" + target.encode("ascii") + b":" not in image:
        raise ValueError("Application image does not contain the requested field-update target")
    return {"target": target, "size": len(image), "sha256": digest,
            "signature": Ed25519PrivateKey.from_private_bytes(seed).sign(message).hex()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--target", default="xiao-esp32s3")
    parser.add_argument("--key-file", type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(sign(args.image.read_bytes(), args.target, read_key(args.key_file))))


if __name__ == "__main__":
    main()
