#include "sip_rtp.hpp"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cctype>
#include <cstring>
#include <iostream>
#include <map>
#include <sstream>
#include <string>

#ifdef V92_HAVE_OPENSSL
#include <openssl/evp.h>
#endif

using namespace v92;

static std::string trim(std::string s) {
    while (!s.empty() && (s.back()=='\r' || s.back()=='\n' || std::isspace((unsigned char)s.back()))) s.pop_back();
    size_t i=0; while(i<s.size() && std::isspace((unsigned char)s[i])) ++i;
    return s.substr(i);
}
static std::string header(const std::string& msg, const std::string& name) {
    size_t p=0;
    while(p<msg.size()) {
        size_t e=msg.find('\n',p); if(e==std::string::npos)e=msg.size();
        std::string line=msg.substr(p,e-p); if(!line.empty()&&line.back()=='\r')line.pop_back();
        if(line.size()>name.size()+1 && strncasecmp(line.c_str(),name.c_str(),name.size())==0 && line[name.size()]==':')
            return trim(line.substr(name.size()+1));
        p=e+1;
    }
    return {};
}
static std::map<std::string,std::string> digest_params(std::string s) {
    auto sp=s.find(' '); if(sp!=std::string::npos)s=s.substr(sp+1);
    std::map<std::string,std::string> out; size_t p=0;
    while(p<s.size()) {
        while(p<s.size() && (s[p]==' '||s[p]==','))++p;
        auto eq=s.find('=',p); if(eq==std::string::npos)break;
        std::string k=trim(s.substr(p,eq-p)); size_t v=eq+1; std::string val;
        if(v<s.size()&&s[v]=='"') { ++v; auto q=s.find('"',v); if(q==std::string::npos)break; val=s.substr(v,q-v); p=q+1; }
        else { auto q=s.find(',',v); if(q==std::string::npos)q=s.size(); val=trim(s.substr(v,q-v)); p=q; }
        out[k]=val;
    }
    return out;
}
#ifdef V92_HAVE_OPENSSL
static std::string md5(const std::string& s) {
    unsigned char d[EVP_MAX_MD_SIZE]; unsigned n=0; EVP_MD_CTX* c=EVP_MD_CTX_new(); assert(c);
    assert(EVP_DigestInit_ex(c,EVP_md5(),nullptr)==1);
    assert(EVP_DigestUpdate(c,s.data(),s.size())==1);
    assert(EVP_DigestFinal_ex(c,d,&n)==1); EVP_MD_CTX_free(c);
    const char* h="0123456789abcdef"; std::string o; o.reserve(n*2);
    for(unsigned i=0;i<n;++i){o.push_back(h[d[i]>>4]);o.push_back(h[d[i]&15]);}
    return o;
}
#endif
static bool wait_readable(int fd, int ms=1500){ pollfd p{fd,POLLIN,0}; return ::poll(&p,1,ms)>0 && (p.revents&POLLIN); }
static std::string recv_msg(int fd, sockaddr_in* peer=nullptr) {
    char b[16384]; sockaddr_in p{}; socklen_t n=sizeof(p); ssize_t r=::recvfrom(fd,b,sizeof(b),0,(sockaddr*)&p,&n); assert(r>0); if(peer)*peer=p; return std::string(b,(size_t)r);
}

int main(){
#ifndef V92_HAVE_OPENSSL
    std::cout << "[SKIP] OpenSSL not compiled; empty-password Digest test unavailable\n";
    return 0;
#else
    int reg=::socket(AF_INET,SOCK_DGRAM,0); assert(reg>=0);
    sockaddr_in rsa{}; rsa.sin_family=AF_INET; rsa.sin_addr.s_addr=htonl(INADDR_LOOPBACK); rsa.sin_port=0;
    assert(::bind(reg,(sockaddr*)&rsa,sizeof(rsa))==0); socklen_t rsl=sizeof(rsa); assert(::getsockname(reg,(sockaddr*)&rsa,&rsl)==0);
    unsigned rport=ntohs(rsa.sin_port);

    SipRtpConfig c; c.bind_ip="127.0.0.1"; c.advertise_ip="127.0.0.1"; c.sip_port=0; c.rtp_port=0;
    c.registrar_host="127.0.0.1"; c.registrar_port=(uint16_t)rport; c.registrar_user="101"; c.registrar_password="";
    SipRtpServer s(c); assert(s.open()); assert(s.send_register());
    assert(wait_readable(reg)); sockaddr_in client{}; std::string q1=recv_msg(reg,&client);
    assert(q1.rfind("REGISTER sip:127.0.0.1:"+std::to_string(rport)+" SIP/2.0",0)==0);
    assert(header(q1,"Authorization").empty());

    const std::string realm="v92isp-selftest", nonce="abcdef0123456789";
    std::ostringstream challenge;
    challenge << "SIP/2.0 401 Unauthorized\r\n"
              << "Via: " << header(q1,"Via") << "\r\n"
              << "From: " << header(q1,"From") << "\r\n"
              << "To: " << header(q1,"To") << ";tag=regtest\r\n"
              << "Call-ID: " << header(q1,"Call-ID") << "\r\n"
              << "CSeq: " << header(q1,"CSeq") << "\r\n"
              << "WWW-Authenticate: Digest realm=\""<<realm<<"\", nonce=\""<<nonce<<"\", algorithm=MD5, qop=\"auth\"\r\n"
              << "Content-Length: 0\r\n\r\n";
    auto ch=challenge.str(); assert(::sendto(reg,ch.data(),ch.size(),0,(sockaddr*)&client,sizeof(client))==(ssize_t)ch.size());
    assert(wait_readable(s.sip_fd())); auto ev=s.handle_sip(); assert(ev.type==SipEventType::None);

    assert(wait_readable(reg)); sockaddr_in client2{}; std::string q2=recv_msg(reg,&client2);
    std::string auth=header(q2,"Authorization"); assert(auth.rfind("Digest ",0)==0);
    auto p=digest_params(auth); assert(p["username"]=="101"); assert(p["realm"]==realm); assert(p["nonce"]==nonce); assert(p["qop"]=="auth");
    std::string uri="sip:127.0.0.1:"+std::to_string(rport);
    std::string ha1=md5("101:"+realm+":"); // deliberately empty password
    std::string ha2=md5("REGISTER:"+uri);
    std::string expected=md5(ha1+":"+nonce+":"+p["nc"]+":"+p["cnonce"]+":auth:"+ha2);
    assert(p["response"]==expected);

    std::ostringstream ok;
    ok << "SIP/2.0 200 OK\r\n"
       << "Via: "<<header(q2,"Via")<<"\r\nFrom: "<<header(q2,"From")<<"\r\nTo: "<<header(q2,"To")<<";tag=ok\r\n"
       << "Call-ID: "<<header(q2,"Call-ID")<<"\r\nCSeq: "<<header(q2,"CSeq")<<"\r\nExpires: 300\r\nContent-Length: 0\r\n\r\n";
    auto oks=ok.str(); assert(::sendto(reg,oks.data(),oks.size(),0,(sockaddr*)&client2,sizeof(client2))==(ssize_t)oks.size());
    assert(wait_readable(s.sip_fd())); ev=s.handle_sip(); assert(ev.type==SipEventType::RegistrationOk); assert(s.registered());
    ::close(reg);
    std::cout << "[ OK ] SIP Digest REGISTER with user 101 and empty password\n";
    return 0;
#endif
}
