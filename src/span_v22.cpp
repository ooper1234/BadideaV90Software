#include "span_v22.hpp"
#include "async_serial.hpp"

#include <algorithm>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#ifdef V92_HAVE_SPANDSP
extern "C" {
#include <spandsp.h>
#include <spandsp/v22bis.h>
#include <spandsp/v42.h>
#include <spandsp/v42bis.h>
#include <spandsp/hdlc.h>
#include <spandsp/private/logging.h>
#include <spandsp/private/hdlc.h>
#include <spandsp/private/v42.h>
}
#endif

namespace v92 {

struct SpanV22Modem::Impl {
    int bit_rate = 2400;
    V22LinkMode requested_link = V22LinkMode::V42Detect;
    bool raw_mode = false;
    bool v42_detecting = false;
    bool v42_connected = false;
    bool v42bis_negotiated = false;
    bool v42bis_active = false;
    bool switch_raw_pending = false;
    bool v42_started = false;
    bool odp_seen = false;
    bool adp_extension_active = false;
    bool lapm_monitor_owns_rx = false;
    size_t adp_extension_bit = 0;
    uint8_t hdlc_flag_shift = 0;
    unsigned lapm_valid_frames = 0;
    unsigned lapm_bad_frames = 0;
    uint64_t v42_detection_samples = 0;
    uint64_t v42_link_samples = 0;
    std::string link_text = "idle";
    V42OdpDetector odp_detector;

    std::deque<int> raw_tx_bits;
    std::deque<uint8_t> v42_tx_bytes;
    std::vector<uint8_t> rx_bytes;
    std::vector<int> v42_probe_bits;

    bool carrier_seen = false;
    bool is_connected = false;
    bool is_failed = false;
    bool rx_in_frame = false;
    bool raw_rx_synchronized = false;
    unsigned raw_rx_marks = 0;
    unsigned rx_pos = 0;
    uint8_t rx_byte = 0;
#ifdef V92_HAVE_SPANDSP
    v22bis_state_t* state = nullptr;
    v42_state_t* v42 = nullptr;
    v42bis_state_t* v42bis = nullptr;
    hdlc_rx_state_t* xid_monitor = nullptr;
#endif
};

bool span_v22_available() {
#ifdef V92_HAVE_SPANDSP
    return true;
#else
    return false;
#endif
}

bool v42_detection_timeout_reached(uint64_t samples_after_carrier) {
    return samples_after_carrier >= kV42DetectionTimerSamples;
}

bool v42_establishment_timeout_reached(uint64_t samples_after_carrier) {
    return samples_after_carrier >= kV42EstablishmentWatchdogSamples;
}

std::vector<uint8_t> build_v42_answer_xid_response(
    uint8_t response_address,
    bool final_bit,
    uint16_t tx_n401_octets,
    uint16_t rx_n401_octets,
    uint8_t tx_window,
    uint8_t rx_window,
    uint8_t compression_p0,
    uint16_t compression_p1,
    uint8_t compression_p2) {
    const auto put16=[](std::vector<uint8_t>& out,uint16_t value){
        out.push_back(static_cast<uint8_t>(value>>8));
        out.push_back(static_cast<uint8_t>(value));
    };
    std::vector<uint8_t> out;
    out.reserve(compression_p0 ? 44u : 26u);
    out.push_back(response_address);
    out.push_back(static_cast<uint8_t>(0xAFu | (final_bit ? 0x10u : 0x00u)));
    out.push_back(0x82); // general-format identifier

    out.push_back(0x80); // parameter-negotiation group
    put16(out,20);
    out.insert(out.end(),{0x03,0x04,0x8A,0x89,0x00,0x00});
    out.insert(out.end(),{0x05,0x02});
    put16(out,static_cast<uint16_t>(std::min<uint32_t>(0xFFFFu,
                                                       uint32_t(tx_n401_octets)*8u)));
    out.insert(out.end(),{0x06,0x02});
    put16(out,static_cast<uint16_t>(std::min<uint32_t>(0xFFFFu,
                                                       uint32_t(rx_n401_octets)*8u)));
    out.insert(out.end(),{0x07,0x01,static_cast<uint8_t>(std::min<unsigned>(15,tx_window)),
                          0x08,0x01,static_cast<uint8_t>(std::min<unsigned>(15,rx_window))});

    if(compression_p0){
        out.insert(out.end(),{0xF0,0x00,0x0F,
                              0x00,0x03,'V','4','2',
                              0x01,0x01,static_cast<uint8_t>(compression_p0&0x03u),
                              0x02,0x02});
        put16(out,compression_p1);
        out.insert(out.end(),{0x03,0x01,compression_p2});
    }
    return out;
}

void V42OdpDetector::start_character() {
    in_character_ = true;
    character_bit_ = 0;
    character_ = 0;
}

void V42OdpDetector::finish_character(bool stop_bit) {
    in_character_ = false;
    const bool dc1 = stop_bit && (character_ == 0x11u || character_ == 0x91u);
    if (dc1 && (!have_previous_ || character_ == static_cast<uint8_t>(previous_ ^ 0x80u))) {
        previous_ = character_;
        have_previous_ = true;
        ++valid_characters_;
        if (valid_characters_ >= 4) detected_ = true;
    } else if (dc1) {
        previous_ = character_;
        have_previous_ = true;
        valid_characters_ = 1;
    } else {
        have_previous_ = false;
        valid_characters_ = 0;
    }
    marks_ = 0;
}

bool V42OdpDetector::feed(int bit) {
    if (detected_ || bit < 0) return detected_;
    bit &= 1;
    if (in_character_) {
        if (character_bit_ < 8) {
            character_ |= static_cast<uint8_t>(bit << character_bit_++);
        } else {
            finish_character(bit != 0);
        }
        return detected_;
    }

    if (!have_previous_) {
        if (bit == 0) start_character();
        return detected_;
    }

    if (bit != 0) {
        if (++marks_ > 16) {
            have_previous_ = false;
            valid_characters_ = 0;
            marks_ = 0;
        }
        return detected_;
    }

    // A new DC1 must follow the preceding stop bit by 8..16 mark bits.
    // Treat this zero as a possible fresh start even after a malformed gap so
    // the observer can recover without waiting for another idle interval.
    if (marks_ < 8 || marks_ > 16) {
        have_previous_ = false;
        valid_characters_ = 0;
    }
    start_character();
    return detected_;
}

void V42OdpDetector::reset() {
    in_character_ = false;
    character_bit_ = 0;
    character_ = 0;
    have_previous_ = false;
    previous_ = 0;
    valid_characters_ = 0;
    marks_ = 0;
    detected_ = false;
}

#ifdef V92_HAVE_SPANDSP
static void raw_rx_bit(SpanV22Modem::Impl* s, int bit) {
    bit &= 1;
    if (!s->rx_in_frame) {
        if (!s->raw_rx_synchronized) {
            if (bit) {
                s->raw_rx_marks = std::min(s->raw_rx_marks + 1u, 32u);
                return;
            }
            if (s->raw_rx_marks < 8u) {
                s->raw_rx_marks = 0;
                return;
            }
            s->raw_rx_synchronized = true;
            s->raw_rx_marks = 0;
        }
        if (bit == 0) {
            s->rx_in_frame = true;
            s->rx_pos = 0;
            s->rx_byte = 0;
        }
        return;
    }
    if (s->rx_pos < 8) {
        s->rx_byte |= static_cast<uint8_t>(bit << s->rx_pos);
        ++s->rx_pos;
        return;
    }
    if (bit == 1) {
        s->rx_bytes.push_back(s->rx_byte);
    } else {
        // A missing stop bit means this was not an asynchronous character.
        // Require a fresh mark-idle guard before exposing any more bytes.
        s->raw_rx_synchronized = false;
        s->raw_rx_marks = 0;
    }
    s->rx_in_frame = false;
    s->rx_pos = 0;
    s->rx_byte = 0;
}

static int raw_get_bit(SpanV22Modem::Impl* s) {
    if (s->raw_tx_bits.empty()) return 1;
    int b = s->raw_tx_bits.front();
    s->raw_tx_bits.pop_front();
    return b & 1;
}

static int v42_get_frame(void* u, uint8_t msg[], int len) {
    auto* s = static_cast<SpanV22Modem::Impl*>(u);
    if (!s || len <= 0) return 0;
    int n = std::min<int>(len, static_cast<int>(s->v42_tx_bytes.size()));
    for (int i = 0; i < n; ++i) {
        msg[i] = s->v42_tx_bytes.front();
        s->v42_tx_bytes.pop_front();
    }
    return n;
}

static void v42bis_encode_put(void* u, const uint8_t* msg, int len) {
    auto* s = static_cast<SpanV22Modem::Impl*>(u);
    if (!s || !msg || len <= 0) return;
    for (int i=0;i<len;++i) s->v42_tx_bytes.push_back(msg[i]);
}

static void v42bis_decode_put(void* u, const uint8_t* msg, int len) {
    auto* s = static_cast<SpanV22Modem::Impl*>(u);
    if (!s || !msg || len <= 0) return;
    s->rx_bytes.insert(s->rx_bytes.end(), msg, msg + len);
}

static void lapm_monitor_frame_cb(void* u, const uint8_t* pkt, int len, int ok) {
    auto* s = static_cast<SpanV22Modem::Impl*>(u);
    if (!s || len < 0) return;
    if (!ok || !pkt || len < 2) {
        if (s->lapm_monitor_owns_rx && !s->v42_connected) {
            ++s->lapm_bad_frames;
            s->link_text = "V.42 LAPM frame received with bad CRC; waiting for peer retry";
        }
        return;
    }

    ++s->lapm_valid_frames;
    const uint8_t control = static_cast<uint8_t>(pkt[1] & 0xECu);

    // SpanDSP's V.42 implementation is explicitly unfinished and some builds
    // enter LAPM_IDLE after ADP without reliably delivering the first hardware
    // XID/SABME frame to lapm_receive().  The independent HDLC observer has
    // already seen every incoming bit.  Once the caller's first flag ends our
    // extended ADP, make that observer the sole HDLC RX and pass each CRC-valid
    // frame into SpanDSP's LAPM state machine.  Keeping a single owner avoids
    // duplicate XID responses, duplicate I frames, and sequence-number drift.
    const bool xid = control == 0xACu && len >= 3 && pkt[2] == 0x82;
    const int ctrl_before = s->v42 ? s->v42->lapm.ctrl_put : 0;
    if (s->lapm_monitor_owns_rx && s->v42)
        lapm_receive(s->v42, pkt, len, ok);

    // SpanDSP 0.0.6 transmit_xid() writes the next parameter at the start of
    // the required four-byte optional-functions value (it omits buf += 4).
    // It also emits XID/F=0 even when the caller set P=1.  lapm_receive() has
    // already parsed the command and reserved exactly one control slot; repair
    // that slot before the HDLC transmitter dequeues it.
    bool xid_response_corrected=false;
    if(xid && s->lapm_monitor_owns_rx && s->v42 &&
       s->v42->lapm.ctrl_put!=ctrl_before){
        auto response=build_v42_answer_xid_response(
            s->v42->lapm.rsp_addr,(pkt[1]&0x10u)!=0,
            s->v42->config.v42_tx_n401,s->v42->config.v42_rx_n401,
            s->v42->config.v42_tx_window_size_k,s->v42->config.v42_rx_window_size_k,
            static_cast<uint8_t>(s->v42->config.comp),
            static_cast<uint16_t>(s->v42->config.comp_dict_size),
            static_cast<uint8_t>(s->v42->config.comp_max_string));
        auto& frame=s->v42->lapm.ctrl_buf[ctrl_before];
        if(response.size()<=sizeof(frame.buf)){
            std::memcpy(frame.buf,response.data(),response.size());
            frame.len=static_cast<int>(response.size());
            xid_response_corrected=true;
        }
    }

    if (control == 0x6Cu) {
        if (s->v42_connected)
            s->link_text = "V.42 LAPM SABME received; UA queued; data link connected";
        else
            s->link_text = "V.42 LAPM SABME received; waiting for data state";
        return;
    }

    if (!xid) return;

    s->link_text = xid_response_corrected ?
        "V.42 LAPM XID received; corrected 44-byte XID/F response queued" :
        "V.42 LAPM XID received; response queue unavailable; waiting for peer retry";

    // Observe the private V.42 XID group. SpanDSP's answer-side V.42 response
    // advertises P0=1, P1=512, P2=6. Activate decompression only when the
    // caller's command also offers initiator->responder compression with
    // compatible dictionary/string limits.
    int p0=0,p1=0,p2=0;
    bool v42_set=false;
    size_t pos=3;
    while(pos+3<=static_cast<size_t>(len)){
        const uint8_t group=pkt[pos++];
        const size_t group_len=(static_cast<size_t>(pkt[pos])<<8)|pkt[pos+1];
        pos+=2;
        if(pos+group_len>static_cast<size_t>(len))break;
        const size_t end=pos+group_len;
        if(group==0xF0){
            while(pos+2<=end){
                const uint8_t id=pkt[pos++];
                const size_t plen=pkt[pos++];
                if(pos+plen>end)break;
                int value=0;for(size_t i=0;i<plen;++i)value=(value<<8)|pkt[pos+i];
                if(id==0x00 && plen==3 && pkt[pos]=='V' && pkt[pos+1]=='4' && pkt[pos+2]=='2')v42_set=true;
                else if(id==0x01)p0=value;
                else if(id==0x02)p1=value;
                else if(id==0x03)p2=value;
                pos+=plen;
            }
        }
        pos=end;
    }
    s->v42bis_negotiated=v42_set && (p0&1) && p1>=512 && p2>=6;
    s->v42bis_active=s->v42_connected && s->v42bis_negotiated;
    if(s->v42bis_active)s->link_text="V.42 LAPM + V.42bis RX (P0=1, P1=512, P2=6)";
}

static void v42_put_frame(void* u, const uint8_t msg[], int len) {
    auto* s = static_cast<SpanV22Modem::Impl*>(u);
    if (!s || len <= 0 || !msg) return;
    if(s->v42bis && s->v42bis_active)v42bis_decompress(s->v42bis,msg,len);
    else s->rx_bytes.insert(s->rx_bytes.end(), msg, msg + len);
}

static void v42_status_cb(void* u, int status) {
    auto* s = static_cast<SpanV22Modem::Impl*>(u);
    if (!s) return;
    if (status >= 0) {
        const char* name = lapm_status_to_str(status);
        s->link_text = name ? name : "LAPM";
        if (s->link_text == "LAPM_V42_UNSUPPORTED" ||
            s->link_text == "LAPM_UNSUPPORTED") {
            s->switch_raw_pending = true;
            s->v42_detecting = false;
        } else if (s->link_text == "LAPM_DATA") {
            s->v42_connected = true;
            s->v42_detecting = false;
            s->v42bis_active = s->v42bis_negotiated;
            if(s->v42bis_active)s->link_text="V.42 LAPM + V.42bis RX (P0=1, P1=512, P2=6)";
        } else if (s->link_text == "LAPM_ESTABLISH") {
            // A real ODP/protocol start was recognized. T400 no longer
            // applies, but the outer establishment watchdog remains armed
            // until SABME/UA reaches LAPM_DATA.
            if (s->v42_detecting)
                s->v42_link_samples = 0;
            s->v42_detecting = false;
            s->v42_probe_bits.clear();
            s->adp_extension_active = false;
        } else if (s->link_text == "LAPM_IDLE" && s->odp_seen) {
            // V.42 Appendix III.1 recommends sending substantially more than
            // the minimum ten ADPs, possibly until originator flags arrive.
            // SpanDSP stops at ten; extend EC patterns without consuming any
            // queued LAPM response bits.
            s->adp_extension_active = true;
            s->adp_extension_bit = 0;
            s->link_text = "V.42 ODP detected; extending ADP until LAPM flags";
        } else if (s->link_text == "LAPM_RELEASE") {
            // V.42 7.9 permits non-error-correcting fallback after LAPM
            // establishment fails. Preserve the trained physical carrier.
            if (s->requested_link == V22LinkMode::V42Detect)
                s->switch_raw_pending = true;
        }
        return;
    }
    if (status == SIG_STATUS_LINK_CONNECTED) {
        s->v42_connected = true;
        s->v42_detecting = false;
        s->v42bis_active = s->v42bis_negotiated;
        s->adp_extension_active = false;
        s->link_text = s->v42bis_active ?
            "V.42 LAPM + V.42bis RX (P0=1, P1=512, P2=6)" :
            "V.42 LAPM connected (V.42bis not negotiated)";
    } else if (status == SIG_STATUS_LINK_DISCONNECTED) {
        s->v42_connected = false;
        s->v42bis_active = false;
        s->link_text = "LAPM disconnected";
    }
}

static void switch_to_raw(SpanV22Modem::Impl* s, const char* reason) {
    if (!s || s->raw_mode) return;
    s->raw_mode = true;
    s->v42_detecting = false;
    s->v42_connected = false;
    s->v42bis_active = false;
    s->adp_extension_active = false;
    s->lapm_monitor_owns_rx = false;
    s->switch_raw_pending = false;
    s->link_text = reason ? reason : "transparent async fallback";
    s->rx_bytes.clear();
    s->rx_in_frame = false;
    s->raw_rx_synchronized = false;
    s->raw_rx_marks = 0;
    // Anything queued by pppd while V.42 was being detected must not be lost.
    if (!s->v42_tx_bytes.empty()) {
        std::vector<uint8_t> pending;
        pending.reserve(s->v42_tx_bytes.size());
        while (!s->v42_tx_bytes.empty()) {
            pending.push_back(s->v42_tx_bytes.front());
            s->v42_tx_bytes.pop_front();
        }
        auto bits = async_encode(pending);
        for (auto b : bits) s->raw_tx_bits.push_back(b & 1);
    }
    // Replay caller bits accumulated during the ODP/ADP detection window. If
    // the caller was plain async, this lets PPP find a later intact frame.
    for (int b : s->v42_probe_bits) raw_rx_bit(s, b);
    s->v42_probe_bits.clear();
}

static int sp_get_bit(void* u) {
    auto* s = static_cast<SpanV22Modem::Impl*>(u);
    if (s->raw_mode || !s->v42) return raw_get_bit(s);
    if (!s->v42_started) return 1;
    if (s->adp_extension_active) {
        // EC ADP: E and C asynchronous characters, each followed by eight
        // marks. These are the same standard bit patterns used by SpanDSP,
        // emitted LSB/first-in-time from its 18-bit constants.
        const size_t p = s->adp_extension_bit++ % 36u;
        return static_cast<int>(((p < 18u ? 0x3FE8Au : 0x3FE86u) >>
                                (p < 18u ? p : p - 18u)) & 1u);
    }
    const int bit = v42_tx_bit(s->v42) & 1;
    // The callback which ends SpanDSP's tenth ADP occurs inside v42_tx_bit().
    // Replace its trailing mark with the first extended ADP bit immediately.
    if (s->adp_extension_active) {
        s->adp_extension_bit = 1;
        return static_cast<int>(0x3FE8Au & 1u);
    }
    return bit;
}

static void sp_put_bit(void* u, int bit) {
    auto* s = static_cast<SpanV22Modem::Impl*>(u);
    if (bit < 0) {
        switch (bit) {
        case SIG_STATUS_CARRIER_UP:
            s->carrier_seen = true;
            break;
        case SIG_STATUS_TRAINING_SUCCEEDED:
            s->carrier_seen = true;
            s->is_connected = true;
            if (!s->raw_mode && s->v42 && !s->v42_started) {
                // SpanDSP otherwise assumes 28.8 kbit/s for all V.42 timers.
                // Start the control layer at actual carrier-up and use the
                // real V.22bis bit rate so T400/T401 count wall time correctly.
                s->v42->tx_bit_rate = s->bit_rate;
                v42_restart(s->v42);
                s->v42_started = true;
            }
            break;
        case SIG_STATUS_CARRIER_DOWN:
            s->carrier_seen = false;
            s->is_connected = false;
            s->rx_in_frame = false;
            break;
        case SIG_STATUS_TRAINING_FAILED:
            s->is_failed = true;
            s->is_connected = false;
            break;
        default:
            break;
        }
        // Also tell V.42 about carrier/status changes; its implementation
        // safely ignores negative signal status while detecting/receiving.
        if (!s->raw_mode && s->v42 && s->v42_started) v42_rx_bit(s->v42, bit);
        return;
    }

    bit &= 1;
    if (s->raw_mode || !s->v42) {
        raw_rx_bit(s, bit);
        return;
    }
    if (!s->v42_started) return;
    if (s->odp_seen) {
        s->hdlc_flag_shift = static_cast<uint8_t>((s->hdlc_flag_shift << 1) | bit);
        if (s->hdlc_flag_shift == 0x7Eu && s->adp_extension_active) {
            s->adp_extension_active = false;
            s->lapm_monitor_owns_rx = true;
            s->link_text = "V.42 ADP accepted; LAPM flags received";
        }
    }
    if (s->v42_detecting) {
        s->v42_probe_bits.push_back(bit);
        if (s->odp_detector.feed(bit)) {
            // SpanDSP remains responsible for transmitting ADP and for LAPM.
            // This observer only prevents the application's T400 from racing
            // that valid exchange before SpanDSP reports a later LAPM state.
            s->v42_detecting = false;
            s->odp_seen = true;
            s->v42_link_samples = 0;
            s->v42_probe_bits.clear();
            s->link_text = "V.42 ODP detected; sending ADP / establishing LAPM";
        }
    }
    if (s->xid_monitor) hdlc_rx_put_bit(s->xid_monitor, bit);
    // Before the first post-ADP flag SpanDSP still owns detection RX. After
    // that boundary the independent monitor owns HDLC and calls lapm_receive
    // exactly once for every complete frame.
    if (!s->lapm_monitor_owns_rx) v42_rx_bit(s->v42, bit);
    if (s->switch_raw_pending)
        switch_to_raw(s,"transparent async fallback (peer reported V.42 unsupported)");
}
#endif

SpanV22Modem::SpanV22Modem(int bit_rate) : impl_(new Impl) {
    impl_->bit_rate = (bit_rate == 1200) ? 1200 : 2400;
}

SpanV22Modem::~SpanV22Modem() {
#ifdef V92_HAVE_SPANDSP
    if (impl_ && impl_->state) { v22bis_free(impl_->state); impl_->state = nullptr; }
    if (impl_ && impl_->v42) { v42_free(impl_->v42); impl_->v42 = nullptr; }
    if (impl_ && impl_->v42bis) { v42bis_free(impl_->v42bis); impl_->v42bis = nullptr; }
    if (impl_ && impl_->xid_monitor) { hdlc_rx_free(impl_->xid_monitor); impl_->xid_monitor = nullptr; }
#endif
}

bool SpanV22Modem::start_answer(V22LinkMode link_mode) {
    impl_->requested_link = link_mode;
    impl_->raw_mode = link_mode == V22LinkMode::TransparentAsync;
    impl_->v42_detecting = link_mode == V22LinkMode::V42Detect;
    impl_->v42_connected = false;
    impl_->v42bis_negotiated = false;
    impl_->v42bis_active = false;
    impl_->switch_raw_pending = false;
    impl_->v42_started = false;
    impl_->odp_seen = false;
    impl_->adp_extension_active = false;
    impl_->lapm_monitor_owns_rx = false;
    impl_->adp_extension_bit = 0;
    impl_->hdlc_flag_shift = 0;
    impl_->lapm_valid_frames = 0;
    impl_->lapm_bad_frames = 0;
    impl_->v42_detection_samples = 0;
    impl_->v42_link_samples = 0;
    impl_->link_text = impl_->raw_mode ? "transparent async" : (impl_->v42_detecting ? "V.42 detect" : "LAPM (V.8 negotiated)");
    impl_->raw_tx_bits.clear();
    impl_->v42_tx_bytes.clear();
    impl_->rx_bytes.clear();
    impl_->v42_probe_bits.clear();
    impl_->odp_detector.reset();
    impl_->carrier_seen = false;
    impl_->is_connected = false;
    impl_->is_failed = false;
    impl_->rx_in_frame = false;
    impl_->raw_rx_synchronized = false;
    impl_->raw_rx_marks = 0;
    impl_->rx_pos = 0;
    impl_->rx_byte = 0;
#ifdef V92_HAVE_SPANDSP
    if (impl_->state) { v22bis_free(impl_->state); impl_->state = nullptr; }
    if (impl_->v42) { v42_free(impl_->v42); impl_->v42 = nullptr; }
    if (impl_->v42bis) { v42bis_free(impl_->v42bis); impl_->v42bis = nullptr; }
    if (impl_->xid_monitor) { hdlc_rx_free(impl_->xid_monitor); impl_->xid_monitor = nullptr; }

    if (!impl_->raw_mode) {
        impl_->v42 = v42_init(nullptr,
                              false,
                              link_mode == V22LinkMode::V42Detect,
                              v42_get_frame,
                              v42_put_frame,
                              impl_.get());
        if (!impl_->v42) { impl_->is_failed = true; return false; }
        // Stay on SpanDSP's public V.42 API. Ubuntu's 0.0.6 package keeps
        // v42_state_t opaque, so touching private fields breaks compilation.
        v42_set_status_callback(impl_->v42, v42_status_cb, impl_.get());
        impl_->v42->tx_bit_rate = impl_->bit_rate;
        impl_->v42bis = v42bis_init(nullptr,
                                    V42BIS_P0_INITIATOR_RESPONDER,
                                    512,
                                    6,
                                    v42bis_encode_put,
                                    impl_.get(),
                                    1024,
                                    v42bis_decode_put,
                                    impl_.get(),
                                    1024);
        impl_->xid_monitor = hdlc_rx_init(nullptr, false, false, 1,
                                          lapm_monitor_frame_cb, impl_.get());
        if (!impl_->v42bis || !impl_->xid_monitor) { impl_->is_failed = true; return false; }
    }

    impl_->state = v22bis_init(nullptr,
                              impl_->bit_rate,
                              V22BIS_GUARD_TONE_NONE,
                              false,
                              sp_get_bit,
                              impl_.get(),
                              sp_put_bit,
                              impl_.get());
    if (!impl_->state) { impl_->is_failed = true; return false; }
    v22bis_tx_power(impl_->state, -12.0f);
    return true;
#else
    (void)link_mode;
    impl_->is_failed = true;
    return false;
#endif
}

void SpanV22Modem::receive_pcm(const std::vector<int16_t>& pcm) {
#ifdef V92_HAVE_SPANDSP
    if (impl_->state && !pcm.empty()) {
        v22bis_rx(impl_->state, pcm.data(), static_cast<int>(pcm.size()));
        if (impl_->switch_raw_pending)
            switch_to_raw(impl_.get(),
                "transparent async fallback (V.42/LAPM establishment failed)");
        // Some SpanDSP releases do not surface V42_UNSUPPORTED when an old
        // client simply sends no ODP. V.42 requires the answerer to wait T400
        // after physical carrier establishment, then retain that carrier and
        // interwork in non-error-correcting V.14/transparent mode. Do not wait
        // 20 seconds and throw away a successfully trained V.22bis link.
        if (impl_->is_connected &&
            impl_->requested_link == V22LinkMode::V42Detect &&
            !impl_->raw_mode && !impl_->v42_connected) {
            impl_->v42_link_samples += pcm.size();
            if (impl_->v42_detecting) {
                impl_->v42_detection_samples += pcm.size();
                if (v42_detection_timeout_reached(impl_->v42_detection_samples))
                    switch_to_raw(impl_.get(),
                        "transparent async fallback (no V.42 ODP within T400=750 ms)");
            } else if (v42_establishment_timeout_reached(impl_->v42_link_samples)) {
                std::string reason;
                if (impl_->lapm_valid_frames) {
                    reason = "transparent async fallback (" +
                        std::to_string(impl_->lapm_valid_frames) +
                        " CRC-valid LAPM frame(s) received but SABME/UA did not reach data within 10 s)";
                } else if (impl_->lapm_bad_frames) {
                    reason = "transparent async fallback (" +
                        std::to_string(impl_->lapm_bad_frames) +
                        " LAPM frame(s) failed CRC within 10 s)";
                } else {
                    reason = "transparent async fallback (V.42 ODP/flags seen but no complete LAPM XID/SABME frame within 10 s)";
                }
                switch_to_raw(impl_.get(), reason.c_str());
            }
        }
    }
#else
    (void) pcm;
#endif
}

std::vector<int16_t> SpanV22Modem::next_tx_pcm(size_t samples) {
    std::vector<int16_t> out(samples, 0);
#ifdef V92_HAVE_SPANDSP
    if (impl_->state && samples) {
        int n = v22bis_tx(impl_->state, out.data(), static_cast<int>(samples));
        if (n < 0) { impl_->is_failed = true; std::fill(out.begin(), out.end(), 0); }
        else if (static_cast<size_t>(n) < samples) std::fill(out.begin()+n, out.end(), 0);
    }
#endif
    return out;
}

void SpanV22Modem::feed_bytes(const std::vector<uint8_t>& bytes) {
    if (bytes.empty()) return;
    bool have_v42 = false;
#ifdef V92_HAVE_SPANDSP
    have_v42 = impl_->v42 != nullptr;
#endif
    if (impl_->raw_mode || !have_v42) {
        auto bits = async_encode(bytes);
        for (uint8_t b : bits) impl_->raw_tx_bits.push_back(b & 1);
        return;
    }
    for (uint8_t b : bytes) impl_->v42_tx_bytes.push_back(b);
}

std::vector<uint8_t> SpanV22Modem::take_bytes() {
    auto out = std::move(impl_->rx_bytes);
    impl_->rx_bytes.clear();
    return out;
}

bool SpanV22Modem::connected() const { return impl_->is_connected; }
bool SpanV22Modem::failed() const { return impl_->is_failed; }
bool SpanV22Modem::lapm_connected() const { return impl_->v42_connected; }
bool SpanV22Modem::v42bis_active() const { return impl_->v42bis_active; }
bool SpanV22Modem::transparent_mode() const { return impl_->raw_mode; }
std::string SpanV22Modem::link_status() const { return impl_->link_text; }
int SpanV22Modem::current_bit_rate() const {
#ifdef V92_HAVE_SPANDSP
    if (impl_->state) {
        int r = v22bis_get_current_bit_rate(impl_->state);
        if (r == 1200 || r == 2400) return r;
    }
#endif
    return impl_->bit_rate;
}

} // namespace v92
