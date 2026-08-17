#include "sip_rtp.hpp"

#include <cerrno>
#include <cstring>
#include <cstdio>
#include <random>
#include <sstream>
#include <string_view>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#ifdef V92_HAVE_OPENSSL
#include <openssl/evp.h>
#endif

namespace v92 {

#ifdef _WIN32
using io_count_t = int;
static int sock_errno() { return WSAGetLastError(); }
static bool sock_would_block(int e) { return e == WSAEWOULDBLOCK || e == WSAEINTR; }
static void close_sock(socket_handle_t s) { if (s != invalid_socket) ::closesocket(s); }
static int ci_ncmp(const char* a, const char* b, size_t n) { return _strnicmp(a, b, n); }
static int ci_cmp(const char* a, const char* b) { return _stricmp(a, b); }
static std::string sock_error_text(int e) {
    char* text = nullptr;
    DWORD n = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                             FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, static_cast<DWORD>(e), 0,
                             reinterpret_cast<LPSTR>(&text), 0, nullptr);
    std::string out = n && text ? std::string(text, n) : ("Winsock error " + std::to_string(e));
    if (text) LocalFree(text);
    while (!out.empty() && (out.back() == '\r' || out.back() == '\n')) out.pop_back();
    return out;
}
static bool set_nonblocking(socket_handle_t s) {
    u_long one = 1;
    return ioctlsocket(s, FIONBIO, &one) == 0;
}
#else
using io_count_t = ssize_t;
static int sock_errno() { return errno; }
static bool sock_would_block(int e) { return e == EAGAIN || e == EWOULDBLOCK || e == EINTR; }
static void close_sock(socket_handle_t s) { if (s != invalid_socket) ::close(s); }
static int ci_ncmp(const char* a, const char* b, size_t n) { return strncasecmp(a, b, n); }
static int ci_cmp(const char* a, const char* b) { return strcasecmp(a, b); }
static std::string sock_error_text(int e) { return std::strerror(e); }
static bool set_nonblocking(socket_handle_t s) {
    int fl = ::fcntl(s, F_GETFL, 0);
    return fl >= 0 && ::fcntl(s, F_SETFL, fl | O_NONBLOCK) == 0;
}
#endif

static std::string trim(std::string s) {
    while (!s.empty() && (s.back()=='\r' || s.back()=='\n' || s.back()==' ' || s.back()=='\t')) s.pop_back();
    size_t i=0; while(i<s.size() && (s[i]==' ' || s[i]=='\t')) ++i;
    return s.substr(i);
}

static std::string header_value(const std::string& msg, const std::string& name) {
    size_t p=0;
    while(p<msg.size()) {
        size_t e=msg.find('\n',p); if(e==std::string::npos) e=msg.size();
        std::string line=msg.substr(p,e-p);
        if(!line.empty() && line.back()=='\r') line.pop_back();
        if(line.size()>name.size()+1 && ci_ncmp(line.c_str(),name.c_str(),name.size())==0 && line[name.size()]==':')
            return trim(line.substr(name.size()+1));
        p=e+1;
    }
    return {};
}

static std::string sdp_line_value(const std::string& body, const std::string& prefix) {
    size_t p=0;
    while(p<body.size()) {
        size_t e=body.find('\n',p); if(e==std::string::npos) e=body.size();
        std::string line=body.substr(p,e-p);
        if(!line.empty() && line.back()=='\r') line.pop_back();
        if(line.rfind(prefix,0)==0) return trim(line.substr(prefix.size()));
        p=e+1;
    }
    return {};
}

static bool parse_offer(const std::string& msg, std::string& ip, uint16_t& port, bool& pcmu) {
    auto sep=msg.find("\r\n\r\n"); size_t skip=4;
    if(sep==std::string::npos){ sep=msg.find("\n\n"); skip=2; }
    if(sep==std::string::npos) return false;
    std::string body=msg.substr(sep+skip);
    std::string c=sdp_line_value(body,"c=");
    if(c.rfind("IN IP4 ",0)==0) ip=trim(c.substr(7));
    std::string m=sdp_line_value(body,"m=audio ");
    if(m.empty()) return false;
    std::istringstream ms(m); std::string proto; ms>>port>>proto;
    int pt=-1; pcmu=false; while(ms>>pt) if(pt==0) pcmu=true;
    return !ip.empty() && port!=0 && proto.find("RTP/AVP")!=std::string::npos;
}

static socket_handle_t make_udp(const std::string& ip, uint16_t port, std::string& err) {
    socket_handle_t fd=::socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    if(fd==invalid_socket){int e=sock_errno();err=sock_error_text(e);return invalid_socket;}
    int one=1; ::setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,reinterpret_cast<const char*>(&one),sizeof(one));
    sockaddr_in sa{}; sa.sin_family=AF_INET; sa.sin_port=htons(port);
    if(::inet_pton(AF_INET,ip.c_str(),&sa.sin_addr)!=1){err="bad bind IP: "+ip;close_sock(fd);return invalid_socket;}
    if(::bind(fd,reinterpret_cast<sockaddr*>(&sa),sizeof(sa))!=0){int e=sock_errno();err=sock_error_text(e);close_sock(fd);return invalid_socket;}
    if(!set_nonblocking(fd)){int e=sock_errno();err="cannot set UDP socket nonblocking: "+sock_error_text(e);close_sock(fd);return invalid_socket;}
#ifdef _WIN32
    // Windows Winsock quirk: if an outgoing UDP packet gets an ICMP Port Unreachable,
    // subsequent recvfrom() calls return WSAECONNRESET (10054) and drop incoming packets.
    // Disabling SIO_UDP_CONNRESET matches PJSIP/MicroSIP behavior on Windows.
    #ifndef SIO_UDP_CONNRESET
    #define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
    #endif
    BOOL bNewBehavior = FALSE;
    DWORD dwBytesReturned = 0;
    ::WSAIoctl(fd, SIO_UDP_CONNRESET, &bNewBehavior, sizeof(bNewBehavior), NULL, 0, &dwBytesReturned, NULL, NULL);
#endif
    return fd;
}

bool sip_digest_compiled() {
#ifdef V92_HAVE_OPENSSL
    return true;
#else
    return false;
#endif
}

#ifdef V92_HAVE_OPENSSL
static std::string md5_hex(const std::string& s) {
    unsigned char d[EVP_MAX_MD_SIZE]; unsigned n=0;
    EVP_MD_CTX* c=EVP_MD_CTX_new();
    if(!c) return {};
    bool ok=EVP_DigestInit_ex(c,EVP_md5(),nullptr)==1 &&
            EVP_DigestUpdate(c,s.data(),s.size())==1 &&
            EVP_DigestFinal_ex(c,d,&n)==1;
    EVP_MD_CTX_free(c); if(!ok) return {};
    static const char h[]="0123456789abcdef"; std::string out; out.reserve(n*2);
    for(unsigned i=0;i<n;++i){out.push_back(h[d[i]>>4]);out.push_back(h[d[i]&15]);}
    return out;
}
#endif

static std::string auth_param(const std::string& h, const std::string& key) {
    size_t p=h.find(' '); std::string s=(p==std::string::npos)?h:h.substr(p+1);
    p=0;
    while(p<s.size()){
        while(p<s.size() && (s[p]==' '||s[p]==',')) ++p;
        size_t eq=s.find('=',p); if(eq==std::string::npos) break;
        std::string k=trim(s.substr(p,eq-p)); size_t v=eq+1; std::string val;
        if(v<s.size() && s[v]=='"'){
            ++v; size_t q=v; while(q<s.size() && s[q]!='"') ++q; val=s.substr(v,q-v); p=(q<s.size()?q+1:q);
        } else {
            size_t q=s.find(',',v); if(q==std::string::npos) q=s.size(); val=trim(s.substr(v,q-v)); p=q;
        }
        if(ci_cmp(k.c_str(),key.c_str())==0) return val;
    }
    return {};
}

SipRtpServer::SipRtpServer(SipRtpConfig cfg):cfg_(std::move(cfg)) {
    std::random_device rd; std::mt19937 g(rd());
    tx_seq_=static_cast<uint16_t>(g()); tx_ts_=g(); ssrc_=g();
    to_tag_=std::to_string(g()); reg_call_id_="reg-"+std::to_string(g())+"@"+cfg_.advertise_ip;
    reg_from_tag_=std::to_string(g());
}
SipRtpServer::~SipRtpServer(){close();}

bool SipRtpServer::resolve_registrar(){
    if(!registration_enabled()) return true;
    registrar_addr_={}; registrar_addr_.sin_family=AF_INET; registrar_addr_.sin_port=htons(cfg_.registrar_port);
    if(::inet_pton(AF_INET,cfg_.registrar_host.c_str(),&registrar_addr_.sin_addr)==1){have_registrar_addr_=true;return true;}
    addrinfo hints{}; hints.ai_family=AF_INET; hints.ai_socktype=SOCK_DGRAM; addrinfo* res=nullptr;
    int r=getaddrinfo(cfg_.registrar_host.c_str(),nullptr,&hints,&res);
    if(r!=0||!res){last_error_="cannot resolve registrar "+cfg_.registrar_host;return false;}
    registrar_addr_.sin_addr=reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr; freeaddrinfo(res); have_registrar_addr_=true; return true;
}

bool SipRtpServer::open(){
    close(); last_error_.clear();
    if(cfg_.advertise_ip.empty()) {
        if(cfg_.bind_ip=="0.0.0.0") { last_error_="--advertise-ip is required when --bind is 0.0.0.0"; return false; }
        cfg_.advertise_ip=cfg_.bind_ip;
    }
    if(reg_call_id_.find('@')==std::string::npos || reg_call_id_.back()=='@') reg_call_id_ += cfg_.advertise_ip;
    std::string bind_error;
    sip_fd_=make_udp(cfg_.bind_ip,cfg_.sip_port,bind_error);
    if(sip_fd_==invalid_socket) {
        last_error_ = "SIP UDP bind " + cfg_.bind_ip + ":" + std::to_string(cfg_.sip_port) + " failed: " + bind_error;
        return false;
    }
    bind_error.clear();
    rtp_fd_=make_udp(cfg_.bind_ip,cfg_.rtp_port,bind_error);
    if(rtp_fd_==invalid_socket){
        last_error_ = "RTP UDP bind " + cfg_.bind_ip + ":" + std::to_string(cfg_.rtp_port) + " failed: " + bind_error;
        close_sock(sip_fd_);sip_fd_=invalid_socket;return false;
    }
    if(!resolve_registrar()){close();return false;}
    return true;
}

void SipRtpServer::close(){
    close_sock(sip_fd_); close_sock(rtp_fd_);
    sip_fd_=rtp_fd_=invalid_socket; call_active_=false; have_rtp_peer_=false; symmetric_rtp_locked_=false; have_rx_seq_=false;
    registered_=false; have_registrar_addr_=false;
}

std::string SipRtpServer::build_sdp() const {
    std::ostringstream o;
    o<<"v=0\r\n"
     <<"o=v92isp 1 1 IN IP4 "<<cfg_.advertise_ip<<"\r\n"
     <<"s=v92isp dial-up modem\r\n"
     <<"c=IN IP4 "<<cfg_.advertise_ip<<"\r\n"
     <<"t=0 0\r\n"
     <<"m=audio "<<cfg_.rtp_port<<" RTP/AVP 0\r\n"
     <<"a=rtpmap:0 PCMU/8000\r\n"
     <<"a=ptime:20\r\n"
     <<"a=sendrecv\r\n";
    return o.str();
}

bool SipRtpServer::send_sip_response(int code,const std::string& reason,const std::string& extra,const std::string& body){
    if(sip_fd_==invalid_socket) return false;
    std::ostringstream o; o<<"SIP/2.0 "<<code<<" "<<reason<<"\r\n";
    if(!via_.empty()) o<<"Via: "<<via_<<"\r\n";
    if(!from_.empty()) o<<"From: "<<from_<<"\r\n";
    if(!to_.empty()) { o<<"To: "<<to_; if(to_.find("tag=")==std::string::npos && code>=180) o<<";tag="<<to_tag_; o<<"\r\n"; }
    if(!call_id_.empty()) o<<"Call-ID: "<<call_id_<<"\r\n";
    if(!cseq_.empty()) o<<"CSeq: "<<cseq_<<"\r\n";
    o<<"Server: v92isp\r\n"; if(!extra.empty()) o<<extra;
    if(!body.empty()) o<<"Content-Type: application/sdp\r\n";
    o<<"Content-Length: "<<body.size()<<"\r\n\r\n"<<body;
    auto s=o.str();
    if (cfg_.debug && code >= 180) {
        std::fprintf(stderr, "[SIP-TRACE] >>> SIP/2.0 %d %s\n", code, reason.c_str());
        if (!body.empty()) std::fprintf(stderr, "[SIP-TRACE] local SDP:\n%s", body.c_str());
    }
    return ::sendto(sip_fd_,s.data(),static_cast<int>(s.size()),0,reinterpret_cast<sockaddr*>(&sip_peer_),sizeof(sip_peer_))==static_cast<io_count_t>(s.size());
}

bool SipRtpServer::send_register_request(){
    if(!registration_enabled() || !have_registrar_addr_ || sip_fd_==invalid_socket) return false;
    std::string hostport=cfg_.registrar_host+":"+std::to_string(cfg_.registrar_port);
    std::string uri="sip:"+hostport;
    std::random_device rd; std::string branch="z9hG4bK-v92isp-"+std::to_string(rd());
    std::ostringstream o;
    o<<"REGISTER "<<uri<<" SIP/2.0\r\n"
     <<"Via: SIP/2.0/UDP "<<cfg_.advertise_ip<<":"<<cfg_.sip_port<<";branch="<<branch<<";rport\r\n"
     <<"Max-Forwards: 70\r\n"
     <<"From: <sip:"<<cfg_.registrar_user<<"@"<<cfg_.registrar_host<<">;tag="<<reg_from_tag_<<"\r\n"
     <<"To: <sip:"<<cfg_.registrar_user<<"@"<<cfg_.registrar_host<<">\r\n"
     <<"Call-ID: "<<reg_call_id_<<"\r\n"
     <<"CSeq: "<<reg_cseq_++<<" REGISTER\r\n"
     <<"Contact: <sip:"<<cfg_.registrar_user<<"@"<<cfg_.advertise_ip<<":"<<cfg_.sip_port<<">\r\n"
     <<"Expires: "<<cfg_.registrar_expires<<"\r\n"
     <<"User-Agent: v92isp\r\n";
    if(!reg_authorization_.empty()) o<<reg_authorization_<<"\r\n";
    o<<"Content-Length: 0\r\n\r\n";
    auto s=o.str();
    char dst[INET_ADDRSTRLEN] = {};
    ::inet_ntop(AF_INET, &registrar_addr_.sin_addr, dst, sizeof(dst));
    io_count_t sent = ::sendto(sip_fd_,s.data(),static_cast<int>(s.size()),0,reinterpret_cast<sockaddr*>(&registrar_addr_),sizeof(registrar_addr_));
    if (cfg_.debug) {
        std::fprintf(stderr, "[SIP] REGISTER -> %s:%u (%lld/%zu bytes)\n",
                     dst, static_cast<unsigned>(ntohs(registrar_addr_.sin_port)), static_cast<long long>(sent), s.size());
    }
    if (sent != static_cast<io_count_t>(s.size())) {
        int e=sock_errno(); last_error_ = std::string("REGISTER sendto failed: ") + sock_error_text(e);
        return false;
    }
    return true;
}

bool SipRtpServer::send_register(){
    if(!registration_enabled()) return false;
    if(!have_registrar_addr_ && !resolve_registrar()) return false;
    // A periodic refresh reuses a successfully built Digest Authorization.
    return send_register_request();
}

SipEvent SipRtpServer::handle_sip_response(const std::string& msg){
    SipEvent ev; size_t e=msg.find('\n'); std::string first=trim(msg.substr(0,e));
    std::istringstream is(first); std::string proto; int code=0; is>>proto>>code;
    std::string cs=header_value(msg,"CSeq");
    if(cs.find("REGISTER")==std::string::npos) return ev;
    if(code>=200 && code<300){registered_=true;reg_auth_attempted_=false;ev.type=SipEventType::RegistrationOk;ev.detail="registered as "+cfg_.registrar_user+" at "+cfg_.registrar_host+":"+std::to_string(cfg_.registrar_port);return ev;}
    if((code==401||code==407) && !reg_auth_attempted_){
        std::string challenge=header_value(msg,code==407?"Proxy-Authenticate":"WWW-Authenticate");
#ifdef V92_HAVE_OPENSSL
        std::string realm=auth_param(challenge,"realm"), nonce=auth_param(challenge,"nonce"), qop=auth_param(challenge,"qop"), opaque=auth_param(challenge,"opaque");
        if(!realm.empty()&&!nonce.empty()){
            std::string hostport=cfg_.registrar_host+":"+std::to_string(cfg_.registrar_port); std::string uri="sip:"+hostport;
            std::string ha1=md5_hex(cfg_.registrar_user+":"+realm+":"+cfg_.registrar_password);
            std::string ha2=md5_hex("REGISTER:"+uri); std::string resp; std::string nc="00000001", cnonce=md5_hex(reg_call_id_).substr(0,16);
            bool use_qop=qop.find("auth")!=std::string::npos;
            if(use_qop) resp=md5_hex(ha1+":"+nonce+":"+nc+":"+cnonce+":auth:"+ha2); else resp=md5_hex(ha1+":"+nonce+":"+ha2);
            std::ostringstream a; a<<(code==407?"Proxy-Authorization: Digest ":"Authorization: Digest ")
                <<"username=\""<<cfg_.registrar_user<<"\", realm=\""<<realm<<"\", nonce=\""<<nonce<<"\", uri=\""<<uri<<"\", response=\""<<resp<<"\", algorithm=MD5";
            if(use_qop) a<<", qop=auth, nc="<<nc<<", cnonce=\""<<cnonce<<"\"";
            if(!opaque.empty()) a<<", opaque=\""<<opaque<<"\"";
            reg_authorization_=a.str(); reg_auth_attempted_=true;
            if(send_register_request()){ev.type=SipEventType::None;ev.detail="retrying REGISTER with Digest authentication";return ev;}
        }
#endif
        registered_=false;ev.type=SipEventType::RegistrationFailed;ev.detail="REGISTER authentication challenge could not be satisfied";return ev;
    }
    registered_=false; ev.type=SipEventType::RegistrationFailed; ev.detail="REGISTER failed with SIP "+std::to_string(code); return ev;
}

SipEvent SipRtpServer::handle_sip(){
    SipEvent ev;
    char buf[8192]; sockaddr_in peer{};
#ifdef _WIN32
    int plen=sizeof(peer);
#else
    socklen_t plen=sizeof(peer);
#endif
    io_count_t n=::recvfrom(sip_fd_,buf,sizeof(buf)-1,0,reinterpret_cast<sockaddr*>(&peer),&plen);
    if(n<0){int e=sock_errno();if(!sock_would_block(e)){ev.type=SipEventType::Error;ev.detail=sock_error_text(e);}return ev;}
    buf[n]=0; std::string msg(buf,(size_t)n);
    size_t e=msg.find('\n'); std::string first=trim(msg.substr(0,e));
    if(first.rfind("SIP/2.0 ",0)==0) return handle_sip_response(msg);

    std::string method=first.substr(0,first.find(' ')); sip_peer_=peer;
    via_=header_value(msg,"Via"); from_=header_value(msg,"From"); to_=header_value(msg,"To");
    call_id_=header_value(msg,"Call-ID"); cseq_=header_value(msg,"CSeq");

    if(method=="INVITE") {
        if(call_active_){send_sip_response(486,"Busy Here");ev.type=SipEventType::Error;ev.detail="second simultaneous call rejected";return ev;}
        std::string rip; uint16_t rport=0; bool pcmu=false;
        if(!parse_offer(msg,rip,rport,pcmu) || !pcmu) {
            send_sip_response(488,"Not Acceptable Here","Accept: application/sdp\r\n");
            ev.type=SipEventType::Error; ev.detail="INVITE did not offer PCMU/8000 payload type 0"; return ev;
        }
        sockaddr_in ra{}; ra.sin_family=AF_INET; ra.sin_port=htons(rport);
        if(::inet_pton(AF_INET,rip.c_str(),&ra.sin_addr)!=1){send_sip_response(400,"Bad Request");ev.type=SipEventType::Error;ev.detail="bad SDP c= IP";return ev;}
        rtp_peer_=ra; have_rtp_peer_=true; symmetric_rtp_locked_=false; have_rx_seq_=false;
        if (cfg_.debug) {
            std::fprintf(stderr, "[SIP-TRACE] INVITE media offer: %s:%u PCMU=%s\n",
                         rip.c_str(), static_cast<unsigned>(rport), pcmu ? "yes" : "no");
            auto sep = msg.find("\r\n\r\n");
            if (sep != std::string::npos) std::fprintf(stderr, "[SIP-TRACE] remote SDP:\n%s", msg.substr(sep + 4).c_str());
        }
        send_sip_response(100,"Trying");
        std::ostringstream h; h<<"Contact: <sip:v92isp@"<<cfg_.advertise_ip<<":"<<cfg_.sip_port<<">\r\n"
                               <<"Allow: INVITE, ACK, BYE, OPTIONS\r\n";
        if(!send_sip_response(200,"OK",h.str(),build_sdp())){ev.type=SipEventType::Error;ev.detail="send 200 OK failed";return ev;}
        call_active_=true; ev.type=SipEventType::CallStarted; ev.detail=rip+":"+std::to_string(rport); return ev;
    }
    if(method=="ACK") { if(cfg_.debug) std::fprintf(stderr,"[SIP-TRACE] <<< ACK received\n"); ev.type=SipEventType::Ack; return ev; }
    if(method=="BYE") { send_sip_response(200,"OK"); call_active_=false; have_rtp_peer_=false; ev.type=SipEventType::CallEnded; return ev; }
    if(method=="OPTIONS") { send_sip_response(200,"OK","Allow: INVITE, ACK, BYE, OPTIONS\r\nAccept: application/sdp\r\n"); return ev; }
    if(method=="CANCEL") { send_sip_response(200,"OK"); call_active_=false; have_rtp_peer_=false; ev.type=SipEventType::CallEnded; return ev; }
    send_sip_response(501,"Not Implemented"); return ev;
}

bool SipRtpServer::recv_rtp(RtpAudio& out){
    out={}; if(rtp_fd_==invalid_socket)return false;
    uint8_t b[2048]; sockaddr_in src{};
    for (;;) {
#ifdef _WIN32
        int sl=sizeof(src);
#else
        socklen_t sl=sizeof(src);
#endif
        io_count_t n = ::recvfrom(rtp_fd_,reinterpret_cast<char*>(b),sizeof(b),0,reinterpret_cast<sockaddr*>(&src),&sl);
        if (n < 0) {
            int e = sock_errno();
#ifdef _WIN32
            if (e == WSAECONNRESET) continue; // Ignore ICMP Port Unreachable
#endif
            if (sock_would_block(e)) return false;
            return false;
        }
        if (n < 12) continue;
        unsigned version = b[0] >> 6;
        if (version != 2) continue;

        if (!symmetric_rtp_locked_) {
            rtp_peer_ = src;
            have_rtp_peer_ = true;
            symmetric_rtp_locked_ = true;
        }

        unsigned pt = b[1] & 0x7f;
        if (pt != 0) {
            // Ignore non-PCMU signaling packets (such as Cisco NSE 100 or RFC 2833 101) and keep reading
            continue;
        }

        bool padding = (b[0] & 0x20) != 0;
        bool ext = (b[0] & 0x10) != 0;
        unsigned cc = b[0] & 0x0f;
        size_t off = 12 + 4 * cc;
        if (off > (size_t)n) continue;
        if (ext) {
            if (off + 4 > (size_t)n) continue;
            uint16_t words = (uint16_t(b[off + 2]) << 8) | b[off + 3];
            off += 4 + 4 * words;
            if (off > (size_t)n) continue;
        }
        size_t end = (size_t)n;
        if (padding) {
            uint8_t p = b[n - 1];
            if (p == 0 || p > end - off) continue;
            end -= p;
        }
        uint16_t seq = (uint16_t(b[2]) << 8) | b[3];
        uint32_t ts = (uint32_t(b[4]) << 24) | (uint32_t(b[5]) << 16) | (uint32_t(b[6]) << 8) | b[7];
        if (have_rx_seq_) {
            uint16_t expect = static_cast<uint16_t>(last_rx_seq_ + 1);
            uint16_t gap = static_cast<uint16_t>(seq - expect);
            if (gap > 0 && gap < 1000) {
                out.had_gap = true;
                out.missing_packets = gap;
            }
        }
        have_rx_seq_ = true;
        last_rx_seq_ = seq;
        out.sequence = seq;
        out.timestamp = ts;
        out.pcmu.assign(b + off, b + end);
        return true;
    }
}

bool SipRtpServer::send_rtp_pcmu(const uint8_t* data,size_t n,bool marker){
    if(!call_active_||!have_rtp_peer_||rtp_fd_==invalid_socket)return false;
    std::vector<uint8_t> p(12+n); p[0]=0x80; p[1]=static_cast<uint8_t>((marker?0x80:0)|0);
    p[2]=tx_seq_>>8;p[3]=tx_seq_&0xff; p[4]=tx_ts_>>24;p[5]=tx_ts_>>16;p[6]=tx_ts_>>8;p[7]=tx_ts_;
    p[8]=ssrc_>>24;p[9]=ssrc_>>16;p[10]=ssrc_>>8;p[11]=ssrc_;
    std::memcpy(p.data()+12,data,n);
    io_count_t w=::sendto(rtp_fd_,reinterpret_cast<const char*>(p.data()),static_cast<int>(p.size()),0,reinterpret_cast<sockaddr*>(&rtp_peer_),sizeof(rtp_peer_));
    if(w!=static_cast<io_count_t>(p.size())) {
        if (cfg_.debug) {
            int e = sock_errno();
            std::fprintf(stderr, "[RTP] sendto error: %d (%s)\n", e, sock_error_text(e).c_str());
        }
        return false;
    }
    ++tx_seq_; tx_ts_+=static_cast<uint32_t>(n); return true;
}

} // namespace v92
