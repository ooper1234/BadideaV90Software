#pragma once
#include <cstdint>
#include <vector>

namespace v92 {

enum class V23ForwardMode { Baud600, Baud1200 };

// V.23 forward FSK. Per V.23: symbol 1/mark = 1300 Hz;
// symbol 0/space = 1700 Hz at 600 baud, 2100 Hz at 1200 baud.
std::vector<int16_t> v23_modulate_forward(const std::vector<uint8_t>& bits,
                                          V23ForwardMode mode,
                                          double amplitude = 7000.0,
                                          int sample_rate = 8000);
std::vector<uint8_t> v23_demodulate_forward_nominal(const std::vector<int16_t>& pcm,
                                                     V23ForwardMode mode,
                                                     int sample_rate = 8000);

// Optional V.23 backward channel: up to 75 baud; mark=390 Hz, space=450 Hz.
std::vector<int16_t> v23_modulate_backward75(const std::vector<uint8_t>& bits,
                                             double amplitude = 3500.0,
                                             int sample_rate = 8000);
std::vector<uint8_t> v23_demodulate_backward75_nominal(const std::vector<int16_t>& pcm,
                                                        int sample_rate = 8000);

} // namespace v92
