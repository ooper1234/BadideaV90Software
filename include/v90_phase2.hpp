#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace v92 {

// V.90 digital-modem INFO0d capability fields. Bit numbering follows
// ITU-T V.90: bit 0 is the first bit transmitted and numeric ranges marked
// LSB:MSB are serialized least-significant bit first.
struct V90Info0dConfig {
    // Bits 12:13 are reserved (0) in V.90 Table 2 (V.90 upstream supports only 3000, 3200, 3429).
    bool sr_3429 = true;
    bool sr_3000_low = true;
    bool sr_3000_high = true;
    bool sr_3200_low = true;
    bool sr_3200_high = true;
    bool disallow_3429 = false;
    bool power_reduction = true;
    uint8_t max_symbol_rate_difference = 5; // 0..5
    bool cme_modem = false;
    bool constellation_1664 = true;
    // V.92 reuses V.90 INFO0. Bits 26 and 27 request/confirm short Phase 2.
    // Leave both clear for a conventional V.90 full Phase 2 exchange.
    bool request_short_phase2 = false;
    bool v92_capable = false;
    bool acknowledge_info0a = false;
    int nominal_tx_power_dbm0 = -12;        // -6..-21 dBm0
    int max_digital_tx_power_dbm0_x2 = -24; // half-dB units: -1 means -0.5 dBm0
    bool codec_output_power_measured = false;
    bool alaw = false;                      // false = mu-law, true = A-law
    bool v90_upstream_3429 = true;
};

// Subset of analogue INFO0a fields needed by the digital modem during
// Phase 2. The raw frame is retained so later Phase-2 work can consume all
// advertised V.34 capabilities without changing the receiver API.
struct V90Info0aFrame {
    bool valid = false;
    bool v92_capable = false;          // V.92 INFO0a bit 26
    bool requests_short_phase2 = false;// V.92 INFO0a bit 27
    bool acknowledge_info0d = false; // V.90 bit 28
    std::vector<uint8_t> bits;        // exact 49 transmitted bits
    size_t first_sample = 0;          // estimated first INFO0a data symbol
};

// V.34/V.90 INFO CRC. Input is one bit per byte in time order.
uint16_t v34_info_crc(const std::vector<uint8_t>& information_bits);
std::vector<uint8_t> build_v90_info0d_bits(const V90Info0dConfig& cfg = {});
bool check_v90_info0d_crc(const std::vector<uint8_t>& bits);

// Test/reference analogue INFO0a builder. Production receives INFO0a from
// the hardware modem; this helper exists so the receiver can be regression
// tested with a standards-shaped frame.
std::vector<uint8_t> build_v90_info0a_bits(bool acknowledge_info0d = false,
                                           bool v92_capable = false,
                                           bool request_short_phase2 = false);
bool check_v90_info0a_crc(const std::vector<uint8_t>& bits);


// V.90 INFO1d is sent by the digital modem after the second Phase-2
// L1/L2 probing exchange. Each 9-bit probe result is:
// high-carrier flag, 4-bit pre-emphasis index, 4-bit projected max rate /2400.
struct V90Info1dConfig {
    uint8_t min_power_reduction_db = 0;
    uint8_t additional_power_reduction_db = 0;
    uint8_t md_length_35ms = 0;
    std::array<bool,6> high_carrier{{false,false,false,false,false,false}};
    std::array<uint8_t,6> preemphasis{{0,0,0,0,0,0}};
    // Conservative default: 9600 bit/s projected on each enabled V.34 symbol rate.
    std::array<uint8_t,6> projected_rate_x2400{{4,4,4,4,4,4}};
    int frequency_offset_x002_hz = -512; // -512 means measurement unavailable
};

// Select only symbol-rate/carrier combinations which the CRC-valid analogue
// INFO0a actually advertised. Unsupported entries are encoded with projected
// rate zero instead of claiming an impossible 9600-bit/s path.
V90Info1dConfig v90_info1d_config_from_info0a(const std::vector<uint8_t>& info0a_bits);

struct V90Info1aPhase2Frame {
    bool valid = false;
    bool requests_v90 = false;
    bool pcm_upstream = false;
    uint8_t md_length_35ms = 0;
    uint8_t upstream_symbol_rate_index = 0; // 3=3000, 4=3200, 5=3429, 6=V.92 PCM upstream
    uint8_t uinfo = 0;
    int frequency_offset_x002_hz = -512;
    std::vector<uint8_t> bits;
    size_t first_sample = 0;
};

std::vector<uint8_t> build_v90_info1d_bits(const V90Info1dConfig& cfg = {});
bool check_v90_info1d_crc(const std::vector<uint8_t>& bits);

// Test/reference Phase-2 INFO1a frame and asynchronous receiver. Production
// receives INFO1a from the hardware modem after INFO1d.
std::vector<uint8_t> build_v90_info1a_phase2_bits(bool request_v90 = true,
                                                  uint8_t upstream_symbol_rate_index = 3,
                                                  uint8_t uinfo = 80,
                                                  uint8_t md_length_35ms = 0);
bool check_v90_info1a_phase2_crc(const std::vector<uint8_t>& bits);
std::optional<V90Info1aPhase2Frame> find_v90_info1a_phase2(const std::vector<int16_t>& pcm,
                                                           int sample_rate = 8000);

// V.34 10.1.2.4 line probing waveform used verbatim by V.90 Phase 2. L1 is
// the same multitone as L2, but L1 is sent for 160 ms at +6 dB relative to
// nominal; callers select duration/power explicitly.
std::vector<int16_t> v34_line_probe(double seconds, double total_power_dbm0,
                                    int sample_rate = 8000);
double v34_line_probe_metric_dbfs(const std::vector<int16_t>& pcm,
                                  int sample_rate = 8000);
bool v34_line_probe_present(const std::vector<int16_t>& pcm,
                            int sample_rate = 8000);

// INFO sequences are 600 bit/s binary DPSK. V.90/V.34 require each INFO
// sequence to be preceded by one modulation point at arbitrary carrier phase;
// prepend_reference_point defaults true for real line transmission.
std::vector<int16_t> v90_info_dbpsk_modulate(const std::vector<uint8_t>& bits,
                                             double carrier_hz,
                                             double amplitude = 6500.0,
                                             int sample_rate = 8000,
                                             bool prepend_reference_point = true);

// Nominal demodulator for aligned laboratory captures. For real asynchronous
// RTP captures use find_v90_info0a(), which searches symbol timing and CRC.
std::vector<uint8_t> v90_info_dbpsk_demodulate(const std::vector<int16_t>& pcm,
                                               double carrier_hz,
                                               int sample_rate = 8000,
                                               bool has_reference_point = true);

// Build the analogue INFO waveform including the 1800-Hz guard tone defined
// by V.90. This is used by tests, not by the digital/server transmitter.
std::vector<int16_t> v90_info0a_waveform(const std::vector<uint8_t>& bits,
                                         int sample_rate = 8000);

// Search an arbitrary receive capture for a complete, CRC-valid analogue
// INFO0a frame. Handles arbitrary INFO start time relative to RTP packet
// boundaries and ignores the 1800-Hz guard by coherent 2400-Hz detection.
std::optional<V90Info0aFrame> find_v90_info0a(const std::vector<int16_t>& pcm,
                                              int sample_rate = 8000);

std::vector<int16_t> v90_tone(double hz, double seconds,
                              bool phase_reversed = false,
                              double amplitude = 6500.0,
                              int sample_rate = 8000);

struct V90ToneObservation {
    bool present = false;
    double re = 0.0;
    double im = 0.0;
    double energy = 0.0;
    // Fraction of total block energy that is coherent at the requested
    // frequency. A pure sinusoid is close to 0.5; wideband QAM/TRN is low.
    double coherence = 0.0;
};
V90ToneObservation observe_v90_tone(const std::vector<int16_t>& pcm,
                                    double hz,
                                    int sample_rate = 8000);
bool v90_retrain_tone_present(const std::vector<int16_t>& pcm,
                              int sample_rate = 8000);
// INFOMARKSa is a run of binary ones at 600 bit/s DBPSK on 2400 Hz. It is the
// other post-INFO1d recovery indication defined by V.90, and is deliberately
// distinguished from an unmodulated 2400-Hz Tone A.
bool v90_infomarksa_present(const std::vector<int16_t>& pcm,
                            int sample_rate = 8000);
bool v90_phase_reversed(const V90ToneObservation& a,
                        const V90ToneObservation& b);

// Locate a 180-degree phase reversal of a continuous tone in an arbitrary
// capture. Returns the sample index of the estimated transition. A sliding
// coherent detector makes this independent of RTP frame boundaries.
std::optional<size_t> find_v90_phase_reversal(const std::vector<int16_t>& pcm,
                                              double hz = 2400.0,
                                              int sample_rate = 8000,
                                              size_t window_samples = 80,
                                              size_t min_split = 0);

} // namespace v92
