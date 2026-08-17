#pragma once
#include <cstdint>
#include <optional>
#include <vector>

namespace v92 {

enum class Parity { None, Even, Odd };

struct AsyncFormat {
    int data_bits = 8;
    Parity parity = Parity::None;
    int stop_bits = 1;
};

// UART/start-stop framing, LSB first. Idle/mark is binary 1.
std::vector<uint8_t> async_encode(const std::vector<uint8_t>& bytes,
                                  AsyncFormat fmt = {});

// Decode a nominally aligned start-stop bit stream. Invalid frames are skipped.
std::vector<uint8_t> async_decode(const std::vector<uint8_t>& bits,
                                  AsyncFormat fmt = {});

} // namespace v92
