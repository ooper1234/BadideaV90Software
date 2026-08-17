#include "async_serial.hpp"

namespace v92 {

static uint8_t parity_bit(uint8_t v, int nbits, Parity p) {
    int ones = 0;
    for (int i=0;i<nbits;++i) ones += (v >> i) & 1;
    if (p == Parity::Even) return static_cast<uint8_t>(ones & 1);
    if (p == Parity::Odd) return static_cast<uint8_t>((ones & 1) ? 0 : 1);
    return 1;
}

std::vector<uint8_t> async_encode(const std::vector<uint8_t>& bytes, AsyncFormat fmt) {
    std::vector<uint8_t> out;
    const int pbits = fmt.parity == Parity::None ? 0 : 1;
    out.reserve(bytes.size() * (1 + fmt.data_bits + pbits + fmt.stop_bits));
    for (uint8_t ch : bytes) {
        out.push_back(0); // start
        for (int i=0;i<fmt.data_bits;++i) out.push_back((ch >> i) & 1);
        if (fmt.parity != Parity::None) out.push_back(parity_bit(ch, fmt.data_bits, fmt.parity));
        for (int i=0;i<fmt.stop_bits;++i) out.push_back(1);
    }
    return out;
}

std::vector<uint8_t> async_decode(const std::vector<uint8_t>& bits, AsyncFormat fmt) {
    std::vector<uint8_t> out;
    const int pbits = fmt.parity == Parity::None ? 0 : 1;
    const size_t frame = static_cast<size_t>(1 + fmt.data_bits + pbits + fmt.stop_bits);
    size_t i = 0;
    while (i + frame <= bits.size()) {
        while (i < bits.size() && (bits[i] & 1)) ++i; // hunt start bit
        if (i + frame > bits.size()) break;
        uint8_t ch = 0;
        for (int b=0;b<fmt.data_bits;++b) ch |= static_cast<uint8_t>((bits[i+1+b] & 1) << b);
        size_t pos = i + 1 + fmt.data_bits;
        bool ok = true;
        if (fmt.parity != Parity::None) {
            ok = (bits[pos] & 1) == parity_bit(ch, fmt.data_bits, fmt.parity);
            ++pos;
        }
        for (int s=0;s<fmt.stop_bits;++s) ok = ok && ((bits[pos+s] & 1) == 1);
        if (ok) out.push_back(ch);
        i += frame;
    }
    return out;
}

} // namespace v92
