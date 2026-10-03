# SPDX-License-Identifier: Apache-2.0
"""Generate a synthetic cross-language on-air fixture; no operator secrets."""
import hashlib

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ed25519

import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.hardware.rf import (
    frame, frame_with_context, query_with_context, verify_receipt, verify_status,
)

management_seed = bytes(reversed(range(32)))
operator_seed = bytes(range(32))
management = ed25519.Ed25519PrivateKey.from_private_bytes(management_seed)
operator = ed25519.Ed25519PrivateKey.from_private_bytes(operator_seed)
public = lambda key: key.public_key().public_bytes(
    serialization.Encoding.Raw, serialization.PublicFormat.Raw
)
expanded = bytearray(hashlib.sha512(management_seed).digest())
expanded[0] &= 248
expanded[31] &= 63
expanded[31] |= 64
target = public(management)
sender_seed = bytes(range(1, 33))
sender_expanded = bytearray(hashlib.sha512(sender_seed).digest())
sender_expanded[0] &= 248
sender_expanded[31] &= 63
sender_expanded[31] |= 64
sender = ed25519.Ed25519PrivateKey.from_private_bytes(sender_seed)
if len(sys.argv) == 2:
    Path(sys.argv[1]).write_bytes(
        bytes(expanded) + public(operator) + target +
        bytes(sender_expanded) + public(sender) +
        frame(operator_seed, target, 3, 0x12345678, 4, sender_seed)
    )
elif len(sys.argv) == 4 and sys.argv[1] == "verify":
    packet, context = frame_with_context(
        operator_seed, target, 3, 0x12345678, 4, sender_seed
    )
    assert Path(sys.argv[2]).read_bytes().endswith(packet)
    assert verify_receipt(Path(sys.argv[3]).read_bytes(), context) == 1
    print("PASS native signed RF receipt verified by Python")
elif len(sys.argv) == 3 and sys.argv[1] == "query":
    packet, _ = query_with_context(
        operator_seed, target, 0x12345678, sender_seed
    )
    Path(sys.argv[2]).write_bytes(
        bytes(expanded) + public(operator) + target +
        bytes(sender_expanded) + public(sender) + packet
    )
elif len(sys.argv) == 5 and sys.argv[1] == "verify-status":
    packet, context = query_with_context(
        operator_seed, target, 0x12345678, sender_seed
    )
    assert Path(sys.argv[2]).read_bytes().endswith(packet)
    status = verify_status(Path(sys.argv[3]).read_bytes(), context)
    assert status == {
        "valid": True, "sealed": False,
        "profile_generation": int(sys.argv[4]),
        "saved_mask": 0, "applied_mask": 0,
    }
    print("PASS native signed RF status verified by Python")
else:
    raise ValueError(
        "Usage: rf_vector.py VECTOR | verify VECTOR NATIVE_RECEIPT | "
        "query VECTOR | verify-status VECTOR NATIVE_STATUS GENERATION"
    )
