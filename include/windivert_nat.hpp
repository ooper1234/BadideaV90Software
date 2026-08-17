#pragma once

#ifdef _WIN32

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <atomic>

namespace v92 {

class WinDivertNat {
public:
    WinDivertNat() = default;
    ~WinDivertNat();
    WinDivertNat(const WinDivertNat&) = delete;
    WinDivertNat& operator=(const WinDivertNat&) = delete;

    // Starts a small IPv4 user-mode NAT for the single PPP peer. This is only
    // used when Windows ICS/WinNAT is unavailable. Requires Administrator.
    bool open(const std::string& peer_ip);
    void close();
    bool is_open() const { return opened_.load(); }

    // Packet from the PPP peer to the Internet.
    bool send_from_peer(const std::vector<uint8_t>& packet);

    // Packets translated from the Internet back to the PPP peer.
    bool take_to_peer(std::vector<uint8_t>& packet);

    const std::string& last_error() const { return last_error_; }
    std::string public_ip() const;

private:
    struct FlowKey {
        uint8_t proto = 0;
        uint32_t src = 0, dst = 0; // network byte order
        uint16_t sport = 0, dport = 0; // host order
        bool operator<(const FlowKey& o) const;
    };
    struct RevKey {
        uint8_t proto = 0;
        uint16_t nat_port = 0;
        uint32_t remote = 0; // network byte order
        uint16_t remote_port = 0;
        bool operator<(const RevKey& o) const;
    };
    struct Mapping {
        FlowKey flow;
        uint16_t nat_port = 0;
        uint64_t last_seen_ms = 0;
    };

    void recv_loop();
    uint16_t get_or_create_mapping(const FlowKey& key, uint64_t now);
    bool translate_inbound(std::vector<uint8_t>& packet, uint64_t now);
    void expire_old(uint64_t now);
    bool inject_outbound(std::vector<uint8_t>& packet);
    void set_error(const std::string& s);

    void* dll_ = nullptr;
    void* handle_ = nullptr;
    std::atomic_bool opened_{false};
    std::atomic_bool stopping_{false};
    std::thread recv_thread_;

    std::string last_error_;
    uint32_t public_addr_ = 0; // network byte order
    uint32_t peer_addr_ = 0;   // network byte order
    uint32_t public_ifindex_ = 0;
    uint16_t next_nat_port_ = 20000;

    mutable std::mutex mu_;
    std::map<FlowKey, Mapping> forward_;
    std::map<RevKey, Mapping> reverse_;
    std::deque<std::vector<uint8_t>> to_peer_;

    // WinDivert functions are loaded dynamically so the executable can still
    // start when the fallback DLL is absent and ICS works normally.
    using OpenFn = void* (*)(const char*, int, short, unsigned long long);
    using RecvFn = int (*)(void*, void*, unsigned int, unsigned int*, void*);
    using SendFn = int (*)(void*, const void*, unsigned int, unsigned int*, const void*);
    using ShutdownFn = int (*)(void*, int);
    using CloseFn = int (*)(void*);
    using CalcFn = int (*)(void*, unsigned int, void*, unsigned long long);

    OpenFn wd_open_ = nullptr;
    RecvFn wd_recv_ = nullptr;
    SendFn wd_send_ = nullptr;
    ShutdownFn wd_shutdown_ = nullptr;
    CloseFn wd_close_ = nullptr;
    CalcFn wd_calc_ = nullptr;
};

} // namespace v92

#endif
