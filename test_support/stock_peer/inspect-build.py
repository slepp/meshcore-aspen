#!/usr/bin/env python3
"""Check the actual build flags and erase bounds before publishing an image."""
import json
import pathlib
import shlex
import struct
import sys

tree, environment = pathlib.Path(sys.argv[1]).resolve(), sys.argv[2]
expected = {
    "LORA_FREQ": "912.525", "LORA_BW": "250", "LORA_SF": "7",
    "LORA_CR": "5", "LORA_TX_POWER": "2", "ENABLE_ADVERT_ON_BOOT": "0",
    "ARDUINO_USB_MODE": "1", "ARDUINO_USB_CDC_ON_BOOT": "1",
    "P_LORA_NSS": "41", "P_LORA_DIO_1": "39", "P_LORA_RESET": "42",
    "P_LORA_BUSY": "40", "P_LORA_SCLK": "7", "P_LORA_MISO": "8", "P_LORA_MOSI": "9",
    "SX126X_RXEN": "38", "SX126X_TXEN": "RADIOLIB_NC",
    "SX126X_DIO2_AS_RF_SWITCH": "true", "SX126X_DIO3_TCXO_VOLTAGE": "1.8",
    "RADIO_CLASS": "CustomSX1262", "WRAPPER_CLASS": "CustomSX1262Wrapper",
}
commands = json.loads((tree / "compile_commands.json").read_text())
checked = []
for suffix in ("examples/companion_radio/main.cpp", "examples/companion_radio/MyMesh.cpp",
               "variants/xiao_s3_wio/target.cpp"):
    matches = [c for c in commands if pathlib.Path(c["file"]).as_posix().endswith(suffix)]
    if len(matches) != 1:
        raise SystemExit(f"Expected exactly one compiler command for {suffix}")
    command = matches[0]
    args = command.get("arguments") or shlex.split(command["command"])
    defines = []
    for index, arg in enumerate(args):
        if arg == "-D":
            defines.append(args[index + 1])
        elif arg.startswith("-D"):
            defines.append(arg[2:])
    for key, value in expected.items():
        actual = [d.partition("=")[2] for d in defines if d.partition("=")[0] == key]
        if actual != [value]:
            raise SystemExit(f"{suffix}: expected one {key}={value}, found {actual}")
    if not any(d.partition("=")[0] == "ENABLE_USB_INTERFACE" for d in defines):
        raise SystemExit(f"{suffix}: USB companion interface missing")
    checked.append(suffix)

data = (tree / ".pio/build" / environment / "partitions.bin").read_bytes()
partitions = []
for offset in range(0, len(data), 32):
    magic, kind, subtype, address, size, label, flags = struct.unpack("<HBBII16sI", data[offset:offset+32])
    if magic != 0x50aa:
        break
    partitions.append({"name": label.rstrip(b"\0").decode(), "type": kind, "subtype": subtype,
                       "offset": hex(address), "size": hex(size), "flags": flags})
spiffs = [p for p in partitions if p["name"] == "spiffs"]
if len(spiffs) != 1 or (spiffs[0]["type"], spiffs[0]["subtype"], spiffs[0]["offset"], spiffs[0]["size"]) != (
        1, 0x82, "0x670000", "0x180000"):
    raise SystemExit("SPIFFS bounds changed; refusing to publish hard-coded erase instructions")
print(json.dumps({"environment": environment, "defines": expected, "translation_units": checked,
                  "partitions": partitions, "erase_region": spiffs[0],
                  "scope": "Offline build configuration checks only; no device accessed."}, indent=2))
