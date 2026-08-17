#include "answer_tones.hpp"
#include <cmath>

namespace v92 {

static std::vector<int16_t> build_2100(double seconds, bool am15, bool reversals,
                                       double amplitude, int fs) {
    const size_t n=static_cast<size_t>(std::ceil(seconds*fs));
    std::vector<int16_t> out; out.reserve(n);
    double phase=0.0;
    int sign=1;
    const size_t rev=static_cast<size_t>(std::llround(0.450*fs));
    for(size_t i=0;i<n;++i) {
        if (reversals && i>0 && rev>0 && i%rev==0) sign=-sign;
        // V.8 ANSam envelope 0.8..1.2 times average => 20% AM depth.
        const double env = am15 ? (1.0 + 0.2*std::sin(2.0*M_PI*15.0*i/fs)) : 1.0;
        out.push_back(static_cast<int16_t>(std::lround(sign*amplitude*env*std::sin(phase))));
        phase += 2.0*M_PI*2100.0/fs;
        if (phase > 2.0*M_PI) phase=std::fmod(phase,2.0*M_PI);
    }
    return out;
}

std::vector<int16_t> build_ans_2100(double seconds, bool phase_reversals,
                                    double amplitude, int sample_rate) {
    return build_2100(seconds,false,phase_reversals,amplitude,sample_rate);
}

std::vector<int16_t> build_ansam(double seconds, double amplitude, int sample_rate) {
    return build_2100(seconds,true,true,amplitude,sample_rate);
}

} // namespace v92
