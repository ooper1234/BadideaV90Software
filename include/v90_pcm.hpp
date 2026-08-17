#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace v92 {

// V.90 PCM mapping parameters learned during phase 4 (CP/CPt exchange).
// This core is deliberately independent from the startup state machine so the
// algebra can be tested before the full V.34-upstream/CP receiver is complete.
struct V90PcmParameters {
    bool alaw = false; // PCMU live path currently supports mu-law only.
    int S = 6;         // sign bits per six-sample mapping frame, 3..6
    int K = 24;        // modulo/ring coder bits
    int ld = 0;        // spectral shaping depth, 0..3
    int a1 = 0, a2 = 0, b1 = 0, b2 = 0; // 6-bit fractional coefficients
    std::array<std::vector<uint8_t>, 6> allowed_ucodes;
};

// A standards-shaped deterministic laboratory constellation useful for unit
// testing the PCM mapper. It is NOT a substitute for the CP parameters sent by
// the remote analogue modem on a real V.90 call.
V90PcmParameters v90_pcm_lab_parameters();

class V90PcmMapper {
public:
    explicit V90PcmMapper(V90PcmParameters p);
    bool valid() const { return valid_; }
    int bits_per_mapping_frame() const { return p_.S + p_.K; }
    int nominal_bit_rate() const { return (bits_per_mapping_frame() * 8000) / 6; }

    // Input/output bits are one bit per byte, in transmission order.
    std::array<int16_t, 6> encode(const std::vector<uint8_t>& bits);
    std::vector<uint8_t> decode(const std::array<int16_t, 6>& samples);

private:
    V90PcmParameters p_;
    bool valid_ = false;
    std::array<std::vector<uint8_t>,6> m_to_ucode_;
    std::array<std::vector<int16_t>,6> m_to_linear_;

    static constexpr int kRingBuf = 32;
    std::array<uint8_t,kRingBuf> ucode_{};
    std::array<uint8_t,kRingBuf> pp_{};
    int ucode_ptr_ = 0;
    int enc_last_sign_ = 0, enc_t_ = 0, enc_Q_ = 0;
    int enc_x_ = 0, enc_y_ = 0, enc_v_ = 0;
    int dec_last_sign_ = 0, dec_t_ = 0, dec_Q_ = 0;

    int linear_for_ucode(uint8_t ucode) const;
    void select_best_signs(int frame_size);
};

// V.90 8.6.5: generate CPt-parameterized Phase-4 TRN2d by applying GPC-
// scrambled binary ones to a freshly initialized PCM mapper. Output is exact
// G.711 mu-law and the duration is rounded down to a whole six-symbol frame.
std::vector<uint8_t> v90_trn2d_pcmu(const V90PcmParameters& parameters,
                                    size_t symbols = 2040);

// Build the un-scrambled V.90 Table-16 Type-0 MP information sequence.  The
// returned bit vector includes frame sync, CRC and zero fill through the next
// complete six-PCM-symbol data-frame boundary for the supplied D=S+K.
std::vector<uint8_t> build_v90_mp0_bits(int bits_per_data_frame,
                                        bool acknowledge = false,
                                        uint8_t max_upstream_rate_x2400 = 14,
                                        uint16_t upstream_rate_mask = 0x1FFF);

// Streaming Phase-4 transmitter.  TRN2d and the following MP/MP' sequences
// share one GPC scrambler and one PCM-mapper state, so ld>0 spectral-shaping
// look-ahead remains continuous across the TRN2d -> MP and MP -> MP'
// boundaries.  This is required on real modems; restarting the mapper for each
// sequence creates an invalid boundary even though each isolated block looks
// plausible in a spectrogram.
class V90Phase4DigitalTx {
public:
    explicit V90Phase4DigitalTx(const V90PcmParameters& parameters);
    bool valid() const { return valid_; }
    int nominal_bit_rate() const { return nominal_bit_rate_; }

    // Begin a fresh initial-training stream.  The returned PCM contains exactly
    // the requested whole-frame TRN2d followed immediately by the beginning of
    // the first unacknowledged MP sequence.  The fixed mapper look-ahead delay
    // is removed only once at the start of TRN2d.
    std::vector<uint8_t> start_trn2d_and_mp(size_t trn2d_symbols = 2400);

    // Append one complete MP (or MP' when acknowledge=true) to the same
    // streaming encoder.  The first few returned samples may complete the
    // preceding sequence when ld>0; there is no reset or gap on the wire.
    std::vector<uint8_t> next_mp(bool acknowledge);

    // Transmit Sequence Ed (20 data frames) followed immediately by Sequence B1d
    // (48 data frames) using the continuous GPC scrambler and spectral shaper.
    std::vector<uint8_t> start_ed_and_b1d();

    // Queue asynchronous serial octets (8-N-1 framed) for live V.90 downstream data mode.
    void feed_async_bytes(const std::vector<uint8_t>& bytes);

    // Pull continuous G.711 mu-law PCM data samples for live transmission.
    std::vector<uint8_t> produce_data_pcmu(size_t samples);

private:
    V90PcmParameters parameters_;
    V90PcmMapper mapper_;
    bool valid_ = false;
    int nominal_bit_rate_ = 0;
    int frame_bits_ = 0;
    size_t initial_discard_samples_ = 0;
    uint32_t scrambler_ = 0;
    std::deque<uint8_t> async_tx_bits_;
    std::deque<uint8_t> remaining_data_pcmu_;

    uint8_t scramble(uint8_t input_bit);
    std::vector<uint8_t> encode_raw_bits(const std::vector<uint8_t>& raw_bits);
};

} // namespace v92
