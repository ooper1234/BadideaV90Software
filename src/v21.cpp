#include "v21.hpp"
#include <cmath>

namespace v92 {

static std::pair<double,double> freqs(V21Band b) {
    // V.21 mark (binary 1) / space (binary 0).
    return (b == V21Band::Low) ? std::make_pair(980.0, 1180.0)
                               : std::make_pair(1650.0, 1850.0);
}

std::vector<int16_t> v21_modulate(const std::vector<uint8_t>& bits, V21Band band,
                                  double amplitude, int sample_rate) {
    const auto [f1, f0] = freqs(band);
    std::vector<int16_t> out;
    out.reserve(static_cast<size_t>(std::ceil(bits.size() * sample_rate / 300.0)));
    double phase = 0.0;
    double sample_cursor = 0.0;
    for (size_t bi = 0; bi < bits.size(); ++bi) {
        const double next = (bi + 1) * sample_rate / 300.0;
        const double f = bits[bi] ? f1 : f0;
        while (sample_cursor + 0.5 < next) {
            out.push_back(static_cast<int16_t>(std::lround(amplitude * std::sin(phase))));
            phase += 2.0 * M_PI * f / sample_rate;
            if (phase > 2.0 * M_PI) phase = std::fmod(phase, 2.0 * M_PI);
            sample_cursor += 1.0;
        }
    }
    return out;
}

static double corr_energy(const int16_t* x, size_t n, double f, int fs) {
    double c = 0.0, s = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double a = 2.0 * M_PI * f * static_cast<double>(i) / fs;
        c += x[i] * std::cos(a);
        s += x[i] * std::sin(a);
    }
    return c*c + s*s;
}

std::vector<uint8_t> v21_demodulate_nominal(const std::vector<int16_t>& pcm, V21Band band,
                                            int sample_rate) {
    const auto [f1, f0] = freqs(band);
    std::vector<uint8_t> bits;
    size_t start = 0;
    size_t bi = 0;
    while (start < pcm.size()) {
        size_t end = static_cast<size_t>(std::llround((bi + 1) * sample_rate / 300.0));
        if (end > pcm.size()) break;
        size_t n = end - start;
        double e1 = corr_energy(pcm.data() + start, n, f1, sample_rate);
        double e0 = corr_energy(pcm.data() + start, n, f0, sample_rate);
        bits.push_back(e1 >= e0 ? 1 : 0);
        start = end;
        ++bi;
    }
    return bits;
}

} // namespace v92
