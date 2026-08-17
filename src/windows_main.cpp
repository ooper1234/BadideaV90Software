#ifdef _WIN32

#include "g711.hpp"
#include "live_modem.hpp"
#include "ppp_userspace.hpp"
#include "sip_rtp.hpp"
#include "span_v22.hpp"
#include "span_v8.hpp"
#include "wintun_adapter.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace v92;

namespace {
std::atomic_bool g_quit{false};
BOOL WINAPI ctrl_handler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT ||
        type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT) {
        g_quit = true;
        return TRUE;
    }
    return FALSE;
}

std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    size_t p = 0;
    while (p < s.size() && std::isspace(static_cast<unsigned char>(s[p]))) ++p;
    return s.substr(p);
}

std::map<std::string, std::string> load_env(const std::string& path) {
    std::map<std::string, std::string> out;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq));
        std::string v = trim(line.substr(eq + 1));
        if (v.size() >= 2 && ((v.front() == '"' && v.back() == '"') || (v.front() == '\'' && v.back() == '\'')))
            v = v.substr(1, v.size() - 2);
        out[k] = v;
    }
    return out;
}

bool parse_bool(std::string v, bool d = false) {
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    if (v == "1" || v == "yes" || v == "true" || v == "on") return true;
    if (v == "0" || v == "no" || v == "false" || v == "off") return false;
    return d;
}

bool parse_host_port(const std::string& s, std::string& host, uint16_t& port) {
    auto c = s.rfind(':');
    if (c != std::string::npos && s.find(':') == c) {
        host = s.substr(0, c);
        if (host.empty()) return false;
        try {
            int p = std::stoi(s.substr(c + 1));
            if (p < 1 || p > 65535) return false;
            port = static_cast<uint16_t>(p);
        } catch (...) { return false; }
    } else host = s;
    return !host.empty();
}

LiveMode parse_mode(const std::string& m) {
    if (m == "v90") return LiveMode::V90Digital;
    if (m == "v92") return LiveMode::V92QuickConnect;
    if (m == "v22bis") return LiveMode::V22bis_2400;
    if (m == "v22") return LiveMode::V22_1200;
    if (m == "v21") return LiveMode::V21_300;
    return LiveMode::Auto;
}

uint64_t now_ms() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

bool ensure_internet_sharing(const std::string& private_prefix) {
    // Prefer WinNAT when its CIM provider is installed. On Windows editions/builds
    // without MSFT_NetNat (notably machines where New-NetNat reports
    // "Invalid class"), windows\Ensure-Internet.ps1 falls back to the public
    // Internet Connection Sharing API.
    std::string cmd =
        "powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "
        "\"windows\\Ensure-Internet.ps1\" -PrivateAlias \"v92isp\" -PrivatePrefix \"" +
        private_prefix + "\"";
    int rc = std::system(cmd.c_str());
    return rc == 0;
}

bool packet_is_ipv4_for_peer(const std::vector<uint8_t>& p, const std::string& peer) {
    if (p.size() < 20 || (p[0] >> 4) != 4) return false;
    in_addr a{};
    if (InetPtonA(AF_INET, peer.c_str(), &a) != 1) return false;
    return std::memcmp(&p[16], &a.s_addr, 4) == 0;
}

void print_features() {
    std::cout << "windows-native=yes\n"
              << "winsock-sip-rtp=yes\n"
              << "wintun-ip=yes\n"
              << "userspace-ppp=yes\n"
              << "builtin-v21=yes\n"
              << "stable-auto-v8=" << ((span_v8_available() && span_v22_available()) ? "yes" : "no") << "\n"
              << "spandsp-v22=" << (span_v22_available() ? "yes" : "no") << "\n"
              << "v42-lapm=" << (span_v22_available() ? "yes" : "no") << "\n"
              << "sip-digest=" << (sip_digest_compiled() ? "yes" : "no") << "\n"
              << "v92-quickconnect-experimental=yes\n"
              << "v90-v92-full-data=no\n";
}

void usage() {
    std::cout <<
        "v92isp-windows.exe [--config FILE] [--features] [--debug]\n\n"
        "Native Windows SIP/RTP dial-up ISP. No WSL required.\n"
        "Default config: config\\v92isp-windows.env\n";
}
}

int main(int argc, char** argv) {
    std::string cfg_path = "config\\v92isp-windows.env";
    bool features = false, cli_debug = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--config" && i + 1 < argc) cfg_path = argv[++i];
        else if (a == "--features") features = true;
        else if (a == "--debug") cli_debug = true;
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else { std::cerr << "unknown option: " << a << "\n"; usage(); return 2; }
    }

    WSADATA wd{};
    if (WSAStartup(MAKEWORD(2,2), &wd) != 0) {
        std::cerr << "[WIN] WSAStartup failed\n";
        return 1;
    }
    SetConsoleCtrlHandler(ctrl_handler, TRUE);

    if (features) { print_features(); WSACleanup(); return 0; }

    auto e = load_env(cfg_path);
    auto get = [&](const std::string& k, const std::string& d) {
        auto it = e.find(k); return it == e.end() ? d : it->second;
    };

    SipRtpConfig sc;
    sc.bind_ip = get("BIND_IP", "192.168.2.55");
    sc.advertise_ip = get("ADVERTISE_IP", sc.bind_ip);
    sc.sip_port = static_cast<uint16_t>(std::stoi(get("SIP_PORT", "5060")));
    sc.rtp_port = static_cast<uint16_t>(std::stoi(get("RTP_PORT", "40000")));
    sc.registrar_user = get("SIP_USER", "101");
    sc.registrar_password = get("SIP_PASSWORD", "");
    sc.registrar_expires = static_cast<unsigned>(std::stoul(get("REGISTER_EXPIRES", "300")));
    sc.debug = cli_debug || parse_bool(get("DEBUG", "1"), true);
    std::string registrar = get("SIP_SERVER", "192.168.2.40:5060");
    if (!parse_host_port(registrar, sc.registrar_host, sc.registrar_port)) {
        std::cerr << "[CFG] invalid SIP_SERVER=" << registrar << "\n";
        WSACleanup(); return 2;
    }

    LiveMode mode = parse_mode(get("MODE", "auto"));
    if ((mode == LiveMode::V22bis_2400 || mode == LiveMode::V22_1200 || mode == LiveMode::Auto) &&
        (!span_v8_available() || !span_v22_available())) {
        if (mode == LiveMode::Auto) {
            std::cerr << "[MODEM] SpanDSP unavailable; auto mode falling back to built-in V.21 300\n";
            mode = LiveMode::V21_300;
        } else {
            std::cerr << "[MODEM] selected V.22/V.22bis but this build lacks SpanDSP\n";
            WSACleanup(); return 2;
        }
    }

    UserPppConfig pc;
    pc.local_ip = get("PPP_LOCAL_IP", "10.77.0.1");
    pc.peer_ip = get("PPP_PEER_IP", "10.77.0.2");
    pc.dns1 = get("DNS1", "1.1.1.1");
    pc.dns2 = get("DNS2", "8.8.8.8");
    pc.mtu = static_cast<uint16_t>(std::stoi(get("PPP_MTU", "296")));
    pc.require_pap = parse_bool(get("REQUIRE_PAP", "0"));
    pc.pap_secrets = get("PAP_SECRETS", "config\\pap-secrets.txt");
    pc.debug = sc.debug;

    std::cout << "v92isp native Windows Internet server\n"
              << "  LAN/SIP IP : " << sc.bind_ip << "\n"
              << "  SIP proxy  : " << sc.registrar_host << ":" << sc.registrar_port << "\n"
              << "  SIP user   : " << sc.registrar_user << "\n"
              << "  SIP pass   : " << (sc.registrar_password.empty() ? "<empty>" : "<set>") << "\n"
              << "  SIP/RTP    : UDP " << sc.sip_port << " / " << sc.rtp_port << ", PCMU/8000\n"
              << "  PPP        : " << pc.peer_ip << " <-> " << pc.local_ip << " MTU " << pc.mtu << "\n"
              << "  Internet   : Wintun + Windows NAT/ICS (NO WSL)\n";

    WintunAdapter tun;
    if (!tun.open(L"v92isp", pc.local_ip, 24)) {
        std::cerr << "[TUN] " << tun.last_error() << "\n"
                  << "[TUN] Put the official amd64 wintun.dll beside v92isp-windows.exe and run as Administrator.\n";
        WSACleanup(); return 1;
    }
    std::cout << "[TUN] v92isp adapter ready at " << pc.local_ip << "/24\n";
    std::string private_prefix = pc.local_ip.substr(0, pc.local_ip.rfind('.') + 1) + "0/24";
    if (!ensure_internet_sharing(private_prefix)) {
        std::cerr << "[NAT] WARNING: Windows Internet sharing setup failed.\n"
                  << "[NAT] Run windows\\Ensure-Internet.ps1 as Administrator for diagnostics.\n";
    } else {
        std::cout << "[NAT] Internet sharing ready for " << private_prefix << "\n";
    }

    SipRtpServer net(sc);
    if (!net.open()) {
        std::cerr << "[NET] " << net.last_error() << "\n";
        WSACleanup(); return 1;
    }
    std::cout << "[SIP] listening udp://" << sc.bind_ip << ":" << sc.sip_port << "\n";
    std::cout << "[RTP] listening udp://" << sc.bind_ip << ":" << sc.rtp_port << " PCMU/8000\n";
    if (net.registration_enabled()) {
        std::cout << "[SIP] registrar " << sc.registrar_host << ":" << sc.registrar_port << " user " << sc.registrar_user << "\n";
        if (!net.send_register()) std::cerr << "[SIP] initial REGISTER send failed\n";
    }

    LiveModem modem(mode);
    UserPppServer ppp(pc);
    bool ppp_started = false;
    bool first_rtp = true;
    unsigned rx_packets = 0, rx_lost = 0;
    bool warned_no_rx = false, warned_rx_stalled = false;
    uint64_t diag_samples = 0; long double diag_sumsq = 0; int diag_peak = 0; unsigned diag_packets = 0;

    using clock = std::chrono::steady_clock;
    auto next_tx = clock::now();
    auto call_started_at = clock::now();
    auto last_rtp_at = clock::time_point{};
    auto next_rtp_diag = clock::now() + std::chrono::seconds(1);
    auto next_register = clock::now() + std::chrono::seconds(std::max<unsigned>(60, sc.registrar_expires / 2));

    while (!g_quit) {
        fd_set rfds; FD_ZERO(&rfds); FD_SET(net.sip_fd(), &rfds); FD_SET(net.rtp_fd(), &rfds);
        timeval tv{}; tv.tv_sec = 0; tv.tv_usec = 10000; // 10 ms keeps RTP timing smooth.
        int sr = select(0, &rfds, nullptr, nullptr, &tv);
        if (sr == SOCKET_ERROR) {
            std::cerr << "[WIN] select failed: " << WSAGetLastError() << "\n";
            break;
        }

        if (FD_ISSET(net.sip_fd(), &rfds)) {
            for (;;) {
                auto ev = net.handle_sip();
                if (ev.type == SipEventType::None) break;
                if (ev.type == SipEventType::CallStarted) {
                    std::cout << "[SIP] call answered; RTP peer " << ev.detail << "\n";
                    std::cout << "[RTP] media transport started\n";
                    std::cout << "[RTP] remote endpoint = " << ev.detail << "\n";
                    modem.start_call(); ppp.reset(); ppp_started = false;
                    next_tx = clock::now(); first_rtp = true; rx_packets = rx_lost = 0;
                    call_started_at = clock::now(); last_rtp_at = clock::time_point{};
                    next_rtp_diag = clock::now() + std::chrono::seconds(1);
                    diag_samples = 0; diag_sumsq = 0; diag_peak = 0; diag_packets = 0;
                    warned_no_rx = warned_rx_stalled = false;

                    // Immediately transmit the first legitimate modem PCM answer frame
                    auto audio = modem.next_tx_pcmu(160);
                    if (net.send_rtp_pcmu(audio, first_rtp)) {
                        std::cout << "[RTP] first outbound RTP sent\n";
                        first_rtp = false;
                        next_tx = clock::now() + std::chrono::milliseconds(20);
                    }
                } else if (ev.type == SipEventType::CallEnded) {
                    std::cout << "[SIP] call ended; RTP packets=" << rx_packets << " lost=" << rx_lost << "\n";
                    modem.end_call(); ppp.reset(); ppp_started = false;
                } else if (ev.type == SipEventType::RegistrationOk) std::cout << "[SIP] " << ev.detail << "\n";
                else if (ev.type == SipEventType::RegistrationFailed) std::cerr << "[SIP] registration: " << ev.detail << "\n";
                else if (ev.type == SipEventType::Ack && sc.debug) std::cout << "[SIP] ACK\n";
                else if (ev.type == SipEventType::Error) std::cerr << "[SIP] " << ev.detail << "\n";
            }
        }

        // Drain RTP packets on every loop iteration (socket is non-blocking)
        for (;;) {
            RtpAudio a;
            if (!net.recv_rtp(a)) break;
            if (rx_packets == 0) {
                std::cout << "[RTP] first inbound RTP received\n";
            }
            ++rx_packets;
            if (a.had_gap) {
                rx_lost += a.missing_packets;
                std::vector<int16_t> z(static_cast<size_t>(a.missing_packets) * 160, 0);
                modem.receive_pcm(z);
            }
            std::vector<int16_t> pcm; pcm.reserve(a.pcmu.size());
            for (uint8_t u : a.pcmu) pcm.push_back(ulaw_to_linear(u));
            last_rtp_at = clock::now(); ++diag_packets;
            for (int16_t x : pcm) {
                long double y = x; diag_sumsq += y * y; ++diag_samples;
                diag_peak = std::max(diag_peak, std::abs(static_cast<int>(x)));
            }
            modem.receive_pcm(pcm);
        }

        auto now = clock::now();
        if (net.call_active() && now >= next_tx) {
            auto audio = modem.next_tx_pcmu(160);
            net.send_rtp_pcmu(audio, first_rtp);
            first_rtp = false;
            next_tx += std::chrono::milliseconds(20);
            if (now - next_tx > std::chrono::milliseconds(100)) next_tx = now + std::chrono::milliseconds(20);
        }

        now = clock::now();
        if (net.call_active() && now >= next_rtp_diag) {
            auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - call_started_at).count();
            if (rx_packets == 0 && age_ms > 1000 && !warned_no_rx) {
                std::cerr << "[RTP] WARNING: no inbound caller RTP on Windows UDP " << sc.rtp_port << "\n";
                warned_no_rx = true;
            } else if (last_rtp_at != clock::time_point{} && now - last_rtp_at > std::chrono::seconds(1) && !warned_rx_stalled) {
                std::cerr << "[RTP] WARNING: inbound RTP stopped for >1 second\n";
                warned_rx_stalled = true;
            } else if (diag_samples) {
                warned_rx_stalled = false;
                double rms = std::sqrt(static_cast<double>(diag_sumsq / static_cast<long double>(diag_samples)));
                double dbfs = rms > 0 ? 20.0 * std::log10(rms / 32768.0) : -120.0;
                if (sc.debug) std::cout << "[RTP] inbound " << diag_packets << " pkt/s, " << dbfs << " dBFS, state " << to_string(modem.state()) << "\n";
            }
            diag_samples = 0; diag_sumsq = 0; diag_peak = 0; diag_packets = 0;
            next_rtp_diag = now + std::chrono::seconds(1);
        }

        if (!modem.last_event().empty()) {
            std::cout << "[MODEM] " << modem.last_event() << " [" << to_string(modem.state()) << "]\n";
            modem.clear_event();
        }

        uint64_t ms = now_ms();
        if (modem.data_connected() && !ppp_started) {
            ppp.start(ms); ppp_started = true;
            std::cout << "[PPP] modem data connected; starting userspace PPP\n";
        }
        if (modem.data_connected() && ppp_started) {
            auto from_modem = modem.take_ppp_bytes();
            if (!from_modem.empty()) ppp.feed_serial(from_modem, ms);
            ppp.tick(ms);

            for (auto& ip : ppp.take_ip_packets()) {
                if (!tun.send_packet(ip) && sc.debug) std::cerr << "[TUN] inject failed: " << tun.last_error() << "\n";
            }

            // Packets routed by Windows/WinNAT back to the dial-up client appear
            // as outbound packets on the Wintun interface. Drain the queue fully.
            if (WaitForSingleObject(tun.read_event(), 0) == WAIT_OBJECT_0) {
                for (;;) {
                    std::vector<uint8_t> ip;
                    if (!tun.receive_packet(ip)) break;
                    if (packet_is_ipv4_for_peer(ip, pc.peer_ip)) ppp.feed_ip_packet(ip);
                }
            }

            auto to_modem = ppp.take_serial_tx();
            if (!to_modem.empty()) modem.feed_ppp_bytes(to_modem);

            if (!ppp.last_event().empty()) {
                std::cout << "[PPP] " << ppp.last_event() << " [" << to_string(ppp.phase()) << "]\n";
                ppp.clear_event();
            }
        }

        if (net.registration_enabled() && clock::now() >= next_register) {
            if (!net.send_register()) std::cerr << "[SIP] REGISTER refresh send failed\n";
            next_register = clock::now() + std::chrono::seconds(std::max<unsigned>(60, sc.registrar_expires / 2));
        }
    }

    std::cout << "\n[WIN] shutting down\n";
    net.close();
    tun.close();
    WSACleanup();
    return 0;
}

#endif // _WIN32
