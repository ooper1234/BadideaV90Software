#ifdef _WIN32

#include "pjsip_frontend.hpp"
#include "g711.hpp"

#include <pjmedia/alaw_ulaw.h>
#include <pjsua2.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <sstream>
#include <thread>

namespace v92 {
using namespace pj;


class ModemAudioPort final : public AudioMediaPort {
public:
    explicit ModemAudioPort(PjsipFrontend::Impl& owner) : owner_(owner) {}
    void onFrameRequested(MediaFrame& frame) override;
    void onFrameReceived(MediaFrame& frame) override;
private:
    PjsipFrontend::Impl& owner_;
};

class V92Call final : public Call {
public:
    V92Call(Account& acc, int call_id, PjsipFrontend::Impl& owner)
        : Call(acc, call_id), owner_(owner) {}
    void onCallState(OnCallStateParam& prm) override;
    void onCallMediaState(OnCallMediaStateParam& prm) override;
private:
    PjsipFrontend::Impl& owner_;
    bool media_wired_ = false;
};

class V92Account final : public Account {
public:
    explicit V92Account(PjsipFrontend::Impl& owner) : owner_(owner) {}
    void onRegState(OnRegStateParam& prm) override;
    void onIncomingCall(OnIncomingCallParam& prm) override;
private:
    PjsipFrontend::Impl& owner_;
};

class PjsipFrontend::Impl {
public:
    Impl(PjsipFrontendConfig c, LiveMode m)
        : cfg(std::move(c)), modem(m), audio(*this), account(*this) {}

    PjsipFrontendConfig cfg;
    LiveModem modem;
    Endpoint ep;
    ModemAudioPort audio;
    V92Account account;
    V92Call* call = nullptr;

    mutable std::mutex mu;
    std::mutex events_mu;
    std::deque<FrontendEvent> events;
    std::atomic_bool opened{false};
    std::atomic_bool active{false};
    std::atomic<uint64_t> rx_count{0};

    void push(FrontendEventType t, std::string s) {
        std::lock_guard<std::mutex> lk(events_mu);
        events.push_back({t, std::move(s)});
    }

    void begin_call(V92Call* c) {
        {
            std::lock_guard<std::mutex> lk(mu);
            call = c;
            modem.start_call();
        }
        rx_count = 0;
        active = true;
        push(FrontendEventType::CallStarted, "incoming call answered by PJSIP");
    }

    void end_call(V92Call* c) {
        {
            std::lock_guard<std::mutex> lk(mu);
            if (call == c) call = nullptr;
            modem.end_call();
        }
        active = false;
        push(FrontendEventType::CallEnded, "call ended; inbound audio frames=" + std::to_string(rx_count.load()));
    }

    void wire_media(V92Call& c) {
        try {
            AudioMedia am = c.getAudioMedia(-1);
            audio.startTransmit(am);       // modem -> call
            am.startTransmit(audio);       // call -> modem
            push(FrontendEventType::MediaActive, "PJSIP RTP/PCMU media active in both directions");
        } catch (const Error& e) {
            push(FrontendEventType::Error, "media wire failed: " + e.info());
        }
    }
};

void ModemAudioPort::onFrameRequested(MediaFrame& frame) {
    std::vector<uint8_t> u;
    {
        std::lock_guard<std::mutex> lk(owner_.mu);
        u = owner_.modem.next_tx_pcmu(160);
    }
    if (u.size() < 160) u.resize(160, 0xFF);
    frame.type = PJMEDIA_FRAME_TYPE_AUDIO;
    frame.size = 160 * sizeof(int16_t);
    frame.buf.resize(frame.size);
    for (size_t i = 0; i < 160; ++i) {
        // Preserve PCMU negative zero (0x7F).  V.90 Sd/S-bar uses both +0
        // and -0 as distinct PCM codewords; decoding 0x7F to integer zero
        // here made PJSIP re-encode it as 0xFF and corrupted Phase 3.
        int16_t s = ulaw_to_linear_for_reencode(u[i]);
        std::memcpy(frame.buf.data() + i * sizeof(int16_t), &s, sizeof(s));
    }
}

void ModemAudioPort::onFrameReceived(MediaFrame& frame) {
    if (frame.type != PJMEDIA_FRAME_TYPE_AUDIO || frame.size < sizeof(int16_t)) return;
    size_t n = std::min<size_t>(frame.size / sizeof(int16_t), frame.buf.size() / sizeof(int16_t));
    std::vector<int16_t> pcm(n);
    std::memcpy(pcm.data(), frame.buf.data(), n * sizeof(int16_t));
    {
        std::lock_guard<std::mutex> lk(owner_.mu);
        owner_.modem.receive_pcm(pcm);
    }
    auto prev = owner_.rx_count.fetch_add(1, std::memory_order_relaxed);
    if (prev == 0) {
        owner_.push(FrontendEventType::MediaActive,
                    "FIRST inbound caller PCM frame received through PJSIP");
    }
}

void V92Call::onCallState(OnCallStateParam&) {
    try {
        CallInfo ci = getInfo();
        std::ostringstream s;
        s << "call state " << ci.stateText;
        owner_.push(FrontendEventType::Registration, s.str());
        if (ci.state == PJSIP_INV_STATE_DISCONNECTED) {
            owner_.end_call(this);
            delete this;
        }
    } catch (const Error& e) {
        owner_.push(FrontendEventType::Error, "call state error: " + e.info());
    }
}

void V92Call::onCallMediaState(OnCallMediaStateParam&) {
    if (media_wired_) return;
    try {
        CallInfo ci = getInfo();
        for (unsigned i = 0; i < ci.media.size(); ++i) {
            const CallMediaInfo& mi = ci.media[i];
            if (mi.type == PJMEDIA_TYPE_AUDIO &&
                (mi.status == PJSUA_CALL_MEDIA_ACTIVE || mi.status == PJSUA_CALL_MEDIA_REMOTE_HOLD)) {
                AudioMedia am = getAudioMedia(static_cast<int>(i));
                owner_.audio.startTransmit(am);
                am.startTransmit(owner_.audio);
                media_wired_ = true;
                owner_.push(FrontendEventType::MediaActive,
                            "PJSIP negotiated audio media; modem PCM bridge connected");
                break;
            }
        }
    } catch (const Error& e) {
        owner_.push(FrontendEventType::Error, "onCallMediaState: " + e.info());
    }
}

void V92Account::onRegState(OnRegStateParam& prm) {
    try {
        AccountInfo ai = getInfo();
        std::ostringstream s;
        s << (ai.regIsActive ? "registered" : "registration state")
          << " SIP " << prm.code << " " << prm.reason;
        owner_.push(FrontendEventType::Registration, s.str());
    } catch (const Error& e) {
        owner_.push(FrontendEventType::Error, "registration callback: " + e.info());
    }
}

void V92Account::onIncomingCall(OnIncomingCallParam& prm) {
    if (owner_.active.load()) {
        auto* busy = new V92Call(*this, prm.callId, owner_);
        CallOpParam op;
        op.statusCode = PJSIP_SC_BUSY_HERE;
        try { busy->answer(op); } catch (...) {}
        delete busy;
        return;
    }
    auto* c = new V92Call(*this, prm.callId, owner_);
    owner_.begin_call(c);
    try {
        CallOpParam op;
        op.statusCode = PJSIP_SC_OK;
        c->answer(op);
    } catch (const Error& e) {
        owner_.push(FrontendEventType::Error, "answer failed: " + e.info());
        owner_.end_call(c);
        delete c;
    }
}

PjsipFrontend::PjsipFrontend(PjsipFrontendConfig cfg, LiveMode mode)
    : impl_(std::make_unique<Impl>(std::move(cfg), mode)) {}
PjsipFrontend::~PjsipFrontend() { close(); }

bool PjsipFrontend::open() {
    if (impl_->opened.load()) return true;

    // V.90 Sd/S-bar deliberately alternates the two G.711 u-law zero
    // codewords.  The modem bridge decodes 0x7f as -1 so it can survive this
    // signed-linear port; refuse to start with an unpatched PJSIP encoder
    // instead of silently corrupting every negative-zero training symbol.
    if (pjmedia_linear2ulaw(-1) != 0x7f) {
        last_error_ = "PJSIP PCMU encoder does not preserve V.90 negative zero; rebuild with windows/Build-Windows.ps1";
        return false;
    }

    try {
        impl_->ep.libCreate();
        EpConfig ec;
        ec.uaConfig.userAgent = "v92isp-pjsip/1.0";
        ec.uaConfig.maxCalls = 1;
        ec.logConfig.level = impl_->cfg.debug ? 5 : 3;
        ec.logConfig.consoleLevel = impl_->cfg.debug ? 4 : 3;
        ec.logConfig.msgLogging = impl_->cfg.debug;
        ec.medConfig.clockRate = 8000;
        ec.medConfig.sndClockRate = 8000;
        ec.medConfig.channelCount = 1;
        ec.medConfig.audioFramePtime = 20;
        ec.medConfig.noVad = true;
        ec.medConfig.ecTailLen = 0;
        impl_->ep.libInit(ec);

        TransportConfig tc;
        tc.port = impl_->cfg.sip_port;
        tc.boundAddress = impl_->cfg.bind_ip;
        tc.publicAddress = impl_->cfg.advertise_ip.empty() ? impl_->cfg.bind_ip : impl_->cfg.advertise_ip;
        impl_->ep.transportCreate(PJSIP_TRANSPORT_UDP, tc);
        impl_->ep.libStart();
        impl_->ep.audDevManager().setNullDev();

        // Disable every speech codec except G.711 u-law. This prevents the PBX
        // from selecting a lossy voice codec that would destroy modem tones.
        bool have_pcmu = false;
        for (const auto& c : impl_->ep.codecEnum2()) {
            bool pcmu = c.codecId.rfind("PCMU/8000", 0) == 0;
            impl_->ep.codecSetPriority(c.codecId, pcmu ? 255 : 0);
            if (pcmu) have_pcmu = true;
        }
        if (!have_pcmu) { last_error_ = "PJSIP has no PCMU/8000 codec"; impl_->ep.libDestroy(); return false; }

        MediaFormatAudio fmt;
        fmt.init(PJMEDIA_FORMAT_PCM, 8000, 1, 20000, 16);
        impl_->audio.createPort("v92isp-modem-pcm", fmt);

        AccountConfig ac;
        std::string hostport = impl_->cfg.registrar_host + ":" + std::to_string(impl_->cfg.registrar_port);
        ac.idUri = "sip:" + impl_->cfg.user + "@" + impl_->cfg.registrar_host;
        ac.regConfig.registrarUri = "sip:" + hostport;
        ac.regConfig.registerOnAdd = true;
        ac.sipConfig.authCreds.push_back(AuthCredInfo("digest", "*", impl_->cfg.user, 0, impl_->cfg.password));
        ac.natConfig.iceEnabled = false;
        ac.natConfig.sipStunUse = PJSUA_STUN_USE_DISABLED;
        ac.natConfig.mediaStunUse = PJSUA_STUN_USE_DISABLED;
        // The ESP32 MiniPBX treats PJSIP's default UDP CRLF keep-alive as an
        // unsupported SIP method and replies with a malformed 405 lacking
        // CSeq. We are on the same LAN and do not need NAT keep-alives, so
        // disable them entirely. This removes the repeating PJSIP_EMISSINGHDR
        // noise without affecting REGISTER refreshes.
        ac.natConfig.udpKaIntervalSec = 0;
        ac.natConfig.sipOutboundUse = 0;
        ac.mediaConfig.transportConfig.port = impl_->cfg.rtp_port;
        ac.mediaConfig.transportConfig.portRange = 0;
        ac.mediaConfig.transportConfig.boundAddress = impl_->cfg.bind_ip;
        ac.mediaConfig.transportConfig.publicAddress = impl_->cfg.advertise_ip.empty() ? impl_->cfg.bind_ip : impl_->cfg.advertise_ip;
        ac.mediaConfig.rtcpMuxEnabled = false;
        impl_->account.create(ac);

        impl_->opened = true;
        impl_->push(FrontendEventType::Registration,
                    "PJSIP frontend started on " + impl_->cfg.bind_ip + ":" + std::to_string(impl_->cfg.sip_port));
        return true;
    } catch (const Error& e) {
        last_error_ = e.info();
        try { impl_->ep.libDestroy(); } catch (...) {}
        return false;
    } catch (const std::exception& e) {
        last_error_ = e.what();
        try { impl_->ep.libDestroy(); } catch (...) {}
        return false;
    }
}

void PjsipFrontend::close() {
    if (!impl_ || !impl_->opened.exchange(false)) return;
    try {
        if (impl_->call) {
            CallOpParam op;
            op.statusCode = PJSIP_SC_DECLINE;
            impl_->call->hangup(op);
        }
    } catch (...) {}
    try { impl_->ep.libDestroy(); } catch (...) {}
}

bool PjsipFrontend::pop_event(FrontendEvent& out) {
    std::lock_guard<std::mutex> lk(impl_->events_mu);
    if (impl_->events.empty()) return false;
    out = std::move(impl_->events.front());
    impl_->events.pop_front();
    return true;
}

bool PjsipFrontend::call_active() const { return impl_->active.load(); }
bool PjsipFrontend::hangup_current_call() {
    if (!impl_ || !impl_->active.load()) return false;
    try {
        V92Call* c = impl_->call;
        if (!c) return false;
        CallOpParam op;
        // Established-dialog hangup: PJSIP will generate BYE.  Dropping the
        // SIP call removes ATA carrier so Windows RAS can finish Disconnect.
        op.statusCode = PJSIP_SC_OK;
        c->hangup(op);
        impl_->push(FrontendEventType::CallEnded,
                    "local hangup requested after client PPP termination");
        return true;
    } catch (const Error& e) {
        impl_->push(FrontendEventType::Error, "hangup failed: " + e.info());
        return false;
    } catch (...) {
        impl_->push(FrontendEventType::Error, "hangup failed: unknown error");
        return false;
    }
}
uint64_t PjsipFrontend::rx_frames() const { return impl_->rx_count.load(); }

bool PjsipFrontend::modem_data_connected() {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->modem.data_connected();
}
std::vector<uint8_t> PjsipFrontend::take_modem_ppp_bytes() {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->modem.take_ppp_bytes();
}
void PjsipFrontend::feed_modem_ppp_bytes(const std::vector<uint8_t>& bytes) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->modem.feed_ppp_bytes(bytes);
}
std::string PjsipFrontend::take_modem_event() {
    std::lock_guard<std::mutex> lk(impl_->mu);
    std::string s = impl_->modem.last_event();
    impl_->modem.clear_event();
    return s;
}
LiveState PjsipFrontend::modem_state() {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->modem.state();
}

} // namespace v92

#endif
