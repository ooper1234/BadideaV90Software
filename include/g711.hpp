#pragma once
#include <cstdint>

namespace v92 {

// ITU-T G.711 mu-law codec helpers.
uint8_t linear_to_ulaw(int16_t pcm);
int16_t ulaw_to_linear(uint8_t u);

// PJSIP's conference bridge transports signed linear PCM and encodes it back
// to PCMU.  Ordinary decode/re-encode is bit exact for every codeword except
// negative zero (0x7F), which plain linear PCM otherwise collapses to +0.
// Return -1 for that one codeword so the following PCMU encoder recreates it.
int16_t ulaw_to_linear_for_reencode(uint8_t u);

// V.90 Table 1 Ucode -> positive-polarity G.711 mu-law octet.
// Ucode is 0..127. Negative polarity is obtained by toggling the G.711 sign bit.
uint8_t ucode_to_ulaw(uint8_t ucode, bool positive = true);

} // namespace v92
