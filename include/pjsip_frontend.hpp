#pragma once

#ifdef _WIN32

#include "live_modem.hpp"
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace v92 {

struct PjsipFrontendConfig {
    std::string bind_ip = "0.0.0.0";
    std::string advertise_ip;
    uint16_t sip_port = 5060;
    uint16_t rtp_port = 40000;
    std::string registrar_host;
    uint16_t registrar_port = 5060;
    std::string user;
    std::string password;
    unsigned expires = 300;
    bool debug = true;
};

enum class FrontendEventType { None, Registration, CallStarted, MediaActive, CallEnded, Error };
struct FrontendEvent { FrontendEventType type = FrontendEventType::None; std::string text; };

class PjsipFrontend {
public:
    class Impl;
    PjsipFrontend(PjsipFrontendConfig cfg, LiveMode mode);
    ~PjsipFrontend();
    PjsipFrontend(const PjsipFrontend&) = delete;
    PjsipFrontend& operator=(const PjsipFrontend&) = delete;

    bool open();
    void close();
    bool pop_event(FrontendEvent& out);
    bool call_active() const;
    bool hangup_current_call();
    uint64_t rx_frames() const;

    bool modem_data_connected();
    std::vector<uint8_t> take_modem_ppp_bytes();
    void feed_modem_ppp_bytes(const std::vector<uint8_t>& bytes);
    std::string take_modem_event();
    LiveState modem_state();

    const std::string& last_error() const { return last_error_; }

private:
    std::unique_ptr<Impl> impl_;
    std::string last_error_;
};

} // namespace v92

#endif
