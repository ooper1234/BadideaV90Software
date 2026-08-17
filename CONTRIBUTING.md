# Contributing

Contributions are welcome. Keep modem/DSP changes testable with generated sample streams where possible, and do not advertise a modulation mode until its live data path is implemented.

Useful checks before submitting a change:

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
./scripts/selftest.sh
./build/v92isp-live --features
```

High-value areas are V.8 capability negotiation, V.22/V.22bis hardware interoperability, V.32/V.32bis, V.34, V.42/LAPM, and the complete V.90/V.92 PCM data pump.
