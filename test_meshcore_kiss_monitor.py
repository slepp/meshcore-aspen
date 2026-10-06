import argparse
import socket
import struct
import threading
import unittest
from unittest.mock import patch

from meshcore_kiss_monitor import (
    FEND,
    FESC,
    KissDecoder,
    ConfigurationError,
    TcpTransport,
    configure_modem,
    split_tcp_endpoint,
    decode_kiss_frame,
    estimate_lora_airtime_ms,
    kiss_frame,
    open_configured_transport,
    read_until,
    request_hardware,
    signal_quality,
)


class KissMonitorTests(unittest.TestCase):
    def configure_with_replies(self, replies):
        test = self

        class Port:
            def __init__(self):
                self.replies = list(replies)
                self.data = b""

            def write(self, wire):
                expected, reply = self.replies.pop(0)
                test.assertEqual(KissDecoder().feed(wire), [expected])
                self.data += reply

            def flush(self):
                pass

            def read(self, _size):
                data, self.data = self.data, b""
                return data

        port = Port()
        pending, _ = configure_modem(port, KissDecoder(), 910_525_000, 62_500, 7, 5)
        self.assertEqual(port.replies, [])
        return pending

    def monitor_tail(self):
        return [
            (b"\x06\x19\x01", kiss_frame(6, b"\x9a\x01")),
            (b"\x06\x10", kiss_frame(6, b"\x90" + struct.pack("<h", -95))),
            (b"\x06\x12", kiss_frame(6, b"\x92" + struct.pack("<III", 1, 2, 0))),
            (b"\x06\x16", kiss_frame(6, b"\x96modem")),
        ]

    def test_matching_radio_is_observed_without_ownership_or_retuning(self):
        radio = struct.pack("<IIBB", 910_525_000, 62_500, 7, 5)
        pending = self.configure_with_replies([
            (b"\x06\x0b", kiss_frame(0, b"observed") + kiss_frame(6, b"\x8b" + radio)),
            *self.monitor_tail(),
        ])
        self.assertIn(b"\x00observed", pending)

    def test_stock_modem_configuration_remains_supported(self):
        old = struct.pack("<IIBB", 915_000_000, 125_000, 7, 5)
        radio = struct.pack("<IIBB", 910_525_000, 62_500, 7, 5)
        self.configure_with_replies([
            (b"\x06\x0b", kiss_frame(6, b"\x8b" + old)),
            (b"\x06\x20\x01\x01", kiss_frame(6, b"\xf1\x05")),
            (b"\x06\x09" + radio, kiss_frame(6, b"\xf0")),
            (b"\x06\x0b", kiss_frame(6, b"\x8b" + radio)),
            *self.monitor_tail(),
        ])

    def test_shared_modem_change_preserves_power_budget_and_carrier_policy(self):
        old = struct.pack("<IIBB", 912_525_000, 250_000, 7, 5)
        radio = struct.pack("<IIBB", 910_525_000, 62_500, 7, 5)
        tail = struct.pack("<BfBh", 2, 99.25, 1, 8)
        self.configure_with_replies([
            (b"\x06\x0b", kiss_frame(6, b"\x8b" + old)),
            (b"\x06\x20\x01\x01", kiss_frame(6, b"\xa0\x01\x00" + struct.pack("<II", 7, 3) + b"\x10\x01")),
            (b"\x06\x22\x01\x00", kiss_frame(6, b"\xa2\x01\x00" + struct.pack("<I", 3) + old + tail)),
            (b"\x06\x22\x01\x01" + struct.pack("<I", 3) + radio + tail,
             kiss_frame(6, b"\xa2\x01\x00" + struct.pack("<I", 4) + radio + tail)),
            (b"\x06\x0b", kiss_frame(6, b"\x8b" + radio)),
            *self.monitor_tail(),
        ])

    def test_shared_owner_refusal_does_not_send_configuration(self):
        old = struct.pack("<IIBB", 912_525_000, 250_000, 7, 5)
        with self.assertRaisesRegex(ConfigurationError, "owned by another client"):
            self.configure_with_replies([
                (b"\x06\x0b", kiss_frame(6, b"\x8b" + old)),
                (b"\x06\x20\x01\x01", kiss_frame(6, b"\xa0\x01\x04" + bytes(10))),
            ])

    def test_stream_decoder_handles_escaping_and_chunking(self):
        wire = kiss_frame(0, bytes((1, FEND, 2, FESC, 3)))
        decoder = KissDecoder()

        self.assertEqual(decoder.feed(wire[:4]), [])
        self.assertEqual(decoder.feed(wire[4:]), [bytes((0, 1, FEND, 2, FESC, 3))])

    def test_meshcore_packet_header_and_path(self):
        # v1, flood, text-message, two one-byte path hashes, encrypted envelope.
        packet = bytes.fromhex("0902aabb10203412deadbeef")
        result = decode_kiss_frame(bytes((0,)) + packet)["meshcore"]

        self.assertEqual(result["version"], 1)
        self.assertEqual(result["route"], "flood")
        self.assertEqual(result["payload_type"], "text-message")
        self.assertEqual(result["path"], ["aa", "bb"])
        self.assertEqual(result["destination_hash"], "10")
        self.assertEqual(result["source_hash"], "20")
        self.assertEqual(result["cipher_mac"], "3412")
        self.assertEqual(result["ciphertext_hex"], "deadbeef")

    def test_sample_group_packet_decodes_encrypted_envelope(self):
        packet = bytes.fromhex(
            "154011f93b91b53ed8b488868e1ca65af86d2358d7a17a8f6423775fc73466ee"
            "436d49f06d3c72d20d35f617466da9d69faea1a393344fa5678f927aff0e9341"
            "3d474194d488181039265e941997ca1aaf25adcf738dc3b85a368a0246e3496ac"
            "2c7362a0499e6886681ff5d3dc6249b351ead2c0edbb3af80db92188822d4b861"
            "d3d0a87fbcc40befdb90ceeac7513a4a802cb44d"
        )
        result = decode_kiss_frame(b"\x00" + packet)["meshcore"]

        self.assertEqual(result["header"], "0x15")
        self.assertEqual(result["route"], "flood")
        self.assertEqual(result["payload_type"], "group-text")
        self.assertEqual(result["path_metadata"], "0x40")
        self.assertEqual(result["hop_count"], 0)
        self.assertEqual(result["path_hash_size"], 2)
        self.assertEqual(result["channel_hash"], "11")
        self.assertEqual(result["cipher_mac"], "f93b")
        self.assertEqual(result["ciphertext_length"], 144)
        self.assertEqual(result["cipher_blocks"], 9)

    def test_trace_separates_route_hashes_from_hop_snr(self):
        packet = bytes.fromhex("2602f820010203040506070800aabb")
        result = decode_kiss_frame(b"\x00" + packet)["meshcore"]

        self.assertEqual(result["trace_tag"], 0x04030201)
        self.assertEqual(result["trace_auth_code"], "0x08070605")
        self.assertEqual(result["trace_route"], ["aa", "bb"])
        self.assertEqual(result["trace_snr_samples_db"], [-2.0, 8.0])
        self.assertNotIn("path", result)

    def test_discovery_response_decodes_node_and_signal(self):
        packet = bytes.fromhex("2e0093f8785634120102030405060708")
        result = decode_kiss_frame(b"\x00" + packet)["meshcore"]

        self.assertEqual(result["control_subtype"], "discover-response")
        self.assertEqual(result["node_type"], "room-server")
        self.assertEqual(result["discovery_snr_db"], -2.0)
        self.assertEqual(result["discovery_tag"], "0x12345678")
        self.assertEqual(result["node_public_key"], "0102030405060708")

    def test_rx_metadata_and_link_budget(self):
        result = decode_kiss_frame(bytes((0x06, 0xF9, 0xF8, 0x9C)))
        margin, quality = signal_quality(result["snr_db"], 7)

        self.assertEqual(result["snr_db"], -2.0)
        self.assertEqual(result["rssi_dbm"], -100)
        self.assertEqual(margin, 5.5)
        self.assertEqual(quality, "good")

    def test_reserved_rx_metadata_marks_local_loopback(self):
        result = decode_kiss_frame(bytes((0x06, 0xF9, 0x80, 0x7F)))

        self.assertTrue(result["local_loopback"])
        self.assertEqual(result["snr_db"], -32.0)
        self.assertEqual(result["rssi_dbm"], 127)

    def test_lora_airtime_for_sample_packet(self):
        airtime = estimate_lora_airtime_ms(149, 62_500, 7, 5)

        self.assertAlmostEqual(airtime, 131.584, places=3)

    def test_hardware_status_responses(self):
        noise = decode_kiss_frame(bytes((0x06, 0x90, 0x85, 0xFF)))
        stats = decode_kiss_frame(
            bytes((0x06, 0x92)) + struct.pack("<III", 12, 3, 1)
        )

        self.assertEqual(noise["noise_floor_dbm"], -123)
        self.assertEqual(stats["stats"], {"rx": 12, "tx": 3, "errors": 1})

    def test_tcp_transport_round_trip(self):
        listener = socket.socket()
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        host, port = listener.getsockname()

        def serve():
            connection, _ = listener.accept()
            with connection:
                self.assertEqual(connection.recv(4), b"ping")
                connection.sendall(b"pong")
            listener.close()

        server = threading.Thread(target=serve)
        server.start()
        with TcpTransport(f"{host}:{port}") as transport:
            self.assertEqual(transport.write(b"ping"), 4)
            self.assertEqual(transport.read(4), b"pong")
        server.join(timeout=1)
        self.assertFalse(server.is_alive())

    def test_tcp_endpoint_defaults_to_port_8001(self):
        self.assertEqual(split_tcp_endpoint("10.0.0.5"), ("10.0.0.5", 8001))
        self.assertEqual(split_tcp_endpoint("10.0.0.5:9000"), ("10.0.0.5", 9000))
        with self.assertRaises(ValueError):
            split_tcp_endpoint(":8001")

    def test_read_until_preserves_frames_after_reply(self):
        class Port:
            def __init__(self):
                self.data = kiss_frame(0x06, b"\x8b") + kiss_frame(0, b"packet")

            def read(self, _size):
                data, self.data = self.data, b""
                return data

        reply, pending = read_until(
            Port(), KissDecoder(), lambda frame: frame[:2] == b"\x06\x8b", timeout=0.01
        )

        self.assertEqual(reply, b"\x06\x8b")
        self.assertEqual(pending, [b"\x00packet"])

    def test_hardware_request_retries_after_timeout(self):
        class Port:
            def __init__(self):
                self.writes = 0
                self.sent_reply = False

            def write(self, _data):
                self.writes += 1

            def flush(self):
                pass

            def read(self, _size):
                if self.writes >= 2 and not self.sent_reply:
                    self.sent_reply = True
                    return kiss_frame(0x06, b"\x8b")
                return b""

        port = Port()
        reply, pending = request_hardware(
            port,
            KissDecoder(),
            kiss_frame(0x06, b"\x0b"),
            lambda frame: frame[:2] == b"\x06\x8b",
            timeout=0.001,
        )

        self.assertEqual(reply, b"\x06\x8b")
        self.assertEqual(pending, [])
        self.assertEqual(port.writes, 2)

    def test_monitor_releases_configuration_lease_before_listening(self):
        class Port:
            closed = False

            def reset_input_buffer(self):
                pass

            def close(self):
                self.closed = True

        owner, observer = Port(), Port()
        args = argparse.Namespace(
            tcp="radio.local", port="/dev/ttyACM0", startup_delay=0,
            freq=910_525_000, bw=62_500, sf=7, cr=5,
        )
        with (
            patch("meshcore_kiss_monitor.open_transport", side_effect=[owner, observer]),
            patch("meshcore_kiss_monitor.configure_modem", side_effect=[
                ([b"\x00before"], True), ([b"\x00after"], False),
            ]) as configure,
        ):
            port, _decoder, pending = open_configured_transport(args)
        self.assertTrue(owner.closed)
        self.assertFalse(observer.closed)
        self.assertIs(port, observer)
        self.assertEqual(pending, [b"\x00before", b"\x00after"])
        self.assertTrue(configure.call_args.kwargs["verify_only"])

    def test_tcp_configuration_reconnects_after_failed_handshake(self):
        class Port:
            def __init__(self):
                self.closed = False

            def reset_input_buffer(self):
                pass

            def close(self):
                self.closed = True

        first = Port()
        second = Port()
        args = argparse.Namespace(
            tcp="radio.local",
            port="/dev/ttyACM0",
            startup_delay=0,
            freq=910_525_000,
            bw=62_500,
            sf=7,
            cr=5,
        )
        with (
            patch("meshcore_kiss_monitor.open_transport", side_effect=[first, second]),
            patch(
                "meshcore_kiss_monitor.configure_modem",
                side_effect=[RuntimeError("timeout"), ([], False)],
            ),
            patch("meshcore_kiss_monitor.time.sleep"),
        ):
            port, _decoder, pending = open_configured_transport(args)

        self.assertTrue(first.closed)
        self.assertIs(port, second)
        self.assertEqual(pending, [])


if __name__ == "__main__":
    unittest.main()
