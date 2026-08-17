#include "v90_phase3.hpp"

#include "g711.hpp"
#include "v90_phase2.hpp"

#include <algorithm>

namespace v92 {
namespace {

void put_literal(std::vector<uint8_t>& out, unsigned count, uint32_t value) {
    for (int i = static_cast<int>(count) - 1; i >= 0; --i)
        out.push_back(static_cast<uint8_t>((value >> i) & 1u));
}

void put_uint_lsb(std::vector<uint8_t>& out, unsigned count, uint32_t value) {
    for (unsigned i = 0; i < count; ++i)
        out.push_back(static_cast<uint8_t>((value >> i) & 1u));
}

std::vector<uint8_t> jd_crc_information(const std::vector<uint8_t>& bits) {
    std::vector<uint8_t> information;
    information.reserve(32);
    information.insert(information.end(), bits.begin() + 18, bits.begin() + 34);
    information.insert(information.end(), bits.begin() + 35, bits.begin() + 51);
    return information;
}

} // namespace

V90Phase3DigitalTx::V90Phase3DigitalTx(uint8_t uinfo):uinfo_(uinfo) {
    // Sd uses Ucode 16+UINFO, so values above 111 cannot form the required W.
    valid_ = uinfo_ >= 67 && uinfo_ <= 111;
}

std::vector<uint8_t> V90Phase3DigitalTx::sd_and_sbar_pcmu() const {
    if (!valid_) return {};
    std::vector<uint8_t> out;
    out.reserve(432);
    const uint8_t wp = ucode_to_ulaw(static_cast<uint8_t>(uinfo_ + 16), true);
    const uint8_t wn = ucode_to_ulaw(static_cast<uint8_t>(uinfo_ + 16), false);
    const uint8_t zp = ucode_to_ulaw(0, true);
    const uint8_t zn = ucode_to_ulaw(0, false);
    for (unsigned r = 0; r < 64; ++r) {
        out.insert(out.end(), {wp, zp, wp, wn, zn, wn});
    }
    for (unsigned r = 0; r < 8; ++r) {
        out.insert(out.end(), {wn, zn, wn, wp, zp, wp});
    }
    return out;
}

uint8_t V90Phase3DigitalTx::scramble(uint8_t input_bit) {
    // V.34 equation 7-1, GPC = 1 + x^-18 + x^-23. At the transmitter the
    // quotient bit is the input XOR the output bits from 18 and 23 bits ago.
    const uint8_t out = static_cast<uint8_t>((input_bit & 1u) ^
                        ((scrambler_ >> 17) & 1u) ^
                        ((scrambler_ >> 22) & 1u));
    scrambler_ = ((scrambler_ << 1) | out) & 0x7FFFFFu;
    return out;
}

uint8_t V90Phase3DigitalTx::encode_sign(uint8_t input_bit) {
    const uint8_t scrambled = scramble(input_bit);
    differential_sign_ = static_cast<uint8_t>(differential_sign_ ^ scrambled);
    return differential_sign_;
}

std::vector<uint8_t> V90Phase3DigitalTx::pp_and_trn1d_pcmu(size_t trn1d_symbols) {
    if (!valid_) return {};
    
    // V.90 8.6.2: "The first symbol of TRN1d shall be transmitted such that the preceding PP sequence 
    // and TRN1d together form a whole number of six-symbol data frames."
    // PP is 2 symbols.
    const size_t total_symbols = 2 + trn1d_symbols;
    const size_t aligned_total = total_symbols - (total_symbols % 6);
    const size_t actual_trn1d_symbols = aligned_total - 2;

    scrambler_ = 0;
    differential_sign_ = 0;
    std::vector<uint8_t> out;
    out.reserve(aligned_total);

    // V.90 8.6.2: "Sequence PP shall consist of two symbols. The first symbol... Ucode value of zero 
    // with positive sign. The second symbol... Ucode value of zero but its sign shall be identical 
    // to the sign of the first symbol of TRN1d."
    // Since TRN1d initializes scrambler to 0 and scrambles 1, the first TRN1d symbol is always positive (1).
    const uint8_t pp_sym = ucode_to_ulaw(0, true);
    out.push_back(pp_sym);
    out.push_back(pp_sym);

    uint8_t final_sign = 0;
    for (size_t i = 0; i < actual_trn1d_symbols; ++i) {
        final_sign = scramble(1);
        out.push_back(ucode_to_ulaw(uinfo_, final_sign != 0));
    }
    // Jd differential encoding is initialized with the final TRN1d symbol.
    differential_sign_ = final_sign;
    return out;
}

std::vector<uint8_t> V90Phase3DigitalTx::jd_bits(uint32_t rate_mask,
                                                 uint8_t max_lookahead) const {
    std::vector<uint8_t> bits;
    bits.reserve(72);
    put_literal(bits, 17, 0x1FFFFu); // frame sync, bits 0:16
    bits.push_back(0);               // start bit 17
    for (unsigned i = 0; i < 16; ++i) bits.push_back((rate_mask >> i) & 1u);
    bits.push_back(0);               // start bit 34
    for (unsigned i = 16; i < 22; ++i) bits.push_back((rate_mask >> i) & 1u);
    for (unsigned i = 0; i < 6; ++i) bits.push_back(0); // ITU reserved 41:46
    bits.push_back(0);               // 4-point CP/E/SCR during training
    bits.push_back(0);               // 4-point during rate renegotiation
    put_uint_lsb(bits, 2, std::clamp<unsigned>(max_lookahead, 1, 3));
    bits.push_back(0);               // start bit 51

    // V.34 10.1.2.3.2: the CRC covers information bits only.  Frame sync,
    // every start bit (17, 34 and 51), and fill bits are not CRC input.
    const std::vector<uint8_t> information = jd_crc_information(bits);
    put_literal(bits, 16, v34_info_crc(information));
    put_literal(bits, 4, 0);         // fill bits 68:71
    return bits;
}

bool V90Phase3DigitalTx::jd_crc_ok(const std::vector<uint8_t>& bits) const {
    if (bits.size() != 72) return false;
    for (size_t i = 0; i < 17; ++i) if (bits[i] != 1) return false;
    if (bits[17] || bits[34] || bits[51]) return false;
    for (size_t i = 68; i < 72; ++i) if (bits[i]) return false;
    const std::vector<uint8_t> information = jd_crc_information(bits);
    const uint16_t want = v34_info_crc(information);
    uint16_t got = 0;
    for (size_t i = 52; i < 68; ++i)
        got = static_cast<uint16_t>((got << 1) | (bits[i] & 1u));
    return got == want;
}

std::vector<uint8_t> V90Phase3DigitalTx::jd_frame_pcmu(uint32_t rate_mask,
                                                        uint8_t max_lookahead) {
    if (!valid_) return {};
    const auto bits = jd_bits(rate_mask, max_lookahead);
    std::vector<uint8_t> out;
    out.reserve(bits.size());
    for (uint8_t bit : bits)
        out.push_back(ucode_to_ulaw(uinfo_, encode_sign(bit) != 0));
    return out;
}

std::vector<uint8_t> V90Phase3DigitalTx::jd_bar_pcmu() {
    if (!valid_) return {};
    std::vector<uint8_t> out;
    out.reserve(12);
    for (unsigned i = 0; i < 12; ++i)
        out.push_back(ucode_to_ulaw(uinfo_, encode_sign(0) != 0));
    return out;
}

std::vector<uint8_t> V90Phase3DigitalTx::dil_segment_pcmu(
    const V90DILDescriptor& d, size_t segment_index) const {
    if (!valid_ || !d.valid || d.segment_count == 0 ||
        d.sign_pattern.empty() || d.training_pattern.empty() ||
        d.training_ucode.size() != d.segment_count)
        return {};
    segment_index %= d.segment_count;
    const uint8_t training = d.training_ucode[segment_index];
    if (training > 127) return {};
    const size_t chord = training / 16u;
    const size_t symbols = (static_cast<size_t>(d.segment_length_h[chord]) + 1u) * 6u;
    std::vector<uint8_t> out;
    out.reserve(symbols);
    for (size_t i = 0; i < symbols; ++i) {
        const bool use_training = d.training_pattern[i % d.training_pattern.size()] != 0;
        const uint8_t ucode = use_training ? training : d.reference_ucode[chord];
        const bool positive = d.sign_pattern[i % d.sign_pattern.size()] != 0;
        out.push_back(ucode_to_ulaw(ucode, positive));
    }
    return out;
}

std::vector<uint8_t> V90Phase3DigitalTx::ri_pcmu(size_t symbols,
                                                  bool inverted) const {
    if (!valid_) return {};
    symbols -= symbols % 6u;
    std::vector<uint8_t> out;
    out.reserve(symbols);
    for (size_t i = 0; i < symbols; ++i) {
        bool positive = (i % 6u) < 3u; // R is +++---.
        if (inverted) positive = !positive;
        out.push_back(ucode_to_ulaw(uinfo_, positive));
    }
    return out;
}

} // namespace v92
