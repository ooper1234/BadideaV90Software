#include "v34_qam.hpp"

#include <algorithm>
#include <iostream>
#include <cmath>
#include <limits>

namespace v92 {
namespace {

constexpr double kPi = 3.1415926535897932384626433832795;
constexpr double kSampleRate = 8000.0;

double rrc_pulse(double t, double beta) {
    if (std::abs(t) < 1.0e-10)
        return 1.0 + beta * (4.0 / kPi - 1.0);
    const double singular = 1.0 / (4.0 * beta);
    if (std::abs(std::abs(t) - singular) < 1.0e-8) {
        const double a = (1.0 + 2.0 / kPi) * std::sin(kPi / (4.0 * beta));
        const double b = (1.0 - 2.0 / kPi) * std::cos(kPi / (4.0 * beta));
        return beta * (a + b) / std::sqrt(2.0);
    }
    const double pi_t = kPi * t;
    const double denom = 1.0 - 16.0 * beta * beta * t * t;
    return (std::sin(pi_t * (1.0 - beta)) + 4.0 * beta * t * std::cos(pi_t * (1.0 + beta))) / (pi_t * denom);
}

inline double exact_symbol_rate(unsigned rate) {
    if (rate == 3429) return 24000.0 / 7.0;
    return static_cast<double>(rate);
}

inline double exact_carrier_hz(unsigned rate) {
    if (rate == 3000) return 1800.0;
    if (rate == 3429) return (24000.0 / 7.0) * (4.0 / 7.0);
    return 3200.0 * 4.0 / 7.0;
}

inline double exact_samples_per_symbol(unsigned rate) {
    return kSampleRate / exact_symbol_rate(rate);
}

std::vector<Complex> filter_baseband_rrc(const std::vector<int16_t>& pcm,
                                         double carrier,
                                         unsigned rate,
                                         double beta,
                                         unsigned long long start_n) {
    std::vector<Complex> mixed(pcm.size()), out(pcm.size());
    for (size_t n = 0; n < pcm.size(); ++n) {
        const double phase = -2.0 * kPi * carrier * static_cast<double>(start_n + n) / kSampleRate;
        mixed[n] = static_cast<double>(pcm[n]) * Complex(std::cos(phase), std::sin(phase));
    }
    constexpr int half = 25;
    double norm = 0.0;
    std::array<double, 2 * half + 1> h{};
    const double sym_rate = exact_symbol_rate(rate);
    for (int k = -half; k <= half; ++k) {
        h[static_cast<size_t>(k + half)] =
            rrc_pulse(static_cast<double>(k) * sym_rate / kSampleRate, beta);
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

Complex interpolate_sample(const std::vector<Complex>& v, double at) {
    if (at < 0.0 || at + 1.0 >= static_cast<double>(v.size())) return {};
    const size_t i = static_cast<size_t>(std::floor(at));
    const double f = at - static_cast<double>(i);
    return v[i] * (1.0 - f) + v[i + 1] * f;
}

// 4D Coset partition definitions: each coset Lambda_m maps to two 2D subset pairs
struct CosetSubsets {
    V34Subset2D s1_a, s2_a;
    V34Subset2D s1_b, s2_b;
};

const CosetSubsets kCosetMap[8] = {
    {V34Subset2D::A, V34Subset2D::A, V34Subset2D::C, V34Subset2D::C}, // Lambda0
    {V34Subset2D::B, V34Subset2D::B, V34Subset2D::D, V34Subset2D::D}, // Lambda1
    {V34Subset2D::A, V34Subset2D::C, V34Subset2D::C, V34Subset2D::A}, // Lambda2
    {V34Subset2D::B, V34Subset2D::D, V34Subset2D::D, V34Subset2D::B}, // Lambda3
    {V34Subset2D::A, V34Subset2D::B, V34Subset2D::C, V34Subset2D::D}, // Lambda4
    {V34Subset2D::B, V34Subset2D::A, V34Subset2D::D, V34Subset2D::C}, // Lambda5
    {V34Subset2D::A, V34Subset2D::D, V34Subset2D::C, V34Subset2D::B}, // Lambda6
    {V34Subset2D::B, V34Subset2D::C, V34Subset2D::D, V34Subset2D::A}  // Lambda7
};

} // namespace

// ----------------------------------------------------------------------------
// V34Constellation
// ----------------------------------------------------------------------------

V34Constellation::V34Constellation(unsigned bits_per_2d_symbol)
    : bits_per_2d_(bits_per_2d_symbol) {
    // Generate rectangular grid for 2^bits_per_2d points
    int m_max = 2; // default for 16-QAM (4x4)
    if (bits_per_2d_ == 6) m_max = 4; // 64-QAM (8x8)
    else if (bits_per_2d_ == 8) m_max = 8; // 256-QAM (16x16)

    points_.reserve(4 * m_max * m_max);
    double total_energy = 0.0;
    for (int u = -m_max; u < m_max; ++u) {
        for (int v = -m_max; v < m_max; ++v) {
            const double x = 2.0 * u + 1.0;
            const double y = 2.0 * v + 1.0;
            const Complex pt(x, y);
            points_.push_back(pt);
            total_energy += std::norm(pt);

            const V34Subset2D s = subset_of_point(u, v);
            subset_points_[static_cast<size_t>(s)].push_back(pt);
        }
    }
    // Normalize constellation so average symbol power = 1.0
    scale_ = std::sqrt(total_energy / points_.size());
    for (auto& pt : points_) pt /= scale_;
    for (auto& list : subset_points_)
        for (auto& pt : list) pt /= scale_;
}

V34Subset2D V34Constellation::subset_of_point(int u, int v) {
    const bool sum_even = ((u + v) & 1) == 0;
    const bool u_even = (u & 1) == 0;
    if (sum_even && u_even) return V34Subset2D::A;
    if (!sum_even && !u_even) return V34Subset2D::B;
    if (sum_even && !u_even) return V34Subset2D::C;
    return V34Subset2D::D;
}

Complex V34Constellation::nearest_in_subset(V34Subset2D subset, const Complex& r, unsigned* point_index_out) const {
    const auto& list = subset_points_[static_cast<size_t>(subset)];
    double min_dist = std::numeric_limits<double>::max();
    Complex best(0.0, 0.0);
    unsigned best_idx = 0;
    for (size_t i = 0; i < list.size(); ++i) {
        const double d = std::norm(r - list[i]);
        if (d < min_dist) {
            min_dist = d;
            best = list[i];
            best_idx = static_cast<unsigned>(i);
        }
    }
    if (point_index_out) *point_index_out = best_idx;
    return best;
}

double V34Constellation::distance_sq_to_subset(V34Subset2D subset, const Complex& r, Complex* best_point) const {
    const auto& list = subset_points_[static_cast<size_t>(subset)];
    double min_dist = std::numeric_limits<double>::max();
    Complex best(0.0, 0.0);
    for (const auto& pt : list) {
        const double d = std::norm(r - pt);
        if (d < min_dist) {
            min_dist = d;
            best = pt;
        }
    }
    if (best_point) *best_point = best;
    return min_dist;
}

void V34Constellation::compute_4d_branch_metrics(
    const Complex& r1, const Complex& r2,
    std::array<double, 8>& branch_metrics,
    std::array<std::pair<Complex, Complex>, 8>& best_points) const {
    
    std::array<Complex, 4> pt1, pt2;
    std::array<double, 4> d1, d2;
    for (size_t s = 0; s < 4; ++s) {
        d1[s] = distance_sq_to_subset(static_cast<V34Subset2D>(s), r1, &pt1[s]);
        d2[s] = distance_sq_to_subset(static_cast<V34Subset2D>(s), r2, &pt2[s]);
    }

    for (size_t m = 0; m < 8; ++m) {
        const auto& map = kCosetMap[m];
        const double cost_a = d1[static_cast<size_t>(map.s1_a)] + d2[static_cast<size_t>(map.s2_a)];
        const double cost_b = d1[static_cast<size_t>(map.s1_b)] + d2[static_cast<size_t>(map.s2_b)];
        if (cost_a <= cost_b) {
            branch_metrics[m] = cost_a;
            best_points[m] = {pt1[static_cast<size_t>(map.s1_a)], pt2[static_cast<size_t>(map.s2_a)]};
        } else {
            branch_metrics[m] = cost_b;
            best_points[m] = {pt1[static_cast<size_t>(map.s1_b)], pt2[static_cast<size_t>(map.s2_b)]};
        }
    }
}

std::pair<Complex, Complex> V34Constellation::map_4d(V34Coset4D coset, uint32_t uncoded_bits) const {
    const auto& map = kCosetMap[static_cast<size_t>(coset)];
    const bool select_pair_b = (uncoded_bits & 1u) != 0;
    uncoded_bits >>= 1;

    const V34Subset2D s1 = select_pair_b ? map.s1_b : map.s1_a;
    const V34Subset2D s2 = select_pair_b ? map.s2_b : map.s2_a;

    const auto& list1 = subset_points_[static_cast<size_t>(s1)];
    const auto& list2 = subset_points_[static_cast<size_t>(s2)];

    const unsigned pts_per_subset = list1.size();
    const unsigned idx1 = uncoded_bits % pts_per_subset;
    uncoded_bits /= pts_per_subset;
    const unsigned idx2 = uncoded_bits % pts_per_subset;

    return {list1[idx1], list2[idx2]};
}

uint32_t V34Constellation::demap_uncoded(V34Coset4D coset, const Complex& p1, const Complex& p2) const {
    const auto& map = kCosetMap[static_cast<size_t>(coset)];
    unsigned idx1 = 0, idx2 = 0;
    
    // Check if (p1, p2) belongs to pair_a or pair_b
    double dist_a = distance_sq_to_subset(map.s1_a, p1) + distance_sq_to_subset(map.s2_a, p2);
    double dist_b = distance_sq_to_subset(map.s1_b, p1) + distance_sq_to_subset(map.s2_b, p2);

    bool pair_b = dist_b < dist_a;
    V34Subset2D s1 = pair_b ? map.s1_b : map.s1_a;
    V34Subset2D s2 = pair_b ? map.s2_b : map.s2_a;

    nearest_in_subset(s1, p1, &idx1);
    nearest_in_subset(s2, p2, &idx2);

    const unsigned pts_per_subset = subset_points_[static_cast<size_t>(s1)].size();
    uint32_t uncoded = (pair_b ? 1u : 0u);
    uncoded |= (idx1 << 1);
    uncoded |= (idx2 << (1 + static_cast<unsigned>(std::log2(pts_per_subset))));
    return uncoded;
}

// ----------------------------------------------------------------------------
// V34TrellisEncoder
// ----------------------------------------------------------------------------

V34TrellisEncoder::V34TrellisEncoder() {
    reset();
}

void V34TrellisEncoder::reset() {
    state_ = 0;
}

V34Coset4D V34TrellisEncoder::encode(uint8_t y0, uint8_t y1) {
    const uint8_t s0 = state_ & 1u;
    const uint8_t s1 = (state_ >> 1) & 1u;
    const uint8_t s2 = (state_ >> 2) & 1u;
    const uint8_t s3 = (state_ >> 3) & 1u;

    const uint8_t c0 = y0 & 1u;
    const uint8_t c1 = y1 & 1u;
    const uint8_t c2 = (s0 ^ s1 ^ s2 ^ s3) & 1u;
    const uint8_t coset_idx = (c2 << 2) | (c1 << 1) | c0;

    const uint8_t next_s0 = (y0 ^ s2) & 1u;
    const uint8_t next_s1 = (y1 ^ s3) & 1u;
    const uint8_t next_s2 = s0;
    const uint8_t next_s3 = s1;
    state_ = (next_s3 << 3) | (next_s2 << 2) | (next_s1 << 1) | next_s0;

    return static_cast<V34Coset4D>(coset_idx);
}

// ----------------------------------------------------------------------------
// V34ViterbiDecoder
// ----------------------------------------------------------------------------

V34ViterbiDecoder::V34ViterbiDecoder(size_t traceback_depth)
    : traceback_depth_(traceback_depth), history_(traceback_depth) {
    reset();
}

void V34ViterbiDecoder::reset() {
    path_metrics_.fill(1.0e6);
    path_metrics_[0] = 0.0;
    history_head_ = 0;
    symbols_fed_ = 0;
    for (auto& node : history_) {
        node.prev_state.fill(0);
        node.coset.fill(0);
        node.uncoded_bits.fill(0);
    }
}

V34ViterbiDecoder::Decoded4D V34ViterbiDecoder::update(
    const std::array<double, 8>& branch_metrics,
    const std::array<uint32_t, 8>& uncoded_bits_per_coset) {
    
    std::array<double, 16> new_metrics;
    new_metrics.fill(1.0e6);
    HistoryNode current_node;

    // For each current state s (0..15) and input (y0, y1) in {00, 01, 10, 11}
    for (uint8_t s = 0; s < 16; ++s) {
        if (path_metrics_[s] >= 1.0e5) continue;
        const uint8_t s0 = s & 1u;
        const uint8_t s1 = (s >> 1) & 1u;
        const uint8_t s2 = (s >> 2) & 1u;
        const uint8_t s3 = (s >> 3) & 1u;

        for (uint8_t y1 = 0; y1 < 2; ++y1) {
            for (uint8_t y0 = 0; y0 < 2; ++y0) {
                const uint8_t c0 = y0;
                const uint8_t c1 = y1;
                const uint8_t c2 = (s0 ^ s1 ^ s2 ^ s3) & 1u;
                const uint8_t coset_idx = (c2 << 2) | (c1 << 1) | c0;

                const uint8_t next_s0 = (y0 ^ s2) & 1u;
                const uint8_t next_s1 = (y1 ^ s3) & 1u;
                const uint8_t next_s2 = s0;
                const uint8_t next_s3 = s1;
                const uint8_t next_state = (next_s3 << 3) | (next_s2 << 2) | (next_s1 << 1) | next_s0;

                const double metric = path_metrics_[s] + branch_metrics[coset_idx];
                if (metric < new_metrics[next_state]) {
                    new_metrics[next_state] = metric;
                    current_node.prev_state[next_state] = s;
                    current_node.coset[next_state] = coset_idx;
                    current_node.uncoded_bits[next_state] = uncoded_bits_per_coset[coset_idx];
                }
            }
        }
    }

    // Renormalize path metrics to avoid floating point overflow
    double min_metric = *std::min_element(new_metrics.begin(), new_metrics.end());
    for (double& m : new_metrics) m -= min_metric;
    path_metrics_ = new_metrics;

    history_[history_head_] = current_node;
    history_head_ = (history_head_ + 1) % traceback_depth_;
    ++symbols_fed_;
    if (symbols_fed_ < traceback_depth_) return {};

    // Traceback from the state with minimum path metric
    uint8_t trace_state = static_cast<uint8_t>(
        std::min_element(path_metrics_.begin(), path_metrics_.end()) - path_metrics_.begin());
    
    size_t ptr = (history_head_ + traceback_depth_ - 1) % traceback_depth_;
    uint8_t oldest_coset = 0;
    uint32_t oldest_uncoded = 0;

    for (size_t step = 0; step < traceback_depth_; ++step) {
        const auto& node = history_[ptr];
        oldest_coset = node.coset[trace_state];
        oldest_uncoded = node.uncoded_bits[trace_state];
        trace_state = node.prev_state[trace_state];
        ptr = (ptr + traceback_depth_ - 1) % traceback_depth_;
    }

    Decoded4D res;
    res.valid = true;
    res.y0 = oldest_coset & 1u;
    res.y1 = (oldest_coset >> 1) & 1u;
    res.uncoded_bits = oldest_uncoded;
    return res;
}

// ----------------------------------------------------------------------------
// V34QamModulator
// ----------------------------------------------------------------------------

V34QamModulator::V34QamModulator(unsigned symbol_rate, unsigned bits_per_2d, double amplitude)
    : symbol_rate_(symbol_rate), bits_per_2d_(bits_per_2d), amplitude_(amplitude),
      constellation_(bits_per_2d) {
    carrier_hz_ = exact_carrier_hz(symbol_rate_);
}

std::vector<int16_t> V34QamModulator::modulate_bytes(const std::vector<uint8_t>& bytes, size_t pad_symbols) {
    trellis_.reset();
    scrambler_.reset();

    // 1. Pack 8-N-1 UART framing: start bit (0), 8 data bits LSB first, stop bit (1)
    std::vector<uint8_t> bits;
    // Preamble to train/synchronize GPA scrambler (23-bit self-synchronizing polynomial)
    for (size_t i = 0; i < 80; ++i) bits.push_back(1u);
    for (uint8_t b : bytes) {
        bits.push_back(0u); // start
        for (int i = 0; i < 8; ++i) bits.push_back((b >> i) & 1u);
        bits.push_back(1u); // stop
    }
    for (size_t i = 0; i < pad_symbols * 2; ++i) bits.push_back(1u);

    // 2. Scramble with GPA polynomial
    for (auto& bit : bits) bit = scrambler_.scramble(bit);

    // 3. 4D Symbol Mapping: each 4D symbol takes (2*bits_per_2d - 1) information bits
    // (2 coded bits Y0,Y1 + (2*bits_per_2d - 3) uncoded bits)
    const unsigned info_bits_per_4d = 2 * bits_per_2d_ - 1;
    std::vector<Complex> symbols;
    symbols.reserve((bits.size() / info_bits_per_4d + 2) * 2);

    for (size_t bit_idx = 0; bit_idx + info_bits_per_4d <= bits.size(); bit_idx += info_bits_per_4d) {
        const uint8_t y0 = bits[bit_idx];
        const uint8_t y1 = bits[bit_idx + 1];
        uint32_t uncoded = 0;
        for (unsigned k = 0; k < info_bits_per_4d - 2; ++k)
            uncoded |= (static_cast<uint32_t>(bits[bit_idx + 2 + k] & 1u) << k);

        const V34Coset4D coset = trellis_.encode(y0, y1);
        const auto pair = constellation_.map_4d(coset, uncoded);
        symbols.push_back(pair.first);
        symbols.push_back(pair.second);
    }

    // 4. Pulse shape with RRC and modulate to carrier frequency
    const double sps = exact_samples_per_symbol(symbol_rate_);
    const size_t total_samples = static_cast<size_t>(std::ceil(symbols.size() * sps)) + 160;
    std::vector<int16_t> pcm(total_samples, 0);

    for (size_t n = 0; n < total_samples; ++n) {
        const double relative = static_cast<double>(n);
        const long centre = static_cast<long>(std::floor(relative / sps));
        Complex shaped(0.0, 0.0);
        for (long k = centre - 12; k <= centre + 12; ++k) {
            if (k >= 0 && static_cast<size_t>(k) < symbols.size())
                shaped += symbols[static_cast<size_t>(k)] * rrc_pulse((relative - k * sps) / sps, 0.12);
        }
        const double phase = 2.0 * kPi * carrier_hz_ * static_cast<double>(n) / kSampleRate;
        const double y = amplitude_ * std::real(shaped * Complex(std::cos(phase), std::sin(phase)));
        pcm[n] = static_cast<int16_t>(std::lround(std::clamp(y, -32767.0, 32767.0)));
    }
    return pcm;
}

// ----------------------------------------------------------------------------
// V34QamDemodulator
// ----------------------------------------------------------------------------

V34QamDemodulator::V34QamDemodulator(unsigned symbol_rate, unsigned bits_per_2d)
    : symbol_rate_(symbol_rate), bits_per_2d_(bits_per_2d), constellation_(bits_per_2d) {
    nominal_carrier_hz_ = exact_carrier_hz(symbol_rate_);
    sps_ = exact_samples_per_symbol(symbol_rate_);
    reset();
}

void V34QamDemodulator::reset() {
    viterbi_.reset();
    descrambler_.reset();
    pcm_buf_.clear();
    absolute_pcm_samples_processed_ = 0;
    bit_buf_.clear();
    carrier_locked_ = false;
    timing_locked_ = false;
    carrier_phase_ = 0.0;
    carrier_offset_hz_ = 0.0;
    timing_phase_ = 25.0;
    has_first_2d_ = false;
    first_2d_symbol_ = {0.0, 0.0};
    synced_8n1_ = false;
    bit_offset_8n1_ = 0;
    bytes_emitted_ = 0;
}

std::vector<uint8_t> V34QamDemodulator::process_pcm(const std::vector<int16_t>& pcm) {
    if (pcm.empty()) return {};
    pcm_buf_.insert(pcm_buf_.end(), pcm.begin(), pcm.end());
    if (pcm_buf_.size() < 160) return {};

    const auto bb = filter_baseband_rrc(pcm_buf_, nominal_carrier_hz_ + carrier_offset_hz_, symbol_rate_, 0.12, absolute_pcm_samples_processed_);
    if (bb.size() < 30) return {};

    // Automatic Gain Control (AGC) - measure RMS power over active symbols
    double peak_pwr = 0.0;
    for (const auto& sample : bb) peak_pwr = std::max(peak_pwr, std::norm(sample));
    double active_power = 0.0;
    size_t active_count = 0;
    for (const auto& sample : bb) {
        if (std::norm(sample) >= 0.05 * peak_pwr) {
            active_power += std::norm(sample);
            ++active_count;
        }
    }
    const double rms = (active_count > 0) ? std::sqrt(active_power / active_count) : 1.0;
    const double gain = (rms > 1.0e-4) ? (1.0 / rms) : 1.0;

    const unsigned info_bits_per_4d = 2 * bits_per_2d_ - 1;
    std::vector<uint8_t> out_bytes;

    if (!synced_8n1_) {
        const double dt = sps_ / 16.0;
        size_t global_best_frames = 0;
        double global_best_phase = 25.0;
        size_t global_best_sym_offset = 0;
        size_t global_best_bit_offset = 0;

        for (int t_step = 0; t_step < 16; ++t_step) {
            const double test_phase = 25.0 + t_step * dt;
            std::vector<Complex> cand_syms;
            for (double at = test_phase; at + 25.0 < bb.size(); at += sps_) {
                Complex s = interpolate_sample(bb, at) * gain;
                cand_syms.push_back(s);
            }
            if (cand_syms.size() < 10) continue;

            for (size_t sym_offset : {0, 1}) {
                V34ViterbiDecoder test_viterbi(12);
                V34GpaDescrambler test_descrambler;
                std::vector<uint8_t> test_bits;

                for (size_t i = sym_offset; i + 1 < cand_syms.size(); i += 2) {
                    const Complex r1 = cand_syms[i];
                    const Complex r2 = cand_syms[i + 1];

                    std::array<double, 8> bm{};
                    std::array<std::pair<Complex, Complex>, 8> bp{};
                    constellation_.compute_4d_branch_metrics(r1, r2, bm, bp);

                    std::array<uint32_t, 8> uncoded{};
                    for (size_t c = 0; c < 8; ++c)
                        uncoded[c] = constellation_.demap_uncoded(
                            static_cast<V34Coset4D>(c), bp[c].first, bp[c].second);

                    const auto dec = test_viterbi.update(bm, uncoded);
                    if (dec.valid) {
                        test_bits.push_back(test_descrambler.descramble(dec.y0));
                        test_bits.push_back(test_descrambler.descramble(dec.y1));
                        for (unsigned k = 0; k < info_bits_per_4d - 2; ++k)
                            test_bits.push_back(test_descrambler.descramble(static_cast<uint8_t>((dec.uncoded_bits >> k) & 1u)));
                    }
                }

                for (size_t off = 0; off + 10 <= test_bits.size(); ++off) {
                    size_t frames = 0;
                    for (size_t k = off; k + 10 <= test_bits.size(); k += 10) {
                        if (test_bits[k] == 0 && test_bits[k + 9] == 1) ++frames;
                        else break;
                    }
                    if (frames > global_best_frames) {
                        global_best_frames = frames;
                        global_best_phase = test_phase;
                        global_best_sym_offset = sym_offset;
                        global_best_bit_offset = off;
                    }
                }
            }
        }

        if (global_best_frames >= 2) {
            timing_phase_ = global_best_phase;
            if (global_best_sym_offset == 1) {
                timing_phase_ += sps_;
            }
            timing_locked_ = true;
            has_first_2d_ = false;
            viterbi_.reset();
            descrambler_.reset();
            bit_buf_.clear();
            synced_8n1_ = true;
            bit_offset_8n1_ = global_best_bit_offset;
            bytes_emitted_ = 0;
        }
    }

    if (synced_8n1_) {
        while (timing_phase_ + 25.0 < bb.size()) {
            Complex s = interpolate_sample(bb, timing_phase_) * gain;
            timing_phase_ += sps_;
            if (!has_first_2d_) {
                first_2d_symbol_ = s;
                has_first_2d_ = true;
            } else {
                has_first_2d_ = false;
                const Complex r1 = first_2d_symbol_;
                const Complex r2 = s;

                std::array<double, 8> bm{};
                std::array<std::pair<Complex, Complex>, 8> bp{};
                constellation_.compute_4d_branch_metrics(r1, r2, bm, bp);

                std::array<uint32_t, 8> uncoded{};
                for (size_t c = 0; c < 8; ++c)
                    uncoded[c] = constellation_.demap_uncoded(
                        static_cast<V34Coset4D>(c), bp[c].first, bp[c].second);

                const auto dec = viterbi_.update(bm, uncoded);
                if (dec.valid) {
                    bit_buf_.push_back(descrambler_.descramble(dec.y0));
                    bit_buf_.push_back(descrambler_.descramble(dec.y1));
                    for (unsigned k = 0; k < info_bits_per_4d - 2; ++k)
                        bit_buf_.push_back(descrambler_.descramble(static_cast<uint8_t>((dec.uncoded_bits >> k) & 1u)));
                }
            }
        }

        while (bit_offset_8n1_ + 10 <= bit_buf_.size()) {
            if (bit_buf_[bit_offset_8n1_] == 0 && bit_buf_[bit_offset_8n1_ + 9] == 1) {
                uint8_t ch = 0;
                for (int b = 0; b < 8; ++b)
                    ch |= static_cast<uint8_t>((bit_buf_[bit_offset_8n1_ + 1 + b] & 1u) << b);
                out_bytes.push_back(ch);
                bit_offset_8n1_ += 10;
            } else {
                ++bit_offset_8n1_;
            }
        }

        // Keep buffers compact without breaking phase
        if (bit_offset_8n1_ > 200) {
            bit_buf_.erase(bit_buf_.begin(), bit_buf_.begin() + bit_offset_8n1_);
            bit_offset_8n1_ = 0;
        }
        if (timing_phase_ > 200.0) {
            size_t drop = static_cast<size_t>(timing_phase_ - 50.0);
            if (drop < pcm_buf_.size()) {
                pcm_buf_.erase(pcm_buf_.begin(), pcm_buf_.begin() + drop);
                timing_phase_ -= drop;
                absolute_pcm_samples_processed_ += drop;
            }
        }
    }

    return out_bytes;
}

} // namespace v92
