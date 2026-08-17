#include "ppp_userspace.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <poll.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using namespace v92;
static volatile std::sig_atomic_t g_quit = 0;
static void onsig(int) { g_quit = 1; }

static uint64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

static void usage() {
    std::cout <<
"v92isp-ppp-tun [options]\n\n"
"Userspace async-PPP server for WSL/Linux. No kernel CONFIG_PPP or pppd needed.\n"
"It terminates LCP/PAP/IPCP itself and bridges IPv4 through a Linux TUN device.\n\n"
"  --socket PATH       modem byte socket (default /run/v92isp/modem.sock)\n"
"  --ifname NAME       TUN interface (default v92tun0)\n"
"  --local IP          server PPP IP (default 10.77.0.1)\n"
"  --peer IP           caller PPP IP (default 10.77.0.2)\n"
"  --dns IP            primary DNS (default 1.1.1.1)\n"
"  --dns2 IP           secondary DNS (default 8.8.8.8)\n"
"  --mtu N             PPP/TUN MTU (default 296)\n"
"  --pap               require PAP\n"
"  --pap-secrets PATH  tab-separated USER<TAB>PASSWORD file\n"
"  --debug             protocol state logging\n"
"  --selftest           run userspace PPP handshake test; no root/TUN needed\n";
}

static bool mkdir_parent(const std::string& path) {
    auto p = path.find_last_of('/');
    if (p == std::string::npos || p == 0) return true;
    std::string d = path.substr(0,p);
    struct stat st{};
    if (::stat(d.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
    return ::mkdir(d.c_str(),0755) == 0 || errno == EEXIST;
}

static int make_listener(const std::string& path) {
    if (!mkdir_parent(path)) return -1;
    ::unlink(path.c_str());
    int fd = ::socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);
    if (fd < 0) return -1;
    sockaddr_un sa{}; sa.sun_family=AF_UNIX;
    if (path.size() >= sizeof(sa.sun_path)) { ::close(fd); errno=ENAMETOOLONG; return -1; }
    std::strncpy(sa.sun_path,path.c_str(),sizeof(sa.sun_path)-1);
    if (::bind(fd,reinterpret_cast<sockaddr*>(&sa),sizeof(sa)) != 0 || ::listen(fd,1) != 0) {
        int e=errno;::close(fd);errno=e;return -1;
    }
    ::chmod(path.c_str(),0660);
    return fd;
}

static int open_tun(const std::string& name) {
    int fd = ::open("/dev/net/tun", O_RDWR|O_NONBLOCK|O_CLOEXEC);
    if (fd < 0) return -1;
    ifreq ifr{};
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
    std::strncpy(ifr.ifr_name,name.c_str(),IFNAMSIZ-1);
    if (::ioctl(fd,TUNSETIFF,&ifr) != 0) { int e=errno;::close(fd);errno=e;return -1; }
    return fd;
}

static bool run_ip(const std::vector<std::string>& args) {
    std::vector<std::string> all{"/usr/sbin/ip"};
    if (::access(all[0].c_str(),X_OK) != 0) all[0]="/sbin/ip";
    if (::access(all[0].c_str(),X_OK) != 0) all[0]="/usr/bin/ip";
    all.insert(all.end(),args.begin(),args.end());
    std::vector<char*> av; for(auto& s:all) av.push_back(s.data()); av.push_back(nullptr);
    pid_t p=::fork(); if(p<0)return false;
    if(p==0){::execv(all[0].c_str(),av.data());_exit(127);}
    int st=0; while(::waitpid(p,&st,0)<0&&errno==EINTR){}
    return WIFEXITED(st)&&WEXITSTATUS(st)==0;
}

static bool write_all(int fd,const uint8_t* p,size_t n) {
    size_t off=0;
    while(off<n){
        ssize_t w=::write(fd,p+off,n-off);
        if(w>0){off+=(size_t)w;continue;}
        if(w<0&&errno==EINTR)continue;
        if(w<0&&(errno==EAGAIN||errno==EWOULDBLOCK)){pollfd f{fd,POLLOUT,0};::poll(&f,1,1000);continue;}
        return false;
    }
    return true;
}

static std::vector<uint8_t> decode_one(const std::vector<uint8_t>& serial, uint16_t& proto) {
    std::vector<uint8_t> r; bool esc=false;
    bool started=false;
    for(uint8_t c:serial){
        if(c==0x7e){ if(started&&!r.empty())break; started=true; continue; }
        if(!started)continue;
        if(c==0x7d){esc=true;continue;} if(esc){c^=0x20;esc=false;} r.push_back(c);
    }
    if(r.size()<6 || UserPppServer::ppp_fcs16(r.data(),r.size())!=0xf0b8)return {};
    size_t p=0;if(r.size()>=2&&r[0]==0xff&&r[1]==0x03)p=2;
    if(r[p]&1)proto=r[p++]; else {proto=(uint16_t)((r[p]<<8)|r[p+1]);p+=2;}
    return std::vector<uint8_t>(r.begin()+static_cast<long>(p),r.end()-2);
}

static bool selftest() {
    UserPppConfig c; c.mtu=296;
    UserPppServer s(c); s.start(1);
    auto tx=s.take_serial_tx(); uint16_t pr=0; auto lcp=decode_one(tx,pr);
    if(pr!=0xc021||lcp.size()<4||lcp[0]!=1)return false;
    // ACK server LCP request exactly.
    auto ack=lcp; ack[0]=2; s.feed_serial(UserPppServer::encode_async_frame(0xc021,ack),2);
    // Client LCP request: MRU 1500, ACCM 0, Magic.
    std::vector<uint8_t> body={1,4,0x05,0xdc,2,6,0,0,0,0,5,6,0x12,0x34,0x56,0x78};
    std::vector<uint8_t> creq={1,7,0,static_cast<uint8_t>(body.size()+4)};creq.insert(creq.end(),body.begin(),body.end());
    s.feed_serial(UserPppServer::encode_async_frame(0xc021,creq),3);
    tx=s.take_serial_tx();
    // May contain ACK then IPCP request. Feed frames back one by one only for our IPCP request.
    std::vector<std::vector<uint8_t>> frames; std::vector<uint8_t> cur; bool active=false;
    for(uint8_t b:tx){if(b==0x7e){if(active&&!cur.empty()){cur.insert(cur.begin(),0x7e);cur.push_back(0x7e);frames.push_back(cur);cur.clear();}active=true;}else if(active)cur.push_back(b);}
    std::vector<uint8_t> ipcp;
    for(auto& f:frames){uint16_t p=0;auto q=decode_one(f,p);if(p==0x8021&&q.size()>=4&&q[0]==1)ipcp=q;}
    if(ipcp.empty())return false;
    auto iack=ipcp;iack[0]=2;s.feed_serial(UserPppServer::encode_async_frame(0x8021,iack),4);
    // Client asks for IP/DNS with zero values -> NAK.
    std::vector<uint8_t> ib={3,6,0,0,0,0,129,6,0,0,0,0,131,6,0,0,0,0};
    std::vector<uint8_t> ir={1,9,0,static_cast<uint8_t>(ib.size()+4)};ir.insert(ir.end(),ib.begin(),ib.end());
    s.feed_serial(UserPppServer::encode_async_frame(0x8021,ir),5);
    tx=s.take_serial_tx();uint16_t np=0;auto nak=decode_one(tx,np);if(np!=0x8021||nak.size()<4||nak[0]!=3)return false;
    // Request the assigned IP/DNS.
    in_addr a{},d1{},d2{};inet_pton(AF_INET,"10.77.0.2",&a);inet_pton(AF_INET,"1.1.1.1",&d1);inet_pton(AF_INET,"8.8.8.8",&d2);
    auto pushaddr=[&](std::vector<uint8_t>& v,uint8_t t,in_addr x){v.push_back(t);v.push_back(6);auto*p=(uint8_t*)&x.s_addr;v.insert(v.end(),p,p+4);};
    ib.clear();pushaddr(ib,3,a);pushaddr(ib,129,d1);pushaddr(ib,131,d2);
    ir={1,10,0,static_cast<uint8_t>(ib.size()+4)};ir.insert(ir.end(),ib.begin(),ib.end());
    s.feed_serial(UserPppServer::encode_async_frame(0x8021,ir),6);
    s.take_serial_tx();
    if(!s.network_up())return false;
    // IPv4 modem -> TUN.
    std::vector<uint8_t> ip(20,0);ip[0]=0x45;ip[2]=0;ip[3]=20;ip[8]=64;ip[9]=1;
    s.feed_serial(UserPppServer::encode_async_frame(0x0021,ip),7);
    auto ips=s.take_ip_packets();if(ips.size()!=1||ips[0]!=ip)return false;
    // TUN -> modem.
    s.feed_ip_packet(ip);tx=s.take_serial_tx();uint16_t pp=0;auto rip=decode_one(tx,pp);
    if(pp!=0x0021||rip!=ip)return false;

    // PAP mode: prove that public-service authentication is actually enforced.
    std::string sec = "/tmp/v92isp-pap-selftest-" + std::to_string((long long)::getpid());
    { int fd=::open(sec.c_str(),O_CREAT|O_TRUNC|O_WRONLY,0600); if(fd<0)return false;
      const char line[]="retro\tsecret\n"; if(::write(fd,line,sizeof(line)-1)!=(ssize_t)(sizeof(line)-1)){::close(fd);::unlink(sec.c_str());return false;} ::close(fd); }
    UserPppConfig ac;ac.require_pap=true;ac.pap_secrets=sec;UserPppServer aeng(ac);aeng.start(10);
    tx=aeng.take_serial_tx();pr=0;lcp=decode_one(tx,pr);if(pr!=0xc021||lcp.size()<4){::unlink(sec.c_str());return false;}
    ack=lcp;ack[0]=2;aeng.feed_serial(UserPppServer::encode_async_frame(0xc021,ack),11);
    aeng.feed_serial(UserPppServer::encode_async_frame(0xc021,creq),12);aeng.take_serial_tx();
    if(aeng.phase()!=UserPppPhase::Authenticate){::unlink(sec.c_str());return false;}
    std::vector<uint8_t> papBody;std::string user="retro",pass="secret";papBody.push_back((uint8_t)user.size());papBody.insert(papBody.end(),user.begin(),user.end());papBody.push_back((uint8_t)pass.size());papBody.insert(papBody.end(),pass.begin(),pass.end());
    std::vector<uint8_t> pap={1,33,0,(uint8_t)(papBody.size()+4)};pap.insert(pap.end(),papBody.begin(),papBody.end());
    aeng.feed_serial(UserPppServer::encode_async_frame(0xc023,pap),13);::unlink(sec.c_str());
    if(aeng.phase()!=UserPppPhase::Ipcp||aeng.authenticated_user()!="retro")return false;
    return true;
}

int main(int argc,char**argv){
    std::string sock="/run/v92isp/modem.sock",ifname="v92tun0";UserPppConfig cfg;bool self=false;
    for(int i=1;i<argc;++i){std::string a=argv[i];auto need=[&](const char*o){if(i+1>=argc){std::cerr<<o<<" needs value\n";std::exit(2);}return std::string(argv[++i]);};
        if(a=="--socket")sock=need("--socket");else if(a=="--ifname")ifname=need("--ifname");
        else if(a=="--local")cfg.local_ip=need("--local");else if(a=="--peer")cfg.peer_ip=need("--peer");
        else if(a=="--dns")cfg.dns1=need("--dns");else if(a=="--dns2")cfg.dns2=need("--dns2");
        else if(a=="--mtu")cfg.mtu=(uint16_t)std::stoi(need("--mtu"));else if(a=="--pap")cfg.require_pap=true;
        else if(a=="--pap-secrets")cfg.pap_secrets=need("--pap-secrets");else if(a=="--debug")cfg.debug=true;
        else if(a=="--selftest")self=true;else if(a=="--help"||a=="-h"){usage();return 0;}else{std::cerr<<"unknown: "<<a<<"\n";return 2;}}
    if(self){bool ok=selftest();std::cout<<(ok?"[ OK ] userspace PPP LCP/IPCP/IP frame self-test\n":"[FAIL] userspace PPP self-test\n");return ok?0:1;}
    if(cfg.mtu<128||cfg.mtu>1500){std::cerr<<"MTU must be 128..1500\n";return 2;}
    if(::geteuid()!=0){std::cerr<<"Run as root (TUN and network configuration require CAP_NET_ADMIN).\n";return 1;}
    std::signal(SIGINT,onsig);std::signal(SIGTERM,onsig);
    int tun=open_tun(ifname);if(tun<0){std::cerr<<"open /dev/net/tun: "<<std::strerror(errno)<<"\n"<<"On WSL run: sudo modprobe tun; sudo mkdir -p /dev/net; sudo mknod /dev/net/tun c 10 200\n";return 1;}
    if(!run_ip({"link","set","dev",ifname,"mtu",std::to_string(cfg.mtu),"up"}) ||
       !run_ip({"addr","replace",cfg.local_ip,"peer",cfg.peer_ip,"dev",ifname})){
        std::cerr<<"Failed to configure "<<ifname<<" with ip(8)\n";::close(tun);return 1;
    }
    int listener=make_listener(sock);if(listener<0){std::cerr<<"socket "<<sock<<": "<<std::strerror(errno)<<"\n";::close(tun);return 1;}
    std::cout<<"[PPP] userspace PPP ready: "<<cfg.peer_ip<<" <-> "<<cfg.local_ip<<" via "<<ifname<<" MTU "<<cfg.mtu<<"\n";
    std::cout<<"[PPP] waiting for modem engine on "<<sock<<"\n";
    while(!g_quit){pollfd w{listener,POLLIN,0};int r=::poll(&w,1,250);if(r<=0)continue;int c=::accept4(listener,nullptr,nullptr,SOCK_CLOEXEC|SOCK_NONBLOCK);if(c<0)continue;
        // Flush stale TUN packets from a previous call.
        uint8_t buf[65536];while(::read(tun,buf,sizeof(buf))>0){}
        UserPppServer ppp(cfg);ppp.start(now_ms());auto tx=ppp.take_serial_tx();if(!tx.empty())write_all(c,tx.data(),tx.size());
        std::cout<<"[PPP] modem data channel connected; LCP started\n";UserPppPhase last=ppp.phase();
        bool alive=true;while(alive&&!g_quit){pollfd f[2]={{c,POLLIN,0},{tun,POLLIN,0}};int pr=::poll(f,ppp.network_up()?2:1,100);uint64_t t=now_ms();ppp.tick(t);
            if(pr<0&&errno!=EINTR){alive=false;break;}
            if(f[0].revents&(POLLHUP|POLLERR|POLLNVAL)){alive=false;break;}
            if(f[0].revents&POLLIN){ssize_t n=::read(c,buf,sizeof(buf));if(n<=0){if(n==0)alive=false;}else ppp.feed_serial(buf,(size_t)n,t);}
            if(ppp.network_up()&&(f[1].revents&POLLIN)){ssize_t n=::read(tun,buf,sizeof(buf));if(n>0)ppp.feed_ip_packet(buf,(size_t)n);}
            for(auto& ip:ppp.take_ip_packets())if(!ip.empty())write_all(tun,ip.data(),ip.size());
            tx=ppp.take_serial_tx();if(!tx.empty()&&!write_all(c,tx.data(),tx.size()))alive=false;
            if(ppp.phase()!=last){std::cout<<"[PPP] phase "<<to_string(ppp.phase());if(!ppp.last_event().empty())std::cout<<": "<<ppp.last_event();std::cout<<"\n";ppp.clear_event();last=ppp.phase();}
            if(ppp.phase()==UserPppPhase::Failed||ppp.phase()==UserPppPhase::Terminating)alive=false;
        }
        ::close(c);std::cout<<"[PPP] modem disconnected; ready for next call\n";
    }
    ::close(listener);::unlink(sock.c_str());::close(tun);return 0;
}
