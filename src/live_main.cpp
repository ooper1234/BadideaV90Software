#include "g711.hpp"
#include "live_modem.hpp"
#include "sip_rtp.hpp"
#include "span_v22.hpp"
#include "span_v8.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

using namespace v92;
static volatile std::sig_atomic_t g_quit=0;
static void onsig(int){g_quit=1;}

static void usage(){
    std::cout<<
"v92isp-live --bind LAN_IP [options]\n\n"
"SIP/RTP dial-up modem answerer. Stable auto mode negotiates V.8 -> V.22bis/V.22\n"
"with V.42/LAPM when offered, then forwards PPP octets to the configured PPP backend.\n\n"
"  --bind IP             SIP/RTP bind IP\n"
"  --advertise-ip IP     SDP/Contact IP; defaults to --bind\n"
"  --sip-port N          local SIP UDP port (default 5060)\n"
"  --rtp-port N          local RTP UDP port (default 40000)\n"
"  --registrar H[:P]     optional SIP proxy/registrar\n"
"  --sip-user USER       registrar username/extension\n"
"  --sip-password PASS   registrar password; empty is allowed\n"
"  --register-expires N  REGISTER lifetime seconds (default 300)\n"
"  --mode auto|v92|v22bis|v22|v21\n"
"                        auto is stable; v92 is Quick-Connect DSP experiment\n"
"  --ppp-socket PATH     default /run/v92isp/modem.sock\n"
"  --features            print compiled capabilities and exit\n"
"  --debug               verbose state logs\n";
}

static int connect_unix(const std::string& path){
    int fd=::socket(AF_UNIX,SOCK_STREAM,0); if(fd<0)return -1;
    sockaddr_un sa{};sa.sun_family=AF_UNIX;if(path.size()>=sizeof(sa.sun_path)){::close(fd);errno=ENAMETOOLONG;return -1;}
    std::strncpy(sa.sun_path,path.c_str(),sizeof(sa.sun_path)-1);
    if(::connect(fd,reinterpret_cast<sockaddr*>(&sa),sizeof(sa))!=0){int e=errno;::close(fd);errno=e;return -1;}
    int fl=::fcntl(fd,F_GETFL,0); if(fl>=0)::fcntl(fd,F_SETFL,fl|O_NONBLOCK); return fd;
}

static bool write_all_nb(int fd,const std::vector<uint8_t>& b){
    size_t off=0; while(off<b.size()){
        ssize_t n=::write(fd,b.data()+off,b.size()-off); if(n>0){off+=(size_t)n;continue;}
        if(n<0&&errno==EINTR)continue;
        if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK)){::usleep(1000);continue;}
        return false;
    } return true;
}

static bool parse_host_port(const std::string& s,std::string& host,uint16_t& port){
    auto c=s.rfind(':');
    if(c!=std::string::npos && s.find(':')==c){
        host=s.substr(0,c); if(host.empty())return false;
        try{int p=std::stoi(s.substr(c+1));if(p<1||p>65535)return false;port=(uint16_t)p;}catch(...){return false;}
    } else host=s;
    return !host.empty();
}

int main(int argc,char** argv){
    SipRtpConfig sc; LiveMode mode=LiveMode::Auto; std::string ppp_path="/run/v92isp/modem.sock"; bool show_features=false;
    for(int i=1;i<argc;++i){
        std::string a=argv[i];
        auto need=[&](const char* o){if(i+1>=argc){std::cerr<<o<<" needs a value\n";std::exit(2);}return std::string(argv[++i]);};
        if(a=="--bind")sc.bind_ip=need("--bind");
        else if(a=="--advertise-ip")sc.advertise_ip=need("--advertise-ip");
        else if(a=="--sip-port")sc.sip_port=(uint16_t)std::stoi(need("--sip-port"));
        else if(a=="--rtp-port")sc.rtp_port=(uint16_t)std::stoi(need("--rtp-port"));
        else if(a=="--registrar"){
            auto r=need("--registrar"); if(!parse_host_port(r,sc.registrar_host,sc.registrar_port)){std::cerr<<"bad --registrar\n";return 2;}
        }
        else if(a=="--sip-user")sc.registrar_user=need("--sip-user");
        else if(a=="--sip-password")sc.registrar_password=need("--sip-password");
        else if(a=="--register-expires")sc.registrar_expires=(unsigned)std::stoul(need("--register-expires"));
        else if(a=="--ppp-socket")ppp_path=need("--ppp-socket");
        else if(a=="--features")show_features=true;
        else if(a=="--debug")sc.debug=true;
        else if(a=="--mode"){
            auto m=need("--mode");
            if(m=="auto")mode=LiveMode::Auto; else if(m=="v90")mode=LiveMode::V90Digital; else if(m=="v92")mode=LiveMode::V92QuickConnect;
            else if(m=="v22bis")mode=LiveMode::V22bis_2400; else if(m=="v22")mode=LiveMode::V22_1200;
            else if(m=="v21")mode=LiveMode::V21_300; else{std::cerr<<"bad mode\n";return 2;}
        }
        else if(a=="-h"||a=="--help"){usage();return 0;}
        else {std::cerr<<"unknown: "<<a<<"\n";usage();return 2;}
    }

    if(show_features){
        std::cout << "builtin-v21=yes\n"
                  << "stable-auto-v8=" << ((span_v8_available()&&span_v22_available())?"yes":"no") << "\n"
                  << "spandsp-v22=" << (span_v22_available()?"yes":"no") << "\n"
                  << "v42-lapm=" << (span_v22_available()?"yes":"no") << "\n"
                  << "sip-register=yes\n"
                  << "sip-digest=" << (sip_digest_compiled()?"yes":"no") << "\n"
                  << "v92-quickconnect-experimental=yes\n"
                  << "v90-v92-full-data=no\n";
        return 0;
    }
    if((mode==LiveMode::V22bis_2400||mode==LiveMode::V22_1200) && !span_v22_available()){
        std::cerr<<"This build has no SpanDSP. Install libspandsp-dev and rebuild for --mode v22/v22bis.\n";return 2;
    }
    if(!sc.registrar_host.empty() && sc.registrar_user.empty()){std::cerr<<"--registrar also needs --sip-user\n";return 2;}
    if(sc.registrar_expires<60)sc.registrar_expires=60;
    if(sc.bind_ip=="0.0.0.0"&&sc.advertise_ip.empty()){std::cerr<<"Use --bind with the Linux LAN IP, or provide --advertise-ip.\n";return 2;}

    std::signal(SIGINT,onsig);std::signal(SIGTERM,onsig);
    SipRtpServer net(sc); if(!net.open()){std::cerr<<"[NET] "<<net.last_error()<<"\n";return 1;}
    LiveModem modem(mode); int ppp=-1; bool first_rtp=true; unsigned rx_packets=0,rx_lost=0;
    uint64_t diag_samples=0; long double diag_sumsq=0; int diag_peak=0; unsigned diag_packets=0;
    bool warned_no_rx=false, warned_rx_stalled=false;
    std::cout<<"[SIP] listening udp://"<<sc.bind_ip<<":"<<sc.sip_port<<"\n";
    std::cout<<"[RTP] listening udp://"<<sc.bind_ip<<":"<<sc.rtp_port<<" PCMU/8000, 20 ms packets\n";
    std::cout<<"[MODEM] mode "<<(mode==LiveMode::Auto?"auto (stable V.8/V.22bis + fallback)":mode==LiveMode::V21_300?"V.21 300":mode==LiveMode::V22bis_2400?"V.22bis 2400":mode==LiveMode::V22_1200?"V.22 1200":mode==LiveMode::V90Digital?"V.90 DIGITAL PHASE-2 LAB":"V.92 Quick Connect EXPERIMENTAL")<<"\n";
    if(net.registration_enabled()){
        std::cout<<"[SIP] registrar "<<sc.registrar_host<<":"<<sc.registrar_port<<" user "<<sc.registrar_user<<"\n";
        if(!net.send_register())std::cerr<<"[SIP] initial REGISTER send failed; direct SIP still works\n";
    }

    using clock=std::chrono::steady_clock; auto next_tx=clock::now();
    auto call_started_at=clock::now(), last_rtp_at=clock::time_point{}, next_rtp_diag=clock::now()+std::chrono::seconds(1);
    auto next_register=clock::now()+std::chrono::seconds(std::max<unsigned>(60,sc.registrar_expires/2));
    while(!g_quit){
        pollfd f[3]={{net.sip_fd(),POLLIN,0},{net.rtp_fd(),POLLIN,0},{ppp,POLLIN,0}};
        int nf=ppp>=0?3:2; auto now=clock::now();
        int timeout=20; if(net.call_active()){
            auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(next_tx-now).count(); timeout=(int)std::max<long long>(0,std::min<long long>(20,ms));
        }
        int pr=::poll(f,nf,timeout); if(pr<0&&errno!=EINTR){std::cerr<<"poll: "<<std::strerror(errno)<<"\n";break;}
        if(f[0].revents&POLLIN){
            auto ev=net.handle_sip();
            if(ev.type==SipEventType::CallStarted){
                std::cout<<"[SIP] call answered; RTP peer "<<ev.detail<<"\n"; modem.start_call(); next_tx=clock::now(); first_rtp=true; rx_packets=rx_lost=0;
                call_started_at=clock::now(); last_rtp_at=clock::time_point{}; next_rtp_diag=clock::now()+std::chrono::seconds(1);
                diag_samples=0;diag_sumsq=0;diag_peak=0;diag_packets=0;warned_no_rx=false;warned_rx_stalled=false;
                if(ppp>=0){::close(ppp);ppp=-1;}
            } else if(ev.type==SipEventType::Ack&&sc.debug)std::cout<<"[SIP] ACK\n";
            else if(ev.type==SipEventType::CallEnded){std::cout<<"[SIP] call ended; RTP packets="<<rx_packets<<" lost="<<rx_lost<<"\n";modem.end_call();if(ppp>=0){::close(ppp);ppp=-1;}}
            else if(ev.type==SipEventType::RegistrationOk)std::cout<<"[SIP] "<<ev.detail<<"\n";
            else if(ev.type==SipEventType::RegistrationFailed)std::cerr<<"[SIP] registration: "<<ev.detail<<" (direct SIP still works)\n";
            else if(ev.type==SipEventType::Error)std::cerr<<"[SIP] "<<ev.detail<<"\n";
        }
        if(f[1].revents&POLLIN){
            while(true){
                RtpAudio a;if(!net.recv_rtp(a))break;++rx_packets;
                if(a.had_gap){rx_lost+=a.missing_packets;std::vector<int16_t> z((size_t)a.missing_packets*160,0);modem.receive_pcm(z);std::cerr<<"[RTP] lost "<<a.missing_packets<<" packet(s); inserted silence\n";}
                std::vector<int16_t> pcm;pcm.reserve(a.pcmu.size());for(auto u:a.pcmu)pcm.push_back(ulaw_to_linear(u));
                last_rtp_at=clock::now(); ++diag_packets;
                for(auto x:pcm){ long double y=x; diag_sumsq+=y*y; ++diag_samples; diag_peak=std::max(diag_peak,std::abs((int)x)); }
                modem.receive_pcm(pcm);
            }
        }
        now=clock::now();
        if(net.call_active()&&now>=next_tx){
            auto audio=modem.next_tx_pcmu(160);net.send_rtp_pcmu(audio,first_rtp);first_rtp=false;
            next_tx+=std::chrono::milliseconds(20);if(now-next_tx>std::chrono::milliseconds(100))next_tx=now+std::chrono::milliseconds(20);
        }
        now=clock::now();
        if(net.call_active() && now>=next_rtp_diag){
            auto age_ms=std::chrono::duration_cast<std::chrono::milliseconds>(now-call_started_at).count();
            if(rx_packets==0 && age_ms>1000){
                if(!warned_no_rx){
                    std::cerr<<"[RTP] WARNING: no inbound RTP from the caller. Audio is one-way: the modem can hear us, but we cannot hear the modem. Check PBX/ATA RTP routing, codec PCMU, firewall UDP "<<sc.rtp_port<<".\n";
                    warned_no_rx=true;
                }
            } else if(last_rtp_at!=clock::time_point{} && now-last_rtp_at>std::chrono::seconds(1)){
                if(!warned_rx_stalled){
                    std::cerr<<"[RTP] WARNING: inbound RTP stopped for >1 s; modem negotiation cannot continue reliably.\n";
                    warned_rx_stalled=true;
                }
            } else if(diag_samples){
                warned_rx_stalled=false;
                double rms=std::sqrt((double)(diag_sumsq/(long double)diag_samples));
                double dbfs=rms>0 ? 20.0*std::log10(rms/32768.0) : -120.0;
                if(sc.debug) std::cout<<"[RTP] inbound "<<diag_packets<<" pkt/s, level "<<dbfs<<" dBFS, peak "<<diag_peak<<"/32767, state "<<to_string(modem.state())<<"\n";
                if(dbfs < -55.0 && !warned_no_rx){
                    std::cerr<<"[RTP] WARNING: RTP packets arrive but audio is almost silent ("<<dbfs<<" dBFS). Disable VAD/silence suppression and verify the ATA/PBX sends modem audio.\n";
                    warned_no_rx=true;
                }
            }
            diag_samples=0;diag_sumsq=0;diag_peak=0;diag_packets=0;
            next_rtp_diag=now+std::chrono::seconds(1);
        }

        if(net.registration_enabled() && clock::now()>=next_register){
            if(!net.send_register())std::cerr<<"[SIP] REGISTER refresh send failed\n";
            next_register=clock::now()+std::chrono::seconds(std::max<unsigned>(60,sc.registrar_expires/2));
        }
        if(!modem.last_event().empty()){std::cout<<"[MODEM] "<<modem.last_event()<<" ["<<to_string(modem.state())<<"]\n";modem.clear_event();}

        if(modem.data_connected()&&ppp<0){
            ppp=connect_unix(ppp_path); if(ppp>=0)std::cout<<"[PPP] connected to "<<ppp_path<<"; Internet negotiation can begin\n";
            else {static auto last=clock::time_point{};if(clock::now()-last>std::chrono::seconds(2)){std::cerr<<"[PPP] cannot connect "<<ppp_path<<": "<<std::strerror(errno)<<"\n";last=clock::now();}}
        }
        if(modem.data_connected()&&ppp>=0){
            auto b=modem.take_ppp_bytes();if(!b.empty()&&!write_all_nb(ppp,b)){std::cerr<<"[PPP] write failed\n";::close(ppp);ppp=-1;}
            if(ppp>=0){uint8_t bfr[4096];ssize_t n=::read(ppp,bfr,sizeof(bfr));if(n>0)modem.feed_ppp_bytes(std::vector<uint8_t>(bfr,bfr+n));else if(n==0){std::cout<<"[PPP] backend disconnected\n";::close(ppp);ppp=-1;}else if(errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR){std::cerr<<"[PPP] read failed\n";::close(ppp);ppp=-1;}}
        }
    }
    if(ppp>=0)::close(ppp);
    return 0;
}
