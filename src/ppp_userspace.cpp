#include "ppp_userspace.hpp"

#include <algorithm>
#include <array>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif
#include <cctype>
#include <fstream>
#include <sstream>

namespace v92 {

namespace {
constexpr uint16_t PPP_IP   = 0x0021;
constexpr uint16_t PPP_LCP  = 0xc021;
constexpr uint16_t PPP_PAP  = 0xc023;
constexpr uint16_t PPP_IPCP = 0x8021;

constexpr uint16_t PPP_GOODFCS = 0xf0b8;

uint16_t be16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}
void put_be16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x));
}
void put_be32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x >> 24));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x));
}
uint32_t read_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

bool parse_ipv4(const std::string& s, uint32_t& out_be) {
    in_addr a{};
    if (::inet_pton(AF_INET, s.c_str(), &a) != 1) return false;
    out_be = ntohl(a.s_addr);
    return true;
}

std::vector<uint8_t> cp_packet(uint8_t code, uint8_t id, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> p;
    p.reserve(body.size() + 4);
    p.push_back(code);
    p.push_back(id);
    put_be16(p, static_cast<uint16_t>(body.size() + 4));
    p.insert(p.end(), body.begin(), body.end());
    return p;
}

std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    size_t n = 0;
    while (n < s.size() && std::isspace(static_cast<unsigned char>(s[n]))) ++n;
    return s.substr(n);
}

} // namespace

const char* to_string(UserPppPhase p) {
    switch (p) {
    case UserPppPhase::Dead: return "dead";
    case UserPppPhase::Lcp: return "LCP";
    case UserPppPhase::Authenticate: return "PAP";
    case UserPppPhase::Ipcp: return "IPCP";
    case UserPppPhase::Network: return "NETWORK";
    case UserPppPhase::Terminating: return "terminating";
    case UserPppPhase::Failed: return "failed";
    }
    return "?";
}

UserPppServer::UserPppServer(UserPppConfig cfg) : cfg_(std::move(cfg)) {
    uint32_t dummy = 0;
    if (!parse_ipv4(cfg_.local_ip, dummy) || !parse_ipv4(cfg_.peer_ip, dummy) ||
        !parse_ipv4(cfg_.dns1, dummy) || !parse_ipv4(cfg_.dns2, dummy)) {
        phase_ = UserPppPhase::Failed;
        last_event_ = "invalid IPv4 address in PPP configuration";
    }
}

void UserPppServer::reset() {
    phase_ = UserPppPhase::Dead;
    last_event_.clear();
    authenticated_user_.clear();
    rx_frame_.clear();
    rx_escape_ = false;
    serial_tx_.clear();
    ip_rx_.clear();
    next_id_ = 1;
    lcp_id_ = ipcp_id_ = 0;
    our_lcp_acked_ = peer_lcp_acked_ = false;
    pap_ok_ = false;
    our_ipcp_acked_ = peer_ipcp_acked_ = false;
    last_lcp_tx_ms_ = last_ipcp_tx_ms_ = 0;
    lcp_retries_ = ipcp_retries_ = 0;
    last_lcp_request_.clear();
    last_ipcp_request_.clear();
    pap_users_.clear();
}

void UserPppServer::set_phase(UserPppPhase p, const std::string& event) {
    phase_ = p;
    if (!event.empty()) last_event_ = event;
}

uint16_t UserPppServer::ppp_fcs16(const uint8_t* data, size_t len) {
    uint16_t fcs = 0xffff;
    for (size_t i = 0; i < len; ++i) {
        fcs ^= data[i];
        for (int b = 0; b < 8; ++b)
            fcs = (fcs & 1) ? static_cast<uint16_t>((fcs >> 1) ^ 0x8408) : static_cast<uint16_t>(fcs >> 1);
    }
    return fcs;
}

std::vector<uint8_t> UserPppServer::encode_async_frame(uint16_t protocol,
                                                        const std::vector<uint8_t>& info) {
    std::vector<uint8_t> raw{0xff, 0x03};
    put_be16(raw, protocol);
    raw.insert(raw.end(), info.begin(), info.end());
    uint16_t f = static_cast<uint16_t>(~ppp_fcs16(raw.data(), raw.size()));
    raw.push_back(static_cast<uint8_t>(f));
    raw.push_back(static_cast<uint8_t>(f >> 8));

    std::vector<uint8_t> out;
    out.reserve(raw.size() * 2 + 2);
    out.push_back(0x7e);
    for (uint8_t c : raw) {
        // Escaping all ASCII controls is legal even after ACCM=0 and is robust
        // across terminal/modem paths. Flag/Escape are always escaped.
        if (c < 0x20 || c == 0x7d || c == 0x7e) {
            out.push_back(0x7d);
            out.push_back(static_cast<uint8_t>(c ^ 0x20));
        } else {
            out.push_back(c);
        }
    }
    out.push_back(0x7e);
    return out;
}

void UserPppServer::queue_protocol(uint16_t protocol, const std::vector<uint8_t>& info) {
    auto f = encode_async_frame(protocol, info);
    for (uint8_t b : f) serial_tx_.push_back(b);
}

bool UserPppServer::load_pap_users() {
    pap_users_.clear();
    std::ifstream in(cfg_.pap_secrets);
    if (!in) {
        last_event_ = "cannot open PAP secrets: " + cfg_.pap_secrets;
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        auto tab = t.find('\t');
        if (tab == std::string::npos) continue;
        std::string user = t.substr(0, tab);
        std::string pass = t.substr(tab + 1);
        if (!user.empty()) pap_users_[user] = pass;
    }
    if (pap_users_.empty()) {
        last_event_ = "PAP secrets file contains no users";
        return false;
    }
    return true;
}

void UserPppServer::start(uint64_t now_ms) {
    if (phase_ == UserPppPhase::Failed) return;
    if (cfg_.require_pap && !load_pap_users()) {
        set_phase(UserPppPhase::Failed, last_event_);
        return;
    }
    set_phase(UserPppPhase::Lcp, "PPP LCP negotiation started");
    send_lcp_request(now_ms, true);
}

void UserPppServer::send_lcp_request(uint64_t now_ms, bool fresh_id) {
    if (fresh_id) lcp_id_ = next_id_++;
    std::vector<uint8_t> o;
    // MRU
    o.push_back(1); o.push_back(4); put_be16(o, cfg_.mtu);
    // Async-Control-Character-Map = 0. The encoder still escapes controls,
    // which is permitted; this simply tells the peer it need not do so.
    o.push_back(2); o.push_back(6); put_be32(o, 0);
    if (cfg_.require_pap) {
        o.push_back(3); o.push_back(4); put_be16(o, PPP_PAP);
    }
    // Magic number
    o.push_back(5); o.push_back(6); put_be32(o, magic_);
    last_lcp_request_ = cp_packet(1, lcp_id_, o);
    queue_protocol(PPP_LCP, last_lcp_request_);
    last_lcp_tx_ms_ = now_ms;
    ++lcp_retries_;
}

void UserPppServer::send_ipcp_request(uint64_t now_ms, bool fresh_id) {
    if (fresh_id) ipcp_id_ = next_id_++;
    uint32_t local = 0; parse_ipv4(cfg_.local_ip, local);
    std::vector<uint8_t> o{3, 6};
    put_be32(o, local);
    last_ipcp_request_ = cp_packet(1, ipcp_id_, o);
    queue_protocol(PPP_IPCP, last_ipcp_request_);
    last_ipcp_tx_ms_ = now_ms;
    ++ipcp_retries_;
}

void UserPppServer::tick(uint64_t now_ms) {
    if (phase_ == UserPppPhase::Lcp && !our_lcp_acked_ && now_ms >= last_lcp_tx_ms_ + 3000) {
        if (lcp_retries_ >= 10) { set_phase(UserPppPhase::Failed, "LCP timeout"); return; }
        send_lcp_request(now_ms, false);
    }
    if (phase_ == UserPppPhase::Ipcp && !our_ipcp_acked_ && now_ms >= last_ipcp_tx_ms_ + 3000) {
        if (ipcp_retries_ >= 10) { set_phase(UserPppPhase::Failed, "IPCP timeout"); return; }
        send_ipcp_request(now_ms, false);
    }
}

void UserPppServer::feed_serial(const uint8_t* data, size_t len, uint64_t now_ms) {
    if (!data || !len) return;
    for (size_t i = 0; i < len; ++i) {
        uint8_t c = data[i];
        if (c == 0x7e) {
            if (rx_frame_.size() >= 6) process_frame(rx_frame_, now_ms);
            rx_frame_.clear();
            rx_escape_ = false;
            continue;
        }
        if (c == 0x7d) { rx_escape_ = true; continue; }
        if (rx_escape_) { c ^= 0x20; rx_escape_ = false; }
        // RFC 1662 receiver may silently discard unescaped control chars that
        // are mapped in its receive ACCM. We negotiated ACCM=0, so preserve all.
        if (rx_frame_.size() < 65536) rx_frame_.push_back(c);
        else { rx_frame_.clear(); rx_escape_ = false; }
    }
}

void UserPppServer::process_frame(const std::vector<uint8_t>& f, uint64_t now_ms) {
    if (ppp_fcs16(f.data(), f.size()) != PPP_GOODFCS) return;
    if (f.size() < 4) return;
    size_t p = 0;
    // ACFC support on receive even though we reject negotiating it.
    if (f.size() >= 2 && f[0] == 0xff && f[1] == 0x03) p = 2;
    if (p >= f.size() - 2) return;
    uint16_t proto = 0;
    if (f[p] & 1) proto = f[p++];
    else {
        if (p + 1 >= f.size() - 2) return;
        proto = static_cast<uint16_t>((f[p] << 8) | f[p + 1]);
        p += 2;
    }
    if (p > f.size() - 2) return;
    std::vector<uint8_t> info(f.begin() + static_cast<long>(p), f.end() - 2);
    switch (proto) {
    case PPP_LCP: process_lcp(info, now_ms); break;
    case PPP_PAP: process_pap(info, now_ms); break;
    case PPP_IPCP: process_ipcp(info, now_ms); break;
    case PPP_IP:
        if (network_up() && !info.empty() && (info[0] >> 4) == 4 && info.size() <= 65535)
            ip_rx_.push_back(std::move(info));
        break;
    default:
        if (phase_ != UserPppPhase::Dead && phase_ != UserPppPhase::Failed)
            protocol_reject(proto, info);
        break;
    }
}

void UserPppServer::maybe_lcp_open(uint64_t now_ms) {
    if (!our_lcp_acked_ || !peer_lcp_acked_) return;
    if (cfg_.require_pap) {
        set_phase(UserPppPhase::Authenticate, "LCP open; waiting for PAP authentication");
    } else {
        set_phase(UserPppPhase::Ipcp, "LCP open; starting IPCP");
        send_ipcp_request(now_ms, true);
    }
}

void UserPppServer::process_lcp(const std::vector<uint8_t>& in, uint64_t now_ms) {
    if (in.size() < 4) return;
    uint8_t code = in[0], id = in[1];
    uint16_t len = be16(&in[2]);
    if (len < 4 || len > in.size()) return;
    std::vector<uint8_t> body(in.begin() + 4, in.begin() + len);

    if (code == 1) { // Configure-Request
        std::vector<uint8_t> reject, nak;
        size_t i = 0;
        while (i < body.size()) {
            if (i + 2 > body.size()) return;
            uint8_t t = body[i], l = body[i + 1];
            if (l < 2 || i + l > body.size()) return;
            const uint8_t* o = body.data() + i;
            bool supported = false;
            switch (t) {
            case 1: // MRU
                if (l == 4) {
                    uint16_t m = be16(o + 2);
                    if (m >= 128 && m <= 1500) supported = true;
                    else { nak.insert(nak.end(), {1,4}); put_be16(nak, cfg_.mtu); supported = true; }
                }
                break;
            case 2: // ACCM
                supported = (l == 6);
                break;
            case 5: // Magic
                supported = (l == 6);
                break;
            // PFC/ACFC are not needed and are rejected so both sides use the
            // unambiguous full framing on this slow diagnostic-oriented link.
            default:
                break;
            }
            if (!supported) reject.insert(reject.end(), body.begin() + static_cast<long>(i), body.begin() + static_cast<long>(i + l));
            i += l;
        }
        if (!reject.empty()) {
            queue_protocol(PPP_LCP, cp_packet(4, id, reject));
            return;
        }
        if (!nak.empty()) {
            queue_protocol(PPP_LCP, cp_packet(3, id, nak));
            return;
        }
        queue_protocol(PPP_LCP, cp_packet(2, id, body));
        peer_lcp_acked_ = true;
        maybe_lcp_open(now_ms);
        return;
    }
    if (code == 2 && id == lcp_id_) {
        auto expected = last_lcp_request_;
        if (!expected.empty()) expected[0] = 2;
        if (in != expected) return;
        our_lcp_acked_ = true;
        maybe_lcp_open(now_ms);
        return;
    }
    if ((code == 3 || code == 4) && id == lcp_id_) {
        // We can operate without MRU/ACCM/Magic, but PAP auth must never be
        // silently dropped when public mode requires it.
        if (cfg_.require_pap) {
            bool pap_rejected = false;
            for (size_t i = 0; i + 1 < body.size();) {
                uint8_t t = body[i], l = body[i+1];
                if (l < 2 || i + l > body.size()) break;
                if (t == 3) pap_rejected = true;
                i += l;
            }
            if (pap_rejected) {
                set_phase(UserPppPhase::Failed, "peer rejected required PAP authentication");
                return;
            }
        }
        // Retry a conservative request. Most Windows clients ACK the normal one.
        send_lcp_request(now_ms, true);
        return;
    }
    if (code == 5) { // Terminate-Request
        queue_protocol(PPP_LCP, cp_packet(6, id, body));
        set_phase(UserPppPhase::Terminating, "peer requested PPP termination");
        return;
    }
    if (code == 9) { // Echo-Request
        std::vector<uint8_t> r;
        put_be32(r, magic_);
        if (body.size() > 4) r.insert(r.end(), body.begin() + 4, body.end());
        queue_protocol(PPP_LCP, cp_packet(10, id, r));
        return;
    }
}

void UserPppServer::process_pap(const std::vector<uint8_t>& in, uint64_t now_ms) {
    if (phase_ != UserPppPhase::Authenticate || in.size() < 6) return;
    uint8_t code = in[0], id = in[1];
    uint16_t len = be16(&in[2]);
    if (code != 1 || len < 6 || len > in.size()) return;
    size_t p = 4;
    uint8_t ulen = in[p++];
    if (p + ulen + 1 > len) return;
    std::string user(reinterpret_cast<const char*>(in.data() + p), ulen); p += ulen;
    uint8_t plen = in[p++];
    if (p + plen > len) return;
    std::string pass(reinterpret_cast<const char*>(in.data() + p), plen);

    bool ok = false;
    auto it = pap_users_.find(user);
    if (it != pap_users_.end() && it->second == pass) ok = true;
    std::string msg = ok ? "Login OK" : "Login incorrect";
    std::vector<uint8_t> body;
    body.push_back(static_cast<uint8_t>(msg.size()));
    body.insert(body.end(), msg.begin(), msg.end());
    queue_protocol(PPP_PAP, cp_packet(ok ? 2 : 3, id, body));
    if (!ok) {
        set_phase(UserPppPhase::Failed, "PAP authentication failed for '" + user + "'");
        return;
    }
    pap_ok_ = true;
    authenticated_user_ = user;
    set_phase(UserPppPhase::Ipcp, "PAP authenticated '" + user + "'; starting IPCP");
    send_ipcp_request(now_ms, true);
}

void UserPppServer::process_ipcp(const std::vector<uint8_t>& in, uint64_t now_ms) {
    (void)now_ms;
    if (phase_ != UserPppPhase::Ipcp && phase_ != UserPppPhase::Network) return;
    if (in.size() < 4) return;
    uint8_t code = in[0], id = in[1];
    uint16_t len = be16(&in[2]);
    if (len < 4 || len > in.size()) return;
    std::vector<uint8_t> body(in.begin() + 4, in.begin() + len);

    if (code == 1) {
        uint32_t peer=0,d1=0,d2=0; parse_ipv4(cfg_.peer_ip,peer); parse_ipv4(cfg_.dns1,d1); parse_ipv4(cfg_.dns2,d2);
        std::vector<uint8_t> reject, nak;
        bool saw_ip = false;
        size_t i = 0;
        while (i < body.size()) {
            if (i + 2 > body.size()) return;
            uint8_t t = body[i], l = body[i+1];
            if (l < 2 || i + l > body.size()) return;
            const uint8_t* o = body.data() + i;
            if (t == 3 && l == 6) {
                saw_ip = true;
                if (read_be32(o+2) != peer) { nak.insert(nak.end(), {3,6}); put_be32(nak, peer); }
            } else if (t == 129 && l == 6) {
                if (read_be32(o+2) != d1) { nak.insert(nak.end(), {129,6}); put_be32(nak, d1); }
            } else if (t == 131 && l == 6) {
                if (read_be32(o+2) != d2) { nak.insert(nak.end(), {131,6}); put_be32(nak, d2); }
            } else {
                reject.insert(reject.end(), body.begin() + static_cast<long>(i), body.begin() + static_cast<long>(i+l));
            }
            i += l;
        }
        if (!reject.empty()) { queue_protocol(PPP_IPCP, cp_packet(4,id,reject)); return; }
        if (!saw_ip) { nak.insert(nak.end(), {3,6}); put_be32(nak, peer); }
        if (!nak.empty()) { queue_protocol(PPP_IPCP, cp_packet(3,id,nak)); return; }
        queue_protocol(PPP_IPCP, cp_packet(2,id,body));
        peer_ipcp_acked_ = true;
        maybe_ipcp_open();
        return;
    }
    if (code == 2 && id == ipcp_id_) {
        auto expected = last_ipcp_request_;
        if (!expected.empty()) expected[0] = 2;
        if (in != expected) return;
        our_ipcp_acked_ = true;
        maybe_ipcp_open();
        return;
    }
    if ((code == 3 || code == 4) && id == ipcp_id_) {
        set_phase(UserPppPhase::Failed, "peer rejected server IPv4 IPCP configuration");
        return;
    }
    if (code == 5) {
        queue_protocol(PPP_IPCP, cp_packet(6,id,body));
        set_phase(UserPppPhase::Terminating, "peer terminated IPCP");
    }
}

void UserPppServer::maybe_ipcp_open() {
    if (our_ipcp_acked_ && peer_ipcp_acked_)
        set_phase(UserPppPhase::Network, "PPP IPv4 is UP: " + cfg_.peer_ip + " <-> " + cfg_.local_ip);
}

void UserPppServer::protocol_reject(uint16_t protocol, const std::vector<uint8_t>& info) {
    std::vector<uint8_t> b;
    put_be16(b, protocol);
    // Keep reject packets small on slow links.
    size_t n = std::min<size_t>(info.size(), 64);
    b.insert(b.end(), info.begin(), info.begin() + static_cast<long>(n));
    queue_protocol(PPP_LCP, cp_packet(8, next_id_++, b));
}

std::vector<uint8_t> UserPppServer::take_serial_tx() {
    std::vector<uint8_t> out;
    out.reserve(serial_tx_.size());
    while (!serial_tx_.empty()) { out.push_back(serial_tx_.front()); serial_tx_.pop_front(); }
    return out;
}

std::vector<std::vector<uint8_t>> UserPppServer::take_ip_packets() {
    auto out = std::move(ip_rx_);
    ip_rx_.clear();
    return out;
}

void UserPppServer::feed_ip_packet(const uint8_t* data, size_t len) {
    if (!network_up() || !data || len < 20 || len > 65535 || (data[0] >> 4) != 4) return;

    const size_t ihl = size_t(data[0] & 0x0f) * 4;
    if (ihl < 20 || ihl > len) return;
    const uint16_t ip_total = be16(data + 2);
    if (ip_total < ihl || ip_total > len) return;

    // PPP MRU applies to the entire PPP information field, i.e. the whole IP
    // datagram. Internet packets are often ~1500 bytes while this slow-modem
    // profile deliberately negotiates a much smaller MRU. Fragment here so a
    // forwarded TCP/UDP reply never violates the peer's negotiated PPP MRU.
    if (ip_total <= cfg_.mtu) {
        queue_protocol(PPP_IP, std::vector<uint8_t>(data, data + ip_total));
        return;
    }

    if (cfg_.mtu <= ihl + 8) return;
    const size_t max_payload = ((size_t(cfg_.mtu) - ihl) / 8) * 8;
    if (max_payload < 8) return;

    const uint16_t old_fo = be16(data + 6);
    const uint16_t base_off_units = uint16_t(old_fo & 0x1fff);
    const bool old_more = (old_fo & 0x2000) != 0;
    const size_t payload_len = ip_total - ihl;

    auto checksum16 = [](const uint8_t* p, size_t n) -> uint16_t {
        uint32_t sum = 0;
        while (n >= 2) { sum += (uint16_t(p[0]) << 8) | p[1]; p += 2; n -= 2; }
        if (n) sum += uint16_t(p[0]) << 8;
        while (sum >> 16) sum = (sum & 0xffffu) + (sum >> 16);
        return uint16_t(~sum);
    };

    for (size_t off = 0; off < payload_len; ) {
        const size_t chunk = std::min(max_payload, payload_len - off);
        const bool more = (off + chunk < payload_len) || old_more;
        std::vector<uint8_t> frag;
        frag.reserve(ihl + chunk);
        frag.insert(frag.end(), data, data + ihl);
        frag.insert(frag.end(), data + ihl + off, data + ihl + off + chunk);

        const uint16_t total = uint16_t(ihl + chunk);
        frag[2] = uint8_t(total >> 8); frag[3] = uint8_t(total);
        // Clear DF because this router is deliberately fragmenting for the
        // low-MRU last hop. Preserve/rebase an existing fragment offset.
        uint16_t fo = uint16_t(base_off_units + off / 8);
        if (more) fo |= 0x2000;
        frag[6] = uint8_t(fo >> 8); frag[7] = uint8_t(fo);
        frag[10] = frag[11] = 0;
        const uint16_t c = checksum16(frag.data(), ihl);
        frag[10] = uint8_t(c >> 8); frag[11] = uint8_t(c);
        queue_protocol(PPP_IP, frag);
        off += chunk;
    }
}

} // namespace v92
