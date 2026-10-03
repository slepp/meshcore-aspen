#!/usr/bin/env python3
"""Configure a MeshCore KISS modem and print received packets."""

import argparse
import json
import math
import socket
import struct
import sys
import time
from datetime import datetime
from typing import Callable

import serial

FEND = 0xC0
FESC = 0xDB
TFEND = 0xDC
TFESC = 0xDD

PAYLOAD_TYPES = {
    0x0: "request",
    0x1: "response",
    0x2: "text-message",
    0x3: "ack",
    0x4: "advert",
    0x5: "group-text",
    0x6: "group-data",
    0x7: "anonymous-request",
    0x8: "returned-path",
    0x9: "trace",
    0xA: "multipart",
    0xB: "control",
    0xF: "raw-custom",
}
ROUTE_TYPES = {
    0: "transport-flood",
    1: "flood",
    2: "direct",
    3: "transport-direct",
}
HARDWARE_EVENTS = {
    0x81: "Identity",
    0x82: "Random",
    0x83: "SignatureVerification",
    0x84: "Signature",
    0x85: "EncryptedData",
    0x86: "DecryptedData",
    0x87: "KeyExchange",
    0x88: "Hash",
    0x8B: "Radio",
    0x8C: "TxPower",
    0x8D: "CurrentRssi",
    0x8E: "ChannelBusy",
    0x8F: "Airtime",
    0x90: "NoiseFloor",
    0x91: "Version",
    0x92: "Stats",
    0x93: "Battery",
    0x94: "McuTemperature",
    0x95: "Sensors",
    0x96: "DeviceName",
    0x97: "Ping",
    0x9A: "SignalReport",
    0xF0: "OK",
    0xF1: "Error",
    0xF8: "TxDone",
    0xF9: "RxMeta",
}
ADVERT_TYPES = {
    0: "none",
    1: "chat",
    2: "repeater",
    3: "room-server",
    4: "sensor",
}
CONTROL_TYPES = {
    8: "discover-request",
    9: "discover-response",
}
REQUIRED_SNR_DB = {
    5: -2.5,
    6: -5.0,
    7: -7.5,
    8: -10.0,
    9: -12.5,
    10: -15.0,
    11: -17.5,
    12: -20.0,
}
ERRORS = {
    1: "InvalidLength",
    2: "InvalidParam",
    3: "NoCallback",
    4: "MacFailed",
    5: "UnknownCmd",
    6: "EncryptFailed",
    7: "TxBusy",
}


def signed_byte(value: int) -> int:
    return value - 256 if value >= 128 else value


def kiss_frame(type_byte: int, payload: bytes = b"") -> bytes:
    encoded = bytearray([FEND])
    for value in bytes([type_byte]) + payload:
        if value == FEND:
            encoded.extend((FESC, TFEND))
        elif value == FESC:
            encoded.extend((FESC, TFESC))
        else:
            encoded.append(value)
    encoded.append(FEND)
    return bytes(encoded)


def set_hardware(subcommand: int, payload: bytes = b"") -> bytes:
    return kiss_frame(0x06, bytes([subcommand]) + payload)


DEFAULT_TCP_PORT = 8001


def split_tcp_endpoint(address: str) -> tuple[str, int]:
    """Split HOST or HOST:PORT, defaulting to the KISS TCP port."""
    host, separator, port = address.rpartition(":")
    if not separator:
        host, port = address, str(DEFAULT_TCP_PORT)
    if not host:
        raise ValueError("TCP endpoint must be HOST or HOST:PORT")
    return host, int(port)


class TcpTransport:
    def __init__(self, address: str, timeout: float = 0.1) -> None:
        host, port = split_tcp_endpoint(address)
        self.socket = socket.create_connection((host, port), timeout=15)
        self.socket.settimeout(timeout)

    def read(self, size: int) -> bytes:
        try:
            data = self.socket.recv(size)
        except socket.timeout:
            return b""
        if not data:
            raise ConnectionError("KISS TCP connection closed")
        return data

    def write(self, data: bytes) -> int:
        self.socket.sendall(data)
        return len(data)

    def flush(self) -> None:
        pass

    def reset_input_buffer(self) -> None:
        pass

    def close(self) -> None:
        self.socket.close()

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_value, traceback) -> None:
        self.close()


class KissDecoder:
    """Incrementally decode a byte stream into unescaped KISS frames."""

    def __init__(self) -> None:
        self.frame = bytearray()
        self.in_frame = False
        self.escaped = False

    def feed(self, data: bytes) -> list[bytes]:
        frames = []
        for value in data:
            if value == FEND:
                if self.in_frame and self.frame:
                    frames.append(bytes(self.frame))
                self.frame.clear()
                self.in_frame = True
                self.escaped = False
                continue
            if not self.in_frame:
                continue
            if self.escaped:
                if value == TFEND:
                    self.frame.append(FEND)
                elif value == TFESC:
                    self.frame.append(FESC)
                else:
                    self.frame.extend((FESC, value))
                self.escaped = False
            elif value == FESC:
                self.escaped = True
            elif len(self.frame) < 512:
                self.frame.append(value)
            else:
                self.frame.clear()
                self.in_frame = False
                self.escaped = False
        return frames


def printable(data: bytes) -> str:
    return "".join(chr(value) if 32 <= value < 127 else "." for value in data)


def timestamp_iso(timestamp: int) -> str | None:
    try:
        return datetime.fromtimestamp(timestamp).astimezone().isoformat(timespec="seconds")
    except (OSError, OverflowError, ValueError):
        return None


def estimate_lora_airtime_ms(
    payload_length: int,
    bandwidth_hz: int,
    sf: int,
    cr: int,
) -> float:
    symbol_seconds = (2**sf) / bandwidth_hz
    low_data_rate_optimization = int(symbol_seconds >= 0.016)
    numerator = 8 * payload_length - 4 * sf + 28 + 16
    denominator = 4 * (sf - 2 * low_data_rate_optimization)
    payload_symbols = 8 + max(math.ceil(numerator / denominator) * (cr - 4), 0)
    return (8 + 4.25 + payload_symbols) * symbol_seconds * 1000


def signal_quality(snr_db: float, sf: int) -> tuple[float, str]:
    margin = snr_db - REQUIRED_SNR_DB[sf]
    if margin >= 15:
        quality = "excellent"
    elif margin >= 10:
        quality = "strong"
    elif margin >= 5:
        quality = "good"
    elif margin >= 0:
        quality = "marginal"
    else:
        quality = "below-demodulation-threshold"
    return margin, quality


def decode_encrypted_wrapper(payload: bytes, result: dict, group: bool = False) -> None:
    prefix_length = 1 if group else 2
    minimum_length = prefix_length + 2
    if len(payload) < minimum_length:
        result["error"] = "truncated encrypted payload wrapper"
        return
    if group:
        result["channel_hash"] = f"{payload[0]:02x}"
    else:
        result["destination_hash"] = f"{payload[0]:02x}"
        result["source_hash"] = f"{payload[1]:02x}"
    result.update(
        encrypted=True,
        cipher_mac=payload[prefix_length:minimum_length].hex(),
        ciphertext_hex=payload[minimum_length:].hex(),
        ciphertext_length=len(payload) - minimum_length,
        cipher_blocks=(len(payload) - minimum_length) // 16,
    )


def decode_meshcore_packet(packet: bytes) -> dict:
    result = {"raw_hex": packet.hex(), "length": len(packet)}
    if len(packet) < 2:
        result["error"] = "packet is shorter than the MeshCore header"
        return result

    header = packet[0]
    route_id = header & 0x03
    payload_id = (header >> 2) & 0x0F
    result.update(
        header=f"0x{header:02x}",
        version=(header >> 6) + 1,
        route_id=route_id,
        route=ROUTE_TYPES[route_id],
        payload_type_id=payload_id,
        payload_type=PAYLOAD_TYPES.get(payload_id, f"reserved-{payload_id}"),
    )

    offset = 1
    if route_id in (0, 3):
        if len(packet) < offset + 4:
            result["error"] = "truncated transport codes"
            return result
        result["transport_codes"] = [
            int.from_bytes(packet[offset : offset + 2], "little"),
            int.from_bytes(packet[offset + 2 : offset + 4], "little"),
        ]
        offset += 4

    if len(packet) <= offset:
        result["error"] = "missing path metadata"
        return result
    path_metadata = packet[offset]
    offset += 1
    hop_count = path_metadata & 0x3F
    hash_size = (path_metadata >> 6) + 1
    path_bytes = hop_count * hash_size
    result.update(
        path_metadata=f"0x{path_metadata:02x}",
        hop_count=hop_count,
        path_hash_size=hash_size,
        path_byte_length=path_bytes,
    )
    if hash_size == 4:
        result["error"] = "reserved path hash size"
        return result
    if len(packet) < offset + path_bytes:
        result["error"] = "truncated path"
        return result

    path = packet[offset : offset + path_bytes]
    result["path"] = [
        path[index : index + hash_size].hex()
        for index in range(0, len(path), hash_size)
    ]
    offset += path_bytes
    payload = packet[offset:]
    result.update(payload_hex=payload.hex(), payload_length=len(payload))

    if payload_id in (0, 1, 2, 8):
        decode_encrypted_wrapper(payload, result)
    elif payload_id in (5, 6):
        decode_encrypted_wrapper(payload, result, group=True)
    elif payload_id == 3:
        if len(payload) < 4:
            result["error"] = "truncated acknowledgement"
        else:
            result["checksum"] = payload[:4].hex()
            result["ack_crc"] = int.from_bytes(payload[:4], "little")
            if len(payload) > 4:
                result["ack_extra_hex"] = payload[4:].hex()
    elif payload_id == 4:
        decode_advert(payload, result)
    elif payload_id == 7:
        if len(payload) < 35:
            result["error"] = "truncated anonymous request"
        else:
            result.update(
                encrypted=True,
                destination_hash=f"{payload[0]:02x}",
                sender_public_key=payload[1:33].hex(),
                cipher_mac=payload[33:35].hex(),
                ciphertext_hex=payload[35:].hex(),
                ciphertext_length=len(payload) - 35,
                cipher_blocks=(len(payload) - 35) // 16,
            )
    elif payload_id == 9:
        if len(payload) < 9:
            result["error"] = "truncated trace payload"
        else:
            flags = payload[8]
            route_hash_size = 1 << (flags & 0x03)
            route = payload[9:]
            result.update(
                trace_tag=int.from_bytes(payload[:4], "little"),
                trace_auth_code=f"0x{int.from_bytes(payload[4:8], 'little'):08x}",
                trace_flags=f"0x{flags:02x}",
                trace_route_hash_size=route_hash_size,
                trace_route=[
                    route[index : index + route_hash_size].hex()
                    for index in range(0, len(route), route_hash_size)
                    if len(route[index : index + route_hash_size]) == route_hash_size
                ],
                trace_snr_samples_db=[signed_byte(value) / 4 for value in path],
            )
            result.pop("path", None)
    elif payload_id == 0x0A:
        if not payload:
            result["error"] = "truncated multipart payload"
        else:
            descriptor = payload[0]
            embedded_type = descriptor & 0x0F
            result.update(
                multipart_remaining=descriptor >> 4,
                multipart_type_id=embedded_type,
                multipart_type=PAYLOAD_TYPES.get(
                    embedded_type, f"reserved-{embedded_type}"
                ),
                multipart_payload_hex=payload[1:].hex(),
            )
            if embedded_type == 3 and len(payload) >= 5:
                result["ack_crc"] = int.from_bytes(payload[1:5], "little")
    elif payload_id == 0x0B:
        decode_control(payload, result)
    elif payload_id == 0x0F:
        result["custom_payload_hex"] = payload.hex()
        if payload and all(32 <= byte < 127 or byte in (9, 10, 13) for byte in payload):
            result["custom_payload_text"] = payload.decode("utf-8", errors="replace")

    return result


def decode_control(payload: bytes, result: dict) -> None:
    if not payload:
        result["error"] = "empty control payload"
        return
    control = payload[0]
    subtype = control >> 4
    flags = control & 0x0F
    result.update(
        control_byte=f"0x{control:02x}",
        control_subtype_id=subtype,
        control_subtype=CONTROL_TYPES.get(subtype, f"unknown-{subtype}"),
        control_flags=f"0x{flags:x}",
    )
    if subtype == 8:
        if len(payload) < 6:
            result["error"] = "truncated discovery request"
            return
        result.update(
            prefix_only=bool(flags & 0x01),
            type_filter=f"0x{payload[1]:02x}",
            discovery_tag=f"0x{int.from_bytes(payload[2:6], 'little'):08x}",
        )
        if len(payload) >= 10:
            since = int.from_bytes(payload[6:10], "little")
            result["since"] = since
            result["since_iso"] = timestamp_iso(since)
    elif subtype == 9:
        if len(payload) < 6:
            result["error"] = "truncated discovery response"
            return
        result.update(
            node_type_id=flags,
            node_type=ADVERT_TYPES.get(flags, f"unknown-{flags}"),
            discovery_snr_db=signed_byte(payload[1]) / 4,
            discovery_tag=f"0x{int.from_bytes(payload[2:6], 'little'):08x}",
            node_public_key=payload[6:].hex(),
            node_public_key_length=len(payload) - 6,
        )
    elif len(payload) > 1:
        result["control_payload_hex"] = payload[1:].hex()


def decode_advert(payload: bytes, result: dict) -> None:
    if len(payload) < 100:
        result["error"] = "truncated advertisement"
        return
    advert_timestamp = int.from_bytes(payload[32:36], "little")
    result["public_key"] = payload[:32].hex()
    result["public_key_prefix"] = payload[:6].hex()
    result["advert_timestamp"] = advert_timestamp
    result["advert_timestamp_iso"] = timestamp_iso(advert_timestamp)
    result["signature"] = payload[36:100].hex()
    appdata = payload[100:]
    if not appdata:
        return
    flags = appdata[0]
    offset = 1
    advert_type = flags & 0x0F
    result["advert_flags"] = f"0x{flags:02x}"
    result["advert_type_id"] = advert_type
    result["advert_type"] = ADVERT_TYPES.get(advert_type, f"unknown-{advert_type}")
    if flags & 0x10 and len(appdata) >= offset + 8:
        latitude, longitude = struct.unpack_from("<ii", appdata, offset)
        result["latitude"] = latitude / 1_000_000
        result["longitude"] = longitude / 1_000_000
        offset += 8
    if flags & 0x20 and len(appdata) >= offset + 2:
        result["feature_1"] = int.from_bytes(appdata[offset : offset + 2], "little")
        offset += 2
    if flags & 0x40 and len(appdata) >= offset + 2:
        result["feature_2"] = int.from_bytes(appdata[offset : offset + 2], "little")
        offset += 2
    if flags & 0x80:
        result["name"] = appdata[offset:].decode("utf-8", errors="replace")


def decode_kiss_frame(frame: bytes) -> dict:
    if not frame:
        return {"kind": "invalid", "error": "empty KISS frame"}
    type_byte = frame[0]
    payload = frame[1:]
    command = type_byte & 0x0F
    result = {
        "kiss_port": type_byte >> 4,
        "kiss_command": command,
        "payload_length": len(payload),
    }
    if command == 0x00:
        result["kind"] = "data"
        result["meshcore"] = decode_meshcore_packet(payload)
    elif command == 0x06:
        result["kind"] = "set-hardware"
        if not payload:
            result["error"] = "missing SetHardware subcommand"
            return result
        subcommand = payload[0]
        data = payload[1:]
        result.update(
            subcommand=f"0x{subcommand:02X}",
            event=HARDWARE_EVENTS.get(subcommand, "SetHardware"),
            data_hex=data.hex(),
        )
        if subcommand == 0xF9 and len(data) >= 2:
            result["snr_db"] = signed_byte(data[0]) / 4
            result["rssi_dbm"] = signed_byte(data[1])
            result["local_loopback"] = data[:2] == b"\x80\x7f"
        elif subcommand == 0xF1 and data:
            result["error"] = ERRORS.get(data[0], f"code-{data[0]}")
        elif subcommand == 0xF8 and data:
            result["success"] = bool(data[0])
        elif subcommand == 0x8B and len(data) >= 10:
            frequency, bandwidth, sf, cr = struct.unpack_from("<IIBB", data)
            result["radio"] = {
                "frequency_hz": frequency,
                "bandwidth_hz": bandwidth,
                "sf": sf,
                "cr": cr,
            }
        elif subcommand == 0x8C and data:
            result["tx_power_dbm"] = data[0]
        elif subcommand == 0x8D and data:
            result["current_rssi_dbm"] = signed_byte(data[0])
        elif subcommand == 0x8E and data:
            result["channel_busy"] = bool(data[0])
        elif subcommand == 0x8F and len(data) >= 4:
            result["airtime_ms"] = int.from_bytes(data[:4], "little")
        elif subcommand == 0x90 and len(data) >= 2:
            result["noise_floor_dbm"] = int.from_bytes(data[:2], "little", signed=True)
        elif subcommand == 0x91 and data:
            result["firmware_version"] = ".".join(str(value) for value in data)
        elif subcommand == 0x92 and len(data) >= 12:
            rx, tx, errors = struct.unpack_from("<III", data)
            result["stats"] = {"rx": rx, "tx": tx, "errors": errors}
        elif subcommand == 0x93 and len(data) >= 2:
            result["battery_mv"] = int.from_bytes(data[:2], "little")
        elif subcommand == 0x94 and len(data) >= 2:
            result["mcu_temperature_c"] = int.from_bytes(
                data[:2], "little", signed=True
            ) / 10
        elif subcommand == 0x96:
            result["device_name"] = data.decode("utf-8", errors="replace")
        elif subcommand == 0x97:
            result["ping_reply"] = True
        elif subcommand == 0x9A and data:
            result["signal_report_enabled"] = bool(data[0])
    else:
        result.update(kind="unknown", payload_hex=payload.hex(), ascii=printable(payload))
    return result


def read_until(
    port: serial.Serial,
    decoder: KissDecoder,
    predicate: Callable[[bytes], bool],
    timeout: float = 3.0,
) -> tuple[bytes | None, list[bytes]]:
    deadline = time.monotonic() + timeout
    pending = []
    while time.monotonic() < deadline:
        frames = decoder.feed(port.read(256))
        for index, frame in enumerate(frames):
            if predicate(frame):
                pending.extend(frames[index + 1 :])
                return frame, pending
            pending.append(frame)
    return None, pending


def request_hardware(
    port: serial.Serial,
    decoder: KissDecoder,
    request: bytes,
    predicate: Callable[[bytes], bool],
    attempts: int = 2,
    timeout: float = 5.0,
) -> tuple[bytes | None, list[bytes]]:
    pending = []
    for _ in range(attempts):
        port.write(request)
        port.flush()
        reply, received = read_until(port, decoder, predicate, timeout)
        pending.extend(received)
        if reply is not None:
            return reply, pending
    return None, pending


class ConfigurationError(RuntimeError):
    """A rejected configuration will not improve by reconnecting."""


def _configure_shared_radio(port, decoder, radio: bytes, pending: list[bytes]) -> bool:
    reply, received = request_hardware(
        port, decoder, set_hardware(0x20, b"\x01\x01"),
        lambda frame: frame[:2] in (b"\x06\xa0", b"\x06\xf1"), attempts=1,
    )
    pending.extend(received)
    if reply is None:
        raise RuntimeError("shared configuration negotiation timed out")
    if reply in (b"\x06\xf1\x05", b"\x06\xf1\x02"):
        return False  # Stock KISS and the legacy broker do not expose ownership.
    if len(reply) != 14 or reply[2] != 1:
        raise ConfigurationError("invalid shared configuration negotiation response")
    if reply[3] != 0 or reply[13] != 1:
        raise ConfigurationError(
            f"shared radio configuration is owned by another client (reason {reply[3]})"
        )

    def exchange(payload: bytes) -> bytes:
        response, frames = request_hardware(
            port, decoder, set_hardware(0x22, payload),
            lambda frame: frame[:2] in (b"\x06\xa2", b"\x06\xf1"), attempts=1,
        )
        pending.extend(frames)
        if response is None:
            raise RuntimeError("shared radio configuration timed out")
        if len(response) != 26 or response[2] != 1:
            raise ConfigurationError("invalid shared radio configuration response")
        if response[3] != 0:
            raise ConfigurationError(f"shared radio configuration rejected (reason {response[3]})")
        return response

    current = exchange(b"\x01\x00")
    profile = radio + current[18:]  # Preserve power, airtime factor and carrier policy.
    confirmed = exchange(b"\x01\x01" + current[4:8] + profile)
    if confirmed[8:] != profile:
        raise ConfigurationError("shared radio configuration readback differs from request")
    return True


def configure_modem(
    port: serial.Serial,
    decoder: KissDecoder,
    frequency_hz: int,
    bandwidth_hz: int,
    sf: int,
    cr: int,
    *,
    verify_only: bool = False,
) -> tuple[list[bytes], bool]:
    radio = struct.pack("<IIBB", frequency_hz, bandwidth_hz, sf, cr)
    pending = []
    claimed_owner = False

    reply, received = request_hardware(
        port,
        decoder,
        set_hardware(0x0B),
        lambda frame: frame[:2] in (b"\x06\x8b", b"\x06\xf1"),
    )
    pending.extend(received)
    if reply is None:
        raise RuntimeError("GetRadio timed out after 2 attempts")
    decoded = decode_kiss_frame(reply)
    if decoded.get("event") == "Error":
        raise ConfigurationError(f"GetRadio failed: {decoded.get('error', 'unknown error')}")
    actual = decoded.get("radio")
    if actual is None:
        raise ConfigurationError("invalid GetRadio response")
    expected = {
        "frequency_hz": frequency_hz,
        "bandwidth_hz": bandwidth_hz,
        "sf": sf,
        "cr": cr,
    }
    if actual != expected:
        if verify_only:
            raise ConfigurationError("radio configuration changed while reopening the observer connection")
        claimed_owner = _configure_shared_radio(port, decoder, radio, pending)
        if not claimed_owner:
            reply, received = request_hardware(
                port, decoder, set_hardware(0x09, radio),
                lambda frame: frame[:2] in (b"\x06\xf0", b"\x06\xf1"),
            )
            pending.extend(received)
            if reply is None:
                raise RuntimeError("SetRadio timed out after 2 attempts")
            if reply != b"\x06\xf0":
                raise ConfigurationError(f"SetRadio failed: {decode_kiss_frame(reply).get('error', 'invalid response')}")
        reply, received = request_hardware(
            port, decoder, set_hardware(0x0B),
            lambda frame: frame[:2] in (b"\x06\x8b", b"\x06\xf1"),
        )
        pending.extend(received)
        if reply is None:
            raise RuntimeError("GetRadio verification timed out")
        actual = decode_kiss_frame(reply).get("radio")
        if actual != expected:
            raise ConfigurationError(f"radio verification failed: expected {expected}, received {actual}")

    reply, received = request_hardware(
        port,
        decoder,
        set_hardware(0x19, b"\x01"),
        lambda frame: frame[:2] in (b"\x06\x9a", b"\x06\xf1"),
    )
    pending.extend(received)
    if reply is None:
        raise RuntimeError("SetSignalReport timed out after 2 attempts")
    decoded = decode_kiss_frame(reply)
    if decoded.get("event") == "Error":
        raise RuntimeError(
            f"SetSignalReport failed: {decoded.get('error', 'unknown error')}"
        )
    if not decoded.get("signal_report_enabled"):
        raise RuntimeError("modem did not enable per-packet signal reports")

    for subcommand, response in ((0x10, 0x90), (0x12, 0x92), (0x16, 0x96)):
        reply, received = request_hardware(
            port,
            decoder,
            set_hardware(subcommand),
            lambda frame, expected=response: frame[:2]
            in (bytes((0x06, expected)), b"\x06\xf1"),
            attempts=1,
            timeout=1.0,
        )
        pending.extend(received)
        if reply is not None:
            pending.append(reply)

    return pending, claimed_owner


def emit(
    decoded: dict,
    as_json: bool,
    bandwidth_hz: int,
    sf: int,
    cr: int,
    packet_number: int | None = None,
    signal_for: int | None = None,
) -> None:
    timestamp = datetime.now().astimezone().isoformat(timespec="milliseconds")
    if decoded.get("kind") == "data":
        packet = decoded["meshcore"]
        packet["estimated_airtime_ms"] = round(
            estimate_lora_airtime_ms(packet["length"], bandwidth_hz, sf, cr), 2
        )
        if packet_number is not None:
            packet["packet_number"] = packet_number

    if decoded.get("event") == "RxMeta":
        if decoded.get("local_loopback"):
            decoded["signal_quality"] = "local-loopback"
        else:
            margin, quality = signal_quality(decoded["snr_db"], sf)
            decoded["snr_margin_db"] = margin
            decoded["signal_quality"] = quality
        if signal_for is not None:
            decoded["packet_number"] = signal_for

    record = {"timestamp": timestamp, **decoded}
    if as_json:
        print(json.dumps(record, separators=(",", ":")), flush=True)
        return

    if decoded.get("kind") == "data":
        packet = decoded["meshcore"]
        number = f" #{packet_number}" if packet_number is not None else ""
        summary = (
            f"{packet.get('route', '?')} {packet.get('payload_type', '?')} "
            f"v{packet.get('version', '?')} {packet['length']} bytes"
        )
        print(f"[{timestamp}] RX{number} {summary}")
        print(
            f"  header: {packet.get('header', '?')}; hops: {packet.get('hop_count', '?')}; "
            f"path hashes: {packet.get('path_hash_size', '?')} byte(s); "
            f"airtime: ~{packet['estimated_airtime_ms']:.1f} ms"
        )
        if "path" in packet:
            print(f"  path: {packet['path']}")
        if "transport_codes" in packet:
            print(f"  transport codes: {packet['transport_codes']}")
        if "name" in packet:
            print(
                f"  node: {packet['name']} ({packet.get('advert_type', 'unknown')}); "
                f"key: {packet.get('public_key_prefix', '?')}…"
            )
        elif "advert_type" in packet:
            print(
                f"  node type: {packet['advert_type']}; "
                f"key: {packet.get('public_key_prefix', '?')}…"
            )
        if "advert_timestamp_iso" in packet:
            print(f"  advertised: {packet['advert_timestamp_iso']}")
        if "latitude" in packet:
            print(f"  location: {packet['latitude']:.6f}, {packet['longitude']:.6f}")
        if "destination_hash" in packet:
            print(
                f"  src/dst: {packet.get('source_hash', '?')}/"
                f"{packet['destination_hash']} (encrypted)"
            )
        elif "channel_hash" in packet:
            print(f"  channel hash: {packet['channel_hash']} (encrypted group payload)")
        if "ciphertext_length" in packet:
            print(
                f"  cipher: MAC {packet['cipher_mac']}; {packet['ciphertext_length']} bytes, "
                f"{packet['cipher_blocks']} AES block(s); key required for plaintext"
            )
        if "trace_tag" in packet:
            print(
                f"  trace: tag=0x{packet['trace_tag']:08x}; auth={packet['trace_auth_code']}; "
                f"flags={packet['trace_flags']}"
            )
            print(f"  trace route: {packet['trace_route']}")
            print(f"  trace hop SNR: {packet['trace_snr_samples_db']} dB")
        if "control_subtype" in packet:
            print(
                f"  control: {packet['control_subtype']}; flags={packet['control_flags']}"
            )
            if "discovery_tag" in packet:
                print(f"  discovery tag: {packet['discovery_tag']}")
            if "node_type" in packet:
                print(
                    f"  discovered: {packet['node_type']}; "
                    f"SNR {packet['discovery_snr_db']:+.2f} dB; "
                    f"key {packet['node_public_key']}"
                )
        if "multipart_type" in packet:
            print(
                f"  multipart: {packet['multipart_type']}; "
                f"{packet['multipart_remaining']} packet(s) remaining"
            )
        if "ack_crc" in packet:
            print(f"  ack CRC: 0x{packet['ack_crc']:08x}")
        if "custom_payload_text" in packet:
            print(f"  custom text: {packet['custom_payload_text']}")
        if packet.get("error"):
            print(f"  decode error: {packet['error']}")
        print(f"  raw: {packet['raw_hex']}", flush=True)
    elif decoded.get("event") == "RxMeta":
        number = f" RX #{signal_for}" if signal_for is not None else ""
        if decoded.get("local_loopback"):
            print(f"[{timestamp}] SIGNAL{number} LOCAL LOOPBACK", flush=True)
        else:
            print(
                f"[{timestamp}] SIGNAL{number} SNR {decoded['snr_db']:+.2f} dB "
                f"(margin {decoded['snr_margin_db']:+.2f} dB, {decoded['signal_quality']}), "
                f"RSSI {decoded['rssi_dbm']} dBm",
                flush=True,
            )
    elif decoded.get("event") == "NoiseFloor":
        print(f"[{timestamp}] MODEM noise floor {decoded['noise_floor_dbm']} dBm")
    elif decoded.get("event") == "Stats":
        stats = decoded["stats"]
        print(
            f"[{timestamp}] MODEM packets RX {stats['rx']}, TX {stats['tx']}, "
            f"RX errors {stats['errors']}"
        )
    elif decoded.get("event") == "DeviceName":
        print(f"[{timestamp}] MODEM {decoded['device_name']}")
    elif decoded.get("event") not in ("OK", "SignalReport"):
        print(f"[{timestamp}] KISS {decoded}", flush=True)


def open_transport(args: argparse.Namespace):
    if args.tcp:
        return TcpTransport(args.tcp)
    return serial.Serial(
        args.port,
        115200,
        bytesize=serial.EIGHTBITS,
        parity=serial.PARITY_NONE,
        stopbits=serial.STOPBITS_ONE,
        timeout=0.1,
        xonxoff=False,
        rtscts=False,
        dsrdtr=False,
    )


def open_configured_transport(args: argparse.Namespace):
    attempts = 3 if args.tcp else 1
    for attempt in range(1, attempts + 1):
        port = None
        decoder = KissDecoder()
        try:
            port = open_transport(args)
            if not args.tcp:
                time.sleep(args.startup_delay)
            port.reset_input_buffer()
            pending, claimed_owner = configure_modem(port, decoder, args.freq, args.bw, args.sf, args.cr)
            if claimed_owner:
                port.close()
                port = open_transport(args)
                decoder = KissDecoder()
                if not args.tcp:
                    time.sleep(args.startup_delay)
                port.reset_input_buffer()
                received, _ = configure_modem(
                    port, decoder, args.freq, args.bw, args.sf, args.cr, verify_only=True,
                )
                pending.extend(received)
            return port, decoder, pending
        except ConfigurationError:
            if port is not None:
                port.close()
            raise
        except (OSError, RuntimeError):
            if port is not None:
                port.close()
            if attempt == attempts:
                raise
            time.sleep(0.5)
    raise RuntimeError("could not configure KISS transport")


def monitor(args: argparse.Namespace) -> None:
    endpoint = f"tcp://{args.tcp}" if args.tcp else args.port
    port, decoder, pending = open_configured_transport(args)
    packet_number = 0
    signal_queue = []

    def output(frame: bytes) -> None:
        nonlocal packet_number
        decoded = decode_kiss_frame(frame)
        current_packet = None
        signal_for = None
        if decoded.get("kind") == "data":
            packet_number += 1
            current_packet = packet_number
            signal_queue.append(packet_number)
        elif decoded.get("event") == "RxMeta" and signal_queue:
            signal_for = signal_queue.pop(0)
        emit(
            decoded,
            args.json,
            args.bw,
            args.sf,
            args.cr,
            packet_number=current_packet,
            signal_for=signal_for,
        )

    try:
        print(
            f"Listening on {endpoint}: {args.freq / 1_000_000:.3f} MHz, "
            f"BW {args.bw / 1000:g} kHz, SF{args.sf}, CR 4/{args.cr}",
            file=sys.stderr,
        )
        for frame in pending:
            output(frame)
        while True:
            for frame in decoder.feed(port.read(256)):
                output(frame)
    finally:
        port.close()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    endpoint = parser.add_mutually_exclusive_group()
    endpoint.add_argument("--port", default="/dev/ttyACM0", help="serial device")
    endpoint.add_argument(
        "--tcp",
        metavar="HOST[:PORT]",
        help=f"KISS TCP endpoint (port defaults to {DEFAULT_TCP_PORT})",
    )
    parser.add_argument("--freq", type=int, default=910_525_000, help="frequency in Hz")
    parser.add_argument("--bw", type=int, default=62_500, help="bandwidth in Hz")
    parser.add_argument("--sf", type=int, choices=range(5, 13), default=7)
    parser.add_argument("--cr", type=int, choices=range(5, 9), default=5)
    parser.add_argument(
        "--startup-delay",
        type=float,
        default=1.5,
        help="seconds to wait after opening the USB serial port",
    )
    parser.add_argument("--json", action="store_true", help="emit JSON Lines")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        monitor(args)
    except KeyboardInterrupt:
        print("Stopped.", file=sys.stderr)
        return 0
    except (OSError, RuntimeError, ValueError, serial.SerialException) as error:
        print(f"meshcore-kiss-monitor: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
