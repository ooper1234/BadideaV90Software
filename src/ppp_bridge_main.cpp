#include "ppp_backend.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

using namespace v92;

static volatile std::sig_atomic_t quit_flag = 0;
static void on_signal(int) { quit_flag = 1; }

static void usage() {
    std::cout
        << "v92isp-ppp-bridge [options]\n\n"
        << "Bridges a raw modem byte stream on a Unix socket to Linux pppd.\n"
        << "The modem engine connects to the socket after CONNECT and sends/receives\n"
        << "the exact octets from the modem data channel.\n\n"
        << "Options:\n"
        << "  --socket PATH       /run/v92isp/modem.sock\n"
        << "  --local IP          PPP server IP (default 10.77.0.1)\n"
        << "  --peer IP           caller IP (default 10.77.0.2)\n"
        << "  --dns IP            primary DNS (default 1.1.1.1)\n"
        << "  --dns2 IP           secondary DNS (default 8.8.8.8)\n"
        << "  --pppd PATH         pppd executable (default /usr/sbin/pppd)\n"
        << "  --mtu N             PPP MTU/MRU (default 296; good for slow links)\n"
        << "  --server-name NAME  PAP server name (default v92isp)\n"
        << "  --ipparam NAME      pppd ipparam tag (default v92isp)\n"
        << "  --pap               require PAP (configure /etc/ppp/pap-secrets)\n"
        << "  --debug             pppd protocol logging to stderr\n";
}

static bool mkdir_parent(const std::string& path) {
    auto pos = path.find_last_of('/');
    if (pos == std::string::npos || pos == 0) return true;
    std::string dir = path.substr(0, pos);
    struct stat st{};
    if (::stat(dir.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
    return ::mkdir(dir.c_str(), 0755) == 0 || errno == EEXIST;
}

static int make_listener(const std::string& path) {
    if (!mkdir_parent(path)) return -1;
    ::unlink(path.c_str());
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    if (path.size() >= sizeof(sa.sun_path)) { ::close(fd); errno = ENAMETOOLONG; return -1; }
    std::strncpy(sa.sun_path, path.c_str(), sizeof(sa.sun_path)-1);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0 || ::listen(fd, 1) != 0) {
        int e=errno; ::close(fd); errno=e; return -1;
    }
    ::chmod(path.c_str(), 0660);
    return fd;
}

static bool pump(int a, int b) {
    char buf[8192];
    ssize_t n = ::read(a, buf, sizeof(buf));
    if (n == 0) return false;
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return true;
        return false;
    }
    ssize_t off = 0;
    while (off < n) {
        ssize_t w = ::write(b, buf + off, static_cast<size_t>(n - off));
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) { ::usleep(1000); continue; }
            return false;
        }
        off += w;
    }
    return true;
}

int main(int argc, char** argv) {
    PppConfig cfg;
    std::string sock = "/run/v92isp/modem.sock";
    for (int i=1;i<argc;++i) {
        std::string a=argv[i];
        auto need=[&](const char* opt)->std::string {
            if(i+1>=argc){ std::cerr<<opt<<" needs a value\n"; std::exit(2); }
            return argv[++i];
        };
        if(a=="--socket") sock=need("--socket");
        else if(a=="--local") cfg.local_ip=need("--local");
        else if(a=="--peer") cfg.peer_ip=need("--peer");
        else if(a=="--dns") cfg.dns1=need("--dns");
        else if(a=="--dns2") cfg.dns2=need("--dns2");
        else if(a=="--pppd") cfg.pppd_path=need("--pppd");
        else if(a=="--mtu") cfg.mtu=std::stoi(need("--mtu"));
        else if(a=="--server-name") cfg.server_name=need("--server-name");
        else if(a=="--ipparam") cfg.ipparam=need("--ipparam");
        else if(a=="--pap") cfg.require_pap=true;
        else if(a=="--debug") cfg.debug=true;
        else if(a=="--help" || a=="-h") { usage(); return 0; }
        else { std::cerr<<"unknown option: "<<a<<"\n"; usage(); return 2; }
    }

    if(cfg.mtu < 128 || cfg.mtu > 1500){ std::cerr<<"--mtu must be 128..1500\n"; return 2; }
    std::signal(SIGINT,on_signal); std::signal(SIGTERM,on_signal);
    int listener=make_listener(sock);
    if(listener<0){ std::cerr<<"socket "<<sock<<": "<<std::strerror(errno)<<"\n"; return 1; }

    std::cout << "[PPP] waiting for modem engine on " << sock << "\n";
    std::cout << "[PPP] caller will receive " << cfg.peer_ip << ", server " << cfg.local_ip
              << ", DNS " << cfg.dns1 << " / " << cfg.dns2 << "\n";

    while(!quit_flag){
        pollfd p{listener,POLLIN,0};
        int pr=::poll(&p,1,250);
        if(pr<=0) continue;
        int client=::accept(listener,nullptr,nullptr);
        if(client<0) continue;
        std::cout << "[PPP] modem connected; starting pppd\n";

        PppBackend backend(cfg);
        if(!backend.start()){
            std::cerr<<"[PPP] start failed: "<<backend.last_error()<<"\n";
            ::close(client); continue;
        }
        std::cout << "[PPP] pppd PID "<<backend.child_pid()<<" on "<<backend.slave_tty()<<"\n";

        while(!quit_flag && backend.running()){
            pollfd fds[2]={{client,POLLIN,0},{backend.master_fd(),POLLIN,0}};
            int r=::poll(fds,2,500);
            if(r<0){ if(errno==EINTR) continue; break; }
            if(r==0) continue;
            if((fds[0].revents & (POLLHUP|POLLERR|POLLNVAL)) ||
               (fds[1].revents & (POLLHUP|POLLERR|POLLNVAL))) break;
            if((fds[0].revents&POLLIN) && !pump(client,backend.master_fd())) break;
            if((fds[1].revents&POLLIN) && !pump(backend.master_fd(),client)) break;
        }
        backend.stop();
        ::close(client);
        std::cout << "[PPP] modem disconnected; ready for next call\n";
    }
    ::close(listener); ::unlink(sock.c_str());
    return 0;
}
