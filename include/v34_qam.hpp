#pragma once

#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <memory>
#include <vector>

namespace v92 {

using Complex = std::complex<double>;

// 2D Constellation Subset (A, B, C, D) partition of 2Z^2 grid
enum class V34Subset2D : uint8_t {
    A = 0,
    B = 1,
    C = 2,
    D = 3
};

// 4D Coset Index Lambda_0 .. Lambda_7
enum class V34Coset4D : uint8_t {
    Lambda0 = 0,
    Lambda1 = 1,
    Lambda2 = 2,
    Lambda3 = 3,
    Lambda4 = 4,
    Lambda5 = 5,
    Lambda6 = 6,
    Lambda7 = 7
};

// V.34 Constellation Engine: Generates multi-level 16-QAM, 32-QAM, 64-QAM, 128-QAM, 256-QAM
// and partitions points into 2D subsets and 4D cosets per ITU-T V.34 Section 9.
class V34Constellation {
public:
    explicit V34Constellation(unsigned bits_per_2d_symbol = 4); // 4=16QAM, 6=64QAM, 8=256QAM

    unsigned bits_per_2d() const { return bits_per_2d_; }
    unsigned points_count() const { return points_.size(); }
    const std::vector<Complex>& points() const { return points_; }

    // Subset of a 2D point (A, B, C, D)
    static V34Subset2D subset_of_point(int u, int v);

    // Nearest point in a given 2D subset to received complex sample r
    Complex nearest_in_subset(V34Subset2D subset, const Complex& r, unsigned* point_index_out = nullptr) const;

    // Euclidean distance squared to nearest point in subset
    double distance_sq_to_subset(V34Subset2D subset, const Complex& r, Complex* best_point = nullptr) const;

    // 4D Branch metric for Lambda_0..Lambda_7 given received 2D symbol pair (r1, r2)
    void compute_4d_branch_metrics(const Complex& r1, const Complex& r2,
                                   std::array<double, 8>& branch_metrics,
                                   std::array<std::pair<Complex, Complex>, 8>& best_points) const;

    // Map uncoded and coded bits to 4D symbol pair (p1, p2)
    std::pair<Complex, Complex> map_4d(V34Coset4D coset, uint32_t uncoded_bits) const;

    // Demap 4D point pair back to uncoded bits
    uint32_t demap_uncoded(V34Coset4D coset, const Complex& p1, const Complex& p2) const;

private:
    unsigned bits_per_2d_ = 4;
    double scale_ = 1.0;
    std::vector<Complex> points_;
    std::array<std::vector<Complex>, 4> subset_points_;
};

// 16-State 4D Wei Convolutional Trellis Encoder (ITU-T V.34 Section 9.4, Table 6/V.34)
class V34TrellisEncoder {
public:
    V34TrellisEncoder();
    void reset();

    // Encode 2 input information bits (Y0, Y1) -> returns 3-bit 4D coset index (0..7)
    V34Coset4D encode(uint8_t y0, uint8_t y1);

    uint8_t state() const { return state_; }

private:
    uint8_t state_ = 0; // 4-bit state (0..15)
};

// Soft-Decision 16-State 4D Viterbi Decoder
class V34ViterbiDecoder {
public:
    explicit V34ViterbiDecoder(size_t traceback_depth = 12);
    void reset();

    // Process a received 4D branch metric vector (8 cosets) and associated uncoded bits
    // Returns decoded bits when traceback depth is reached
    struct Decoded4D {
        bool valid = false;
        uint8_t y0 = 0;
        uint8_t y1 = 0;
        uint32_t uncoded_bits = 0;
    };

    Decoded4D update(const std::array<double, 8>& branch_metrics,
                     const std::array<uint32_t, 8>& uncoded_bits_per_coset);

private:
    struct HistoryNode {
        std::array<uint8_t, 16> prev_state{};
        std::array<uint8_t, 16> coset{};
        std::array<uint32_t, 16> uncoded_bits{};
    };

    size_t traceback_depth_ = 32;
    std::array<double, 16> path_metrics_{};
    std::vector<HistoryNode> history_;
    size_t history_head_ = 0;
    size_t symbols_fed_ = 0;
};

// V.34 Calling Modem Scrambler / Descrambler (GPA: 1 + x^-5 + x^-23)
class V34GpaScrambler {
public:
    void reset() { reg_ = 0; }
    uint8_t scramble(uint8_t in_bit) {
        const uint8_t out = static_cast<uint8_t>((in_bit & 1u) ^
                                                 ((reg_ >> 4) & 1u) ^
                                                 ((reg_ >> 22) & 1u));
        reg_ = ((reg_ << 1) | (out & 1u)) & 0x7FFFFFu;
        return out;
    }
    uint32_t state() const { return reg_; }

private:
    uint32_t reg_ = 0;
};

class V34GpaDescrambler {
public:
    void reset() { reg_ = 0; }
    uint8_t descramble(uint8_t in_bit) {
        const uint8_t out = static_cast<uint8_t>((in_bit & 1u) ^
                                                 ((reg_ >> 4) & 1u) ^
                                                 ((reg_ >> 22) & 1u));
        reg_ = ((reg_ << 1) | (in_bit & 1u)) & 0x7FFFFFu;
        return out;
    }
    uint32_t state() const { return reg_; }

private:
    uint32_t reg_ = 0;
};

// Reference V.34 QAM Transmitter Modulator (for end-to-end waveform generation & testing)
class V34QamModulator {
public:
    V34QamModulator(unsigned symbol_rate = 3200, unsigned bits_per_2d = 4, double amplitude = 5000.0);

    unsigned symbol_rate() const { return symbol_rate_; }
    double carrier_hz() const { return carrier_hz_; }

    // Feed raw data bytes (8-N-1 UART framed), scramble, 4D trellis encode, modulate to 8kHz PCM audio
    std::vector<int16_t> modulate_bytes(const std::vector<uint8_t>& bytes, size_t pad_symbols = 64);

private:
    unsigned symbol_rate_ = 3200;
    unsigned bits_per_2d_ = 4;
    double carrier_hz_ = 1828.57;
    double amplitude_ = 5000.0;
    V34Constellation constellation_;
    V34TrellisEncoder trellis_;
    V34GpaScrambler scrambler_;
};

// Full V.34 QAM Receiver DSP (RRC matched filtering, carrier recovery, timing recovery, 4D Viterbi decoding, deframing)
class V34QamDemodulator {
public:
    V34QamDemodulator(unsigned symbol_rate = 3200, unsigned bits_per_2d = 4);
    void reset();

    // Process incoming 8kHz PCM samples from RTP/PSTN and return recovered data bytes
    std::vector<uint8_t> process_pcm(const std::vector<int16_t>& pcm);

    bool is_locked() const { return carrier_locked_ && timing_locked_; }
    double carrier_offset_hz() const { return carrier_offset_hz_; }

private:
    unsigned symbol_rate_ = 3200;
    unsigned bits_per_2d_ = 4;
    double nominal_carrier_hz_ = 1828.57;
    double sps_ = 2.5; // 8000 / 3200

    V34Constellation constellation_;
    V34ViterbiDecoder viterbi_;
    V34GpaDescrambler descrambler_;

    std::vector<int16_t> pcm_buf_;
    unsigned long long absolute_pcm_samples_processed_ = 0;
    bool carrier_locked_ = false;
    bool timing_locked_ = false;
    double carrier_phase_ = 0.0;
    double carrier_offset_hz_ = 0.0;
    double timing_phase_ = 0.0;

    // 4D symbol pair assembly
    bool has_first_2d_ = false;
    Complex first_2d_symbol_{0.0, 0.0};

    // Bitstream and async 8-N-1 deframer
    std::vector<uint8_t> bit_buf_;
    bool synced_8n1_ = false;
    size_t bit_offset_8n1_ = 0;
    size_t bytes_emitted_ = 0;
};

} // namespace v92
