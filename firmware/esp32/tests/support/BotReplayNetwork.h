// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <string>

namespace bot_native_test {
// This transport never opens a socket. Native HTTPS admission/encoding/decoding
// still run; TLS itself is exercised by the separate native HTTPS test target.
bool replayNetwork(const std::string &directive, std::string &error);
unsigned replayNetworkSubmissions();
}
