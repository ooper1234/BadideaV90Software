#ifdef _WIN32

#include "pjsip_frontend.hpp"
#include "ppp_userspace.hpp"
#include "span_v22.hpp"
#include "span_v8.hpp"
#include "wintun_adapter.hpp"
#include "windivert_nat.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <vector>

using namespace v92;

namespace {
std::atomic_bool g_quit{false};
BOOL WINAPI ctrl_handler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT ||
        type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT) {
        g_quit = true; return TRUE;
    }
    return FALSE;
}
std::string trim(std::string s) {
    while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
    size_t p=0; while (p<s.size() && std::isspace((unsigned char)s[p])) ++p; return s.substr(p);
}
std::map<std::string,std::string> load_env(const std::string& path) {
    std::map<std::string,std::string> out; std::ifstream in(path); std::string line;
    while (std::getline(in,line)) {
        if (!line.empty() && line.back()=='\r') line.pop_back(); line=trim(line);
        if (line.empty() || line[0]=='#') continue; auto eq=line.find('='); if(eq==std::string::npos) continue;
        auto k=trim(line.substr(0,eq)), v=trim(line.substr(eq+1));
        if(v.size()>=2 && ((v.front()=='"'&&v.back()=='"')||(v.front()=='\''&&v.back()=='\''))) v=v.substr(1,v.size()-2);
        out[k]=v;
    } return out;
}
bool parse_bool(std::string v,bool d=false){std::transform(v.begin(),v.end(),v.begin(),[](unsigned char c){return(char)std::tolower(c);});if(v=="1"||v=="yes"||v=="true"||v=="on")return true;if(v=="0"||v=="no"||v=="false"||v=="off")return false;return d;}
bool parse_host_port(const std::string&s,std::string&h,uint16_t&p){auto c=s.rfind(':');if(c!=std::string::npos&&s.find(':')==c){h=s.substr(0,c);try{int x=std::stoi(s.substr(c+1));if(x<1||x>65535)return false;p=(uint16_t)x;}catch(...){return false;}}else h=s;return !h.empty();}
LiveMode parse_mode(const std::string&m){if(m=="v90")return LiveMode::V90Digital;if(m=="v92")return LiveMode::V92QuickConnect;if(m=="v22bis")return LiveMode::V22bis_2400;if(m=="v22")return LiveMode::V22_1200;if(m=="v21")return LiveMode::V21_300;return LiveMode::Auto;}
const char* mode_name(LiveMode m){switch(m){case LiveMode::V90Digital:return "V.90 DIGITAL PHASE-2 LAB";case LiveMode::V92QuickConnect:return "V.92 QUICK CONNECT LAB";case LiveMode::V22bis_2400:return "V.22bis 2400";case LiveMode::V22_1200:return "V.22 1200";case LiveMode::V21_300:return "V.21 300";case LiveMode::Auto:default:return "AUTO (stable V.22bis/V.22/V.21)";}}
uint64_t now_ms(){using namespace std::chrono;return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();}
bool ensure_internet_sharing(const std::string& prefix,const std::string& local_ip){std::string cmd="powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"windows\\Ensure-Internet.ps1\" -PrivateAlias \"v92isp\" -PrivatePrefix \""+prefix+"\" -PrivateIP \""+local_ip+"\"";return std::system(cmd.c_str())==0;}
bool packet_is_ipv4_for_peer(const std::vector<uint8_t>&p,const std::string&peer){if(p.size()<20||(p[0]>>4)!=4)return false;in_addr a{};if(InetPtonA(AF_INET,peer.c_str(),&a)!=1)return false;return std::memcmp(&p[16],&a.s_addr,4)==0;}
std::string hex_preview(const std::vector<uint8_t>& b, size_t maxn=16){
    static const char* h="0123456789ABCDEF"; std::string out;
    size_t n=std::min(maxn,b.size()); out.reserve(n*3);
    for(size_t i=0;i<n;++i){if(i)out.push_back(' ');out.push_back(h[b[i]>>4]);out.push_back(h[b[i]&15]);}
    if(b.size()>n)out += " ..."; return out;
}
}

int main(int argc,char**argv){
    std::string cfg_path="config\\v92isp-windows.env"; bool cli_debug=false;
    for(int i=1;i<argc;++i){std::string a=argv[i];if(a=="--config"&&i+1<argc)cfg_path=argv[++i];else if(a=="--debug")cli_debug=true;else if(a=="--help"||a=="-h"){std::cout<<"v92isp-windows-pjsip.exe [--config FILE] [--debug]\n";return 0;}}
    WSADATA wd{}; if(WSAStartup(MAKEWORD(2,2),&wd)!=0){std::cerr<<"[WIN] WSAStartup failed\n";return 1;} SetConsoleCtrlHandler(ctrl_handler,TRUE);
    auto e=load_env(cfg_path); auto get=[&](const std::string&k,const std::string&d){auto it=e.find(k);return it==e.end()?d:it->second;};

    PjsipFrontendConfig sc; sc.bind_ip=get("BIND_IP","192.168.2.55"); sc.advertise_ip=get("ADVERTISE_IP",sc.bind_ip); sc.sip_port=(uint16_t)std::stoi(get("SIP_PORT","5060")); sc.rtp_port=(uint16_t)std::stoi(get("RTP_PORT","40000")); sc.user=get("SIP_USER","101"); sc.password=get("SIP_PASSWORD",""); sc.expires=(unsigned)std::stoul(get("REGISTER_EXPIRES","300")); sc.debug=cli_debug||parse_bool(get("DEBUG","1"),true); std::string reg=get("SIP_SERVER","192.168.2.40:5060"); if(!parse_host_port(reg,sc.registrar_host,sc.registrar_port)){std::cerr<<"[CFG] bad SIP_SERVER\n";return 2;}
    LiveMode mode=parse_mode(get("MODE","auto"));
    if(mode==LiveMode::V90Digital && !span_v8_available()){std::cerr<<"[MODEM] explicit V.90 test requires SpanDSP V.8 support; refusing silent fallback\n";return 2;}
    if((mode==LiveMode::Auto||mode==LiveMode::V22bis_2400||mode==LiveMode::V22_1200)&&(!span_v8_available()||!span_v22_available())){if(mode==LiveMode::Auto)mode=LiveMode::V21_300;else{std::cerr<<"[MODEM] SpanDSP missing\n";return 2;}}

    UserPppConfig pc; pc.local_ip=get("PPP_LOCAL_IP","192.168.137.1"); pc.peer_ip=get("PPP_PEER_IP","192.168.137.2"); pc.dns1=get("DNS1","1.1.1.1"); pc.dns2=get("DNS2","8.8.8.8"); pc.mtu=(uint16_t)std::stoi(get("PPP_MTU","296")); pc.require_pap=parse_bool(get("REQUIRE_PAP","0")); pc.pap_secrets=get("PAP_SECRETS","config\\pap-secrets.txt"); pc.debug=sc.debug;

    std::cout<<"v92isp Windows + PJSIP Internet server\n"<<"  LAN/SIP IP : "<<sc.bind_ip<<"\n  SIP proxy  : "<<sc.registrar_host<<":"<<sc.registrar_port<<"\n  SIP user   : "<<sc.user<<"\n  SIP stack  : PJSIP 2.17 (same protocol family used by MicroSIP)\n  media      : PCMU/8000, requested RTP base "<<sc.rtp_port<<"\n  MODEM MODE : "<<mode_name(mode)<<"\n  PPP        : "<<pc.peer_ip<<" <-> "<<pc.local_ip<<" MTU "<<pc.mtu<<"\n";

    WintunAdapter tun;
    bool tun_ready = tun.open(L"v92isp", pc.local_ip, 24);
    if (!tun_ready) {
        std::cerr << "[TUN] Warning: " << tun.last_error() << " (continuing with user-mode WinDivert NAT)\n";
    } else {
        std::cout << "[TUN] ready at " << pc.local_ip << "/24\n";
    }
    std::string prefix=pc.local_ip.substr(0,pc.local_ip.rfind('.')+1)+"0/24";
    // Prefer our single-peer WinDivert NAT.  It avoids the fragile ICS COM
    // configuration that failed on the user's current Windows build, while
    // still keeping WinNAT/ICS as a fallback if WinDivert cannot start.
    WinDivertNat fallback_nat;
    bool windows_nat = false;
    if (fallback_nat.open(pc.peer_ip)) {
        std::cout<<"[NAT] WinDivert user-mode NAT ACTIVE via "<<fallback_nat.public_ip()<<" (TCP/UDP/ICMP)\n";
    } else {
        std::cerr<<"[NAT] WinDivert NAT unavailable: "<<fallback_nat.last_error()<<"\n";
        std::cerr<<"[NAT] Trying Windows WinNAT/ICS fallback...\n";
        windows_nat = ensure_internet_sharing(prefix,pc.local_ip);
        if (windows_nat) {
            std::cout<<"[NAT] Windows Internet sharing ready for "<<prefix<<"\n";
        } else {
            std::cerr<<"[NAT] ERROR: neither WinDivert nor Windows Internet sharing could start.\n";
            std::cerr<<"[NAT] Dial-up/PPP can connect, but Internet forwarding will be unavailable.\n";
        }
    }

    PjsipFrontend sip(sc,mode); if(!sip.open()){std::cerr<<"[PJSIP] startup failed: "<<sip.last_error()<<"\n";return 1;}
    UserPppServer ppp(pc); bool ppp_started=false; bool call_was_active=false; bool first_ppp_tx=false; bool first_ppp_rx=false; bool first_nat_tx=false; bool first_nat_rx=false; bool nat_drop_reported=false; uint64_t client_disconnect_hangup_ms=0;
    while(!g_quit){
        FrontendEvent fe; while(sip.pop_event(fe)){const char* tag=fe.type==FrontendEventType::Error?"[ERROR]":fe.type==FrontendEventType::Registration?"[SIP]":fe.type==FrontendEventType::MediaActive?"[RTP]":"[CALL]";std::cout<<tag<<" "<<fe.text<<"\n";if(fe.type==FrontendEventType::CallStarted){ppp.reset();ppp_started=false;first_ppp_tx=false;first_ppp_rx=false;first_nat_tx=false;first_nat_rx=false;nat_drop_reported=false;client_disconnect_hangup_ms=0;}if(fe.type==FrontendEventType::CallEnded){ppp.reset();ppp_started=false;first_ppp_tx=false;first_ppp_rx=false;first_nat_tx=false;first_nat_rx=false;nat_drop_reported=false;client_disconnect_hangup_ms=0;}}
        bool ca=sip.call_active(); if(ca!=call_was_active){call_was_active=ca;if(ca)std::cout<<"[CALL] modem handshake started\n";}
        std::string me=sip.take_modem_event(); if(!me.empty())std::cout<<"[MODEM] "<<me<<" ["<<to_string(sip.modem_state())<<"]\n";
        uint64_t ms=now_ms(); if(sip.modem_data_connected()&&!ppp_started){ppp.start(ms);ppp_started=true;std::cout<<"[PPP] modem data connected; starting PPP\n";}
        if(sip.modem_data_connected()&&ppp_started){
            auto fm=sip.take_modem_ppp_bytes();
            if(!fm.empty()){
                if(!first_ppp_rx){first_ppp_rx=true;std::cout<<"[PPP-RX] FIRST decoded bytes from caller modem: "<<fm.size()<<" byte(s): "<<hex_preview(fm)<<"\n";}
                ppp.feed_serial(fm,ms);
            }
            ppp.tick(ms);

            // Client -> Internet. Prefer Windows NAT when it is healthy; if
            // ICS/WinNAT failed at startup, the built-in WinDivert NAT owns the
            // forwarding path instead.
            for(auto&ip:ppp.take_ip_packets()){
                if(fallback_nat.is_open()) {
                    if(fallback_nat.send_from_peer(ip)) {
                        if(!first_nat_tx){first_nat_tx=true;std::cout<<"[NAT-TX] FIRST PPP IPv4 packet forwarded to Internet ("<<ip.size()<<" bytes)\n";}
                    } else if(sc.debug && !nat_drop_reported) {
                        nat_drop_reported=true;
                        std::cerr<<"[NAT] unsupported/untranslatable outbound IPv4 packet dropped (further notices suppressed)\n";
                    }
                } else if (tun_ready) {
                    tun.send_packet(ip);
                }
            }

            // Internet -> client.
            if(fallback_nat.is_open()) {
                for(;;){std::vector<uint8_t>ip;if(!fallback_nat.take_to_peer(ip))break;if(!first_nat_rx){first_nat_rx=true;std::cout<<"[NAT-RX] FIRST Internet reply returned to PPP peer ("<<ip.size()<<" bytes before MRU fragmentation)\n";}ppp.feed_ip_packet(ip);}
            } else if(tun_ready && WaitForSingleObject(tun.read_event(),0)==WAIT_OBJECT_0){
                for(;;){std::vector<uint8_t>ip;if(!tun.receive_packet(ip))break;if(packet_is_ipv4_for_peer(ip,pc.peer_ip))ppp.feed_ip_packet(ip);}
            }

            auto tm=ppp.take_serial_tx();
            if(!tm.empty()){
                if(!first_ppp_tx){first_ppp_tx=true;std::cout<<"[PPP-TX] FIRST bytes queued to caller modem: "<<tm.size()<<" byte(s): "<<hex_preview(tm)<<"\n";}
                sip.feed_modem_ppp_bytes(tm);
            }
            if(ppp.phase()==UserPppPhase::Terminating && client_disconnect_hangup_ms==0){client_disconnect_hangup_ms=ms+1200;std::cout<<"[PPP] client requested disconnect; Terminate-Ack queued, dropping modem carrier after flush\n";}
            if(!ppp.last_event().empty()){std::cout<<"[PPP] "<<ppp.last_event()<<" ["<<to_string(ppp.phase())<<"]\n";ppp.clear_event();}
        }
        if(client_disconnect_hangup_ms && ms>=client_disconnect_hangup_ms){std::cout<<"[CALL] client PPP disconnect complete; sending SIP BYE to drop carrier\n";sip.hangup_current_call();client_disconnect_hangup_ms=0;}
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::cout<<"\n[WIN] shutting down\n"; sip.close(); fallback_nat.close(); tun.close(); WSACleanup(); return 0;
}

#endif
