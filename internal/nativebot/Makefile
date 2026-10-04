CXX ?= c++
CXXFLAGS ?= -std=c++17 -O1 -g -Wall -Wextra -Werror -pthread

.PHONY: test spiffs-test
test:
	$(CXX) $(CXXFLAGS) nvs.cpp nvs_test.cpp -o .nvs_test
	@./.nvs_test; status=$$?; rm -f .nvs_test; exit $$status

spiffs-test:
	$(CXX) $(CXXFLAGS) -I../../firmware/esp32/tests/seams -I../../test_support/phy_parity/seams spiffs.cpp spiffs_test.cpp -o .spiffs_test
	@./.spiffs_test; status=$$?; rm -f .spiffs_test; exit $$status
