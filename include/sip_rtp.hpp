#pragma once

#include <cstdint>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
namespace v92 { using socket_handle_t = SOCKET; constexpr socket_handle_t invalid_socket = INVALID_SOCKET; }
#else
#include <netinet/in.h>
namespace v92 { using socket_handle_t = int; constexpr socket_handle_t invalid_socket = -1; }
#endif

namespace v92 {

struct SipRtpConfig {
    std::string bind_ip = "0.0.0.0";
    std::string advertise_ip;
    uint16_t sip_port = 5060;
    uint16_t rtp_port = 40000;
    bool debug = false;

    // Optional SIP registrar/proxy. The UAS still accepts direct LAN INVITEs
    // while registered, so one binary supports both topologies.
    std::string registrar_host;
    uint16_t registrar_port = 5060;
    std::string registrar_user;
    std::string registrar_password;
    unsigned registrar_expires = 300;
};

enum class SipEventType {
    None, CallStarted, Ack, CallEnded,
    RegistrationOk, RegistrationFailed,
    Error
};
struct SipEvent {
    SipEventType type = SipEventType::None;
    std::string detail;
};

struct RtpAudio {
    std::vector<uint8_t> pcmu;
    uint16_t sequence = 0;
    uint32_t timestamp = 0;
    bool had_gap = false;
    unsigned missing_packets = 0;
};

class SipRtpServer {
public:
    explicit SipRtpServer(SipRtpConfig cfg = {});
    ~SipRtpServer();
    SipRtpServer(const SipRtpServer&) = delete;
    SipRtpServer& operator=(const SipRtpServer&) = delete;

    bool open();
    void close();
    socket_handle_t sip_fd() const { return sip_fd_; }
    socket_handle_t rtp_fd() const { return rtp_fd_; }
    bool call_active() const { return call_active_; }
    const std::string& last_error() const { return last_error_; }

    bool registration_enabled() const { return !cfg_.registrar_host.empty() && !cfg_.registrar_user.empty(); }
    bool registered() const { return registered_; }
    bool send_register();

    SipEvent handle_sip();
    bool recv_rtp(RtpAudio& out);
    bool send_rtp_pcmu(const uint8_t* data, size_t n, bool marker = false);
    bool send_rtp_pcmu(const std::vector<uint8_t>& data, bool marker = false) {
        return send_rtp_pcmu(data.data(), data.size(), marker);
    }

private:
    SipRtpConfig cfg_;
    socket_handle_t sip_fd_ = invalid_socket;
    socket_handle_t rtp_fd_ = invalid_socket;
    std::string last_error_;

    bool call_active_ = false;
    sockaddr_in sip_peer_{};
    sockaddr_in rtp_peer_{};
    bool have_rtp_peer_ = false;
    bool symmetric_rtp_locked_ = false;

    std::string via_, from_, to_, call_id_, cseq_, to_tag_;
    uint16_t tx_seq_ = 0;
    uint32_t tx_ts_ = 0;
    uint32_t ssrc_ = 0;
    bool have_rx_seq_ = false;
    uint16_t last_rx_seq_ = 0;

    sockaddr_in registrar_addr_{};
    bool have_registrar_addr_ = false;
    bool registered_ = false;
    unsigned reg_cseq_ = 1;
    std::string reg_call_id_;
    std::string reg_from_tag_;
    bool reg_auth_attempted_ = false;
    std::string reg_authorization_;

    bool send_sip_response(int code, const std::string& reason,
                           const std::string& extra_headers = {},
                           const std::string& body = {});
    std::string build_sdp() const;
    bool resolve_registrar();
    bool send_register_request();
    SipEvent handle_sip_response(const std::string& msg);
};

bool sip_digest_compiled();

} // namespace v92
