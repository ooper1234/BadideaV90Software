#include "span_v8.hpp"

#include <algorithm>
#include <cstring>

#ifdef V92_HAVE_SPANDSP
extern "C" {
#include <spandsp.h>
#include <spandsp/v8.h>
#include <spandsp/modem_connect_tones.h>
}
#endif

namespace v92 {

struct SpanV8Answerer::Impl {
    explicit Impl(V8Profile p):profile(p){}
    V8Profile profile = V8Profile::V22Only;
    V8AnswerResult result;
#ifdef V92_HAVE_SPANDSP
    v8_state_t* state = nullptr;
#endif
};

bool span_v8_available() {
#ifdef V92_HAVE_SPANDSP
    return true;
#else
    return false;
#endif
}

#ifdef V92_HAVE_SPANDSP
static void v8_result_cb(void* user, v8_parms_t* r) {
    auto* s = static_cast<SpanV8Answerer::Impl*>(user);
    if (!s || !r) return;
    // V8_STATUS_V8_OFFERED is the answer side's decoded remote CM. Preserve
    // those values before we rewrite the result into the JM we will send.
    if (r->status == V8_STATUS_V8_OFFERED) {
        s->result.cm_detected = true;
        s->result.remote_modulations = r->modulations;
        s->result.remote_pstn_access = r->pstn_access;
        s->result.remote_pcm_modem_availability = r->pcm_modem_availability;
        s->result.remote_pcm_analogue =
            (r->pcm_modem_availability & V8_PSTN_PCM_MODEM_V90_V92_ANALOGUE) != 0;
        s->result.remote_pcm_digital =
            (r->pcm_modem_availability & V8_PSTN_PCM_MODEM_V90_V92_DIGITAL) != 0;
    }
    switch (r->status) {
    case V8_STATUS_IN_PROGRESS: s->result.status = "V8_STATUS_IN_PROGRESS"; break;
    case V8_STATUS_V8_OFFERED: s->result.status = "V8_STATUS_V8_OFFERED"; break;
    case V8_STATUS_V8_CALL: s->result.status = "V8_STATUS_V8_CALL"; break;
    case V8_STATUS_NON_V8_CALL: s->result.status = "V8_STATUS_NON_V8_CALL"; break;
    case V8_STATUS_FAILED: s->result.status = "V8_STATUS_FAILED"; break;
    default: s->result.status = "V8 status " + std::to_string(r->status); break;
    }
    switch (r->status) {
    case V8_STATUS_V8_OFFERED:
        if (s->profile == V8Profile::V90Digital) {
            // Real V.90 lab path: we are the digitally connected server modem.
            // Advertise V.90, V.34, V.32, V.22 so clients with any modulation can connect.
            r->modulations &= (V8_MOD_V90 | V8_MOD_V34 | V8_MOD_V32 | V8_MOD_V22);
            // V.90 9.1.1 requires a V.90 capable endpoint to advertise its
            // PSTN access type. We are the digitally-connected server modem.
            r->pstn_access = V8_PSTN_ACCESS_DCE_ON_DIGITAL;
            r->pcm_modem_availability = V8_PSTN_PCM_MODEM_V90_V92_DIGITAL;
            r->protocol = V8_PROTOCOL_LAPM_V42;
        } else {
            r->modulations &= (V8_MOD_V34 | V8_MOD_V32 | V8_MOD_V22);
            r->protocol = V8_PROTOCOL_LAPM_V42;
            r->pcm_modem_availability = 0;
        }
        break;
    case V8_STATUS_V8_CALL:
        s->result.done = true;
        s->result.v90 = (r->modulations & V8_MOD_V90) != 0;
#ifdef V8_MOD_V92
        s->result.v92 = (r->modulations & V8_MOD_V92) != 0;
#endif
        s->result.v34 = (r->modulations & V8_MOD_V34) != 0;
        s->result.v22 = (r->modulations & (V8_MOD_V22 | V8_MOD_V32 | V8_MOD_V34)) != 0;
        s->result.lapm = (r->protocol == V8_PROTOCOL_LAPM_V42);
        s->result.v90_pair_valid = s->result.v90 && s->result.remote_pcm_analogue;
        if (s->profile == V8Profile::V90Digital) {
            if (!s->result.v90 && !s->result.v34 && !s->result.v22) s->result.failed = true;
        } else if (!s->result.v22 && !s->result.v34) {
            s->result.failed = true;
        }
        break;
    case V8_STATUS_NON_V8_CALL:
        s->result.done = true;
        s->result.non_v8 = true;
        s->result.v22 = true;
        s->result.lapm = false;
        break;
    case V8_STATUS_FAILED:
        s->result.done = true;
        s->result.failed = true;
        break;
    default:
        break;
    }
}
#endif

SpanV8Answerer::SpanV8Answerer(V8Profile profile) : impl_(new Impl(profile)) {}
SpanV8Answerer::~SpanV8Answerer() {
#ifdef V92_HAVE_SPANDSP
    if (impl_ && impl_->state) { v8_free(impl_->state); impl_->state = nullptr; }
#endif
}

bool SpanV8Answerer::start() {
    impl_->result = {};
#ifdef V92_HAVE_SPANDSP
    if (impl_->state) { v8_free(impl_->state); impl_->state = nullptr; }
    v8_parms_t p{};
    p.modem_connect_tone = MODEM_CONNECT_TONES_ANSAM_PR;
    p.send_ci = true;
    p.v92 = -1;
    p.call_function = V8_CALL_V_SERIES;
    if (impl_->profile == V8Profile::V90Digital) {
        p.modulations = V8_MOD_V90 | V8_MOD_V22;
        p.protocol = V8_PROTOCOL_LAPM_V42;
        // We terminate the V.90 digital side in software and feed the ATA over
        // an 8-kHz G.711 path, so advertise the digital PSTN access category.
        p.pstn_access = V8_PSTN_ACCESS_DCE_ON_DIGITAL;
        p.pcm_modem_availability = V8_PSTN_PCM_MODEM_V90_V92_DIGITAL;
    } else {
        p.modulations = V8_MOD_V22;
        p.protocol = V8_PROTOCOL_LAPM_V42;
        p.pstn_access = 0;
        p.pcm_modem_availability = 0;
    }
    p.nsf = -1;
    p.t66 = -1;
    impl_->state = v8_init(nullptr, false, &p, v8_result_cb, impl_.get());
    if (!impl_->state) {
        impl_->result.done = impl_->result.failed = true;
        impl_->result.status = "v8_init failed";
        return false;
    }
    return true;
#else
    impl_->result.done = impl_->result.failed = true;
    impl_->result.status = "SpanDSP unavailable";
    return false;
#endif
}

size_t SpanV8Answerer::receive_pcm(const std::vector<int16_t>& pcm) {
#ifdef V92_HAVE_SPANDSP
    if (!impl_->state || pcm.empty()) return 0;
    int rem = v8_rx(impl_->state, pcm.data(), static_cast<int>(pcm.size()));
    if (rem < 0) rem = 0;
    if (static_cast<size_t>(rem) > pcm.size()) rem = 0;
    return static_cast<size_t>(rem);
#else
    (void)pcm; return 0;
#endif
}

std::vector<int16_t> SpanV8Answerer::next_tx_pcm(size_t samples) {
    std::vector<int16_t> out(samples, 0);
#ifdef V92_HAVE_SPANDSP
    if (impl_->state && samples) {
        int n = v8_tx(impl_->state, out.data(), static_cast<int>(samples));
        if (n < 0) n = 0;
        if (static_cast<size_t>(n) < samples) std::fill(out.begin() + n, out.end(), 0);
    }
#endif
    return out;
}

const V8AnswerResult& SpanV8Answerer::result() const { return impl_->result; }
bool SpanV8Answerer::active() const { return impl_ && !impl_->result.done; }

} // namespace v92
