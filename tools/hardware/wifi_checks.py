#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Make-driven mast WiFi smoke checks; LAN provisioning never configures a host AP."""
import argparse
import hashlib
import ipaddress
import json
import os
import secrets
import shlex
import signal
import socket
import struct
import subprocess
import time
import urllib.request
import uuid

import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.hardware.esp32_device import DIRECTORY as MAST, ROOT, status
from tools.hardware.admin import NativeClient, WebClient, checked, private_file, wifi_field_command

from tools.hardware.inventory import value as inventory_value

DIRECTORY = ROOT / ".tmp/onchip-beta-wifi"
CREDS = DIRECTORY / "credentials.json"
STATE = DIRECTORY / "network-state.json"
def wifi_interface():
    return inventory_value("wifi_interface")
SUBNET = ipaddress.ip_network("10.77.77.0/24")
HOST = "10.77.77.1"
TABLE = "meshcore_beta_wifi"
NM = "org.freedesktop.NetworkManager"
NMPATH = "/org/freedesktop/NetworkManager"
SUP = "fi.w1.wpa_supplicant1"
SUPPATH = "/fi/w1/wpa_supplicant1"


def write(path, value):
    data = value if isinstance(value, bytes) else (json.dumps(value, indent=2) + "\n").encode()
    temporary = path.with_suffix(path.suffix + ".new")
    fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, "wb") as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())
        if os.geteuid() == 0 and "SUDO_UID" in os.environ:
            os.fchown(stream.fileno(), int(os.environ["SUDO_UID"]), int(os.environ["SUDO_GID"]))
    os.replace(temporary, path)


def load(path):
    return json.loads(private_file(path, 65536))


def run(args, **kwargs):
    return subprocess.run(args, capture_output=True, text=True, check=True, **kwargs)


def default_routes():
    routes = json.loads(run(["ip", "-j", "route", "show", "default"]).stdout)
    return [{k: v for k, v in route.items() if k in ("dst", "gateway", "dev", "metric", "table")}
            for route in routes]


def credentials():
    return load(CREDS)


def prepare():
    DIRECTORY.mkdir(mode=0o700, exist_ok=True)
    if CREDS.exists() or STATE.exists():
        raise ValueError("Existing WiFi lab state must be cleaned up first")
    if DIRECTORY.stat().st_mode & 0o077:
        raise ValueError("WiFi lab directory must be private")
    for route in json.loads(run(["ip", "-j", "route"]).stdout):
        destination = route.get("dst", "default")
        if destination != "default" and SUBNET.overlaps(ipaddress.ip_network(destination, strict=False)):
            raise ValueError("Lab subnet overlaps an existing route")
    gateway = status(inventory_value("gateway_host"))
    if gateway["kiss"]["connected"] or gateway["scheduler"]["queued"]:
        raise ValueError("Isolated gateway has active clients/jobs")
    for key, device in (("mast_port", "ttyACM1"), ("companion_port", "ttyACM2")):
        path = Path(inventory_value(key))
        if not path.is_symlink() or path.resolve().name != device:
            raise ValueError("Configured ESP32 USB mapping changed")
    write(CREDS, {"ssid": "mc-lab-" + secrets.token_hex(6), "password": secrets.token_urlsafe(24)})
    write(DIRECTORY / "dhcp.conf", (
        f"interface={wifi_interface()}\nbind-interfaces\nport=0\n"
        "dhcp-range=10.77.77.2,10.77.77.10,255.255.255.0,5m\n"
        "dhcp-option=3\ndhcp-option=6\n"
        f"dhcp-leasefile={DIRECTORY / 'leases'}\npid-file={DIRECTORY / 'dhcp.pid'}\n"
        "user=root\n").encode())
    print("Prepared private disposable AP credentials; values withheld")


def bus_interfaces():
    import dbus
    from dbus.mainloop.glib import DBusGMainLoop
    DBusGMainLoop(set_as_default=True)
    bus = dbus.SystemBus()
    manager = dbus.Interface(bus.get_object(NM, NMPATH), NM)
    device_path = manager.GetDeviceByIpIface(wifi_interface())
    device = bus.get_object(NM, device_path)
    props = dbus.Interface(device, "org.freedesktop.DBus.Properties")
    supplicant = dbus.Interface(bus.get_object(SUP, SUPPATH), "org.freedesktop.DBus.Properties")
    return dbus, bus, manager, device_path, props, supplicant


def ap_up():
    if os.geteuid() != 0:
        raise ValueError("AP setup requires the explicitly authorized network administrator")
    dbus, bus, manager, device_path, props, supplicant = bus_interfaces()
    if int(props.Get(NM + ".Device", "State")) != 30:
        raise ValueError("WiFi interface is not disconnected; refusing disruption")
    if STATE.exists():
        raise ValueError("Existing network state must be cleaned up first")
    if subprocess.run(["nft", "list", "table", "inet", TABLE],
                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0:
        raise ValueError("Lab firewall table already exists")
    level, domains = manager.GetLogging()
    state = {"routes": default_routes(), "logging": [str(level), str(domains)],
             "supplicant_logging": str(supplicant.Get(SUP, "DebugLevel")),
             "autoconnect": bool(props.Get(NM + ".Device", "Autoconnect"))}
    write(STATE, state)
    # Neither daemon may log the disposable SSID; restore both settings at cleanup.
    manager.SetLogging("OFF", "ALL")
    supplicant.Set(SUP, "DebugLevel", dbus.String("error"))
    props.Set(NM + ".Device", "Autoconnect", dbus.Boolean(False))
    rules = f"""table inet {TABLE} {{
 chain forward {{
  type filter hook forward priority -10; policy accept;
  iifname "{wifi_interface()}" counter drop
  oifname "{wifi_interface()}" counter drop
 }}
 chain input {{
  type filter hook input priority -10; policy accept;
  iifname "{wifi_interface()}" ct state established,related accept
  iifname "{wifi_interface()}" udp dport 67 accept
  iifname "{wifi_interface()}" counter drop
 }}
}}
"""
    run(["nft", "-f", "-"], input=rules)
    state["firewall"] = True
    write(STATE, state)
    secret = credentials()
    settings = dbus.Dictionary({
        "connection": dbus.Dictionary({
            "id": "meshcore-beta-wifi-lab", "uuid": str(uuid.uuid4()),
            "type": "802-11-wireless", "interface-name": wifi_interface(),
            "autoconnect": dbus.Boolean(False)}, signature="sv"),
        "802-11-wireless": dbus.Dictionary({
            "ssid": dbus.ByteArray(secret["ssid"].encode()), "mode": "ap",
            "band": "bg", "channel": dbus.UInt32(6)}, signature="sv"),
        "802-11-wireless-security": dbus.Dictionary({
            "key-mgmt": "wpa-psk", "psk": secret["password"],
            "psk-flags": dbus.UInt32(0)}, signature="sv"),
        "ipv4": dbus.Dictionary({
            "method": "manual", "never-default": dbus.Boolean(True),
            "address-data": dbus.Array([dbus.Dictionary({
                "address": HOST, "prefix": dbus.UInt32(24)}, signature="sv")],
                signature="a{sv}")}, signature="sv"),
        "ipv6": dbus.Dictionary({"method": "disabled"}, signature="sv"),
    }, signature="sa{sv}")
    settings_api = dbus.Interface(bus.get_object(NM, NMPATH + "/Settings"), NM + ".Settings")
    connection, _ = settings_api.AddConnection2(settings, dbus.UInt32(2),
                                               dbus.Dictionary({}, signature="sv"))
    state["connection"] = str(connection)
    write(STATE, state)
    from gi.repository import GLib
    transitions = []
    bus.add_signal_receiver(lambda new, old, reason: transitions.append((int(new), int(reason))),
                            signal_name="StateChanged", dbus_interface=NM + ".Device",
                            path=device_path)
    active = manager.ActivateConnection(connection, device_path, dbus.ObjectPath("/"))
    state["active"] = str(active)
    write(STATE, state)
    deadline = time.monotonic() + 30
    while int(props.Get(NM + ".Device", "State")) != 100:
        context = GLib.MainContext.default()
        while context.pending():
            context.iteration(False)
        failed = [entry for entry in transitions if entry[0] == 120]
        if failed:
            raise ValueError(f"Temporary AP activation failed; device state/reason: {failed[-1]}")
        if time.monotonic() > deadline:
            raise TimeoutError(f"Temporary AP activation did not complete; state/reason: {transitions}")
        time.sleep(.2)
    addresses = json.loads(run(["ip", "-j", "address", "show", "dev", wifi_interface()]).stdout)
    if not any(a.get("local") == HOST for item in addresses for a in item.get("addr_info", [])):
        raise ValueError("Temporary AP address not installed")
    if default_routes() != state["routes"]:
        raise ValueError("Default route changed during AP activation")
    print("Temporary AP active; forwarding blocked, ethernet default route unchanged")


def ap_down():
    if not STATE.exists():
        print("No saved AP network state")
        return
    dbus, bus, manager, device_path, props, supplicant = bus_interfaces()
    state = load(STATE)
    if "active" in state:
        active_paths = dbus.Interface(bus.get_object(NM, NMPATH),
                                     "org.freedesktop.DBus.Properties").Get(NM, "ActiveConnections")
        if state["active"] in [str(path) for path in active_paths]:
            manager.DeactivateConnection(dbus.ObjectPath(state["active"]))
    if "connection" in state:
        settings_api = dbus.Interface(bus.get_object(NM, NMPATH + "/Settings"), NM + ".Settings")
        if state["connection"] in [str(path) for path in settings_api.ListConnections()]:
            dbus.Interface(bus.get_object(NM, state["connection"]), NM + ".Settings.Connection").Delete()
    deadline = time.monotonic() + 15
    while int(props.Get(NM + ".Device", "State")) not in (20, 30):
        if time.monotonic() > deadline:
            raise TimeoutError("AP did not deactivate; daemon logging remains suppressed")
        time.sleep(.1)
    pid_path = DIRECTORY / "dhcp.pid"
    if pid_path.exists():
        pid = int(pid_path.read_text())
        process = Path(f"/proc/{pid}/cmdline")
        if process.exists():
            command = process.read_bytes()
            if b"dnsmasq" not in command or str(DIRECTORY / "dhcp.conf").encode() not in command:
                raise ValueError("DHCP PID no longer identifies this lab process")
            os.kill(pid, signal.SIGTERM)
    if state.get("firewall"):
        run(["nft", "delete", "table", "inet", TABLE])
    props.Set(NM + ".Device", "Autoconnect", dbus.Boolean(state["autoconnect"]))
    supplicant.Set(SUP, "DebugLevel", dbus.String(state["supplicant_logging"]))
    manager.SetLogging(*state["logging"])
    if default_routes() != state["routes"]:
        raise ValueError("Ethernet default route differs after AP cleanup")
    write(DIRECTORY / "network-restored.json", {"default_routes_unchanged": True,
                                               "logging_restored": True})
    STATE.unlink()
    print("Temporary AP/firewall/DHCP removed; logging and ethernet route restored")


def target():
    before = json.loads((MAST / "before.json").read_text())
    return next(role["public_key"] for role in before["roles"] if role["role"] == "management")


def native():
    return NativeClient(inventory_value("gateway_host"), 8001, MAST / "companion.seed", target(), "", 12)


def environment_file():
    return Path(os.environ.get("MESHCORE_ENV_FILE", str(ROOT / ".env"))).expanduser()


def lan_credentials():
    values = {}
    with environment_file().open(encoding="utf-8") as source:
        for line in source:
            name, separator, value = line.strip().removeprefix("export ").partition("=")
            name = name.strip()
            if not separator or name not in ("WIFI_AP", "WIFI_PASSWORD"):
                continue
            value = value.strip()
            if value.startswith(("'", '"')):
                try:
                    words = shlex.split(value, comments=True)
                except ValueError:
                    raise ValueError("Invalid quoted WiFi setting; value withheld") from None
                if len(words) != 1:
                    raise ValueError("Invalid quoted WiFi setting; value withheld")
                value = words[0]
            else:
                value = value.split(" #", 1)[0].rstrip()
            if "\0" in value or "\n" in value or "\r" in value:
                raise ValueError("Invalid WiFi setting; value withheld")
            values[name] = value.encode("utf-8")
    if set(values) != {"WIFI_AP", "WIFI_PASSWORD"} or not 1 <= len(values["WIFI_AP"]) <= 32:
        raise ValueError("Required bounded WIFI_AP/WIFI_PASSWORD settings unavailable")
    if len(values["WIFI_PASSWORD"]) > 64:
        raise ValueError("WiFi password exceeds firmware capacity")
    return values["WIFI_AP"], values["WIFI_PASSWORD"]


def lan_provision():
    if STATE.exists() or CREDS.exists():
        raise ValueError("Stop and remove the temporary AP before LAN provisioning")
    routes = default_routes()
    status(inventory_value("gateway_host"))
    ssid, password = lan_credentials()
    client = native()
    try:
        baseline = checked(client, "status")
        if "PHY=912525000,250000,7,5,2" not in baseline or "temp=0" not in baseline:
            raise ValueError("Mast is not on the permitted lab PHY")
        checked(client, wifi_field_command("ssid", ssid))
        checked(client, wifi_field_command("password", password))
        checked(client, "wifi apply")
    finally:
        client.close()
    deadline = time.monotonic() + 45
    while time.monotonic() < deadline:
        addresses = [inventory_value("mast_host")]
        try:
            resolved = socket.gethostbyname("meshcore-beta-lab.local")
            if resolved not in addresses:
                addresses.insert(0, resolved)
        except socket.gaierror:
            pass
        for address in addresses:
            try:
                data = status(address)
            except OSError:
                continue
            if not any(role.get("public_key") == target() for role in data.get("roles", [])):
                raise ValueError("LAN device does not match the isolated mast identity")
            if default_routes() != routes:
                raise ValueError("Host default route changed during LAN provisioning")
            write(DIRECTORY / "mast-online.json", {"ip": address, "network": "existing operator LAN"})
            print(f"Mast joined existing LAN at {address}; identity/912.525 MHz PHY matched; Ethernet unchanged")
            return
        time.sleep(1)
    raise TimeoutError("Mast LAN association/discovery did not complete")


def bundled_source():
    return (Path(__file__).resolve().parents[2] / "firmware/runtime/BotTypes.cpp").read_text().split('R"lua(', 1)[1].split(')lua"', 1)[0].encode()


def provision():
    client = native()
    try:
        baseline = checked(client, "status")
        source = checked(client, "source hash")
        wifi = checked(client, "wifi status")
        before_file = DIRECTORY / "mast-before.json"
        resuming = (before_file.exists() and before_file.stat().st_mtime >= CREDS.stat().st_mtime and
                    "saved=0" in load(before_file)["wifi"])
        if ("PHY=912525000,250000,7,5,2" not in baseline or "temp=0" not in baseline or
                not source.startswith("SHA256 " + hashlib.sha256(bundled_source()).hexdigest()) or
                ("saved=0" not in wifi and not resuming)):
            raise ValueError("Mast no longer matches the disposable lab baseline")
        if not resuming:
            write(before_file, {"status": baseline, "source": source, "wifi": wifi})
        secret = credentials()
        checked(client, wifi_field_command("ssid", secret["ssid"].encode()))
        checked(client, wifi_field_command("password", secret["password"].encode()))
        checked(client, "wifi apply")
        deadline = time.monotonic() + 45
        while time.monotonic() < deadline:
            lease_file = DIRECTORY / "leases"
            if lease_file.exists():
                for line in lease_file.read_text().splitlines():
                    fields = line.split()
                    if len(fields) >= 3 and fields[1].lower() == inventory_value("mast_mac"):
                        address = fields[2]
                        data = status(address)
                        if not any(role.get("public_key") == target() for role in data.get("roles", [])):
                            raise ValueError("AP peer does not report the expected mast identity")
                        write(DIRECTORY / "mast-online.json", {"ip": address, "wifi": data["wifi"]})
                        print("Mast joined isolated AP; authenticated target identity matched")
                        return
            time.sleep(1)
        raise TimeoutError("Mast did not obtain the expected private AP lease")
    finally:
        client.close()


def mast_ip():
    return load(DIRECTORY / "mast-online.json")["ip"]


def web():
    return WebClient("http://" + mast_ip(), private_file(MAST / "password", 15).decode())


def inspect():
    client = web()
    try:
        result = {command: checked(client, command) for command in
                  ("status", "wifi status", "source status", "source hash", "bot status", "job")}
        write(DIRECTORY / "inspection.json", result)
        for command, value in result.items():
            print(command + ": " + value)
    finally:
        client.close()


def recover_source():
    client = web()
    try:
        print(checked(client, "source remove"))
        time.sleep(3)
        outcome = checked(client, "source status")
        print(outcome)
        if ("durably saved and active" not in outcome or
                not checked(client, "source hash").startswith("SHA256 " + hashlib.sha256(bundled_source()).hexdigest())):
            raise ValueError("Bundled source recovery did not finish")
    finally:
        client.close()


def config_get():
    with socket.create_connection((mast_ip(), 8001), timeout=3) as connection:
        connection.sendall(b"\xc0\x06\x20\x01\x00\xc0\xc0\x06\x22\x01\x00\xc0")
        frame, escaped = bytearray(), False
        deadline = time.monotonic() + 4
        while time.monotonic() < deadline:
            incoming = connection.recv(256)
            if not incoming:
                raise ValueError("Mast disconnected before CONFIG GET")
            for byte in incoming:
                if byte == 0xc0:
                    if len(frame) == 26 and frame[:4] == b"\x06\xa2\x01\x00":
                        return {"generation": struct.unpack_from("<I", frame, 4)[0],
                                "frequency_hz": struct.unpack_from("<I", frame, 8)[0],
                                "sf": frame[16], "power": frame[18]}
                    frame, escaped = bytearray(), False
                elif escaped:
                    if byte not in (0xdc, 0xdd):
                        raise ValueError("Invalid KISS escape")
                    frame.append(0xc0 if byte == 0xdc else 0xdb)
                    escaped = False
                elif byte == 0xdb:
                    escaped = True
                elif len(frame) < 256:
                    frame.append(byte)
                else:
                    raise ValueError("KISS frame exceeded bound")
        raise TimeoutError("No physical CONFIG GET response")


def host_config():
    write(DIRECTORY / "host.json", {
        "radio_address": mast_ip() + ":8001", "phy_authority": "modem", "phy_tracking": "follow",
        "require_parity": True, "enabled_roles": [], "state_dir": "host-state",
        "radio": {"FreqHz": 912525000, "BwHz": 250000, "SF": 7, "CR": 5}, "tx_power": 2,
        "status_listen": "127.0.0.1:19087", "advert_interval_seconds": 0,
        "mqtt": {"url": "", "broker_listen": ""},
    })
    run([str(DIRECTORY / "meshcore-host"), "-config", str(DIRECTORY / "host.json"), "-check"])
    print("Disposable follow-only Go controller config validated; no host RF roles enabled")


def host_status():
    with urllib.request.urlopen("http://127.0.0.1:19087/status", timeout=3) as response:
        return json.load(response)["controller"]["shared_phy"]


def follow_test():
    deadline = time.monotonic() + 25
    while True:
        before = host_status()
        if before["valid"] and before["effective"]["spreading_factor"] == 7:
            break
        if time.monotonic() > deadline:
            raise TimeoutError("Go follow controller not ready")
        time.sleep(.5)
    if before["phy_tracking"] != "follow" or before["phy_authority"] != "modem":
        raise ValueError("Go controller is not readback-only follow mode")
    initial = config_get()
    if initial["sf"] != 7 or initial["frequency_hz"] != 912525000 or initial["power"] != 2:
        raise ValueError("Unexpected initial mast PHY")
    client = web()
    observations = []
    changed = returned = None
    try:
        accepted = checked(client, "tempradio 25 912525000 250000 8 5 2")
        deadline = time.monotonic() + 50
        while time.monotonic() < deadline:
            wire = config_get()
            host = host_status()
            observations.append({"wire": wire, "host": host})
            if wire["sf"] == 8 and wire["generation"] == initial["generation"] + 1:
                if (host["valid"] and host["effective"]["spreading_factor"] == 8 and
                        host["configuration_generation"] == wire["generation"]):
                    changed = observations[-1]
            if wire["sf"] == 7 and wire["generation"] == initial["generation"] + 2:
                if (host["valid"] and host["effective"]["spreading_factor"] == 7 and
                        host["configuration_generation"] == wire["generation"]):
                    returned = observations[-1]
                    break
            time.sleep(.7)
        if changed is None or returned is None:
            raise ValueError("Physical CONFIG GET and Go follow did not both observe change/return")
        if returned["host"]["transitions"] < before["transitions"] + 2:
            raise ValueError("Go host did not record both PHY transitions")
        final = checked(client, "status")
        if "temp=0" not in final or "PHY=912525000,250000,7,5,2" not in final:
            raise ValueError("Mast failed to restore baseline")
        write(DIRECTORY / "follow-results.json", {
            "accepted": accepted, "initial": initial, "changed": changed,
            "returned": returned, "final": final, "observations": observations})
        print("PASS physical TCP CONFIG GET + Go phy_tracking=follow: SF7 -> SF8 -> SF7, generation +1/+2")
    finally:
        client.close()


def restore_mast():
    if not (DIRECTORY / "mast-before.json").exists():
        print("Mast was not provisioned by this run")
        return
    client = native()
    try:
        current = checked(client, "status")
        if "PHY=912525000,250000,7,5,2" not in current or "temp=0" not in current:
            raise ValueError("Mast PHY has not returned to baseline; inspect before cleanup")
        expected = load(DIRECTORY / "mast-before.json")["source"].split()[1]
        checked(client, "wifi forget")
        checked(client, "reboot")
    finally:
        client.close()
    time.sleep(6)
    client = native()
    try:
        if not checked(client, "source hash").startswith("SHA256 " + expected):
            if expected != hashlib.sha256(bundled_source()).hexdigest():
                raise ValueError("Baseline source is not the known bundled recovery target")
            checked(client, "source remove")
            for _ in range(20):
                time.sleep(.5)
                outcome = checked(client, "source status")
                if "Error:" in outcome:
                    raise ValueError(outcome)
                if "durably saved and active" in outcome:
                    break
            else:
                raise ValueError("Bundled source restoration did not finish")
        final = {"status": checked(client, "status"), "wifi": checked(client, "wifi status"),
                 "source": checked(client, "source hash"), "bot": checked(client, "bot status")}
        if ("PHY=912525000,250000,7,5,2" not in final["status"] or
                "saved=0" not in final["wifi"] or
                not final["source"].startswith("SHA256 " + expected) or "ready=1" not in final["bot"]):
            raise ValueError("Mast restoration readback mismatch")
        write(DIRECTORY / "mast-restored.json", final)
        print("Mast baseline/source restored; disposable WiFi override removed across reboot")
    finally:
        client.close()


def remove_credentials():
    if STATE.exists():
        raise ValueError("AP cleanup must finish before deleting credentials")
    if (DIRECTORY / "mast-before.json").exists() and not (DIRECTORY / "mast-restored.json").exists():
        raise ValueError("Mast cleanup must finish before deleting credentials")
    for name in ("credentials.json", "dhcp.conf", "leases", "dhcp.pid"):
        path = DIRECTORY / name
        if path.exists():
            path.unlink()
    print("Temporary AP credential and DHCP files removed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    actions = {"prepare": prepare, "ap-up": ap_up, "ap-down": ap_down, "provision": provision,
               "lan-provision": lan_provision,
               "config-get": lambda: print(json.dumps(config_get())),
               "host-config": host_config, "restore-mast": restore_mast,
               "remove-credentials": remove_credentials, "follow-test": follow_test, "inspect": inspect,
               "recover-source": recover_source}
    parser.add_argument("action", choices=actions)
    args = parser.parse_args()
    os.umask(0o077)
    import dbus
    try:
        actions[args.action]()
    except (OSError, ValueError, subprocess.CalledProcessError, dbus.DBusException) as error:
        message = str(error)
        if CREDS.exists():
            for value in credentials().values():
                message = message.replace(value, "[redacted]").replace(value.encode().hex(), "[redacted]")
        parser.exit(2, f"WiFi lab {args.action} failed: {message}\n")


if __name__ == "__main__":
    main()
