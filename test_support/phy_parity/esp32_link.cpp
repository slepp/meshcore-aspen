#include "Esp32TcpKissLink.h"
#include <cassert>
#include <csignal>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

unsigned long millis() { return 100; }

#ifndef EXPECTED_HOSTNAME
#define EXPECTED_HOSTNAME "meshcore-phyless"
#endif
#ifndef EXPECTED_POWER
#define EXPECTED_POWER WIFI_POWER_8_5dBm
#endif

int main() {
  Esp32TcpKissLink link("test-ssid", "test-password", "test-modem.invalid");
  link.begin();
  assert(WiFi.hostname == EXPECTED_HOSTNAME && link_test::power == EXPECTED_POWER);
  const std::vector<std::string> expected{
      "volatile", "hostname", "station", "power", "awake", "reconnect", "associate"};
  assert(link_test::calls == expected);
  link.begin();
  assert(link_test::calls == expected);
  link.loop();
  assert(link.connected() && link_test::connects == 1 && link.takeConnectedEvent());
  assert(!link.takeConnectedEvent());

  for (int failure = 1; failure <= 5; ++failure) {
    int output[2];
    assert(pipe(output) == 0);
    const pid_t child = fork();
    assert(child >= 0);
    if (!child) {
      const rlimit limit{0, 0};
      setrlimit(RLIMIT_CORE, &limit);
      close(output[0]);
      dup2(output[1], STDERR_FILENO);
      close(output[1]);
      link_test::failure = failure;
      Esp32TcpKissLink broken("test-ssid", "test-password", "test-modem.invalid");
      broken.begin();
      _exit(0);
    }
    close(output[1]);
    std::string diagnostic;
    char bytes[256];
    ssize_t n;
    while ((n = read(output[0], bytes, sizeof(bytes))) > 0)
      diagnostic.append(bytes, static_cast<size_t>(n));
    close(output[0]);
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    assert(diagnostic.find(failure <= 3 ? "hostname configuration failed" :
                           failure == 4 ? "station initialization failed" :
                                          "transmit power configuration failed") != std::string::npos);
    if (failure == 5)
      assert(diagnostic.find("[code=6 a=4294967295 ") != std::string::npos);
    assert(diagnostic.find("test-ssid") == std::string::npos &&
           diagnostic.find("test-password") == std::string::npos &&
           diagnostic.find("test-modem.invalid") == std::string::npos);
  }
}
