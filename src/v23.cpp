#include "v23.hpp"
#include <cmath>

namespace v92 {

static std::vector<int16_t> fsk_mod(const std::vector<uint8_t>& bits, double baud,
                                    double f1, double f0, double amplitude, int fs) {
    std::vector<int16_t> out;
    out.reserve(static_cast<size_t>(std::ceil(bits.size() * fs / baud)));
    double phase = 0.0, cursor = 0.0;
    for (size_t bi=0; bi<bits.size(); ++bi) {
        const double next = (bi + 1) * fs / baud;
        const double f = (bits[bi] & 1) ? f1 : f0;
        while (cursor + 0.5 < next) {
            out.push_back(static_cast<int16_t>(std::lround(amplitude * std::sin(phase))));
            phase += 2.0 * M_PI * f / fs;
            if (phase > 2.0 * M_PI) phase = std::fmod(phase, 2.0 * M_PI);
            cursor += 1.0;
        }
    }
    return out;
}

static double corr_energy(const int16_t* x, size_t n, double f, int fs) {
    double c=0.0, s=0.0;
    for (size_t i=0;i<n;++i) {
        const double a=2.0*M_PI*f*static_cast<double>(i)/fs;
        c += x[i]*std::cos(a); s += x[i]*std::sin(a);
    }
    return c*c+s*s;
}

static std::vector<uint8_t> fsk_demod(const std::vector<int16_t>& pcm, double baud,
                                      double f1, double f0, int fs) {
    std::vector<uint8_t> bits;
    size_t start=0, bi=0;
    while (start < pcm.size()) {
        const size_t end=static_cast<size_t>(std::llround((bi+1)*fs/baud));
        if (end > pcm.size()) break;
        const size_t n=end-start;
        bits.push_back(corr_energy(pcm.data()+start,n,f1,fs) >=
                       corr_energy(pcm.data()+start,n,f0,fs) ? 1 : 0);
        start=end; ++bi;
    }
    return bits;
}

std::vector<int16_t> v23_modulate_forward(const std::vector<uint8_t>& bits,
                                          V23ForwardMode mode,
                                          double amplitude, int sample_rate) {
    const double baud = mode==V23ForwardMode::Baud1200 ? 1200.0 : 600.0;
    const double f0 = mode==V23ForwardMode::Baud1200 ? 2100.0 : 1700.0;
    return fsk_mod(bits, baud, 1300.0, f0, amplitude, sample_rate);
}

std::vector<uint8_t> v23_demodulate_forward_nominal(const std::vector<int16_t>& pcm,
                                                     V23ForwardMode mode,
                                                     int sample_rate) {
    const double baud = mode==V23ForwardMode::Baud1200 ? 1200.0 : 600.0;
    const double f0 = mode==V23ForwardMode::Baud1200 ? 2100.0 : 1700.0;
    return fsk_demod(pcm, baud, 1300.0, f0, sample_rate);
}

std::vector<int16_t> v23_modulate_backward75(const std::vector<uint8_t>& bits,
                                             double amplitude, int sample_rate) {
    return fsk_mod(bits, 75.0, 390.0, 450.0, amplitude, sample_rate);
}

std::vector<uint8_t> v23_demodulate_backward75_nominal(const std::vector<int16_t>& pcm,
                                                        int sample_rate) {
    return fsk_demod(pcm, 75.0, 390.0, 450.0, sample_rate);
}

} // namespace v92
