#pragma once
#include <cstdint>
#include <vector>

namespace v92 {

enum class V21Band { Low, High };

// 300 bit/s V.21 BFSK modulator at 8 kHz PCM.
// V.21: binary 1 (mark) is 980 Hz in the low/originate channel and
// 1650 Hz in the high/answer channel. Binary 0 (space) is 1180/1850 Hz.
std::vector<int16_t> v21_modulate(const std::vector<uint8_t>& bits, V21Band band,
                                  double amplitude = 7000.0, int sample_rate = 8000);

// Lightweight correlator for lab use. Returns one hard decision per nominal bit.
// This is not yet a production timing-recovery implementation.
std::vector<uint8_t> v21_demodulate_nominal(const std::vector<int16_t>& pcm, V21Band band,
                                            int sample_rate = 8000);

} // namespace v92
