#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace v92 {

bool span_v22_available();

// V.42 9.1.1 default detection timer T400: 750 ms at the 8-kHz PCM clock.
constexpr uint64_t kV42DetectionTimerSamples = 6000;
bool v42_detection_timeout_reached(uint64_t samples_after_carrier);

// Once a valid ODP/protocol start is observed, T400 no longer applies.  A
// separate bounded establishment watchdog prevents a failed SABME/UA exchange
// from leaving an otherwise trained V.22bis carrier stuck forever.
constexpr uint64_t kV42EstablishmentWatchdogSamples = 80000; // 10 seconds
bool v42_establishment_timeout_reached(uint64_t samples_after_carrier);

// Build the answer-side V.42 XID response used before SABME/UA.  Keeping this
// encoder outside SpanDSP works around its malformed Figure-11 frame (the
// four-byte HDLC optional-functions value is overwritten in 0.0.6) and makes
// the exact wire octets independently testable.
std::vector<uint8_t> build_v42_answer_xid_response(
    uint8_t response_address = 0x03,
    bool final_bit = true,
    uint16_t tx_n401_octets = 128,
    uint16_t rx_n401_octets = 128,
    uint8_t tx_window = 15,
    uint8_t rx_window = 15,
    uint8_t compression_p0 = 1,
    uint16_t compression_p1 = 512,
    uint8_t compression_p2 = 6);

// V.42 3.1 answerer-side originator detection pattern (ODP) observer.  ODP is
// four or more DC1 characters with alternating even/odd parity (wire octets
// 0x11, 0x91, ...) and 8..16 mark bits between characters.  Keeping this tiny
// observer outside SpanDSP is important because SpanDSP does not report ODP
// until after it has finished transmitting ADP; T400 must be disarmed as soon
// as the incoming ODP itself is valid.
class V42OdpDetector {
public:
    bool feed(int bit);
    void reset();
    bool detected() const { return detected_; }

private:
    bool in_character_ = false;
    unsigned character_bit_ = 0;
    uint8_t character_ = 0;
    bool have_previous_ = false;
    uint8_t previous_ = 0;
    unsigned valid_characters_ = 0;
    unsigned marks_ = 0;
    bool detected_ = false;

    void start_character();
    void finish_character(bool stop_bit);
};

enum class V22LinkMode {
    TransparentAsync, // plain 8-N-1 bytes after training
    V42Detect,        // legacy V.42 ODP/ADP detection, then LAPM or raw fallback
    V42Lapm           // V.8 already negotiated LAPM; skip ODP/ADP detection
};

class SpanV22Modem {
public:
    struct Impl;
    explicit SpanV22Modem(int bit_rate = 2400);
    ~SpanV22Modem();
    SpanV22Modem(const SpanV22Modem&) = delete;
    SpanV22Modem& operator=(const SpanV22Modem&) = delete;

    bool start_answer(V22LinkMode link_mode = V22LinkMode::V42Detect);
    void receive_pcm(const std::vector<int16_t>& pcm);
    std::vector<int16_t> next_tx_pcm(size_t samples);

    void feed_bytes(const std::vector<uint8_t>& bytes);
    std::vector<uint8_t> take_bytes();

    bool connected() const;          // physical V.22/V.22bis data pump trained
    bool failed() const;
    bool lapm_connected() const;
    bool v42bis_active() const;
    bool transparent_mode() const;
    int current_bit_rate() const;
    std::string link_status() const;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace v92
