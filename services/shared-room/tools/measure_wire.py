#!/usr/bin/env python3
"""Reproduce v1 wire sizes. CBOR is a comparison, not a second service protocol.
Use an isolated venv with cbor2==5.6.5. Synthetic keys come from native fixtures.
"""
import argparse
import base64
import json
from pathlib import Path
import time
import tracemalloc
import uuid

root = Path(__file__).resolve().parents[3]
parser = argparse.ArgumentParser()
parser.add_argument("--out", type=Path, default=root / ".tmp/wire-frames")
parser.add_argument("--json-only", action="store_true", help="generate native parser fixtures without CBOR measurements")
args = parser.parse_args()
args.out.mkdir(parents=True, exist_ok=True)
client = "3b6a27bcceb6a42d62a3a8d02a6f0d73653215771de243a63ac048a18b59da29"
author = "e68e0cf7f26c59f963b5846202d2327cc8bc0c4eff8cb9abd4012f9a71decf00"
delivery_id = "00000001-0002-4003-8004-000000000005"
route = base64.b64encode(bytes([1, 1, 0x82, 1, 2, 3, 4, 5, 6])).decode()
attempt = "ab" * 32
stamp = 1700000000
message = dict(seq=1, timestamp=stamp, originAlias="A", author=author, clientTimestamp=stamp, text="hello")
frames = {
 "login": dict(id="1", operation=dict(op="login", client=client, timestamp=stamp, since=0, password="room", attempt=attempt, route=route)),
 "post": dict(id="2", operation=dict(op="post", client=client, timestamp=stamp, text="hello", source="client", attempt=attempt)),
 "history-request": dict(id="3", operation=dict(op="refresh", client=client, timestamp=stamp+1, since=stamp, attempt=attempt, route=route)),
 "history-delivery": dict(type="delivery", alias="A", client=client, deliveryId=delivery_id, route=route, message=message),
 "notification": dict(type="delivery", alias="A", client=client, deliveryId=delivery_id, route=route, message={**message, "seq":2, "text":"A new room post"}),
 "ack": dict(id="4", operation=dict(op="ack", client=client, deliveryId=delivery_id, proof="11223344")),
 "login-result": dict(type="result", id="1", result=dict(respond=True, cursor=0)),
 "ack-result": dict(type="result", id="4", result=dict(cursor=1)),
 "ready": dict(type="ready", version=1, alias="A", publicKey=author, name="WelcomeYEG"),
}
# A longer native post and a byte-preserving route exercise actual upper bounds.
frames["maximum-delivery"] = {**frames["history-delivery"], "route":base64.b64encode(bytes(range(255))).decode(),
 "message":{**message, "text":"\x01"*151}}

for name, frame in frames.items():
 (args.out / (name + ".json")).write_bytes(json.dumps(frame, separators=(",",":"),ensure_ascii=False).encode())
if args.json_only:
 print(f"Generated {len(frames)} JSON parser fixtures.")
 raise SystemExit(0)

import cbor2

def compact(value, key=""):
 if isinstance(value, dict): return {k:compact(v,k) for k,v in value.items()}
 if isinstance(value, list): return [compact(v) for v in value]
 if key in ("client", "author", "attempt", "publicKey", "proof"): return bytes.fromhex(value)
 if key == "route": return base64.b64decode(value, validate=True)
 if key == "deliveryId": return uuid.UUID(value).bytes
 return value

print("frame,json bytes,CBOR same fields,CBOR binary fields")
for name, frame in frames.items():
 data = json.dumps(frame, separators=(",",":"),ensure_ascii=False).encode()
 binary = b"\x01" + cbor2.dumps(compact(frame), canonical=True)
 assert cbor2.loads(binary[1:]) == compact(frame)
 (args.out / (name + ".cbor")).write_bytes(binary)
 print(f"{name},{len(data)},{1+len(cbor2.dumps(frame, canonical=True))},{len(binary)}")
# The native fixture supplies measured ciphertext bytes, not an imagined packet.
for line in (root / "testdata/parity/native-events.jsonl").read_text().splitlines():
 event = json.loads(line)
 if event.get("scenario") == "encrypted-wire-vectors" and event.get("event") == "encrypted_packet":
  packet = bytes.fromhex(event["bytes"])
  rf = json.dumps(dict(type="rx", packet=base64.b64encode(packet).decode()),separators=(",",":")).encode()
  print(f"raw-post-reference,{len(rf)},-,{6+len(packet)} (fixed v/type/id header)")
  break
print("\nHost cbor2 allocation/parse reference; object decoder, NOT an embedded CBOR parser:")
for name in ("login", "history-delivery", "ack"):
 data = (args.out / (name+".cbor")).read_bytes()[1:]
 start = time.perf_counter_ns()
 for _ in range(20000): cbor2.loads(data)
 ns = (time.perf_counter_ns()-start)/20000
 tracemalloc.start()
 cbor2.loads(data)
 current,peak = tracemalloc.get_traced_memory()
 tracemalloc.stop()
 print(f"{name}: {ns:.0f} ns/op, {peak} peak traced bytes; host Python decoder (not comparable to native C parser timing)")
