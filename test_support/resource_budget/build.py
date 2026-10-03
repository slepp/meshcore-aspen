#!/usr/bin/env python3
"""Build existing ESP32 profiles offline from pinned, public inputs."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
BASELINE = "45f6f4ffd5574107f1f353344f15c3c8bf220352"
UPSTREAM = "d92964352441e53b93e8667b802e04f6e072b39e"
EPOCH = "1790798400"
LUA_SHA = "1c4b4068d67061f2a2231ad2b5422e77acea1487ea9890f6320af614f4373dce"
WAMR_SHA = "c197d6d811c23b3df0446e91abffba1091caf282e026156d19ace8ba2fc83f3e"
PROFILES = {
    "modem": ("Xiao_S3_WIO_kiss_wifi", 0),
    "native": ("Xiao_S3_WIO_onchip", 0),
    "lua": ("Xiao_S3_WIO_onchip_bot", 0),
    "wasm": ("Xiao_S3_WIO_onchip_bot", 1),
    "admin": ("Xiao_S3_WIO_onchip_beta", 0),
    "https": ("Xiao_S3_WIO_onchip_https", 0),
    "https-wasm": ("Xiao_S3_WIO_onchip_https", 1),
}
PUBLIC = {
    "MESHCORE_HOSTNAME": "resource-build",
    "WIFI_SSID": "build-only",
    "WIFI_PWD": "not-a-secret",
    "ONCHIP_ADMIN_PASSWORD": "build-only",
    "ONCHIP_ROOM_PASSWORD": "",
    "ONCHIP_MQTT_URI": "",
    "ONCHIP_MAST_PASSWORD": "build-only",
    "ONCHIP_OPERATOR_PUBKEY": "",
    "ONCHIP_TRUSTED_COMPANION_PUBKEY": "",
    "ONCHIP_SERVICE_REGION": "",
}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def dependency_inputs(deps):
    result = {}
    for library in sorted(p for p in deps.iterdir() if p.is_dir()):
        digest = hashlib.sha256()
        for path in sorted(library.rglob("*")):
            relative = path.relative_to(library)
            if path.is_file() and ".git" not in relative.parts:
                digest.update(relative.as_posix().encode() + b"\0")
                digest.update(path.read_bytes())
        result[library.name] = digest.hexdigest()
    return result


def toolchain_inputs():
    home = Path.home() / ".platformio"
    result = {}
    for name in ("toolchain-xtensa-esp32s3", "framework-arduinoespressif32",
                 "tool-esptoolpy", "tool-scons"):
        metadata = json.loads((home / "packages" / name / "package.json").read_text())
        result[name] = metadata["version"]
    result["platformio-core"] = subprocess.check_output(["pio", "--version"], text=True).strip()
    result["espressif32"] = json.loads((home / "platforms/espressif32@6.11.0/platform.json").read_text())["version"]
    return result


def compiler_inputs(commands):
    entries = json.loads(commands.read_text())
    command = next((c for c in entries if c["file"].endswith("/onchip/Runtime.cpp")),
                   next(c for c in entries if c["file"].endswith("/kiss_modem/main.cpp")))
    args = command.get("arguments") or shlex.split(command["command"])
    keys = {"MESHCORE_ONCHIP", "MESHCORE_ONCHIP_BOT", "ONCHIP_BOT_WASM",
            "MESHCORE_MAST_ADMIN", "ONCHIP_BOT_HTTPS", "MAX_CONTACTS",
            "MAX_GROUP_CHANNELS", "MAX_CLIENTS", "MAX_NEIGHBOURS", "MAX_UNSYNCED_POSTS",
            "KISS_LOCAL_SOURCES", "KISS_MAX_TCP_CLIENTS", "KISS_REQUEST_QUEUE_DEPTH",
            "ONCHIP_COMPANION_MAX_CLIENTS", "BOARD_HAS_PSRAM", "CONFIG_LWIP_MAX_SOCKETS"}
    defines = {}
    all_defines = {}
    for arg in args:
        if arg.startswith("-D"):
            key, _, value = arg[2:].partition("=")
            all_defines[key] = value or "1"
            if key in keys:
                defines[key] = value or "1"
    all_defines.pop("ONCHIP_BOT_WASM", None)
    return {"effective_defines": defines,
            "all_other_effective_defines_sha256": hashlib.sha256(
                json.dumps(all_defines, sort_keys=True).encode()).hexdigest(),
            "compiler_options": [arg for arg in args if arg.startswith(("-std=", "-O", "-flto"))]}


def run(args, env, log):
    subprocess.run(args, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT, check=True)


def copy_archive(source, destination, expected):
    if sha(source) != expected:
        raise ValueError(f"Archive hash mismatch: {source.name}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    if not destination.exists():
        shutil.copyfile(source, destination)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--upstream", type=Path, required=True)
    parser.add_argument("--libdeps", type=Path, required=True)
    parser.add_argument("--lua-archive", type=Path, required=True)
    parser.add_argument("--wamr-archive", type=Path, required=True)
    parser.add_argument("--profiles", nargs="+", choices=PROFILES, default=list(PROFILES))
    args = parser.parse_args()
    baseline = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    subprocess.run(["git", "diff", "--exit-code", BASELINE, "--", "firmware",
                    "Makefile", ":(exclude)**/*.md"], cwd=ROOT, check=True,
                   stdout=subprocess.DEVNULL)
    untracked = subprocess.check_output(
        ["git", "ls-files", "--others", "--exclude-standard", "--", "firmware"],
        cwd=ROOT, text=True).splitlines()
    if any(Path(path).suffix != ".md" for path in untracked):
        raise ValueError("Untracked firmware inputs differ from the pinned baseline")
    source = ROOT / ".tmp/onchip-upstream"
    source.parent.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, **PUBLIC, SOURCE_DATE_EPOCH=EPOCH, TMPDIR=str(ROOT / ".tmp"))
    subprocess.run(["git", "-C", str(args.upstream), "cat-file", "-e", UPSTREAM], check=True)
    if not source.exists():
        subprocess.run(["git", "clone", "--quiet", "--no-hardlinks", str(args.upstream), str(source)], check=True)
    subprocess.run(["git", "-C", str(source), "checkout", "--quiet", "--detach", UPSTREAM], check=True)
    copy_archive(args.lua_archive, ROOT / ".tmp/onchip-lua/lua-5.5.1.tar.gz", LUA_SHA)
    copy_archive(args.wamr_archive, ROOT / ".cache/meshcore-wamr/wamr.tar.gz", WAMR_SHA)
    config = (ROOT / "firmware/esp32/platformio.ini.example").read_text()
    config = config.replace("MAX_CONTACTS=256", "MAX_CONTACTS=350")
    config = config.replace("MAX_GROUP_CHANNELS=8", "MAX_GROUP_CHANNELS=40")
    # Hold the C++ language mode constant across the bot/no-bot comparison.
    config = config.replace("[env:Xiao_S3_WIO_onchip]\n", "[env:Xiao_S3_WIO_onchip]\nbuild_unflags = -std=gnu++11\n")
    config = config.replace("  -D MESHCORE_ONCHIP=1", "  -std=gnu++17\n  -D MESHCORE_ONCHIP=1")
    for key, value in PUBLIC.items():
        config = config.replace("${sysenv." + key + "}", value)
    config_path = ROOT / ".tmp/resource-profile.ini"
    config_path.write_text(config)
    artifacts = ROOT / ".tmp/resource-artifacts"
    artifacts.mkdir(exist_ok=True)
    for name in args.profiles:
        target_env, wasm = PROFILES[name]
        build = ROOT / (".tmp/onchip-resource-" + name)
        build_env = dict(env, ONCHIP_BOT_WASM=str(wasm))
        log_path = artifacts / (name + "-build.log")
        with log_path.open("w") as log:
            if name == "modem":
                if not build.exists():
                    run(["git", "clone", "--quiet", "--no-hardlinks", str(source), str(build)], build_env, log)
                    run(["git", "-C", str(build), "apply", str(ROOT / "firmware/shared/radio-reconfigure.patch")], build_env, log)
                dest = build / "examples/kiss_modem"
                shutil.copyfile(ROOT / "firmware/esp32/wifi_kiss_main.cpp", dest / "main.cpp")
                for filename in ("WifiKissMultiplexer.h", "WifiKissMultiplexer.cpp",
                                 "QueuedTxProtocol.h", "RadioDashboard.h", "RadioDashboard.cpp",
                                 "RadioDashboardPage.h", "RadioNetwork.h"):
                    shutil.copyfile(ROOT / "firmware/shared" / filename, dest / filename)
                modem_config = (ROOT / "firmware/esp32/platformio.modem.ini.example").read_text()
                modem_config = modem_config.replace("replace-with-your-ssid", PUBLIC["WIFI_SSID"])
                modem_config = modem_config.replace("replace-with-your-password", PUBLIC["WIFI_PWD"])
                modem_config = modem_config.replace("KISS_MAX_TCP_CLIENTS=8", "KISS_MAX_TCP_CLIENTS=4")
                modem_config = modem_config.replace("build_flags =\n", "build_unflags = -std=gnu++11\nbuild_flags =\n")
                modem_config += ("\n  -std=gnu++17\n  -D KISS_HOSTNAME='\"resource-build\"'\n"
                                 "  -D KISS_LOCAL_SOURCES=0\n  -D KISS_STREAM_ENDPOINT=0\n"
                                 "  -D MAX_CONTACTS=350\n  -D MAX_GROUP_CHANNELS=40\n"
                                 "  -D LORA_FREQ=912.525\n  -D LORA_BW=250.0\n"
                                 "  -D LORA_SF=7\n  -D LORA_CR=5\n  -D LORA_TX_POWER=2\n"
                                 "lib_deps =\n  ${Xiao_S3_WIO.lib_deps}\n"
                                 "  densaugeo/base64 @ ~1.4.0\n")
                (build / "platformio.local.ini").write_text(modem_config)
            else:
                run(["make", "-C", "firmware/esp32",
                     "prepare" if name == "native" else "bot-prepare",
                     "BUILD=" + str(build), "CONFIG=" + str(config_path),
                     "ONCHIP_BOT_WASM=" + str(wasm)], build_env, log)
            deps = build / ".pio/libdeps" / target_env
            if deps.exists():
                if dependency_inputs(deps) != dependency_inputs(args.libdeps):
                    raise ValueError("Existing build dependencies differ; select a fresh isolated build tree")
            else:
                shutil.copytree(args.libdeps, deps)
            run(["pio", "run", "-d", str(build), "-e", target_env, "-j", "2"], build_env, log)
            run(["pio", "run", "-d", str(build), "-e", target_env, "-t", "compiledb"], build_env, log)
        output = build / ".pio/build" / target_env
        profile = build / "platformio.local.ini"
        for filename in ("firmware.bin", "firmware.elf", "firmware.map", "partitions.bin", "bootloader.bin"):
            if (output / filename).exists():
                shutil.copyfile(output / filename, artifacts / (name + "-" + filename))
        shutil.copyfile(profile, artifacts / (name + "-platformio.ini"))
        shutil.copyfile(build / "compile_commands.json", artifacts / (name + "-compile_commands.json"))
        manifest = {
            "baseline": BASELINE, "report_revision": baseline,
            "upstream": UPSTREAM, "profile": name,
            "environment": target_env, "wasm": wasm, "source_date_epoch": EPOCH,
            "lua_archive_sha256": LUA_SHA, "wamr_archive_sha256": WAMR_SHA,
            "profile_sha256": sha(profile),
            "dependencies_sha256": dependency_inputs(deps),
            "toolchain": toolchain_inputs(),
            "artifacts": {p.name: {"bytes": p.stat().st_size, "sha256": sha(p)}
                          for p in sorted(artifacts.glob(name + "-*"))
                          if p.suffix in (".bin", ".elf", ".map")},
        }
        manifest.update(compiler_inputs(build / "compile_commands.json"))
        (artifacts / (name + "-inputs.json")).write_text(json.dumps(manifest, indent=2) + "\n")
        print(name, manifest["artifacts"][name + "-firmware.bin"]["bytes"], flush=True)


if __name__ == "__main__":
    main()
