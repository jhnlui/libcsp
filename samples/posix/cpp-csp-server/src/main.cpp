#include "CSPServer.h"

#include <cassert>
#include <chrono>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <vector>

int main() {
    CSPServer server;
    std::map<std::string, std::string> config = {
        {"port", "25"},
        {"server_address", "0"},
        {"conn_timeout_ms", "500"},
        {"read_timeout_ms", "500"},
        {"accept_timeout_ms", "500"}
    };

    bool configured = server.Config(config);
    assert(configured);

    std::vector<std::vector<uint8_t>> callbacks;
    std::mutex callback_mutex;

    bool opened = server.Open([&](const std::vector<uint8_t> &data) {
        std::lock_guard<std::mutex> lock(callback_mutex);
        callbacks.push_back(data);
    });
    assert(opened);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    const std::vector<uint8_t> payload = {'c', 's', 'p', '_', 't', 'e', 's', 't'};
    bool write_ok = server.Write(payload);
    assert(write_ok);

    std::vector<uint8_t> response = server.Read(2000);
    assert(!response.empty());
    assert(response == payload);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    {
        std::lock_guard<std::mutex> lock(callback_mutex);
        assert(!callbacks.empty());
        assert(callbacks.front() == payload);
    }

    server.Close();

#if CSP_HAVE_LIBSOCKETCAN
    // Verify CANBus configuration path can be initialized from configuration.
    CSPServer can_server;
    std::map<std::string, std::string> can_config = {
        {"interface", "can"},
        {"can_device", "vcan0"},
        {"can_bitrate", "250000"},
        {"server_address", "5"}
    };
    bool can_configured = can_server.Config(can_config);
    assert(can_configured);
#endif

    std::cout << "CSPServer loopback client test passed." << std::endl;
    return 0;
}

