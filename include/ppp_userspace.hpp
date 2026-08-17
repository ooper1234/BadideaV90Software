#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

namespace v92 {

struct UserPppConfig {
    std::string local_ip = "10.77.0.1";
    std::string peer_ip = "10.77.0.2";
    std::string dns1 = "1.1.1.1";
    std::string dns2 = "8.8.8.8";
    uint16_t mtu = 296;
    bool require_pap = false;
    std::string pap_secrets = "/etc/v92isp/pap-secrets";
    bool debug = false;
};

enum class UserPppPhase {
    Dead,
    Lcp,
    Authenticate,
    Ipcp,
    Network,
    Terminating,
    Failed
};

const char* to_string(UserPppPhase phase);

// Minimal RFC 1661/RFC 1662 async PPP server for dial-up Internet access.
// It intentionally implements only the protocols needed by normal Windows
// dial-up clients: LCP, optional PAP, IPCP (IPv4 + MS DNS options), and IPv4.
// IPv6CP/CCP/other NCPs are rejected cleanly rather than partially negotiated.
class UserPppServer {
public:
    explicit UserPppServer(UserPppConfig cfg = {});

    void reset();
    void start(uint64_t now_ms = 0);
    void tick(uint64_t now_ms);

    void feed_serial(const uint8_t* data, size_t len, uint64_t now_ms = 0);
    void feed_serial(const std::vector<uint8_t>& data, uint64_t now_ms = 0) {
        feed_serial(data.data(), data.size(), now_ms);
    }

    std::vector<uint8_t> take_serial_tx();
    std::vector<std::vector<uint8_t>> take_ip_packets();
    void feed_ip_packet(const uint8_t* data, size_t len);
    void feed_ip_packet(const std::vector<uint8_t>& p) { feed_ip_packet(p.data(), p.size()); }

    UserPppPhase phase() const { return phase_; }
    bool network_up() const { return phase_ == UserPppPhase::Network; }
    const std::string& last_event() const { return last_event_; }
    void clear_event() { last_event_.clear(); }
    const std::string& authenticated_user() const { return authenticated_user_; }

    static uint16_t ppp_fcs16(const uint8_t* data, size_t len);
    static std::vector<uint8_t> encode_async_frame(uint16_t protocol,
                                                    const std::vector<uint8_t>& info);

private:
    UserPppConfig cfg_;
    UserPppPhase phase_ = UserPppPhase::Dead;
    std::string last_event_;
    std::string authenticated_user_;

    std::vector<uint8_t> rx_frame_;
    bool rx_escape_ = false;
    std::deque<uint8_t> serial_tx_;
    std::vector<std::vector<uint8_t>> ip_rx_;

    uint8_t next_id_ = 1;
    uint8_t lcp_id_ = 0;
    uint8_t ipcp_id_ = 0;
    uint32_t magic_ = 0x56393249; // "V92I"
    bool our_lcp_acked_ = false;
    bool peer_lcp_acked_ = false;
    bool pap_ok_ = false;
    bool our_ipcp_acked_ = false;
    bool peer_ipcp_acked_ = false;
    uint64_t last_lcp_tx_ms_ = 0;
    uint64_t last_ipcp_tx_ms_ = 0;
    unsigned lcp_retries_ = 0;
    unsigned ipcp_retries_ = 0;
    std::vector<uint8_t> last_lcp_request_;
    std::vector<uint8_t> last_ipcp_request_;

    std::unordered_map<std::string, std::string> pap_users_;

    void set_phase(UserPppPhase p, const std::string& event = {});
    void queue_protocol(uint16_t protocol, const std::vector<uint8_t>& info);
    void process_frame(const std::vector<uint8_t>& frame, uint64_t now_ms);
    void process_lcp(const std::vector<uint8_t>& info, uint64_t now_ms);
    void process_pap(const std::vector<uint8_t>& info, uint64_t now_ms);
    void process_ipcp(const std::vector<uint8_t>& info, uint64_t now_ms);
    void protocol_reject(uint16_t protocol, const std::vector<uint8_t>& info);

    void send_lcp_request(uint64_t now_ms, bool fresh_id);
    void send_ipcp_request(uint64_t now_ms, bool fresh_id);
    void maybe_lcp_open(uint64_t now_ms);
    void maybe_ipcp_open();
    bool load_pap_users();
};

} // namespace v92
