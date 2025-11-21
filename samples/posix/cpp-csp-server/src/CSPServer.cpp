#include "CSPServer.h"

#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
std::once_flag init_once;
}

CSPServer::CSPServer() = default;

CSPServer::~CSPServer() {
    Close();
}

bool CSPServer::Config(const std::map<std::string, std::string> &params) {
    try {
        auto interface_it = params.find("interface");
        if (interface_it != params.end()) {
            if (interface_it->second == "can" || interface_it->second == "socketcan") {
                interface_type_ = InterfaceType::SocketCAN;
            } else {
                interface_type_ = InterfaceType::Loopback;
            }
        }

        auto port_it = params.find("port");
        if (port_it != params.end()) {
            listen_port_ = static_cast<uint8_t>(std::stoul(port_it->second));
        }

        auto addr_it = params.find("server_address");
        if (addr_it != params.end()) {
            server_address_ = static_cast<uint8_t>(std::stoul(addr_it->second));
        }

        auto conn_timeout_it = params.find("conn_timeout_ms");
        if (conn_timeout_it != params.end()) {
            conn_timeout_ms_ = static_cast<uint32_t>(std::stoul(conn_timeout_it->second));
        }

        auto read_timeout_it = params.find("read_timeout_ms");
        if (read_timeout_it != params.end()) {
            read_timeout_ms_ = static_cast<uint32_t>(std::stoul(read_timeout_it->second));
        }

        auto accept_timeout_it = params.find("accept_timeout_ms");
        if (accept_timeout_it != params.end()) {
            accept_timeout_ms_ = static_cast<uint32_t>(std::stoul(accept_timeout_it->second));
        }

        auto opts_it = params.find("conn_options");
        if (opts_it != params.end()) {
            conn_options_ = static_cast<uint32_t>(std::stoul(opts_it->second));
        }

        if (interface_type_ == InterfaceType::SocketCAN) {
#if !CSP_HAVE_LIBSOCKETCAN
            return false;
#else
            auto device_it = params.find("can_device");
            if (device_it == params.end()) {
                return false;
            }
            can_device_ = device_it->second;

            auto bitrate_it = params.find("can_bitrate");
            if (bitrate_it != params.end()) {
                can_bitrate_ = static_cast<uint32_t>(std::stoul(bitrate_it->second));
            }

            auto promisc_it = params.find("can_promisc");
            if (promisc_it != params.end()) {
                can_promisc_ = (promisc_it->second == "1" || promisc_it->second == "true");
            }

            auto ifname_it = params.find("can_ifname");
            if (ifname_it != params.end()) {
                can_ifname_ = ifname_it->second;
            }
#endif
        }
    } catch (const std::exception &) {
        return false;
    }

    return true;
}

bool CSPServer::Open(OpenCallback callback) {
    if (running_.load()) {
        return false;
    }

    callback_ = std::move(callback);

    std::call_once(init_once, []() {
        csp_init();
    });

    if (!setupInterface()) {
        return false;
    }

    running_.store(true);

    router_thread_ = std::thread(&CSPServer::routerLoop, this);
    server_thread_ = std::thread(&CSPServer::serverLoop, this);

    return true;
}

void CSPServer::Close() {
    if (!running_.load()) {
        return;
    }

    running_.store(false);

    if (router_thread_.joinable()) {
        router_thread_.join();
    }

    if (server_thread_.joinable()) {
        server_thread_.join();
    }
}

bool CSPServer::Write(const std::vector<uint8_t> &data) {
    return writeInternal(data.data(), data.size());
}

bool CSPServer::Write(const char *data, size_t length) {
    return writeInternal(reinterpret_cast<const uint8_t *>(data), length);
}

std::vector<uint8_t> CSPServer::Read(uint32_t timeout_ms) {
    std::unique_lock<std::mutex> lock(inbox_mutex_);

    if (timeout_ms == 0) {
        if (inbox_.empty()) {
            return {};
        }
    } else {
        inbox_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this]() { return !inbox_.empty(); });
    }

    if (inbox_.empty()) {
        return {};
    }

    std::vector<uint8_t> data = std::move(inbox_.front());
    inbox_.pop();
    return data;
}

bool CSPServer::writeInternal(const uint8_t *data, size_t length) {
    if (data == nullptr || length == 0) {
        return false;
    }

    csp_conn_t *conn = csp_connect(CSP_PRIO_NORM, server_address_, listen_port_, conn_timeout_ms_, conn_options_);
    if (conn == nullptr) {
        return false;
    }

    csp_packet_t *packet = csp_buffer_get(length);
    if (packet == nullptr) {
        csp_close(conn);
        return false;
    }

    std::memcpy(packet->data, data, length);
    packet->length = static_cast<uint16_t>(length);

    csp_send(conn, packet);
    csp_close(conn);
    return true;
}

bool CSPServer::setupInterface() {
    if (interface_type_ == InterfaceType::SocketCAN) {
#if CSP_HAVE_LIBSOCKETCAN
        int ret = csp_can_socketcan_open_and_add_interface(
            can_device_.c_str(), can_ifname_.c_str(), server_address_, can_bitrate_, can_promisc_, &can_iface_);
        if (ret != CSP_ERR_NONE || can_iface_ == nullptr) {
            return false;
        }
        can_iface_->is_default = 1;
        return true;
#else
        return false;
#endif
    }

    csp_if_lo.is_default = 1;
    return true;
}

void CSPServer::routerLoop() {
    while (running_.load()) {
        csp_route_work();
    }
}

void CSPServer::serverLoop() {
    csp_socket_t socket = {0};
    socket.opts = conn_options_;

    csp_bind(&socket, listen_port_);
    csp_listen(&socket, 5);

    while (running_.load()) {
        csp_conn_t *conn = csp_accept(&socket, accept_timeout_ms_);
        if (conn == nullptr) {
            continue;
        }

        while (running_.load()) {
            csp_packet_t *packet = csp_read(conn, read_timeout_ms_);
            if (packet == nullptr) {
                break;
            }

            std::vector<uint8_t> data(packet->data, packet->data + packet->length);

            {
                std::lock_guard<std::mutex> lock(inbox_mutex_);
                inbox_.push(data);
            }
            inbox_cv_.notify_one();

            if (callback_) {
                callback_(data);
            }

            csp_buffer_free(packet);
        }

        csp_close(conn);
    }
}

