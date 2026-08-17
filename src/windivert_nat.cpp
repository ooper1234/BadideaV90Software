#ifdef _WIN32

#include "windivert_nat.hpp"

// IMPORTANT (MinGW + WinDivert): WinDivert 2.2.x defines SAL annotation
// macros such as __in/__out/__inout when compiling with MinGW. libstdc++
// itself uses identifiers such as __in and __out inside <algorithm>,
// <istream>, and <ostream>. If windivert.h is included first, those macros
// erase the identifiers and make the C++ standard library fail to compile.
// Include every C++ standard header we need before windivert.h, then undef
// WinDivert's annotation macros once its declarations have been parsed.
#include <algorithm>
#include <chrono>
#include <cstring>
#include <sstream>
#include <tuple>

#ifdef V92_HAVE_WINDIVERT

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <windivert.h>

#ifdef __MINGW32__
// WinDivert 2.2.x defines these to empty tokens for MinGW. Do not let the
// macros leak into any headers/code included after this point.
#ifdef __in
#undef __in
#endif
#ifdef __in_opt
#undef __in_opt
#endif
#ifdef __out
#undef __out
#endif
#ifdef __out_opt
#undef __out_opt
#endif
#ifdef __inout
#undef __inout
#endif
#ifdef __inout_opt
#undef __inout_opt
#endif
#endif

namespace v92 {
namespace {
uint64_t now_ms() {
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

uint16_t rd16(const uint8_t* p) { return (uint16_t(p[0]) << 8) | p[1]; }
void wr16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
uint32_t rd32raw(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
void wr32raw(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }

std::string winerr(DWORD code) {
    char* text = nullptr;
    DWORD n = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                             FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, code, 0, reinterpret_cast<LPSTR>(&text), 0, nullptr);
    std::string s = n && text ? std::string(text, n) : ("Windows error " + std::to_string(code));
    if (text) LocalFree(text);
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
    return s;
}

bool ipv4_header(const std::vector<uint8_t>& p, size_t& ihl, uint16_t& total, uint8_t& proto) {
    if (p.size() < 20 || (p[0] >> 4) != 4) return false;
    ihl = size_t(p[0] & 0x0f) * 4;
    if (ihl < 20 || ihl > p.size()) return false;
    total = rd16(&p[2]);
    if (total < ihl || total > p.size()) return false;
    proto = p[9];
    return true;
}
}

bool WinDivertNat::FlowKey::operator<(const FlowKey& o) const {
    return std::tie(proto, src, dst, sport, dport) < std::tie(o.proto, o.src, o.dst, o.sport, o.dport);
}
bool WinDivertNat::RevKey::operator<(const RevKey& o) const {
    return std::tie(proto, nat_port, remote, remote_port) < std::tie(o.proto, o.nat_port, o.remote, o.remote_port);
}

WinDivertNat::~WinDivertNat() { close(); }
void WinDivertNat::set_error(const std::string& s) { last_error_ = s; }

std::string WinDivertNat::public_ip() const {
    in_addr a{}; a.s_addr = public_addr_;
    char b[INET_ADDRSTRLEN]{};
    return InetNtopA(AF_INET, &a, b, sizeof(b)) ? std::string(b) : std::string();
}

bool WinDivertNat::open(const std::string& peer_ip) {
    close();
    last_error_.clear();

    in_addr peer{};
    if (InetPtonA(AF_INET, peer_ip.c_str(), &peer) != 1) {
        set_error("invalid PPP peer IPv4: " + peer_ip);
        return false;
    }
    peer_addr_ = peer.s_addr;

    // Ask Windows which interface/source address it would use for Internet.
    SOCKADDR_INET dst{};
    dst.Ipv4.sin_family = AF_INET;
    InetPtonA(AF_INET, "1.1.1.1", &dst.Ipv4.sin_addr);
    MIB_IPFORWARD_ROW2 route{};
    SOCKADDR_INET src{};
    DWORD rr = GetBestRoute2(nullptr, 0, nullptr, &dst, 0, &route, &src);
    if (rr != NO_ERROR || src.si_family != AF_INET || src.Ipv4.sin_addr.s_addr == 0) {
        set_error("GetBestRoute2 could not find an Internet-facing IPv4 interface");
        return false;
    }
    public_addr_ = src.Ipv4.sin_addr.s_addr;
    public_ifindex_ = route.InterfaceIndex;

    HMODULE d = LoadLibraryExW(L"WinDivert.dll", nullptr,
                               LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!d) d = LoadLibraryW(L"WinDivert.dll");
    if (!d) {
        set_error("WinDivert.dll unavailable: " + winerr(GetLastError()));
        return false;
    }
    dll_ = d;
#define LOAD(name, field) do { \
    field = reinterpret_cast<decltype(field)>(GetProcAddress(d, name)); \
    if (!field) { set_error(std::string("WinDivert.dll missing export ") + name); close(); return false; } \
} while (0)
    LOAD("WinDivertOpen", wd_open_);
    LOAD("WinDivertRecv", wd_recv_);
    LOAD("WinDivertSend", wd_send_);
    LOAD("WinDivertShutdown", wd_shutdown_);
    LOAD("WinDivertClose", wd_close_);
    LOAD("WinDivertHelperCalcChecksums", wd_calc_);
#undef LOAD

    char ipbuf[INET_ADDRSTRLEN]{};
    InetNtopA(AF_INET, &src.Ipv4.sin_addr, ipbuf, sizeof(ipbuf));
    std::ostringstream filter;
    // Only divert the port range owned by this NAT, plus ICMP replies to the
    // selected public address. Unrelated host traffic is never intercepted.
    filter << "inbound and ip.DstAddr == " << ipbuf
           << " and (((tcp or udp) and tcp.DstPort >= 20000 and tcp.DstPort <= 39999)"
              " or ((udp) and udp.DstPort >= 20000 and udp.DstPort <= 39999) or icmp)";
    // The first combined tcp/udp expression is not valid for UDP because it
    // references tcp.DstPort, so use an explicit final filter instead.
    filter.str(""); filter.clear();
    filter << "inbound and ip.DstAddr == " << ipbuf
           << " and ((tcp and tcp.DstPort >= 20000 and tcp.DstPort <= 39999)"
              " or (udp and udp.DstPort >= 20000 and udp.DstPort <= 39999) or icmp)";

    handle_ = wd_open_(filter.str().c_str(), WINDIVERT_LAYER_NETWORK, 0, 0);
    if (handle_ == INVALID_HANDLE_VALUE || !handle_) {
        DWORD e = GetLastError(); handle_ = nullptr;
        set_error("WinDivertOpen failed: " + winerr(e));
        close();
        return false;
    }

    stopping_ = false;
    opened_ = true;
    recv_thread_ = std::thread([this]{ recv_loop(); });
    return true;
}

void WinDivertNat::close() {
    stopping_ = true;
    opened_ = false;
    if (handle_ && wd_shutdown_) wd_shutdown_(handle_, WINDIVERT_SHUTDOWN_BOTH);
    if (recv_thread_.joinable()) recv_thread_.join();
    if (handle_ && wd_close_) wd_close_(handle_);
    handle_ = nullptr;
    if (dll_) FreeLibrary(reinterpret_cast<HMODULE>(dll_));
    dll_ = nullptr;
    wd_open_ = nullptr; wd_recv_ = nullptr; wd_send_ = nullptr;
    wd_shutdown_ = nullptr; wd_close_ = nullptr; wd_calc_ = nullptr;
    std::lock_guard<std::mutex> lk(mu_);
    forward_.clear(); reverse_.clear(); to_peer_.clear();
}

void WinDivertNat::expire_old(uint64_t now) {
    // TCP mappings are cheap; keep them for 15 minutes. UDP/ICMP expire after
    // 2 minutes. This is plenty for dial-up while preventing an unbounded map.
    for (auto it = forward_.begin(); it != forward_.end(); ) {
        uint64_t age = now - it->second.last_seen_ms;
        uint64_t ttl = (it->first.proto == IPPROTO_TCP) ? 15ull*60ull*1000ull : 2ull*60ull*1000ull;
        if (age > ttl) {
            Mapping m = it->second;
            RevKey r{m.flow.proto, m.nat_port, m.flow.dst, m.flow.dport};
            reverse_.erase(r);
            it = forward_.erase(it);
        } else ++it;
    }
}

uint16_t WinDivertNat::get_or_create_mapping(const FlowKey& key, uint64_t now) {
    std::lock_guard<std::mutex> lk(mu_);
    expire_old(now);
    auto f = forward_.find(key);
    if (f != forward_.end()) {
        f->second.last_seen_ms = now;
        RevKey r{key.proto, f->second.nat_port, key.dst, key.dport};
        auto ri = reverse_.find(r); if (ri != reverse_.end()) ri->second.last_seen_ms = now;
        return f->second.nat_port;
    }
    for (unsigned tries=0; tries<20000; ++tries) {
        uint16_t p = next_nat_port_++;
        if (next_nat_port_ > 39999) next_nat_port_ = 20000;
        RevKey r{key.proto, p, key.dst, key.dport};
        if (reverse_.find(r) != reverse_.end()) continue;
        Mapping m{key,p,now};
        forward_[key] = m; reverse_[r] = m;
        return p;
    }
    return 0;
}

bool WinDivertNat::inject_outbound(std::vector<uint8_t>& packet) {
    if (!handle_ || !wd_send_ || !wd_calc_) return false;
    WINDIVERT_ADDRESS a{};
    a.Outbound = 1;
    a.Network.IfIdx = public_ifindex_;
    a.Network.SubIfIdx = 0;
    wd_calc_(packet.data(), (UINT)packet.size(), &a, 0);
    UINT sent=0;
    if (!wd_send_(handle_, packet.data(), (UINT)packet.size(), &sent, &a) || sent != packet.size()) {
        set_error("WinDivertSend failed: " + winerr(GetLastError()));
        return false;
    }
    return true;
}

bool WinDivertNat::send_from_peer(const std::vector<uint8_t>& in) {
    if (!opened_.load()) return false;
    std::vector<uint8_t> p = in;
    size_t ihl=0; uint16_t total=0; uint8_t proto=0;
    if (!ipv4_header(p,ihl,total,proto)) return false;
    p.resize(total);
    uint32_t src = rd32raw(&p[12]), dst = rd32raw(&p[16]);
    if (src != peer_addr_) return false;
    uint16_t frag = rd16(&p[6]);
    if ((frag & 0x1fff) != 0) return false; // non-first fragments need a fragment map

    uint64_t now=now_ms();
    if (proto == IPPROTO_TCP || proto == IPPROTO_UDP) {
        if (p.size() < ihl + 8) return false;
        uint16_t sport = rd16(&p[ihl]), dport = rd16(&p[ihl+2]);
        FlowKey k{proto,src,dst,sport,dport};
        uint16_t np = get_or_create_mapping(k,now); if (!np) return false;
        wr32raw(&p[12],public_addr_); wr16(&p[ihl],np);
        return inject_outbound(p);
    }
    if (proto == IPPROTO_ICMP) {
        if (p.size() < ihl+8 || p[ihl] != 8) return false; // echo request only
        uint16_t ident = rd16(&p[ihl+4]);
        FlowKey k{proto,src,dst,ident,0};
        uint16_t np = get_or_create_mapping(k,now); if (!np) return false;
        wr32raw(&p[12],public_addr_); wr16(&p[ihl+4],np);
        return inject_outbound(p);
    }
    return false;
}

bool WinDivertNat::translate_inbound(std::vector<uint8_t>& p, uint64_t now) {
    size_t ihl=0; uint16_t total=0; uint8_t proto=0;
    if (!ipv4_header(p,ihl,total,proto)) return false;
    p.resize(total);
    if (rd32raw(&p[16]) != public_addr_) return false;
    uint32_t remote = rd32raw(&p[12]);

    Mapping m{}; bool found=false;
    if (proto == IPPROTO_TCP || proto == IPPROTO_UDP) {
        if (p.size() < ihl+8) return false;
        uint16_t remotePort=rd16(&p[ihl]), natPort=rd16(&p[ihl+2]);
        RevKey r{proto,natPort,remote,remotePort};
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it=reverse_.find(r); if(it!=reverse_.end()){m=it->second;it->second.last_seen_ms=now;auto fi=forward_.find(m.flow);if(fi!=forward_.end())fi->second.last_seen_ms=now;found=true;}
        }
        if(!found)return false;
        wr32raw(&p[16],peer_addr_); wr16(&p[ihl+2],m.flow.sport);
    } else if (proto == IPPROTO_ICMP) {
        if (p.size()<ihl+8 || p[ihl]!=0) return false; // echo reply only
        uint16_t natId=rd16(&p[ihl+4]);
        RevKey r{proto,natId,remote,0};
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it=reverse_.find(r); if(it!=reverse_.end()){m=it->second;it->second.last_seen_ms=now;found=true;}
        }
        if(!found)return false;
        wr32raw(&p[16],peer_addr_); wr16(&p[ihl+4],m.flow.sport);
    } else return false;

    WINDIVERT_ADDRESS dummy{};
    wd_calc_(p.data(),(UINT)p.size(),&dummy,0);
    return true;
}

void WinDivertNat::recv_loop() {
    std::vector<uint8_t> buf(65535);
    while (!stopping_.load() && handle_ && wd_recv_) {
        WINDIVERT_ADDRESS a{}; UINT n=0;
        if (!wd_recv_(handle_,buf.data(),(UINT)buf.size(),&n,&a)) {
            DWORD e=GetLastError();
            if(stopping_.load() || e==ERROR_OPERATION_ABORTED || e==ERROR_INVALID_HANDLE) break;
            set_error("WinDivertRecv failed: "+winerr(e));
            continue;
        }
        std::vector<uint8_t> p(buf.begin(),buf.begin()+n);
        if (translate_inbound(p,now_ms())) {
            std::lock_guard<std::mutex> lk(mu_);
            if(to_peer_.size()<256) to_peer_.push_back(std::move(p));
        } else {
            // The filter is narrow, but unmatched packets may still belong to
            // another host application using the same port. Put them back.
            UINT sent=0;
            wd_send_(handle_,buf.data(),n,&sent,&a);
        }
    }
}

bool WinDivertNat::take_to_peer(std::vector<uint8_t>& packet) {
    std::lock_guard<std::mutex> lk(mu_);
    if(to_peer_.empty()) return false;
    packet=std::move(to_peer_.front());to_peer_.pop_front();return true;
}

} // namespace v92

#else // V92_HAVE_WINDIVERT

namespace v92 {
bool WinDivertNat::FlowKey::operator<(const FlowKey& o) const {
    return std::tie(proto, src, dst, sport, dport) < std::tie(o.proto, o.src, o.dst, o.sport, o.dport);
}
bool WinDivertNat::RevKey::operator<(const RevKey& o) const {
    return std::tie(proto, nat_port, remote, remote_port) < std::tie(o.proto, o.nat_port, o.remote, o.remote_port);
}
WinDivertNat::~WinDivertNat() { close(); }
bool WinDivertNat::open(const std::string&) { last_error_ = "WinDivert NAT support was not included in this build"; return false; }
void WinDivertNat::close() { opened_ = false; }
bool WinDivertNat::send_from_peer(const std::vector<uint8_t>&) { return false; }
bool WinDivertNat::take_to_peer(std::vector<uint8_t>&) { return false; }
std::string WinDivertNat::public_ip() const { return {}; }
void WinDivertNat::recv_loop() {}
uint16_t WinDivertNat::get_or_create_mapping(const FlowKey&, uint64_t) { return 0; }
bool WinDivertNat::translate_inbound(std::vector<uint8_t>&, uint64_t) { return false; }
void WinDivertNat::expire_old(uint64_t) {}
bool WinDivertNat::inject_outbound(std::vector<uint8_t>&) { return false; }
void WinDivertNat::set_error(const std::string& s) { last_error_ = s; }
} // namespace v92

#endif // V92_HAVE_WINDIVERT

#endif // _WIN32
