#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <vector>

namespace v92 {

// Parameters carried by the analogue modem's variable-length Ja descriptor
// (V.90 Table 12). They define the exact PCM Digital Impairment Learning
// sequence; N is not a duration or rate and the requested waveform must not be
// replaced by arbitrary noise.
struct V90DILDescriptor {
    bool valid = false;
    uint8_t segment_count = 0;
    std::vector<uint8_t> sign_pattern;
    std::vector<uint8_t> training_pattern;
    std::array<uint8_t, 8> segment_length_h{};
    std::array<uint8_t, 8> reference_ucode{};
    std::vector<uint8_t> training_ucode;
};

// Digital-modem transmitter for the downstream PCM portions of V.90 Phase 3.
// Output is already G.711 mu-law, one octet per 8000-symbol/s PCM interval.
// These are deterministic training sequences; arbitrary random audio is not a
// valid replacement and will not train an analogue V.90 receiver.
class V90Phase3DigitalTx {
public:
    explicit V90Phase3DigitalTx(uint8_t uinfo);

    bool valid() const { return valid_; }
    uint8_t uinfo() const { return uinfo_; }

    // V.90 8.4.4: Sd (384T) followed by S-bar-d (48T).
    std::vector<uint8_t> sd_and_sbar_pcmu() const;

    // V.90 8.4.5: GPC-scrambled binary ones. The scrambler is reset to zero
    // at the beginning of TRN1d and its state continues into Jd.
    std::vector<uint8_t> pp_and_trn1d_pcmu(size_t trn1d_symbols = 4798);

    // V.90 Table 13. Bit zero of rate_mask is 28000 bit/s and bit 21 is
    // 56000 bit/s, in 8000/6-bit/s increments. One conservative rate is
    // enabled by default until CP supplies the real downstream constellations.
    std::vector<uint8_t> jd_bits(uint32_t rate_mask = 1u,
                                 uint8_t max_lookahead = 1) const;
    bool jd_crc_ok(const std::vector<uint8_t>& bits) const;
    std::vector<uint8_t> jd_frame_pcmu(uint32_t rate_mask = 1u,
                                       uint8_t max_lookahead = 1);

    // V.90 8.4.3: twelve zero input bits, with scrambler and differential
    // encoder state continued from the last complete Jd sequence.
    std::vector<uint8_t> jd_bar_pcmu();

    // V.90 8.4.1: emit one complete DIL segment from the validated Ja
    // descriptor. The caller can request up to 255 segments; the live state
    // machine repeats the whole set and stops only on a subsequent S/S-bar.
    std::vector<uint8_t> dil_segment_pcmu(const V90DILDescriptor& descriptor,
                                          size_t segment_index) const;

    // V.90 8.6.4/9.4.1.1: Phase-4 Ri uses UINFO and the +++--- sign pattern.
    // Ri-bar is exactly 24T with the inverted ---+++ pattern.
    std::vector<uint8_t> ri_pcmu(size_t symbols = 192,
                                 bool inverted = false) const;

private:
    uint8_t uinfo_ = 0;
    bool valid_ = false;
    uint32_t scrambler_ = 0; // newest output bit is bit zero; 23-bit GPC state
    uint8_t differential_sign_ = 0;

    uint8_t scramble(uint8_t input_bit);
    uint8_t encode_sign(uint8_t input_bit);
};

} // namespace v92
