#!/usr/bin/env python3
"""Validate Pine's app-only Nordic legacy DFU ZIP; inspect BLE or refresh standard companion time."""
import argparse
import asyncio
import binascii
import hashlib
import json
from pathlib import Path
import struct
import time
import zipfile

DFU_SERVICE = "00001530-1212-efde-1523-785feabcd123"
DFU_REVISION = "00001534-1212-efde-1523-785feabcd123"
UART_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"
UART_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
APP_START, APP_LIMIT = 0x27000, 0xED000


def validate_package(path):
    with zipfile.ZipFile(path) as archive:
        names = archive.namelist()
        if len(names) != len(set(names)) or set(names) != {
                "firmware.bin", "firmware.dat", "manifest.json"}:
            raise ValueError("ZIP must contain only firmware.bin, firmware.dat and manifest.json")
        manifest = json.loads(archive.read("manifest.json"))["manifest"]
        if set(manifest) != {"application", "dfu_version"} or manifest["dfu_version"] != 0.5:
            raise ValueError("Use the Adafruit Nordic legacy app-only ZIP, not Secure DFU/fullflash")
        application = manifest["application"]
        if application["bin_file"] != "firmware.bin" or application["dat_file"] != "firmware.dat":
            raise ValueError("Unexpected application file names")
        binary, init = archive.read("firmware.bin"), archive.read("firmware.dat")
        if len(binary) < 8 or len(binary) % 4 or len(binary) > APP_LIMIT - APP_START:
            raise ValueError("Application size crosses reserved bootloader/InternalFS boundary")
        stack, reset = struct.unpack_from("<II", binary)
        if not (0x20000000 < stack <= 0x20040000 and stack % 8 == 0 and
                reset & 1 and APP_START <= reset - 1 < APP_START + len(binary)):
            raise ValueError("BIN vector table is not an nRF52840 application linked at 0x27000")
        if len(init) != 14:
            raise ValueError("Unexpected Nordic legacy init packet")
        device, revision, version, count, softdevice, checksum = struct.unpack("<HHIHHH", init)
        crc = binascii.crc_hqx(binary, 0xFFFF)
        expected = dict(device_type=device, device_revision=revision,
                        application_version=version, softdevice_req=[softdevice], firmware_crc16=crc)
        if count != 1 or device != 0x52 or softdevice != 0x123 or checksum != crc:
            raise ValueError("Init packet device/SoftDevice/CRC does not match Pine application")
        if application["init_packet_data"] != expected:
            raise ValueError("Manifest and init packet disagree")
        return {"format": "Nordic legacy DFU 0.5 application", "bytes": len(binary),
                "application_sha256": hashlib.sha256(binary).hexdigest(),
                "softdevice_id": softdevice, "signed": False,
                "storage_end_exclusive": hex(APP_START + len(binary))}


def time_frame(epoch):
    if not 1715770351 <= epoch <= 4102444800:
        raise ValueError("UTC must be UNIX seconds 1715770351..4102444800")
    return struct.pack("<BI", 6, epoch)  # MeshCore companion v13 CMD_SET_DEVICE_TIME


async def ble_operation(args):
    try:
        from bleak import BleakClient
    except ImportError as exc:
        raise RuntimeError("BLE inspection/time requires bleak; install it in your chosen host environment") from exc
    # Never scan, select by name, switch controllers or arm/enter DFU.
    async with BleakClient(args.address, adapter=args.adapter, timeout=20) as client:
        if args.operation == "inspect":
            service = client.services.get_service(DFU_SERVICE)
            if not service:
                raise RuntimeError("Selected address has no Nordic legacy DFU service; verify Pine address/image")
            revision = struct.unpack("<H", await client.read_gatt_char(DFU_REVISION))[0]
            print(json.dumps({"address": args.address, "dfu_uuid": DFU_SERVICE,
                              "revision": revision, "mode": "application" if revision == 1 else "bootloader"}))
            return
        replies = asyncio.Queue()
        await client.start_notify(UART_TX, lambda _, data: replies.put_nowait(bytes(data)))
        while True:
            await client.write_gatt_char(UART_RX, time_frame(int(time.time())), response=False)
            reply = await asyncio.wait_for(replies.get(), timeout=10)
            if reply != b"\x00":  # RESP_CODE_OK
                raise RuntimeError("Pine refused UTC; check host UTC/PIN; use authenticated bot time for a >300s correction")
            print("Pine companion UTC refreshed; trusted Lua deadlines valid for 1h", flush=True)
            if not args.repeat:
                return
            await asyncio.sleep(1800)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("operation", choices=("package", "inspect", "time"))
    parser.add_argument("--zip", type=Path)
    parser.add_argument("--address", help="Explicit, previously verified Pine BLE address")
    parser.add_argument("--adapter", help="Explicit Linux controller, e.g. hci0")
    parser.add_argument("--repeat", action="store_true", help="Refresh UTC every 30min on this connection")
    args = parser.parse_args()
    if args.operation == "package":
        if not args.zip:
            parser.error("package requires --zip")
        print(json.dumps(validate_package(args.zip), indent=2))
    else:
        if not args.address or not args.adapter:
            parser.error("BLE operations require explicit --address and --adapter")
        asyncio.run(ble_operation(args))


if __name__ == "__main__":
    main()
