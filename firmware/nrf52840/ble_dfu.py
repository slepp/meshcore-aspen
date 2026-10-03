#!/usr/bin/env python3
"""Run the pinned ecosystem Legacy DFU library against explicitly identified Pine addresses."""
import argparse
import asyncio
import hashlib
import importlib.util
import logging
from pathlib import Path
import struct

from ble_field import DFU_REVISION, validate_package

TOOL_REVISION = "409e4b75d55c8d75859022217dfba0db33730f16"
LIBRARY_SHA256 = "90da46edd370ca9a893aa1ee18585fd814b50e088af4877772351f3eaf34b8a3"


def load_tool(directory):
    path = Path(directory) / "dfu_lib.py"
    if not path.is_file():
        raise ValueError(f"Missing {path}; activate recrof/nrf_dfu_py revision {TOOL_REVISION}")
    if hashlib.sha256(path.read_bytes()).hexdigest() != LIBRARY_SHA256:
        raise ValueError("Legacy DFU library differs from the reviewed revision; refuse to import it")
    spec = importlib.util.spec_from_file_location("pine_ecosystem_dfu", path)
    library = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(library)
    return library


async def exact_device(library, address, adapter):
    device = await library.find_device_by_name_or_address(
        address, force_scan=True, adapter=adapter)
    if device.address.upper() != address.upper():
        raise ValueError("DFU discovery returned another device; refusing to connect")
    return device


async def update(args, library=None):
    validate_package(args.zip)
    if not args.boot_only and not args.app_address:
        raise ValueError("Application entry requires --app-address; bootloader retry uses --boot-only")
    library = library or load_tool(args.tool_dir)
    dfu = library.NordicLegacyDFU(
        str(args.zip), prn=8, packet_delay=0.6, adapter=args.adapter, high_mtu=False)
    dfu.parse_zip()
    if dfu.upload_mode != 4:
        raise ValueError("Ecosystem library did not select application-only DFU")
    original_mtu = dfu._setup_mtu
    verify_boot = False

    async def checked_mtu():
        if verify_boot:
            revision = bytes(await dfu.client.read_gatt_char(DFU_REVISION))
            if revision != struct.pack("<H", 8):
                raise ValueError("Selected address is not a Legacy DFU bootloader (revision 0x0008); no image sent")
        return await original_mtu()

    # Check inside the library's transfer connection, not by opening/closing a
    # separate connection that could make the bootloader exit before transfer.
    dfu._setup_mtu = checked_mtu
    if not args.boot_only:
        app = await exact_device(library, args.app_address, args.adapter)
        await dfu.jump_to_bootloader(app)
        await asyncio.sleep(5)
    boot = await exact_device(library, args.boot_address, args.adapter)
    verify_boot = True
    await dfu.perform_update(boot, max_retries=1)
    print("Legacy DFU transfer finished; verify Pine firmware, identities, source, caller data and RF before acceptance")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--zip", type=Path, required=True, help="Validated Pine app-only firmware.zip")
    parser.add_argument("--adapter", required=True, help="Explicit Linux controller, e.g. hci1")
    parser.add_argument("--app-address", help="Previously verified Pine application BLE address")
    parser.add_argument("--boot-address", required=True, help="Previously verified Pine bootloader BLE address")
    parser.add_argument("--boot-only", action="store_true", help="Retry an already entered bootloader; skip application jump")
    parser.add_argument("--tool-dir", type=Path,
                        default=Path(__file__).resolve().parent / ".build/nrf-dfu-py")
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO)
    asyncio.run(update(args))


if __name__ == "__main__":
    main()
