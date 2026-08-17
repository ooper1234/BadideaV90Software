#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace v92 {

bool span_v8_available();

enum class V8Profile {
    V22Only,
    V90Digital
};

struct V8AnswerResult {
    bool done = false;
    bool failed = false;
    bool non_v8 = false;
    // Latched when the answer-side V.8 receiver decodes the caller's CM.
    // V.92 Quick Connect must listen for ordinary CM at the same time as
    // QC1a; this flag lets it hand the already-running V.8 exchange over
    // without discarding CM and restarting ANSam.
    bool cm_detected = false;
    bool lapm = false;
    bool v22 = false;
    bool v34 = false;
    bool v90 = false;
    bool v92 = false;
    uint32_t remote_modulations = 0;
    int remote_pstn_access = 0;
    int remote_pcm_modem_availability = 0;
    bool remote_pcm_analogue = false;
    bool remote_pcm_digital = false;
    bool v90_pair_valid = false;
    std::string status;
};

// SpanDSP-backed V.8 answerer. Stable automatic mode advertises only the
// data PHYs that are fully usable. Explicit V90Digital mode truthfully offers
// V.90 digital-modem capability so the real phase-2 implementation can be
// exercised; it still does not claim a completed V.90 data connection.
class SpanV8Answerer {
public:
    struct Impl;
    explicit SpanV8Answerer(V8Profile profile = V8Profile::V22Only);
    ~SpanV8Answerer();
    SpanV8Answerer(const SpanV8Answerer&) = delete;
    SpanV8Answerer& operator=(const SpanV8Answerer&) = delete;

    bool start();
    size_t receive_pcm(const std::vector<int16_t>& pcm);
    std::vector<int16_t> next_tx_pcm(size_t samples);

    const V8AnswerResult& result() const;
    bool active() const;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace v92
