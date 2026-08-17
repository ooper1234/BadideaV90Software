# Complete-fix build

This consolidated build replaces the chain of incremental Windows patches.

Included fixes:

1. Native PJSIP 2.17 SIP/RTP frontend and two-way PCMU media.
2. PJSIP MSYS2 build/path/API/link compatibility fixes.
3. Stable raw V.22/V.22bis transparent async PPP path (LAPM bypass for now).
4. PPP LCP/IPCP userspace Internet link.
5. Client-side Windows RAS Disconnect completion via Terminate-Ack + carrier drop/BYE.
6. Built-in WinDivert single-peer NAT, preferred instead of broken ICS.
7. WinNAT/ICS retained as backup.
8. PPP-MRU IPv4 fragmentation for Internet replies.
9. ESP32 MiniPBX keep-alive compatibility fix (PJSIP UDP keep-alive disabled).
10. First-packet diagnostics for PPP and NAT in both directions.

This does not claim full V.90/V.92 56K support; that DSP remains unfinished.
