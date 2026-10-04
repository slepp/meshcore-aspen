#!/usr/bin/env python3
"""Extract unchanged native arithmetic bodies; never application handlers."""
import argparse
import hashlib
import json
import pathlib
import re


def function(text, signature):
    start = text.index(signature)
    opening = text.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def generate(upstream, build):
    fragments = []
    sources = []
    for role in ("simple_repeater", "simple_room_server", "companion_radio"):
        path = pathlib.Path("examples") / role / "MyMesh.cpp"
        text = (upstream / path).read_text()
        name = role + "_kernel"
        fragments.append(f"""struct {name} {{
  struct {{ float rx_delay_base = 0, tx_delay_factor = 0.5f, direct_tx_delay_factor = 0.2f; }} _prefs;
  mesh::Radio* _radio;
  RecordedRNG* rng;
  RecordedRNG* getRNG() {{ return rng; }}
  int calcRxDelay(float score, uint32_t air_time) const;
  uint32_t getRetransmitDelay(const mesh::Packet* packet);
  uint32_t getDirectRetransmitDelay(const mesh::Packet* packet);
}};""")
        for signature in ("int MyMesh::calcRxDelay", "uint32_t MyMesh::getRetransmitDelay",
                          "uint32_t MyMesh::getDirectRetransmitDelay"):
            body = function(text, signature)
            fragments.append(body.replace("MyMesh::", name + "::", 1))
            sources.append({"file": str(path), "symbol": signature,
                            "sha256": hashlib.sha256(body.encode()).hexdigest()})
    path = pathlib.Path("src/helpers/radiolib/RadioLibWrappers.cpp")
    text = (upstream / path).read_text()
    table = re.search(r"static float snr_threshold\[\] = \{.*?\};", text, re.S).group()
    body = function(text, "float RadioLibWrapper::packetScoreInt")
    fragments.extend(["using std::min; using std::max;", table,
                      body.replace("RadioLibWrapper::packetScoreInt", "nativePacketScore", 1)])
    sources.append({"file": str(path), "symbol": "snr_threshold + packetScoreInt",
                    "sha256": hashlib.sha256((table + body).encode()).hexdigest()})
    path = pathlib.Path("examples/simple_repeater/MyMesh.cpp")
    text = (upstream / path).read_text()
    tables = re.findall(r"static uint8_t max_loop_\w+\[\] =.*?;", text)
    if len(tables) != 3:
        raise ValueError("Expected exactly three native loop-threshold tables")
    body = function(text, "bool MyMesh::isLooped")
    fragments.extend(tables)
    fragments.extend(["""struct RepeaterLoopKernel {
  mesh::Identity self_id;
  bool isLooped(const mesh::Packet* packet, const uint8_t max_counters[]);
};""", body.replace("MyMesh::", "RepeaterLoopKernel::", 1)])
    sources.append({"file": str(path), "symbol": "max_loop_* + MyMesh::isLooped",
                    "sha256": hashlib.sha256(("\n".join(tables) + body).encode()).hexdigest()})
    path = pathlib.Path("src/helpers/CommonCLI.cpp")
    text = (upstream / path).read_text()
    duty = re.search(r"_prefs->airtime_factor = \(100\.0f / dc\) - 1\.0f;", text).group()
    direct = re.search(r"_prefs->airtime_factor = atof\(&config\[3\]\);", text).group()
    for name, argument, statement in (("nativeDutycycleAF", "float dc", duty),
                                      ("nativeRuntimeAF", "const char* config", direct)):
        fragments.append(f"""float {name}({argument}) {{
  struct {{ float airtime_factor; }} prefs;
  auto _prefs = &prefs;
  {statement}
  return _prefs->airtime_factor;
}}""")
        sources.append({"file": str(path), "symbol": "CommonCLI::handleSetCmd/" + name,
                        "sha256": hashlib.sha256(statement.encode()).hexdigest(),
                        "scope": "Arithmetic assignment only; not CLI validation, savePrefs or application execution."})
    build.mkdir(parents=True, exist_ok=True)
    (build / "kernels.inc").write_text("\n\n".join(fragments) + "\n")
    (build / "kernels.json").write_text(json.dumps({
        "kind": "extracted-native-arithmetic",
        "not_application_execution": True,
        "adaptation": "Only class qualifiers replaced; fields supplied by narrow fixture. RecordedRNG observes nextInt bounds then invokes actual mesh::RNG::nextInt.",
        "sources": sources}, indent=2) + "\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--upstream", type=pathlib.Path, required=True)
    parser.add_argument("--build", type=pathlib.Path, required=True)
    args = parser.parse_args()
    generate(args.upstream, args.build)
