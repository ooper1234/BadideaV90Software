# Third-party components

v92isp itself is GPL-2.0-or-later. The Windows bootstrap downloads third-party dependencies from their upstream projects rather than vendoring their source trees in this ZIP.

## PJSIP 2.17

Used for native Windows SIP/SDP/RTP and the PJSUA2 media bridge. The build
script fetches the pinned PJSIP 2.17 source release and builds it locally. It
applies `third_party/patches/pjproject-pcmu-negative-zero.patch`, a one-entry
G.711 encoder-table correction needed to preserve both V.90 zero codewords
through PJSUA2's signed-linear media port.

## SpanDSP

Used for V.8 and V.22/V.22bis modem DSP. The Windows MSYS2 build installs the distribution SpanDSP package. SpanDSP is not vendored in this repository.

## Wintun 0.14.1

The Windows build downloads the official Wintun 0.14.1 archive and verifies the published SHA-256 before staging `wintun.dll`.

## WinDivert 2.2.2-A

Used by the built-in Windows single-peer user-mode IPv4 NAT fallback/primary path. The build downloads the official pre-built WinDivert 2.2.2-A x64 DLL/driver/header and checks the driver Authenticode status. WinDivert is dual-licensed upstream under LGPLv3 or GPLv2; this GPL project uses it under the GPL-compatible option.

## Linmodem research reference

Fabrice Bellard's Linmodem has been researched as a possible future source/reference for higher-speed modem DSP. No Linmodem source code is included here.

## Fabrice Bellard Linmodem V.90 research reference

The V.90 PCM mapping implementation in `src/v90_pcm.cpp` is a C++ integration
informed by Fabrice Bellard's public `linmodem` V.90 research code (`v90.c`),
which is distributed under GNU GPL version 2. The project itself is
GPL-2.0-or-later, so this code is distributed under GPLv2-compatible terms.
Bellard's project is used as an implementation reference only; its own project
page explicitly describes the V.34 startup/demodulator and complete modem as
unfinished, so v92isp does not claim it supplies a turnkey 56K modem.
