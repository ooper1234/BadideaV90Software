#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace v92 {

struct PppConfig {
    std::string pppd_path = "/usr/sbin/pppd";
    std::string local_ip = "10.77.0.1";
    std::string peer_ip = "10.77.0.2";
    std::string dns1 = "1.1.1.1";
    std::string dns2 = "8.8.8.8";
    int tty_speed = 115200;
    int mtu = 296;
    bool debug = false;
    bool require_pap = false;
    std::string server_name = "v92isp";
    std::string ipparam = "v92isp";
};

// Build the exact argv passed to pppd. Exposed so it can be unit-tested.
std::vector<std::string> build_pppd_args(const PppConfig& cfg,
                                         const std::string& slave_tty);

// PTY-backed PPP terminator. The modem side exchanges already-demodulated
// octets with master_fd(); pppd owns the slave side and creates pppN.
class PppBackend {
public:
    explicit PppBackend(PppConfig cfg = {});
    ~PppBackend();

    PppBackend(const PppBackend&) = delete;
    PppBackend& operator=(const PppBackend&) = delete;

    bool start();
    void stop();
    bool running();

    int master_fd() const { return master_fd_; }
    int child_pid() const { return child_pid_; }
    const std::string& slave_tty() const { return slave_tty_; }
    const std::string& last_error() const { return last_error_; }

private:
    PppConfig cfg_;
    int master_fd_ = -1;
    int child_pid_ = -1;
    int slave_hold_fd_ = -1;
    std::string slave_tty_;
    std::string last_error_;
};

} // namespace v92
