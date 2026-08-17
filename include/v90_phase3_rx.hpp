#pragma once

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "v90_phase3.hpp"
#include "v90_pcm.hpp"
#include "v34_qam.hpp"

namespace v92 {

struct V90Phase3RxObservation {
    bool training_locked = false;
    bool training_lock_new = false;
    bool ja_detected = false;
    bool ja_detected_new = false;
    double training_correlation = 0.0;
    double symbol_clock_ppm = 0.0;
    size_t ja_symbol = 0;
    size_t ja_descriptor_bits = 0;
    uint8_t dil_segment_count = 0;
    V90DILDescriptor dil_descriptor;
    bool s_detected = false;
    bool s_detected_new = false;
    double s_correlation = 0.0;
    bool cpt_detected = false;
    bool cpt_detected_new = false;
    bool cpt_ordinary = false;       // false=CPt, true=ordinary CP/CP'
    bool cpt_acknowledge = false;    // Table-14 bit 33
    size_t cpt_bits = 0;
    bool cpt_equalizer_active = false;
    bool cpt_header_seen = false;
    double cpt_decision_error = 0.0;
    V90PcmParameters cpt_parameters;
};

// Receive-side observer for the analogue modem's V.34-modulated part of
// V.90 Phase 3. It locks symbol timing/carrier on the known GPA-scrambled
// TRN sequence, trains a short complex equalizer, and only declares Ja after
// descrambling and checking the complete variable-length DIL descriptor,
// including its start/reserved/padding fields and V.34 CRC. This is
// intentionally separate from the downstream PCM transmitter: the digital
// modem must not start Sd from a timer or a partial Ja prefix.
class V90Phase3AnalogueRx {
public:
    V90Phase3AnalogueRx(uint8_t symbol_rate_index,
                        uint8_t md_length_35ms,
                        int frequency_offset_x002_hz = -512);

    bool valid() const { return symbol_rate_ != 0; }
    V90Phase3RxObservation feed(const std::vector<int16_t>& pcm);
    V90Phase3RxObservation observation() const;

    // Begin looking for the analogue modem's post-Jd S signal. A delay is
    // useful when re-arming during DIL so the 16T S-bar that follows Jd-bar is
    // not mistaken for the later DIL-completion S sequence.
    void arm_for_s(size_t ignore_samples = 0,
                   bool require_full_burst = false);

    // Phase 4 begins with repeated CPt parameter sequences from the analogue
    // modem. Keep using the trained upstream symbol clock/carrier and accept
    // CPt only after its complete variable-length payload and V.34 CRC pass.
    void arm_for_cpt(size_t ignore_samples = 0);

    // After TRN2d/MP, the same V.34/QPSK receive path carries ordinary CP and
    // CP' (Table-14 discriminator bit 19 = 1).  Re-arm the existing trained
    // receiver but require that discriminator and retain the acknowledge bit.
    void arm_for_cp(size_t ignore_samples = 0);

    // Decode incoming V.34 async data stream during V.90 data mode.
    std::vector<uint8_t> demodulate_data(const std::vector<int16_t>& pcm);

private:
    unsigned symbol_rate_ = 0;
    double carrier_hz_ = 0.0;
    uint8_t md_length_35ms_ = 0;
    int frequency_offset_x002_hz_ = -512;
    std::vector<int16_t> samples_;
    bool training_locked_ = false;
    bool ja_detected_ = false;
    double training_correlation_ = 0.0;
    double trn_symbol_zero_sample_ = 0.0;
    double locked_carrier_hz_ = 0.0;
    double locked_samples_per_symbol_ = 0.0;
    double symbol_clock_ppm_ = 0.0;
    // The caller's TRN is the channel-training signal for all later upstream
    // V.34-modulated Phase-3/4 messages. Preserve that seven-tap solution for
    // CPt instead of going back to an unequalized slicer after DIL.
    bool equalizer_valid_ = false;
    std::complex<double> equalizer_gain_{1.0, 0.0};
    std::array<std::complex<double>, 7> equalizer_taps_{};
    size_t ja_symbol_ = 0;
    size_t ja_descriptor_bits_ = 0;
    uint8_t dil_segment_count_ = 0;
    V90DILDescriptor dil_descriptor_;
    bool s_armed_ = false;
    bool s_detected_ = false;
    bool s_require_full_burst_ = false;
    double s_correlation_ = 0.0;
    size_t s_scan_sample_ = 0;
    bool cpt_armed_ = false;
    bool cpt_detected_ = false;
    bool cpt_expect_ordinary_ = false;
    bool cpt_acknowledge_ = false;
    size_t cpt_bits_ = 0;
    bool cpt_header_seen_ = false;
    double cpt_decision_error_ = 3.14159265358979323846;
    V90PcmParameters cpt_parameters_;
    size_t cpt_scan_sample_ = 0;
    size_t last_analysis_samples_ = 0;

    // Incremental baseband processing cache
    std::vector<std::complex<double>> bb_cache_;
    size_t last_ja_search_symbol_ = 512;
    size_t last_s_search_sample_ = 0;
    size_t last_cpt_search_sample_ = 0;
    void update_bb_cache();

    std::unique_ptr<V34QamDemodulator> v34_data_demod_;

    bool try_lock_training();
    bool try_detect_ja();
    bool try_detect_s();
    bool try_detect_cpt();
};

// Standards-shaped caller-side Phase-3 waveform used by protocol regressions.
// It contains 70 ms silence, S/S-bar, optional MD, PP, 512T TRN, and the
// three complete, CRC-valid Ja DIL descriptors. Tests may select N=0 (with
// the optional second fill) or N=1 (without it) to cover both frame lengths.
std::vector<int16_t> v90_phase3_analogue_test_waveform(
    uint8_t symbol_rate_index,
    uint8_t md_length_35ms = 0,
    double amplitude = 5000.0,
    double carrier_offset_hz = 0.0,
    bool reset_scrambler_at_ja = false,
    uint8_t dil_segment_count = 0,
    size_t training_symbols = 512,
    double symbol_clock_ppm = 0.0);

// Caller-side V.34 S signal used to verify the post-Jd receive gate and
// detector. S alternates point 0 with point 0 rotated counter-clockwise 90°.
std::vector<int16_t> v90_phase3_analogue_s_test_waveform(
    uint8_t symbol_rate_index,
    size_t symbols = 128,
    double amplitude = 5000.0,
    double carrier_offset_hz = 0.0);

// Repeated, CRC-valid 4-point CPt sequence for Phase-4 receiver regressions.
std::vector<int16_t> v90_phase4_cpt_test_waveform(
    uint8_t symbol_rate_index,
    double amplitude = 5000.0,
    double carrier_offset_hz = 0.0,
    unsigned repeats = 3,
    uint8_t max_constellation_index = 0,
    bool separate_codec_constellations = false,
    bool ordinary_cp = false,
    bool short_fill = false,
    bool acknowledge = false);

} // namespace v92
