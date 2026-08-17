#include "g711.hpp"

extern "C" {
#include <pjmedia/alaw_ulaw.h>
}

#include <cstdint>
#include <iomanip>
#include <iostream>

int main()
{
    for (unsigned code = 0; code < 256; ++code) {
        const auto pcm = v92::ulaw_to_linear_for_reencode(
            static_cast<uint8_t>(code));
        const unsigned encoded = pjmedia_linear2ulaw(pcm);
        if (encoded != code) {
            std::cerr << "PJSIP PCMU round-trip mismatch: 0x"
                      << std::hex << std::setw(2) << std::setfill('0') << code
                      << " -> 0x" << std::setw(2) << encoded << "\n";
            return 1;
        }
    }
    std::cout << "all 256 PJSIP PCMU codewords round-trip exactly\n";
    return 0;
}
