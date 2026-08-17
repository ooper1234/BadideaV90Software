#include "ppp_backend.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <utility>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

namespace v92 {

std::vector<std::string> build_pppd_args(const PppConfig& c,
                                         const std::string& tty) {
    std::vector<std::string> a;
    a.push_back(c.pppd_path);
    a.push_back(tty);
    a.push_back(std::to_string(c.tty_speed));

    // The PTY is a permanently-present local data circuit. Do not use modem
    // control or hardware flow control here; the modem PHY handles that side.
    a.push_back("local");
    a.push_back("nocrtscts");
    a.push_back("nodetach");
    a.push_back("noipdefault");
    a.push_back("nodefaultroute");
    a.push_back("noccp");
    // This release routes IPv4 only. Prevent a client from installing an IPv6
    // link that has no corresponding firewall/NAT policy.
    a.push_back("noipv6");
    a.push_back("lcp-echo-interval");
    a.push_back("30");
    a.push_back("lcp-echo-failure");
    a.push_back("4");
    a.push_back("mtu");
    a.push_back(std::to_string(c.mtu));
    a.push_back("mru");
    a.push_back(std::to_string(c.mtu));
    a.push_back(c.local_ip + ":" + c.peer_ip);
    a.push_back("ipparam");
    a.push_back(c.ipparam);

    if (!c.dns1.empty()) {
        a.push_back("ms-dns");
        a.push_back(c.dns1);
    }
    if (!c.dns2.empty()) {
        a.push_back("ms-dns");
        a.push_back(c.dns2);
    }

    if (c.require_pap) {
        a.push_back("auth");
        a.push_back("+pap");
        a.push_back("-chap");
        a.push_back("refuse-eap");
        a.push_back("refuse-mschap");
        a.push_back("refuse-mschap-v2");
        a.push_back("name");
        a.push_back(c.server_name);
    } else {
        // Lab default: Windows may send a username, but the server does not
        // require one. This makes first bring-up much easier.
        a.push_back("noauth");
    }

    if (c.debug) {
        a.push_back("debug");
        a.push_back("dump");
        a.push_back("logfd");
        a.push_back("2");
    }
    return a;
}

PppBackend::PppBackend(PppConfig cfg) : cfg_(std::move(cfg)) {}
PppBackend::~PppBackend() { stop(); }

bool PppBackend::start() {
    if (running()) return true;
    last_error_.clear();

    int fd = ::posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        last_error_ = std::string("posix_openpt: ") + std::strerror(errno);
        return false;
    }
    if (::grantpt(fd) != 0 || ::unlockpt(fd) != 0) {
        last_error_ = std::string("grantpt/unlockpt: ") + std::strerror(errno);
        ::close(fd);
        return false;
    }
    char* n = ::ptsname(fd);
    if (!n) {
        last_error_ = std::string("ptsname: ") + std::strerror(errno);
        ::close(fd);
        return false;
    }
    slave_tty_ = n;

    // Put the PTY into an 8-bit-clean state before any modem octets can reach
    // the slave. pppd will apply its own serial settings after exec, but doing
    // this here avoids a startup race with the default terminal line discipline.
    int sfd = ::open(slave_tty_.c_str(), O_RDWR | O_NOCTTY);
    if (sfd < 0) {
        last_error_ = std::string("open slave PTY: ") + std::strerror(errno);
        ::close(fd);
        return false;
    }
    termios tio{};
    if (::tcgetattr(sfd, &tio) == 0) {
        ::cfmakeraw(&tio);
        tio.c_cflag |= (CLOCAL | CREAD);
        ::tcsetattr(sfd, TCSANOW, &tio);
    }
    // Keep one slave descriptor open in the parent for the backend lifetime.
    // Otherwise the PTY master can briefly report POLLHUP before pppd opens
    // the slave, which loses the first PPP bytes on a fast caller.
    slave_hold_fd_ = sfd;

    auto args = build_pppd_args(cfg_, slave_tty_);
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& s : args) argv.push_back(s.data());
    argv.push_back(nullptr);

    pid_t p = ::fork();
    if (p < 0) {
        last_error_ = std::string("fork: ") + std::strerror(errno);
        ::close(slave_hold_fd_); slave_hold_fd_ = -1;
        ::close(fd);
        return false;
    }
    if (p == 0) {
        ::close(slave_hold_fd_);
        ::execv(cfg_.pppd_path.c_str(), argv.data());
        _exit(127);
    }

    master_fd_ = fd;
    child_pid_ = static_cast<int>(p);
    return true;
}

void PppBackend::stop() {
    if (child_pid_ > 0) {
        ::kill(child_pid_, SIGTERM);
        int status = 0;
        for (int i = 0; i < 20; ++i) {
            pid_t r = ::waitpid(child_pid_, &status, WNOHANG);
            if (r == child_pid_) break;
            ::usleep(50000);
        }
        if (::waitpid(child_pid_, &status, WNOHANG) == 0) {
            ::kill(child_pid_, SIGKILL);
            ::waitpid(child_pid_, &status, 0);
        }
        child_pid_ = -1;
    }
    if (master_fd_ >= 0) {
        ::close(master_fd_);
        master_fd_ = -1;
    }
    if (slave_hold_fd_ >= 0) {
        ::close(slave_hold_fd_);
        slave_hold_fd_ = -1;
    }
    slave_tty_.clear();
}

bool PppBackend::running() {
    if (child_pid_ <= 0) return false;
    int status = 0;
    pid_t r = ::waitpid(child_pid_, &status, WNOHANG);
    if (r == 0) return true;
    if (r == child_pid_) { child_pid_ = -1; return false; }
    return errno == EINTR;
}

} // namespace v92
