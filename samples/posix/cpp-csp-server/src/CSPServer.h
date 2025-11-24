#ifndef CSP_SERVER_H
#define CSP_SERVER_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <csp/csp.h>
#include <csp/interfaces/csp_if_lo.h>
#if CSP_HAVE_LIBSOCKETCAN
#include <csp/drivers/can_socketcan.h>
#endif
}

class CSPServer {
public:
    using OpenCallback = std::function<void(const std::vector<uint8_t> &)>;

    CSPServer();
    ~CSPServer();

    bool Config(const std::map<std::string, std::string> &params);
    bool Open(OpenCallback callback);
    void Close();

    bool Write(const std::vector<uint8_t> &data);
    bool Write(const char *data, size_t length);

    std::vector<uint8_t> Read(uint32_t timeout_ms = 0);

private:
    bool writeInternal(const uint8_t *data, size_t length);
    bool setupInterface();
    void routerLoop();
    void serverLoop();

    enum class InterfaceType {
        Loopback,
        SocketCAN
    };

    std::atomic<bool> running_{false};
    OpenCallback callback_;

    std::thread router_thread_;
    std::thread server_thread_;

    std::mutex inbox_mutex_;
    std::condition_variable inbox_cv_;
    std::queue<std::vector<uint8_t>> inbox_;

    uint8_t server_address_ = 0;
    uint8_t listen_port_ = 10;
    uint32_t conn_timeout_ms_ = 1000;
    uint32_t read_timeout_ms_ = 1000;
    uint32_t accept_timeout_ms_ = 1000;
    uint32_t conn_options_ = CSP_O_NONE;

    InterfaceType interface_type_ = InterfaceType::Loopback;
    std::string can_device_ = "";
#if CSP_HAVE_LIBSOCKETCAN
    std::string can_ifname_ = CSP_IF_CAN_DEFAULT_NAME;
#else
    std::string can_ifname_ = "can";
#endif
    uint32_t can_bitrate_ = 1000000;
    bool can_promisc_ = true;
#if CSP_HAVE_LIBSOCKETCAN
    csp_iface_t *can_iface_ = nullptr;
#endif
};

#endif // CSP_SERVER_H
