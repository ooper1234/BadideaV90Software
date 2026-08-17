#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace v92 {

struct QC1a {
    bool lapm = true;
    uint8_t uqts = 61;
};

struct QCA1d {
    bool lapm = true;
    int anspcm_level_dbm0 = -12;
};

// Parse the fixed 60-bit QC1a structure after V.21 demodulation.
std::optional<QC1a> parse_qc1a(const std::vector<uint8_t>& bits);

// Build the fixed 70-bit QCA1d structure, ready for V.21(H) modulation.
std::vector<uint8_t> build_qca1d_bits(const QCA1d& qca);

// V.92 QTS followed by QTS-bar, as raw G.711 mu-law octets at 8 ksym/s.
std::vector<uint8_t> build_qts_ulaw(uint8_t uqts);

// Generate ANSpcm as raw G.711 mu-law octets. Duration is rounded up to samples.
std::vector<uint8_t> build_anspcm_ulaw(int level_dbm0, double seconds);

// Simple 980-Hz detector for TONEq, intended for >=50 ms windows.
bool detect_toneq_980(const std::vector<int16_t>& pcm, int sample_rate = 8000);

enum class QCState {
    Idle,
    AnswerSilence,
    SendANSam,
    WaitQC1aOrCM,
    SendQCA1d,
    Send75msSilence,
    SendQTS,
    SendANSpcm,
    WaitTONEq,
    SendPostTONEqSilence,
    ShortPhase2,
    FullV8Fallback
};

const char* to_string(QCState s);

// Protocol state skeleton for the digital answering modem direction.
class QuickConnectAnswerSM {
public:
    void on_call_answered();
    void on_200ms_elapsed();
    void on_qc1a(const QC1a& qc);
    void on_cm_detected();
    void on_qca1d_sent();
    void on_75ms_elapsed();
    void on_qts_sent();
    void on_toneq_detected();
    void on_timeout();

    QCState state() const { return state_; }
    uint8_t uqts() const { return uqts_; }
    bool lapm() const { return lapm_; }

private:
    QCState state_ = QCState::Idle;
    uint8_t uqts_ = 61;
    bool lapm_ = true;
};

} // namespace v92
