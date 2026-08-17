#include "v90_phase3_rx.hpp"
#include "v90_phase2.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <iostream>
#include <limits>
#include <vector>
#include <memory>

namespace v92 {
namespace {

constexpr double kPi = 3.1415926535897932384626433832795;
constexpr double kSampleRate = 8000.0;
using Complex = std::complex<double>;

unsigned symbol_rate(uint8_t index) {
    switch (index) {
        case 3: return 3000;
        case 4: return 3200;
        case 5: return 3429;
        default: return 0;
    }
}

double low_carrier(unsigned rate) {
    switch (rate) {
        case 3000: return 1800.0;
        case 3200: return 3200.0 * 4.0 / 7.0;
        case 3429: return (24000.0 / 7.0) * 4.0 / 7.0;
        default: return 0.0;
    }
}

double rrc(double t, double beta) {
    if (std::abs(t) < 1.0e-10)
        return 1.0 + beta * (4.0 / kPi - 1.0);
    const double singular = 1.0 / (4.0 * beta);
    if (std::abs(std::abs(t) - singular) < 1.0e-8) {
        const double a = (1.0 + 2.0 / kPi) * std::sin(kPi / (4.0 * beta));
        const double b = (1.0 - 2.0 / kPi) * std::cos(kPi / (4.0 * beta));
        return beta * (a + b) / std::sqrt(2.0);
    }
    const double num = std::sin(kPi * t * (1.0 - beta)) +
                       4.0 * beta * t * std::cos(kPi * t * (1.0 + beta));
    const double den = kPi * t * (1.0 - 16.0 * beta * beta * t * t);
    return num / den;
}

uint8_t scramble_gpa(uint32_t& reg, uint8_t input) {
    // V.34 equation 7-2, GPA = 1 + x^-5 + x^-23 (calling modem).
    const uint8_t out = static_cast<uint8_t>((input & 1u) ^
                         ((reg >> 4) & 1u) ^ ((reg >> 22) & 1u));
    reg = ((reg << 1) | out) & 0x7FFFFFu;
    return out;
}

uint8_t descramble_gpa(uint32_t& reg, uint8_t input) {
    const uint8_t out = static_cast<uint8_t>((input & 1u) ^
                         ((reg >> 4) & 1u) ^ ((reg >> 22) & 1u));
    reg = ((reg << 1) | (input & 1u)) & 0x7FFFFFu;
    return out;
}

Complex qpsk_clockwise(unsigned quadrant) {
    const double phase = -0.5 * kPi * static_cast<double>(quadrant & 3u);
    return Complex(std::cos(phase), std::sin(phase));
}

struct TrainingReference {
    std::vector<Complex> points;
    std::vector<uint32_t> state_after_symbol;
};

struct TrainingClockEstimate {
    double samples_per_symbol = 0.0;
    double ppm = 0.0;
    size_t matched_symbols = 0;
    double mean_score = 0.0;
};

TrainingReference training_reference(size_t symbols) {
    TrainingReference r;
    r.points.reserve(symbols);
    r.state_after_symbol.reserve(symbols + 1);
    uint32_t reg = 0;
    r.state_after_symbol.push_back(reg);
    for (size_t n = 0; n < symbols; ++n) {
        const unsigned i1 = scramble_gpa(reg, 1);
        const unsigned i2 = scramble_gpa(reg, 1);
        r.points.push_back(qpsk_clockwise(2u * i2 + i1));
        r.state_after_symbol.push_back(reg);
    }
    return r;
}

std::vector<Complex> baseband_rrc(const std::vector<int16_t>& pcm,
                                  double carrier,
                                  unsigned rate,
                                  size_t phase_offset = 0) {
    std::vector<Complex> mixed(pcm.size()), out(pcm.size());
    for (size_t n = 0; n < pcm.size(); ++n) {
        const double phase = -2.0 * kPi * carrier * static_cast<double>(n + phase_offset) / kSampleRate;
        mixed[n] = static_cast<double>(pcm[n]) * Complex(std::cos(phase), std::sin(phase));
    }
    constexpr int half = 13;
    double norm = 0.0;
    std::array<double, 2 * half + 1> h{};
    for (int k = -half; k <= half; ++k) {
        h[static_cast<size_t>(k + half)] =
            rrc(static_cast<double>(k) * rate / kSampleRate, 0.25);
        norm += std::abs(h[static_cast<size_t>(k + half)]);
    }
    if (norm <= 0.0) norm = 1.0;
    for (size_t n = half; n + half < pcm.size(); ++n) {
        Complex y(0.0, 0.0);
        for (int k = -half; k <= half; ++k)
            y += mixed[static_cast<size_t>(static_cast<long long>(n) - k)] *
                 (h[static_cast<size_t>(k + half)] / norm);
        out[n] = y;
    }
    return out;
}

Complex interpolate(const std::vector<Complex>& v, double at) {
    if (at < 0.0 || at + 1.0 >= static_cast<double>(v.size())) return {};
    const size_t i = static_cast<size_t>(std::floor(at));
    const double f = at - static_cast<double>(i);
    return v[i] * (1.0 - f) + v[i + 1] * f;
}

size_t find_signal_start(const std::vector<int16_t>& pcm) {
    constexpr size_t window = 40;
    if (pcm.size() < 4 * window) return std::numeric_limits<size_t>::max();
    auto window_rms = [&](size_t a) {
        long double e = 0.0;
        for (size_t i = a; i < a + window; ++i) {
            const long double x = pcm[i];
            e += x * x;
        }
        return std::sqrt(static_cast<double>(e / window));
    };
    // INFO1a has just ended, so ignore only the first few milliseconds. The
    // specified 70-ms guard may be shorter in this buffer because the INFO1a
    // detector can consume the tail of an RTP packet. Look for a rising edge,
    // not just the first energy: an unrelated burst must not permanently shift
    // the predicted TRN location.
    size_t latest = std::numeric_limits<size_t>::max();
    const size_t end = std::min<size_t>(pcm.size(), 2400);
    for (size_t a = window; a + 3 * window <= end; a += 4) {
        const double before = window_rms(a - window);
        const double r0 = window_rms(a), r1 = window_rms(a + window),
                     r2 = window_rms(a + 2 * window);
        const double after = std::min({r0, r1, r2});
        if (after >= 250.0 && before < std::max(200.0, after / 2.5)) latest = a;
    }
    return latest;
}

int modulo4(int x) {
    x %= 4;
    return x < 0 ? x + 4 : x;
}

unsigned get_uint_lsb(const std::vector<uint8_t>& bits, size_t offset,
                      unsigned count) {
    unsigned value = 0;
    for (unsigned i = 0; i < count; ++i)
        value |= static_cast<unsigned>(bits[offset + i] & 1u) << i;
    return value;
}

bool fixed_ja_header_ok(const std::vector<uint8_t>& bits, size_t offset) {
    if (offset + 52 > bits.size()) return false;
    unsigned sync_errors = 0;
    for (size_t i = 0; i < 17; ++i) sync_errors += bits[offset + i] != 1;
    if (sync_errors > 1 || bits[offset + 17] != 0) return false;
    for (size_t i = 26; i <= 34; ++i)
        if (bits[offset + i] != 0) return false;
    return bits[offset + 42] == 0 && bits[offset + 50] == 0 &&
           bits[offset + 51] == 0;
}

size_t ja_descriptor_size(const std::vector<uint8_t>& bits, size_t offset) {
    if (!fixed_ja_header_ok(bits, offset)) return 0;
    const unsigned n = get_uint_lsb(bits, offset + 18, 8);
    const unsigned lsp = get_uint_lsb(bits, offset + 35, 7) + 1;
    const unsigned ltp = get_uint_lsb(bits, offset + 43, 7) + 1;
    if (n == 0 && (lsp != 1 || ltp != 1)) return 0;
    const size_t a = ((lsp + 15u) / 16u) * 17u;
    const size_t b = a + ((ltp + 15u) / 16u) * 17u;
    // Table 12/V.90 requires one fill bit after the CRC. A second fill bit is
    // present only when necessary to make the descriptor length even.
    const size_t mandatory = 205u + b + ((n + 1u) / 2u) * 17u;
    return mandatory + (mandatory & 1u);
}

std::vector<uint8_t> ja_crc_information(const std::vector<uint8_t>& bits,
                                        size_t offset,
                                        size_t b,
                                        unsigned n) {
    const size_t parameters = 51u + b;
    const size_t ucodes = parameters + 136u;
    const size_t crc_start = ucodes + ((n + 1u) / 2u) * 17u;
    std::vector<uint8_t> information;
    information.reserve(crc_start - 18u);
    for (size_t p = 18u; p < crc_start; ++p) {
        const bool start_bit =
            p == 34u ||
            (p >= 51u && p < parameters && (p - 51u) % 17u == 0) ||
            (p >= parameters && p < ucodes && (p - parameters) % 17u == 0) ||
            (p >= ucodes && (p - ucodes) % 17u == 0);
        if (!start_bit) information.push_back(bits[offset + p]);
    }
    return information;
}

bool complete_ja_descriptor_ok(const std::vector<uint8_t>& bits,
                               size_t offset,
                               uint8_t& n_out,
                               size_t& length_out) {
    const size_t length = ja_descriptor_size(bits, offset);
    if (!length || offset + length > bits.size()) return false;
    const unsigned n = get_uint_lsb(bits, offset + 18, 8);
    const unsigned lsp = get_uint_lsb(bits, offset + 35, 7) + 1;
    const unsigned ltp = get_uint_lsb(bits, offset + 43, 7) + 1;
    const size_t a = ((lsp + 15u) / 16u) * 17u;
    const size_t b = a + ((ltp + 15u) / 16u) * 17u;

    // Each 16-bit SP/TP block is introduced by a start bit. Padding beyond
    // the declared pattern lengths is required to be zero.
    for (size_t block = 0; block < a / 17u; ++block) {
        const size_t p = offset + 51u + block * 17u;
        if (bits[p] != 0) return false;
        for (size_t j = 0; j < 16; ++j)
            if (block * 16u + j >= lsp && bits[p + 1u + j] != 0) return false;
    }
    for (size_t block = 0; block < (b - a) / 17u; ++block) {
        const size_t p = offset + 51u + a + block * 17u;
        if (bits[p] != 0) return false;
        for (size_t j = 0; j < 16; ++j)
            if (block * 16u + j >= ltp && bits[p + 1u + j] != 0) return false;
    }

    const size_t parameters = offset + 51u + b;
    // Four H groups followed by four REF groups; each group contains a start
    // bit and two seven-bit values separated/terminated by reserved zeroes.
    for (size_t group = 0; group < 8; ++group) {
        const size_t p = parameters + group * 17u;
        if (bits[p] != 0 || bits[p + 8u] != 0 || bits[p + 16u] != 0)
            return false;
    }

    const size_t ucode_groups = (n + 1u) / 2u;
    const size_t ucodes = parameters + 136u;
    for (size_t group = 0; group < ucode_groups; ++group) {
        const size_t p = ucodes + group * 17u;
        if (bits[p] != 0 || bits[p + 8u] != 0) return false;
        if (2u * group + 1u < n) {
            if (bits[p + 16u] != 0) return false;
        } else {
            for (size_t j = 8; j <= 16; ++j)
                if (bits[p + j] != 0) return false;
        }
    }

    const size_t crc_start = ucodes + ucode_groups * 17u;
    if (bits[crc_start] != 0 || bits[crc_start + 17u] != 0) return false;
    if (length == crc_start + 19u && bits[crc_start + 18u] != 0) return false;
    // V.34 10.1.2.3.2 excludes frame sync, all start bits, and fill bits
    // from the CRC.  This matters for real Ja frames whose internal start
    // bits were previously (and incorrectly) included by our receiver.
    const std::vector<uint8_t> information =
        ja_crc_information(bits, offset, b, n);
    const uint16_t want = v34_info_crc(information);
    uint16_t got = 0;
    for (size_t i = 1; i <= 16; ++i)
        got = static_cast<uint16_t>((got << 1) | (bits[crc_start + i] & 1u));
    if (got != want) return false;

    n_out = static_cast<uint8_t>(n);
    length_out = length;
    return true;
}

V90DILDescriptor parse_ja_descriptor(const std::vector<uint8_t>& bits,
                                      size_t offset) {
    V90DILDescriptor d;
    uint8_t n = 0;
    size_t length = 0;
    if (!complete_ja_descriptor_ok(bits, offset, n, length)) return d;
    (void)length;
    const unsigned lsp = get_uint_lsb(bits, offset + 35u, 7u) + 1u;
    const unsigned ltp = get_uint_lsb(bits, offset + 43u, 7u) + 1u;
    const size_t a = ((lsp + 15u) / 16u) * 17u;
    const size_t b = a + ((ltp + 15u) / 16u) * 17u;
    d.segment_count = n;
    d.sign_pattern.reserve(lsp);
    d.training_pattern.reserve(ltp);
    for (unsigned i = 0; i < lsp; ++i) {
        const size_t block = i / 16u, within = i % 16u;
        d.sign_pattern.push_back(bits[offset + 52u + block * 17u + within]);
    }
    for (unsigned i = 0; i < ltp; ++i) {
        const size_t block = i / 16u, within = i % 16u;
        d.training_pattern.push_back(
            bits[offset + 52u + a + block * 17u + within]);
    }
    const size_t parameters = offset + 51u + b;
    for (size_t pair = 0; pair < 4; ++pair) {
        const size_t p = parameters + pair * 17u;
        d.segment_length_h[2u * pair] =
            static_cast<uint8_t>(get_uint_lsb(bits, p + 1u, 7u));
        d.segment_length_h[2u * pair + 1u] =
            static_cast<uint8_t>(get_uint_lsb(bits, p + 9u, 7u));
    }
    const size_t refs = parameters + 68u;
    for (size_t pair = 0; pair < 4; ++pair) {
        const size_t p = refs + pair * 17u;
        d.reference_ucode[2u * pair] =
            static_cast<uint8_t>(get_uint_lsb(bits, p + 1u, 7u));
        d.reference_ucode[2u * pair + 1u] =
            static_cast<uint8_t>(get_uint_lsb(bits, p + 9u, 7u));
    }
    const size_t ucodes = parameters + 136u;
    d.training_ucode.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        const size_t group = i / 2u, within = i % 2u;
        d.training_ucode.push_back(static_cast<uint8_t>(get_uint_lsb(
            bits, ucodes + group * 17u + 1u + within * 8u, 7u)));
    }
    d.valid = d.sign_pattern.size() == lsp &&
              d.training_pattern.size() == ltp &&
              d.training_ucode.size() == n;
    return d;
}

size_t cpt_sequence_size(const std::vector<uint8_t>& bits, size_t offset,
                         bool ordinary_cp = false, size_t* sync_len_out = nullptr) {
    if (offset + 120u > bits.size()) return 0;

    // Try standard 17-bit preamble, 16-bit preamble, and 14-bit (V.34-derived) preamble
    for (size_t sync_len : {17u, 16u, 14u}) {
        if (offset + sync_len + 115u > bits.size()) continue;

        // Check the all-ones preamble (allow at most 1 bit error)
        unsigned sync_errors = 0;
        for (size_t i = 0; i < sync_len; ++i)
            sync_errors += bits[offset + i] != 1u;
        if (sync_errors > 2u) continue;

        // Start bit, reserved bit, and CP/CPt discriminator
        const size_t start_bit = sync_len;
        if (bits[offset + start_bit] != 0u) continue;
        if (bits[offset + start_bit + 1u] != 0u) continue;
        if (!ordinary_cp && bits[offset + start_bit + 2u] != 0u) continue;

        // Fixed-zero interior positions (period of 17 bits)
        unsigned fixed_errors = 0;
        for (size_t k = 1; k <= 6; ++k) {
            const size_t p = start_bit + k * 17u;
            if (offset + p < bits.size())
                fixed_errors += bits[offset + p] != 0u;
        }
        if (fixed_errors > 2u) continue;

        // Constellation indices (6 frames of 4 bits each)
        unsigned max_index = 0;
        bool indices_valid = true;
        const size_t indices_base = start_bit + 86u;
        for (size_t frame = 0; frame < 6u; ++frame) {
            const unsigned index = get_uint_lsb(bits, offset + indices_base + 4u * frame, 4u);
            if (index > 5u) { indices_valid = false; break; }
            max_index = std::max(max_index, index);
        }
        if (!indices_valid) continue;

        const size_t g = 136u * max_index;
        const size_t codec_bit = start_bit + 111u;
        const size_t d = (offset + codec_bit < bits.size() && bits[offset + codec_bit]) ? (2u * g + 136u) : g;
        if (sync_len_out) *sync_len_out = sync_len;
        return sync_len + 275u + d;
    }
    return 0;
}

std::vector<uint8_t> cpt_crc_information(const std::vector<uint8_t>& bits,
                                         size_t offset, size_t sync_len, size_t crc_start) {
    std::vector<uint8_t> information;
    const size_t start_bit = sync_len;
    information.reserve(crc_start - start_bit);
    for (size_t p = start_bit + 1u; p < crc_start; ++p) {
        // Skip periodic start bits (every 17 bits starting at start_bit + 17)
        const bool is_start_bit = (p >= start_bit + 17u) && ((p - start_bit) % 17u == 0u);
        if (!is_start_bit)
            information.push_back(bits[offset + p]);
    }
    return information;
}

bool complete_cpt_sequence_ok(const std::vector<uint8_t>& bits,
                              size_t offset,
                              V90PcmParameters& parameters,
                              size_t& length_out,
                              bool short_fill = false,
                              bool ordinary_cp = false,
                              bool* acknowledge_out = nullptr,
                              bool verbose = false) {
    size_t sync_len = 17u;
    const size_t standard_length = cpt_sequence_size(bits, offset, ordinary_cp, &sync_len);
    if (!standard_length) return false;
    const size_t length = standard_length - (short_fill ? 2u : 0u);
    if (offset + length > bits.size()) return false;
    const size_t d = standard_length - (sync_len + 275u);
    const size_t crc_start = sync_len + 255u + d;

    // Period-start and fill bits should be 0 per V.90 Table 14
    unsigned structural_errors = 0;
    const size_t start_bit = sync_len;
    for (size_t p = start_bit + 119u; p < crc_start; p += 17u)
        structural_errors += bits[offset + p] != 0u;
    structural_errors += bits[offset + crc_start] != 0u;
    const size_t fill_end = crc_start + (short_fill ? 17u : 19u);
    for (size_t p = crc_start + 17u; p <= fill_end && offset + p < bits.size(); ++p)
        structural_errors += bits[offset + p] != 0u;
    if (structural_errors > 3u) {
        if (verbose)
            fprintf(stderr, "[CPt] header@%zu: sync=%zu structural_errors=%u > 3, rejecting\n",
                    offset, sync_len, structural_errors);
        return false;
    }

    const auto information = cpt_crc_information(bits, offset, sync_len, crc_start);
    const uint16_t want = v34_info_crc(information);
    uint16_t got = 0;
    for (size_t p = crc_start + 1u; p <= crc_start + 16u; ++p)
        got = static_cast<uint16_t>((got << 1) | (bits[offset + p] & 1u));
    if (got != want) {
        if (verbose)
            fprintf(stderr, "[CPt] header@%zu: sync=%zu CRC mismatch got=0x%04X want=0x%04X struct_errs=%u\n",
                    offset, sync_len, got, want, structural_errors);
        return false;
    }

    const unsigned drn = get_uint_lsb(bits, offset + start_bit + 3u, 5u);
    const unsigned sr  = get_uint_lsb(bits, offset + start_bit + 14u, 2u);
    const bool acknowledge = bits[offset + start_bit + 16u] != 0u;
    if (drn > 31u) return false;

    V90PcmParameters p;
    p.alaw = bits[offset + start_bit + 18u] != 0u;
    p.S = 6 - static_cast<int>(sr);
    p.K = static_cast<int>(std::min(drn, 22u) + (ordinary_cp ? 20u : 8u)) - p.S;
    p.ld = static_cast<int>(get_uint_lsb(bits, offset + start_bit + 32u, 2u));
    auto signed_q16 = [&](size_t at) {
        return static_cast<int>(static_cast<int8_t>(
            get_uint_lsb(bits, offset + at, 8u)));
    };
    p.a1 = signed_q16(start_bit + 52u); p.a2 = signed_q16(start_bit + 60u);
    p.b1 = signed_q16(start_bit + 69u); p.b2 = signed_q16(start_bit + 77u);

    std::array<unsigned, 6> indices{};
    const size_t indices_base = start_bit + 86u;
    for (size_t frame = 0; frame < 6u; ++frame)
        indices[frame] = get_uint_lsb(bits, offset + indices_base + 4u * frame, 4u);

    const size_t transmit_base = start_bit + 119u;
    for (size_t frame = 0; frame < 6u; ++frame) {
        const size_t base = transmit_base + 136u * indices[frame];
        for (size_t chord = 0; chord < 8u; ++chord) {
            const size_t mask = base + 17u * chord + 1u;
            for (size_t within = 0; within < 16u; ++within)
                if (bits[offset + mask + within])
                    p.allowed_ucodes[frame].push_back(static_cast<uint8_t>(
                        16u * chord + within));
        }
    }

    parameters = std::move(p);
    length_out = length;
    if (acknowledge_out) *acknowledge_out = acknowledge;
    return true;
}

std::vector<uint8_t> test_ja_descriptor(uint8_t n) {
    // Independent fixed vectors for the two descriptor parities exercised by
    // the waveform regression. N=0 has 192 all-zero information bits and an
    // optional second fill; N=1 has 208 information bits (only the first is
    // one) and no optional fill. Do not use the parser's CRC coverage helper.
    if (n > 1) return {};
    const size_t crc_start = 221u + (n ? 17u : 0u);
    std::vector<uint8_t> bits(crc_start + 1u, 0);
    std::fill(bits.begin(), bits.begin() + 17, 1);
    bits[18] = n;
    const uint16_t crc = n ? 0x3AFDu : 0xEF8Au;
    for (int i = 15; i >= 0; --i)
        bits.push_back(static_cast<uint8_t>((crc >> i) & 1u));
    bits.push_back(0); // mandatory fill
    if (bits.size() & 1u) bits.push_back(0); // optional even-length fill
    return bits;
}

struct EqualizerModel {
    Complex gain{1.0, 0.0};
    std::array<Complex, 7> taps{};
    bool valid = false;
};

std::vector<Complex> apply_equalizer(std::vector<Complex> raw,
                                     const EqualizerModel& model) {
    if (!model.valid || std::abs(model.gain) < 1.0e-9) return {};
    for (auto& z : raw) z /= model.gain;
    constexpr int taps = 7;
    constexpr int centre = taps / 2;
    std::vector<Complex> out(raw.size());
    for (size_t n = centre; n + centre < raw.size(); ++n)
        for (int j = 0; j < taps; ++j)
            out[n] += model.taps[j] *
                raw[static_cast<size_t>(static_cast<long long>(n) + j - centre)];
    return out;
}


std::vector<Complex> adapt_qpsk_equalizer(std::vector<Complex> raw,
                                           const EqualizerModel& seed) {
    if (!seed.valid || std::abs(seed.gain) < 1.0e-9 || raw.size() < 48u)
        return {};
    for (auto& z : raw) z /= seed.gain;
    constexpr int taps = 7;
    constexpr int centre = taps / 2;
    auto w = seed.taps;

    // Start with a few constant-modulus passes. This does not need carrier
    // phase or data decisions and pulls a stale TRN equalizer back toward the
    // current four-point CPt channel before decision-directed refinement.
    for (int epoch = 0; epoch < 3; ++epoch) {
        for (size_t n = centre; n + centre < raw.size(); ++n) {
            Complex y(0.0, 0.0);
            double energy = 1.0e-6;
            for (int j = 0; j < taps; ++j) {
                const Complex x = raw[static_cast<size_t>(
                    static_cast<long long>(n) + j - centre)];
                y += w[j] * x;
                energy += std::norm(x);
            }
            const double power = std::norm(y);
            if (power < 0.04 || power > 9.0) continue;
            const Complex error = (1.0 - power) * y;
            const double mu = 0.045 / energy;
            for (int j = 0; j < taps; ++j) {
                const Complex x = raw[static_cast<size_t>(
                    static_cast<long long>(n) + j - centre)];
                w[j] += mu * error * std::conj(x);
            }
        }
    }

    // Phase 4 introduces a new, strong period-6 downstream Ri signal. Even
    // after echo cancellation the effective upstream channel can differ
    // slightly from the silent-downstream TRN channel that produced the
    // retained Phase-3 equalizer.  CPt itself is 4-point constant-modulus
    // QPSK, so it can safely refine those seven taps without knowing any CPt
    // data bits.  The fourth-power phase estimate removes the QPSK rotation
    // ambiguity and the normalized LMS step only follows high-confidence
    // points. CRC/framing remain the final acceptance criterion.
    for (int epoch = 0; epoch < 8; ++epoch) {
        Complex fourth(0.0, 0.0);
        size_t fourth_count = 0;
        for (size_t n = centre; n + centre < raw.size(); ++n) {
            Complex y(0.0, 0.0);
            for (int j = 0; j < taps; ++j)
                y += w[j] * raw[static_cast<size_t>(
                    static_cast<long long>(n) + j - centre)];
            const double mag = std::abs(y);
            if (mag < 0.15 || mag > 3.5) continue;
            const Complex unit = y / mag;
            fourth += unit * unit * unit * unit;
            ++fourth_count;
        }
        if (fourth_count < 24u || std::abs(fourth) < 1.0e-8) break;
        const double phase = std::arg(fourth) / 4.0;
        const Complex rot(std::cos(-phase), std::sin(-phase));
        const Complex unrot(std::cos(phase), std::sin(phase));

        for (size_t n = centre; n + centre < raw.size(); ++n) {
            Complex y(0.0, 0.0);
            double energy = 1.0e-6;
            for (int j = 0; j < taps; ++j) {
                const Complex x = raw[static_cast<size_t>(
                    static_cast<long long>(n) + j - centre)];
                y += w[j] * x;
                energy += std::norm(x);
            }
            const double mag = std::abs(y);
            if (mag < 0.20 || mag > 3.0) continue;
            const Complex yr = y * rot;
            const int quadrant = static_cast<int>(std::llround(
                std::arg(yr) / (0.5 * kPi)));
            const double target_phase = 0.5 * kPi * quadrant;
            const Complex target = Complex(std::cos(target_phase),
                                           std::sin(target_phase)) * unrot;
            const Complex error = target - y;
            // Reject points already too far from any QPSK decision; they are
            // usually burst/echo contamination and should not steer the taps.
            if (std::abs(error) > 1.20) continue;
            const double mu = 0.20 / energy;
            for (int j = 0; j < taps; ++j) {
                const Complex x = raw[static_cast<size_t>(
                    static_cast<long long>(n) + j - centre)];
                w[j] += mu * error * std::conj(x);
            }
        }
    }

    std::vector<Complex> out(raw.size());
    for (size_t n = centre; n + centre < raw.size(); ++n)
        for (int j = 0; j < taps; ++j)
            out[n] += w[j] * raw[static_cast<size_t>(
                static_cast<long long>(n) + j - centre)];
    return out;
}

std::vector<Complex> equalized_symbols(const std::vector<Complex>& bb,
                                       double symbol0,
                                       double samples_per_symbol,
                                       const std::vector<Complex>& target,
                                       EqualizerModel* model_out = nullptr) {
    if (model_out) *model_out = EqualizerModel{};
    if (bb.size() < 28 || symbol0 < 14.0) return {};
    const size_t count = static_cast<size_t>(std::max(0.0,
        std::floor((static_cast<double>(bb.size()) - 15.0 - symbol0) /
                   samples_per_symbol)));
    if (count < 40) return {};
    std::vector<Complex> raw(count);
    for (size_t n = 0; n < count; ++n)
        raw[n] = interpolate(bb, symbol0 + n * samples_per_symbol);

    const size_t train = std::min<size_t>({512, raw.size(), target.size()});
    Complex gain(0.0, 0.0);
    double target_energy = 0.0;
    for (size_t n = 8; n + 8 < train; ++n) {
        gain += raw[n] * std::conj(target[n]);
        target_energy += std::norm(target[n]);
    }
    if (target_energy <= 0.0 || std::abs(gain) < 1.0e-9) return {};
    gain /= target_energy;
    for (auto& z : raw) z /= gain;

    constexpr int taps = 7;
    constexpr int centre = taps / 2;
    std::array<Complex, taps> w{};
    w[centre] = Complex(1.0, 0.0);
    for (int epoch = 0; epoch < 3; ++epoch) {
        for (size_t n = centre; n + centre < train; ++n) {
            Complex y(0.0, 0.0);
            double energy = 1.0e-6;
            for (int j = 0; j < taps; ++j) {
                const Complex x = raw[static_cast<size_t>(static_cast<long long>(n) + j - centre)];
                y += w[j] * x;
                energy += std::norm(x);
            }
            const Complex error = target[n] - y;
            const double mu = 0.22 / energy;
            for (int j = 0; j < taps; ++j) {
                const Complex x = raw[static_cast<size_t>(static_cast<long long>(n) + j - centre)];
                w[j] += mu * error * std::conj(x);
            }
        }
    }

    EqualizerModel model;
    model.gain = gain;
    model.taps = w;
    model.valid = true;
    if (model_out) *model_out = model;
    // raw was normalized above, so use unity gain when applying the freshly
    // trained taps. The retained model keeps the original gain for later CPt.
    model.gain = Complex(1.0, 0.0);
    return apply_equalizer(std::move(raw), model);
}

TrainingClockEstimate estimate_training_clock(
    const std::vector<Complex>& bb,
    double symbol0,
    double nominal_samples_per_symbol,
    const std::vector<Complex>& target) {
    TrainingClockEstimate best;
    best.samples_per_symbol = nominal_samples_per_symbol;
    if (target.size() < 160 || bb.size() < 64 || symbol0 < 14.0) return best;

    constexpr size_t window = 96;
    constexpr size_t first = 32;
    // V.34 allows small independent clock error. Over a multi-second TRN it
    // accumulates to more than a symbol, so a fixed 8000/R sampling interval
    // can lock the beginning perfectly yet miss every repeated Ja descriptor.
    // Select the clock which keeps consecutive known-TRN windows correlated
    // for the longest prefix; the first low-correlation window is normally Ja.
    for (int ppm_i = -300; ppm_i <= 300; ppm_i += 10) {
        const double ppm = static_cast<double>(ppm_i);
        const double sps = nominal_samples_per_symbol / (1.0 + ppm * 1.0e-6);
        size_t matched = 0;
        double scores = 0.0;
        for (size_t start = first;
             start + window < target.size() &&
             symbol0 + (start + window) * sps + 14.0 < bb.size();
             start += window) {
            Complex corr(0.0, 0.0);
            double received_energy = 0.0;
            for (size_t n = start; n < start + window; ++n) {
                const Complex y = interpolate(bb, symbol0 + n * sps);
                corr += y * std::conj(target[n]);
                received_energy += std::norm(y);
            }
            const double score = received_energy > 1.0e-12 ?
                std::norm(corr) / (received_energy * window) : 0.0;
            if (score < 0.15) break;
            matched += window;
            scores += score;
        }
        const double mean = matched ? scores / (matched / window) : 0.0;
        if (matched > best.matched_symbols ||
            (matched == best.matched_symbols && mean > best.mean_score + 1.0e-6) ||
            (matched == best.matched_symbols &&
             std::abs(mean - best.mean_score) <= 1.0e-6 &&
             std::abs(ppm) < std::abs(best.ppm))) {
            best.samples_per_symbol = sps;
            best.ppm = ppm;
            best.matched_symbols = matched;
            best.mean_score = mean;
        }
    }
    return best;
}

} // namespace

V90Phase3AnalogueRx::V90Phase3AnalogueRx(uint8_t symbol_rate_index,
                                         uint8_t md_length_35ms,
                                         int frequency_offset_x002_hz)
    : symbol_rate_(symbol_rate(symbol_rate_index)),
      carrier_hz_(low_carrier(symbol_rate_)),
      md_length_35ms_(md_length_35ms),
      frequency_offset_x002_hz_(frequency_offset_x002_hz) {}

V90Phase3RxObservation V90Phase3AnalogueRx::observation() const {
    V90Phase3RxObservation r;
    r.training_locked = training_locked_;
    r.ja_detected = ja_detected_;
    r.training_correlation = training_correlation_;
    r.symbol_clock_ppm = symbol_clock_ppm_;
    r.ja_symbol = ja_symbol_;
    r.ja_descriptor_bits = ja_descriptor_bits_;
    r.dil_segment_count = dil_segment_count_;
    r.dil_descriptor = dil_descriptor_;
    r.s_detected = s_detected_;
    r.s_correlation = s_correlation_;
    r.cpt_detected = cpt_detected_;
    r.cpt_ordinary = cpt_expect_ordinary_;
    r.cpt_acknowledge = cpt_acknowledge_;
    r.cpt_bits = cpt_bits_;
    r.cpt_equalizer_active = equalizer_valid_;
    r.cpt_header_seen = cpt_header_seen_;
    r.cpt_decision_error = cpt_decision_error_;
    r.cpt_parameters = cpt_parameters_;
    return r;
}

V90Phase3RxObservation V90Phase3AnalogueRx::feed(const std::vector<int16_t>& pcm) {
    V90Phase3RxObservation before = observation();
    if (!valid() || pcm.empty()) return before;
    samples_.insert(samples_.end(), pcm.begin(), pcm.end());
    // A compliant Phase 3 reaches Ja quickly. Bound capture memory and CPU but
    // retain enough time for long MD signals and real RTP jitter. Once TRN is
    // locked, a 40-ms cadence leaves ample room inside Ja's 500-ms response
    // window while avoiding a full clock/equalizer search on every RTP packet.
    const size_t analysis_step = (s_armed_ || cpt_armed_) ? 160u :
                                 (training_locked_ ? 320u : 120u);
    if (samples_.size() < last_analysis_samples_ + analysis_step) return observation();
    last_analysis_samples_ = samples_.size();

    bool lock_new = false;
    bool ja_new = false;
    bool s_new = false;
    bool cpt_new = false;
    if (!training_locked_) lock_new = try_lock_training();
    if (training_locked_) update_bb_cache();
    if (training_locked_ && !ja_detected_) ja_new = try_detect_ja();
    if (training_locked_ && ja_detected_ && s_armed_ && !s_detected_)
        s_new = try_detect_s();
    if (training_locked_ && ja_detected_ && cpt_armed_ && !cpt_detected_)
        cpt_new = try_detect_cpt();
    auto r = observation();
    r.training_lock_new = lock_new;
    r.ja_detected_new = ja_new;
    r.s_detected_new = s_new;
    r.cpt_detected_new = cpt_new;
    return r;
}

void V90Phase3AnalogueRx::update_bb_cache() {
    constexpr int half = 13;
    const size_t processed = bb_cache_.size();
    if (processed + half >= samples_.size()) return;
    const size_t start_pcm = (processed > half) ? processed - half : 0;
    std::vector<int16_t> chunk(samples_.begin() + start_pcm, samples_.end());
    auto bb_chunk = baseband_rrc(chunk, locked_carrier_hz_, symbol_rate_, start_pcm);
    
    // The FIR filter requires `half` samples of lookahead. 
    // So `bb_chunk` is only valid up to `chunk.size() - half - 1`.
    // Only push valid samples so they can be properly filtered next time.
    size_t valid_end = chunk.size() > static_cast<size_t>(half) ? chunk.size() - half : 0;
    for (size_t i = processed - start_pcm; i < valid_end; ++i) {
        bb_cache_.push_back(bb_chunk[i]);
    }
}

void V90Phase3AnalogueRx::arm_for_s(size_t ignore_samples,
                                    bool require_full_burst) {
    s_armed_ = true;
    s_detected_ = false;
    s_require_full_burst_ = require_full_burst;
    s_correlation_ = 0.0;
    s_scan_sample_ = samples_.size() + ignore_samples;
    last_analysis_samples_ = samples_.size();
}

void V90Phase3AnalogueRx::arm_for_cpt(size_t ignore_samples) {
    cpt_armed_ = true;
    cpt_detected_ = false;
    cpt_expect_ordinary_ = false;
    cpt_acknowledge_ = false;
    cpt_bits_ = 0;
    cpt_header_seen_ = false;
    cpt_decision_error_ = kPi;
    cpt_parameters_ = V90PcmParameters{};
    cpt_scan_sample_ = samples_.size() + ignore_samples;
    last_analysis_samples_ = samples_.size();
}

void V90Phase3AnalogueRx::arm_for_cp(size_t ignore_samples) {
    cpt_armed_ = true;
    cpt_detected_ = false;
    cpt_expect_ordinary_ = true;
    cpt_acknowledge_ = false;
    cpt_bits_ = 0;
    cpt_header_seen_ = false;
    cpt_decision_error_ = kPi;
    cpt_parameters_ = V90PcmParameters{};
    cpt_scan_sample_ = samples_.size() + ignore_samples;
    last_analysis_samples_ = samples_.size();
}

bool V90Phase3AnalogueRx::try_lock_training() {
    const size_t signal_start = find_signal_start(samples_);
    if (signal_start == std::numeric_limits<size_t>::max()) return false;
    const double sps = kSampleRate / symbol_rate_;
    const size_t prefix_symbols = 432u + (md_length_35ms_ ? 144u : 0u);
    const double expected = static_cast<double>(signal_start) + prefix_symbols * sps +
                            static_cast<double>(md_length_35ms_) * 280.0;
    if (static_cast<double>(samples_.size()) < expected + 280.0 * sps + 30.0) return false;

    const auto ref = training_reference(280);
    double best_score = 0.0, best_at = 0.0, best_carrier = carrier_hz_;
    // INFO1a's probe-offset field is a useful centre when available, while the
    // search still tolerates independent caller clock error and RTP resampling.
    const double indicated = frequency_offset_x002_hz_ == -512 ? 0.0 :
                             0.02 * static_cast<double>(frequency_offset_x002_hz_);
    const std::array<double,3> centres{{0.0, indicated, -indicated}};
    for (size_t ci = 0; ci < centres.size(); ++ci) {
        const double centre = centres[ci];
        bool duplicate = false;
        for (size_t prior = 0; prior < ci; ++prior)
            duplicate = duplicate || std::abs(centre - centres[prior]) < 0.01;
        if (duplicate) continue;
        for (int fi = -24; fi <= 24; ++fi) {
            const double carrier = carrier_hz_ + centre + 0.5 * fi;
            const auto bb = baseband_rrc(samples_, carrier, symbol_rate_);
            for (int qi = -320; qi <= 320; ++qi) {
                const double at = expected + 0.25 * qi;
                if (at < 14.0 || at + 260.0 * sps + 15.0 >= samples_.size()) continue;
                Complex corr(0.0, 0.0);
                double received_energy = 0.0;
                for (size_t n = 24; n < 256; ++n) {
                    const Complex y = interpolate(bb, at + n * sps);
                    corr += y * std::conj(ref.points[n]);
                    received_energy += std::norm(y);
                }
                const double score = received_energy > 1.0e-12 ?
                    std::norm(corr) / (received_energy * 232.0) : 0.0;
                if (score > best_score) {
                    best_score = score;
                    best_at = at;
                    best_carrier = carrier;
                }
            }
        }
    }
    // Random data has an expected normalized correlation near zero. This
    // threshold accepts a moderately distorted analogue path but cannot lock
    // from mere energy or an unrelated tone.
    if (best_score < 0.18) return false;
    training_locked_ = true;
    training_correlation_ = best_score;
    trn_symbol_zero_sample_ = best_at;
    locked_carrier_hz_ = best_carrier;
    locked_samples_per_symbol_ = sps;
    return true;
}

bool V90Phase3AnalogueRx::try_detect_ja() {
    // The Phase-3 TRN interval may approach the four-second procedure bound,
    // which is more than 12,000 symbols at 3200 symbols/s.
    const auto ref = training_reference(20000);
    const double nominal_sps = kSampleRate / symbol_rate_;
    const auto clock = estimate_training_clock(
        bb_cache_, trn_symbol_zero_sample_, nominal_sps, ref.points);
    locked_samples_per_symbol_ = clock.samples_per_symbol;
    symbol_clock_ppm_ = clock.ppm;
    EqualizerModel trained_equalizer;
    auto eq = equalized_symbols(bb_cache_, trn_symbol_zero_sample_,
                                locked_samples_per_symbol_, ref.points,
                                &trained_equalizer);
    if (eq.size() < 512 + 120 + 4) return false;

    const size_t last = std::min<size_t>(eq.size() - 26 - 3,
                                         ref.state_after_symbol.size() - 1);
    for (size_t start = std::max<size_t>(512, last_ja_search_symbol_); start <= last; ++start) {
        last_ja_search_symbol_ = start;
        for (int direction : {-1, 1}) {
            for (bool swap : {false, true}) {
                uint32_t reg = ref.state_after_symbol[start];
                std::vector<uint8_t> bits;
                bits.reserve(2654);
                double phase_error = 0.0;
                std::vector<double> symbol_error;
                symbol_error.reserve(1327);
                for (size_t n = start; n < start + 26; ++n) {
                    const Complex d = eq[n] * std::conj(eq[n - 1]);
                    const double qreal = direction * std::arg(d) / (0.5 * kPi);
                    const int qround = static_cast<int>(std::llround(qreal));
                    const double error = std::abs(qreal - qround) * (0.5 * kPi);
                    phase_error += error;
                    symbol_error.push_back(error);
                    const unsigned q = static_cast<unsigned>(modulo4(qround));
                    uint8_t i1 = static_cast<uint8_t>(q & 1u);
                    uint8_t i2 = static_cast<uint8_t>((q >> 1) & 1u);
                    if (swap) std::swap(i1, i2);
                    bits.push_back(descramble_gpa(reg, i1));
                    bits.push_back(descramble_gpa(reg, i2));
                }
                const size_t descriptor_bits = ja_descriptor_size(bits, 0);
                if (!descriptor_bits) continue;
                const size_t descriptor_symbols = (descriptor_bits + 1u) / 2u;
                if (start + descriptor_symbols + 3u >= eq.size()) continue;
                for (size_t n = start + 26; n < start + descriptor_symbols; ++n) {
                    const Complex d = eq[n] * std::conj(eq[n - 1]);
                    const double qreal = direction * std::arg(d) / (0.5 * kPi);
                    const int qround = static_cast<int>(std::llround(qreal));
                    const double error = std::abs(qreal - qround) * (0.5 * kPi);
                    phase_error += error;
                    symbol_error.push_back(error);
                    const unsigned q = static_cast<unsigned>(modulo4(qround));
                    uint8_t i1 = static_cast<uint8_t>(q & 1u);
                    uint8_t i2 = static_cast<uint8_t>((q >> 1) & 1u);
                    if (swap) std::swap(i1, i2);
                    bits.push_back(descramble_gpa(reg, i1));
                    bits.push_back(descramble_gpa(reg, i2));
                }
                uint8_t dil_segments = 0;
                size_t validated_bits = 0;
                if (complete_ja_descriptor_ok(bits, 0, dil_segments, validated_bits) &&
                    phase_error / descriptor_symbols < 0.65) {
                    ja_detected_ = true;
                    ja_symbol_ = start;
                    ja_descriptor_bits_ = validated_bits;
                    dil_segment_count_ = dil_segments;
                    dil_descriptor_ = parse_ja_descriptor(bits, 0);
                    equalizer_valid_ = trained_equalizer.valid;
                    equalizer_gain_ = trained_equalizer.gain;
                    equalizer_taps_ = trained_equalizer.taps;
                    return true;
                }
            }
        }
    }

    // Robust repeated-Ja path. GPA is self-synchronizing: after 23 correctly
    // received scrambled bits, the receiver no longer depends on its initial
    // register contents. Decode differentially from the end of mandatory TRN,
    // let the descrambler synchronize once the stream actually changes to Ja,
    // and search every subsequent bit for a repeated DIL descriptor. This
    // handles a longer-than-minimum TRN, a lost first Ja transition, or a peer
    // that reinitializes its training scrambler differently without guessing a
    // timer-derived transition symbol.
    for (int direction : {-1, 1}) {
        for (bool swap : {false, true}) {
            uint32_t reg = 0; // deliberately arbitrary; GPA settles in 23 bits
            std::vector<uint8_t> bits;
            std::vector<double> symbol_error;
            bits.reserve(2 * (eq.size() - 512));
            symbol_error.reserve(eq.size() - 512);
            for (size_t n = 512; n + 3 < eq.size(); ++n) {
                const Complex d = eq[n] * std::conj(eq[n - 1]);
                const double qreal = direction * std::arg(d) / (0.5 * kPi);
                const int qround = static_cast<int>(std::llround(qreal));
                symbol_error.push_back(std::abs(qreal - qround) * (0.5 * kPi));
                const unsigned q = static_cast<unsigned>(modulo4(qround));
                uint8_t i1 = static_cast<uint8_t>(q & 1u);
                uint8_t i2 = static_cast<uint8_t>((q >> 1) & 1u);
                if (swap) std::swap(i1, i2);
                bits.push_back(descramble_gpa(reg, i1));
                bits.push_back(descramble_gpa(reg, i2));
            }
            // Do not accept a sync pattern before the 23-bit self-sync depth.
            for (size_t bit = 23; bit + 52 <= bits.size(); ++bit) {
                const size_t descriptor_bits = ja_descriptor_size(bits, bit);
                if (!descriptor_bits || bit + descriptor_bits > bits.size()) continue;
                uint8_t dil_segments = 0;
                size_t validated_bits = 0;
                if (!complete_ja_descriptor_ok(bits, bit, dil_segments,
                                               validated_bits)) continue;
                const size_t first_symbol = bit / 2;
                const size_t last_symbol = std::min(symbol_error.size(),
                    (bit + validated_bits + 1u) / 2u);
                double error = 0.0;
                for (size_t i = first_symbol; i < last_symbol; ++i)
                    error += symbol_error[i];
                const size_t count = last_symbol - first_symbol;
                if (count == 0 || error / count < 0.65) {
                    ja_detected_ = true;
                    ja_symbol_ = 512 + first_symbol;
                    ja_descriptor_bits_ = validated_bits;
                    dil_segment_count_ = dil_segments;
                    dil_descriptor_ = parse_ja_descriptor(bits, bit);
                    equalizer_valid_ = trained_equalizer.valid;
                    equalizer_gain_ = trained_equalizer.gain;
                    equalizer_taps_ = trained_equalizer.taps;
                    return true;
                }
            }
        }
    }
    return false;
}

bool V90Phase3AnalogueRx::try_detect_s() {
    if (!s_armed_ || s_detected_ || samples_.size() <= s_scan_sample_) return false;
    // Analyse only a recent window so repeated Jd-era silence/SCR cannot make
    // this increasingly expensive. Differential phase makes the detector
    // insensitive to the unknown absolute carrier phase and line gain.
    const size_t begin = std::max({s_scan_sample_, last_s_search_sample_,
        samples_.size() > 1600u ? samples_.size() - 1600u : size_t(0)});
    if (samples_.size() - begin < 360u) return false;
    last_s_search_sample_ = begin;
    std::vector<int16_t> recent(samples_.begin() + begin, samples_.end());
    long double pcm_energy = 0.0;
    for (int16_t x : recent) pcm_energy += static_cast<long double>(x) * x;
    const double rms = std::sqrt(static_cast<double>(pcm_energy / recent.size()));
    if (rms < 220.0) return false;

    const double sps = locked_samples_per_symbol_ > 0.0 ?
                       locked_samples_per_symbol_ : kSampleRate / symbol_rate_;
    double best = 0.0;
    // S alternates point 0 and point 0 rotated CCW by 90 degrees. Therefore
    // consecutive differential phases alternate +90/-90 degrees. S-bar has
    // the same differential pattern and a 180-degree absolute rotation; for
    // the first post-Jd gate either polarity is intentionally acceptable.
    for (double timing = 14.0; timing < 14.0 + sps; timing += 0.125) {
        std::vector<Complex> symbols;
        for (double at = begin + timing; at + 15.0 < bb_cache_.size(); at += sps)
            symbols.push_back(interpolate(bb_cache_, at));
        // The first post-Jd gate tolerates damaged burst edges. During DIL the
        // caller is explicitly allowed to transmit random SCR, so short-window
        // coincidences must not terminate DIL prematurely; require nearly the
        // complete specified 128T completion S in that state.
        const std::array<size_t,3> windows = s_require_full_burst_ ?
            std::array<size_t,3>{64u,80u,96u} :
            std::array<size_t,3>{32u,48u,64u};
        for (size_t window : windows) {
            if (symbols.size() < window + 1u) continue;
            for (size_t start = 0; start + window < symbols.size(); start += 2u) {
                Complex correlation(0.0, 0.0);
                size_t used = 0;
                for (size_t k = 1; k <= window; ++k) {
                    const Complex d = symbols[start + k] *
                                      std::conj(symbols[start + k - 1u]);
                    const double magnitude = std::abs(d);
                    if (magnitude < 1.0e-6) continue;
                    const Complex expected = (k & 1u) ? Complex(0.0, 1.0) :
                                                        Complex(0.0, -1.0);
                    correlation += (d / magnitude) * std::conj(expected);
                    ++used;
                }
                if (used >= window * 7u / 8u)
                    best = std::max(best, std::abs(correlation) / used);
            }
        }
    }
    s_correlation_ = best;
    // The splitter/PSTN trace delivers both valid S bursts at 0.58-0.60.
    // Requiring a long 64/80/96T window distinguishes DIL's GPA-scrambled
    // SCR (well below this correlation) without imposing a higher threshold
    // than the earlier Jd S detector on the same physical line.
    const double threshold = 0.55;
    if (best < threshold) return false;
    s_detected_ = true;
    s_armed_ = false;
    return true;
}

bool V90Phase3AnalogueRx::try_detect_cpt() {
    if (!cpt_armed_ || cpt_detected_ || samples_.size() <= cpt_scan_sample_)
        return false;
    // CPt is repeated for as long as four seconds. A recent 1-second window
    // contains several minimum-length CPt frames while bounding the timing
    // search and discarding older Phase-3 S/S-bar/SCR energy.
    const size_t begin = std::max({cpt_scan_sample_, last_cpt_search_sample_,
        samples_.size() > 16000u ? samples_.size() - 16000u : size_t(0)});
    if (samples_.size() - begin < 1200u) return false;
    last_cpt_search_sample_ = begin;

    // CPt ordinary uses uncanceled bb_cache_ directly.
    // However, if we need to cancel Ri, we must compute it.
    std::vector<int16_t> recent_raw(samples_.begin() + begin, samples_.end());
    std::vector<int16_t> recent_cancelled = recent_raw;
    std::array<double,6> ri_mean{};
    std::array<size_t,6> ri_count{};
    for (size_t i = 0; i < recent_raw.size(); ++i) {
        ri_mean[i % 6] += recent_raw[i];
        ri_count[i % 6] += 1;
    }
    for (size_t k = 0; k < 6; ++k)
        if (ri_count[k]) ri_mean[k] /= static_cast<double>(ri_count[k]);
    for (size_t i = 0; i < recent_cancelled.size(); ++i) {
        const double c = static_cast<double>(recent_raw[i]) - ri_mean[i % 6];
        recent_cancelled[i] = static_cast<int16_t>(
            std::lround(std::clamp(c, -32767.0, 32767.0)));
    }

    const double sps = locked_samples_per_symbol_ > 0.0 ?
                       locked_samples_per_symbol_ : kSampleRate / symbol_rate_;
    
    // If not expecting ordinary CPt, we must re-run baseband_rrc on the cancelled signal
    const auto bb = cpt_expect_ordinary_ ?
        std::vector<Complex>(bb_cache_.begin() + begin, bb_cache_.end()) :
        baseband_rrc(recent_cancelled, locked_carrier_hz_, symbol_rate_, begin);
    
    for (double timing = 14.0; timing < 14.0 + sps; timing += 0.25) {
        std::vector<Complex> raw;
        for (double at = timing; at + 15.0 < bb.size(); at += sps)
            raw.push_back(interpolate(bb, at));
        if (raw.size() < 180u) continue;

        EqualizerModel retained;
        retained.gain = equalizer_gain_;
        retained.taps = equalizer_taps_;
        retained.valid = equalizer_valid_;
        auto retained_symbols = apply_equalizer(raw, retained);
        auto adapted_symbols = adapt_qpsk_equalizer(raw, retained);

        const std::array<const std::vector<Complex>*, 3> symbol_candidates{{
            &retained_symbols, &raw, &adapted_symbols}};
        for (const auto* candidate_symbols : symbol_candidates) {
            const auto& symbols = *candidate_symbols;
            if (symbols.size() < 180u) continue;
            for (int direction : {-1, 1}) {
                for (bool swap : {false, true}) {
                    Complex d4_sum(0.0, 0.0);
                    for (size_t n = 1; n < symbols.size(); ++n) {
                        const Complex d = symbols[n] * std::conj(symbols[n - 1u]);
                        const double mag = std::abs(d);
                        if (mag >= 1.0e-6) {
                            const Complex u = d / mag;
                            d4_sum += u * u * u * u;
                        }
                    }
                    const double delta_carrier = std::abs(d4_sum) > 1.0e-4 ?
                        (std::arg(d4_sum) / 4.0) : 0.0;

                    for (int rot : {0, 1, 2, 3}) {
                        uint32_t reg = 0; // GPA self-synchronizes after 23 input bits
                        std::vector<uint8_t> bits;
                        std::vector<double> errors;
                        bits.reserve(2u * symbols.size());
                        errors.reserve(symbols.size());
                        for (size_t n = 1; n < symbols.size(); ++n) {
                            const Complex d = symbols[n] * std::conj(symbols[n - 1u]);
                            if (std::abs(d) < 1.0e-8) {
                                bits.push_back(descramble_gpa(reg, 0));
                                bits.push_back(descramble_gpa(reg, 0));
                                errors.push_back(kPi);
                                continue;
                            }
                            double raw_arg = std::arg(d) - delta_carrier;
                            if (raw_arg > kPi) raw_arg -= 2.0 * kPi;
                            else if (raw_arg < -kPi) raw_arg += 2.0 * kPi;
                            const double qreal = direction * raw_arg / (0.5 * kPi);
                            const int qround = static_cast<int>(std::llround(qreal));
                            errors.push_back(std::abs(qreal - qround) * (0.5 * kPi));
                            const unsigned q = static_cast<unsigned>(modulo4(qround + rot));
                            uint8_t i1 = static_cast<uint8_t>(q & 1u);
                            uint8_t i2 = static_cast<uint8_t>((q >> 1) & 1u);
                            if (swap) std::swap(i1, i2);
                            bits.push_back(descramble_gpa(reg, i1));
                            bits.push_back(descramble_gpa(reg, i2));
                        }

                        for (size_t bit = 0u; bit + 280u <= bits.size(); ++bit) {
                            size_t sync_len = 17u;
                            const size_t declared_length =
                                cpt_sequence_size(bits, bit, cpt_expect_ordinary_, &sync_len);
                            if (!declared_length) continue;

                            cpt_header_seen_ = true;
                            const size_t first_symbol = bit / 2u;
                            const size_t available_bits = std::min(
                                declared_length, bits.size() - bit);
                            const size_t last_symbol = std::min(errors.size(),
                                (bit + available_bits + 1u) / 2u);
                            if (last_symbol > first_symbol) {
                                double mean_error = 0.0;
                                for (size_t i = first_symbol; i < last_symbol; ++i)
                                    mean_error += errors[i];
                                cpt_decision_error_ = std::min(
                                    cpt_decision_error_,
                                    mean_error / (last_symbol - first_symbol));
                            }

                            // 1. Try single-frame detection (check short fill first if fill bits indicate next frame)
                            V90PcmParameters parameters;
                            size_t length = 0;
                            bool acknowledge = false;
                            bool frame_ok = false;
                            const size_t d = declared_length - (sync_len + 275u);
                            const size_t crc_start = sync_len + 255u + d;
                            const bool next_frame_sync = (bit + crc_start + 18u < bits.size() && bits[bit + crc_start + 18u] == 1u);

                            if (next_frame_sync) {
                                if (complete_cpt_sequence_ok(bits, bit, parameters, length,
                                                             true, cpt_expect_ordinary_,
                                                             &acknowledge)) {
                                    frame_ok = true;
                                } else if (complete_cpt_sequence_ok(bits, bit, parameters, length,
                                                                    false, cpt_expect_ordinary_,
                                                                    &acknowledge)) {
                                    frame_ok = true;
                                }
                            } else {
                                if (complete_cpt_sequence_ok(bits, bit, parameters, length,
                                                             false, cpt_expect_ordinary_,
                                                             &acknowledge)) {
                                    frame_ok = true;
                                } else if (complete_cpt_sequence_ok(bits, bit, parameters, length,
                                                                    true, cpt_expect_ordinary_,
                                                                    &acknowledge)) {
                                    frame_ok = true;
                                }
                            }

                            // 2. Try repetition combiner if single frame had noise/bit flips
                            if (!frame_ok) {
                                for (bool sf : {next_frame_sync, !next_frame_sync}) {
                                    const size_t fbits = declared_length - (sf ? 2u : 0u);
                                    const size_t copies = (bits.size() - bit) / fbits;
                                    if (copies < 2u) continue;
                                    std::vector<uint8_t> combined(fbits, 0u);
                                    for (size_t k = 0; k < fbits; ++k) {
                                        size_t ones = 0;
                                        for (size_t copy = 0; copy < copies; ++copy)
                                            ones += bits[bit + copy * fbits + k] != 0u;
                                        combined[k] = static_cast<uint8_t>(2u * ones >= copies);
                                    }
                                    if (complete_cpt_sequence_ok(combined, 0u, parameters,
                                                                 length, sf,
                                                                 cpt_expect_ordinary_,
                                                                 &acknowledge) &&
                                        length == fbits) {
                                        frame_ok = true;
                                        break;
                                    }
                                }
                            }

                            if (!frame_ok) continue;

                            const size_t first_sym = bit / 2u;
                            const size_t last_sym = std::min(errors.size(),
                                (bit + length + 1u) / 2u);
                            if (last_sym > first_sym) {
                                double phase_error = 0.0;
                                for (size_t i = first_sym; i < last_sym; ++i)
                                    phase_error += errors[i];
                                if (phase_error / (last_sym - first_sym) >= 0.55)
                                    continue;
                            }
                            cpt_detected_ = true;
                            cpt_armed_ = false;
                            cpt_acknowledge_ = acknowledge;
                            cpt_bits_ = length;
                            cpt_parameters_ = std::move(parameters);
                            return true;
                        }
                    }
                }
            }
        }
    }
    return false;
}

std::vector<int16_t> v90_phase3_analogue_test_waveform(
    uint8_t symbol_rate_index,
    uint8_t md_length_35ms,
    double amplitude,
    double carrier_offset_hz,
    bool reset_scrambler_at_ja,
    uint8_t dil_segment_count,
    size_t training_symbols,
    double symbol_clock_ppm) {
    const unsigned rate = symbol_rate(symbol_rate_index);
    if (!rate || amplitude <= 0.0) return {};
    const double sps = kSampleRate /
        (rate * (1.0 + symbol_clock_ppm * 1.0e-6));
    std::vector<Complex> symbols;
    symbols.reserve(1100);
    for (size_t n = 0; n < 144; ++n)
        symbols.push_back(qpsk_clockwise((n & 1u) ? 3u : 0u));
    if (md_length_35ms) {
        const size_t md_symbols = static_cast<size_t>(std::llround(
            md_length_35ms * 0.035 * rate));
        for (size_t n = 0; n < md_symbols; ++n)
            symbols.push_back(qpsk_clockwise(static_cast<unsigned>(n) & 3u));
        for (size_t n = 0; n < 144; ++n)
            symbols.push_back(qpsk_clockwise((n & 1u) ? 3u : 0u));
    }
    for (size_t i = 0; i < 288; ++i) {
        const size_t k = i / 4, ii = i % 4;
        const double phase = kPi * ((k % 3 == 1) ? (k * ii + 4.0) : k * ii) / 6.0;
        symbols.emplace_back(std::cos(phase), std::sin(phase));
    }

    uint32_t reg = 0;
    unsigned zprev = 0;
    training_symbols = std::max<size_t>(512, training_symbols);
    for (size_t n = 0; n < training_symbols; ++n) {
        const unsigned i1 = scramble_gpa(reg, 1);
        const unsigned i2 = scramble_gpa(reg, 1);
        zprev = 2u * i2 + i1;
        symbols.push_back(qpsk_clockwise(zprev));
    }
    const std::vector<uint8_t> descriptor = test_ja_descriptor(dil_segment_count);
    if (descriptor.empty()) return {};
    std::vector<uint8_t> ja=descriptor;
    ja.insert(ja.end(),descriptor.begin(),descriptor.end()); // Ja is repeated
    ja.insert(ja.end(),descriptor.begin(),descriptor.end());
    if(reset_scrambler_at_ja)reg=0;
    for (size_t b = 0; b + 1 < ja.size(); b += 2) {
        const unsigned i1 = scramble_gpa(reg, ja[b]);
        const unsigned i2 = scramble_gpa(reg, ja[b + 1]);
        zprev = (zprev + 2u * i2 + i1) & 3u;
        symbols.push_back(qpsk_clockwise(zprev));
    }

    const size_t silence = 560;
    const size_t ns = silence + static_cast<size_t>(std::ceil(symbols.size() * sps)) + 40;
    std::vector<int16_t> out(ns, 0);
    const double carrier = low_carrier(rate) + carrier_offset_hz;
    for (size_t n = silence; n < ns; ++n) {
        const double relative = static_cast<double>(n - silence);
        const long centre = static_cast<long>(std::floor(relative / sps));
        Complex shaped(0.0, 0.0);
        for (long k = centre - 8; k <= centre + 8; ++k) {
            if (k < 0 || static_cast<size_t>(k) >= symbols.size()) continue;
            shaped += symbols[static_cast<size_t>(k)] *
                      rrc((relative - k * sps) / sps, 0.12);
        }
        const double phase = 2.0 * kPi * carrier * static_cast<double>(n) / kSampleRate;
        const double y = amplitude * std::real(shaped * Complex(std::cos(phase), std::sin(phase)));
        out[n] = static_cast<int16_t>(std::lround(std::clamp(y, -32767.0, 32767.0)));
    }
    return out;
}

std::vector<int16_t> v90_phase3_analogue_s_test_waveform(
    uint8_t symbol_rate_index,
    size_t count,
    double amplitude,
    double carrier_offset_hz) {
    const unsigned rate = symbol_rate(symbol_rate_index);
    if (!rate || count < 64u || amplitude <= 0.0) return {};
    const double sps = kSampleRate / rate;
    std::vector<Complex> symbols;
    symbols.reserve(count);
    for (size_t i = 0; i < count; ++i)
        symbols.push_back(qpsk_clockwise((i & 1u) ? 3u : 0u));
    const size_t silence = 160u;
    const size_t ns = silence + static_cast<size_t>(std::ceil(count * sps)) + 40u;
    std::vector<int16_t> out(ns, 0);
    const double carrier = low_carrier(rate) + carrier_offset_hz;
    for (size_t n = silence; n < ns; ++n) {
        const double relative = static_cast<double>(n - silence);
        const long centre = static_cast<long>(std::floor(relative / sps));
        Complex shaped(0.0, 0.0);
        for (long k = centre - 8; k <= centre + 8; ++k) {
            if (k < 0 || static_cast<size_t>(k) >= symbols.size()) continue;
            shaped += symbols[static_cast<size_t>(k)] *
                      rrc((relative - k * sps) / sps, 0.12);
        }
        const double phase = 2.0 * kPi * carrier * static_cast<double>(n) / kSampleRate;
        const double y = amplitude * std::real(
            shaped * Complex(std::cos(phase), std::sin(phase)));
        out[n] = static_cast<int16_t>(std::lround(
            std::clamp(y, -32767.0, 32767.0)));
    }
    return out;
}

std::vector<int16_t> v90_phase4_cpt_test_waveform(
    uint8_t symbol_rate_index,
    double amplitude,
    double carrier_offset_hz,
    unsigned repeats,
    uint8_t max_constellation_index,
    bool separate_codec_constellations,
    bool ordinary_cp,
    bool short_fill,
    bool acknowledge) {
    const unsigned rate = symbol_rate(symbol_rate_index);
    if (!rate || amplitude <= 0.0 || repeats < 2u ||
        max_constellation_index > 5u) return {};

    // Valid mu-law CPt: 16 kbit/s Phase-4 signalling, S=6/K=6, no
    // look-ahead or spectral filter. max=0 creates the compact 292-bit case;
    // max=5 plus separate codec constellations creates the 1788-bit case
    // emitted by peers which describe every legal constellation collection.
    const size_t g = 136u * max_constellation_index;
    const size_t d = separate_codec_constellations ? 2u * g + 136u : g;
    const size_t crc_start = 272u + d;
    std::vector<uint8_t> cpt(292u + d, 0u);
    std::fill(cpt.begin(), cpt.begin() + 17, 1u);
    // V.90 Table 14: zero denotes the Phase-4 training parameter sequence
    // CPt; one denotes ordinary CP used after MP/rate negotiation.
    cpt[19u] = ordinary_cp ? 1u : 0u;
    cpt[33u] = acknowledge ? 1u : 0u;
    auto put_lsb = [&](size_t at, unsigned count, unsigned value) {
        for (unsigned i = 0; i < count; ++i)
            cpt[at + i] = static_cast<uint8_t>((value >> i) & 1u);
    };
    put_lsb(20u, 5u, 4u); // (drn+8)*8000/6 = 16000 bit/s
    for (size_t frame = 0; frame < 6u; ++frame)
        put_lsb(103u + 4u * frame, 4u,
                std::min<unsigned>(frame, max_constellation_index));
    cpt[128u] = separate_codec_constellations ? 1u : 0u;
    auto populate_family = [&](size_t family_base, unsigned offset_ucode) {
        for (unsigned index = 0; index <= max_constellation_index; ++index) {
            const size_t base = family_base + 136u * index;
            for (unsigned ucode = 8u + offset_ucode;
                 ucode < 128u; ucode += 8u) {
                const size_t chord = ucode / 16u;
                const size_t within = ucode % 16u;
                cpt[base + 17u * chord + 1u + within] = 1u;
            }
        }
    };
    populate_family(136u, 0u);
    // Deliberately make the post-codec family distinct.  This turns the
    // waveform into a regression for accidentally driving TRN2d from the
    // D/A-output masks instead of the transmitter masks.
    if (separate_codec_constellations) populate_family(272u + g, 1u);
    const uint16_t crc = v34_info_crc(cpt_crc_information(cpt, 0u, 17u, crc_start));
    for (unsigned i = 0; i < 16u; ++i)
        cpt[crc_start + 1u + i] = static_cast<uint8_t>((crc >> (15u - i)) & 1u);
    if (short_fill) cpt.resize(cpt.size() - 2u);

    std::vector<uint8_t> all_bits;
    all_bits.reserve(cpt.size() * repeats);
    for (unsigned i = 0; i < repeats; ++i)
        all_bits.insert(all_bits.end(), cpt.begin(), cpt.end());

    uint32_t reg = 0;
    unsigned zprev = 0;
    std::vector<Complex> symbols;
    symbols.reserve(all_bits.size() / 2u);
    for (size_t b = 0; b + 1u < all_bits.size(); b += 2u) {
        const unsigned i1 = scramble_gpa(reg, all_bits[b]);
        const unsigned i2 = scramble_gpa(reg, all_bits[b + 1u]);
        zprev = (zprev + 2u * i2 + i1) & 3u;
        symbols.push_back(qpsk_clockwise(zprev));
    }

    const double sps = kSampleRate / rate;
    const size_t silence = 160u;
    const size_t ns = silence +
        static_cast<size_t>(std::ceil(symbols.size() * sps)) + 40u;
    std::vector<int16_t> out(ns, 0);
    const double carrier = low_carrier(rate) + carrier_offset_hz;
    for (size_t n = silence; n < ns; ++n) {
        const double relative = static_cast<double>(n - silence);
        const long centre = static_cast<long>(std::floor(relative / sps));
        Complex shaped(0.0, 0.0);
        for (long k = centre - 8; k <= centre + 8; ++k) {
            if (k < 0 || static_cast<size_t>(k) >= symbols.size()) continue;
            shaped += symbols[static_cast<size_t>(k)] *
                      rrc((relative - k * sps) / sps, 0.12);
        }
        const double phase = 2.0 * kPi * carrier * static_cast<double>(n) /
                             kSampleRate;
        const double y = amplitude * std::real(
            shaped * Complex(std::cos(phase), std::sin(phase)));
        out[n] = static_cast<int16_t>(std::lround(
            std::clamp(y, -32767.0, 32767.0)));
    }
    return out;
}

std::vector<uint8_t> V90Phase3AnalogueRx::demodulate_data(const std::vector<int16_t>& pcm) {
    if (!training_locked_ || symbol_rate_ == 0 || pcm.empty()) return {};
    if (!v34_data_demod_) {
        v34_data_demod_ = std::make_unique<V34QamDemodulator>(symbol_rate_, 4);
    }
    return v34_data_demod_->process_pcm(pcm);
}

} // namespace v92
