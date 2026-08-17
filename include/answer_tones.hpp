#pragma once
#include <cstdint>
#include <vector>

namespace v92 {

// V.25-style 2100 Hz answer tone. Optional 180 degree reversals every 450 ms.
std::vector<int16_t> build_ans_2100(double seconds, bool phase_reversals = true,
                                    double amplitude = 7000.0, int sample_rate = 8000);

// V.8 ANSam: 2100 Hz carrier, 15 Hz amplitude modulation, phase reversals every 450 ms.
std::vector<int16_t> build_ansam(double seconds, double amplitude = 7000.0,
                                 int sample_rate = 8000);

} // namespace v92
