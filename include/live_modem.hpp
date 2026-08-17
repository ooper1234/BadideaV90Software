#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <memory>
#include <string>
#include <vector>

namespace v92 {

class SpanV22Modem;
class SpanV8Answerer;
class V90Phase3DigitalTx;
class V90Phase3AnalogueRx;
class V90Phase4DigitalTx;
struct V90DILDescriptor;
enum class V22LinkMode;

enum class LiveMode { Auto, V90Digital, V92QuickConnect, V22bis_2400, V22_1200, V21_300 };

// Both explicit high-speed modes must accept a full V.8/V.90 negotiation.
// V.92 Quick Connect is only the shortened first attempt; a caller without a
// saved matching profile falls back to the ordinary V.90 startup.
bool live_mode_allows_v90(LiveMode mode);

enum class LiveState {
    Idle,
    MediaWait,
    V8Negotiating,
    V90Phase2Silence,
    V90INFO0d,
    V90ToneB,
    V90ToneBReversed,
    V90WaitSecondToneA,
    V90ToneRetryWaitToneA,
    V90Phase2RttComplete,
    V90RecvRemoteL1,
    V90RecvRemoteL2,
    V90ProbeToneB,
    V90ProbeToneBReversed,
    V90SendL1,
    V90SendL2,
    V90INFO1d,
    V90WaitINFO1a,
    V90Phase2Complete,
    V90Phase3WaitAnalogue,
    V90Phase3SendSd,
    V90Phase3SendTRN1d,
    V90Phase3SendJd,
    V90Phase3SendJdBar,
    V90Phase3SendDIL,
    V90Phase4WaitCPt,
    V90Phase4SendTRN2d,
    V90Phase4SendMP,
    V90Phase4SendEd,
    V90Phase4SendB1d,
    V90Data,
    V90RetrainSilence,
    V90Fallback,
    V92AnswerSilence,
    V92ANSam,
    V92QCA1d,
    V92Silence75,
    V92QTS,
    V92ANSpcm,
    V92PostToneqSilence,
    V92ShortPhase2Reached,
    V92Fallback,
    V22Training,
    V22Data,
    V21ANS,
    V21Silence75,
    V21Carrier,
    V21Data
};

const char* to_string(LiveState s);

class LiveModem {
public:
    explicit LiveModem(LiveMode mode = LiveMode::Auto);
    ~LiveModem();

    void start_call();
    void end_call();
    void receive_pcm(const std::vector<int16_t>& pcm);
    std::vector<uint8_t> next_tx_pcmu(size_t samples = 160);
    void feed_ppp_bytes(const std::vector<uint8_t>& bytes);
    std::vector<uint8_t> take_ppp_bytes();

    LiveState state() const { return state_; }
    bool data_connected() const { return state_ == LiveState::V21Data || state_ == LiveState::V22Data || state_ == LiveState::V90Data; }
    bool short_phase2_reached() const { return v92_short_phase2_reached_; }
    const std::string& last_event() const { return last_event_; }
    void clear_event() { last_event_.clear(); }
    void start_v90_phase2();
    void start_v90_phase3(uint8_t uinfo = 79, uint8_t upstream_symbol_rate_index = 4, uint8_t md_length_35ms = 0);

private:
    LiveMode mode_;
    LiveState state_ = LiveState::Idle;
    std::deque<uint8_t> tx_pcmu_;
    std::deque<uint8_t> v21_tx_bits_;
    std::vector<int16_t> rx_history_;
    std::vector<int16_t> toneq_history_;
    std::vector<int16_t> v90_rx_history_;
    std::vector<uint8_t> ppp_rx_bytes_;
    std::string last_event_;
    std::unique_ptr<SpanV22Modem> span_v22_;
    std::unique_ptr<SpanV8Answerer> span_v8_;
    std::unique_ptr<V90Phase3DigitalTx> v90_phase3_tx_;
    std::unique_ptr<V90Phase3AnalogueRx> v90_phase3_rx_;
    std::unique_ptr<V90Phase4DigitalTx> v90_phase4_tx_;
    std::unique_ptr<V90DILDescriptor> v90_dil_descriptor_;

    uint64_t rx_samples_total_ = 0;
    uint64_t state_rx_start_ = 0;
    // TX clock is authoritative for timeouts. RTP may be one-way, and tying
    // timeouts only to received samples can otherwise leave ANSam running forever.
    uint64_t tx_samples_total_ = 0;
    uint64_t state_tx_start_ = 0;
    bool inbound_media_seen_ = false;
    // V.22 carrier training and the V.42/LAPM data link are separate layers.
    // Do not expose PPP until LAPM is actually ready when V.8 negotiated it.
    bool v22_wait_for_lapm_ = false;
    bool v22_carrier_announced_ = false;
    std::string v22_last_link_status_;
    // Hold the V.8 answer tone briefly after SIP answer. Real analogue modems
    // often expect a short quiet interval after answer supervision before ANSam.
    uint64_t v8_tx_hold_until_ = 0;
    int auto_fallback_stage_ = 0; // 0=V8, 1=V22bis, 2=V22, 3=V21
    unsigned v21_mark_blocks_ = 0;
    uint8_t v92_uqts_ = 61;

    bool v90_info0a_sync_seen_ = false; // true only after a CRC-valid INFO0a
    bool v90_info0a_ack_seen_ = false;
    bool v90_info0d_ack_sent_ = false;
    std::vector<uint8_t> v90_info0a_bits_;
    uint64_t v90_history_start_abs_ = 0;
    uint64_t v90_last_reversal_abs_ = 0;
    uint64_t v90_last_diag_abs_ = 0;
    uint32_t v90_v8_remote_modulations_ = 0;
    int v90_v8_remote_pstn_access_ = 0;
    int v90_v8_remote_pcm_availability_ = 0;
    bool v90_tone_a_seen_ = false;
    bool v90_first_tone_a_reversal_ = false;
    double v90_prev_tone_a_re_ = 0.0;
    double v90_prev_tone_a_im_ = 0.0;
    bool v90_prev_tone_a_valid_ = false;
    double v90_toneb_phase_ = 0.0;
    int v90_toneb_sign_ = 1;
    int v90_reverse_countdown_ = -1;
    int v90_post_reverse_countdown_ = -1;
    uint64_t v90_toneb_samples_sent_ = 0;
    bool v90_second_ranging_round_ = false;
    bool v90_remote_probe_seen_ = false;
    double v90_remote_l1_level_dbfs_ = -120.0;
    double v90_remote_l2_level_dbfs_ = -120.0;
    uint64_t v90_remote_probe_start_abs_ = 0;
    bool v90_second_tone_a_seen_ = false;
    bool v90_local_l2_tone_a_seen_ = false;
    bool v90_info1a_seen_ = false;
    bool v90_info1a_recovery_armed_ = false;
    unsigned v90_info1d_retry_count_ = 0;
    uint64_t v90_info1a_tone_samples_ = 0;
    uint8_t v90_phase3_uinfo_ = 0;
    uint8_t v90_phase3_md_length_35ms_ = 0;
    uint8_t v90_phase3_upstream_symbol_rate_index_ = 0;
    bool v90_phase3_analogue_signal_seen_ = false;
    uint64_t v90_phase3_tone_a_samples_ = 0;
    std::vector<int16_t> v90_phase3_tone_a_history_;
    size_t v90_dil_next_segment_ = 0;
    uint64_t v90_dil_s_detected_abs_ = 0;
    bool v90_dil_stop_after_segment_ = false;
    bool v90_mp_acknowledge_ = false;
    bool v90_cp_prime_seen_ = false;
    unsigned v90_tone_retry_count_ = 0;
    bool v92_short_phase2_active_ = false;
    bool v92_short_phase2_reached_ = false;
    bool v92_qc_lapm_ = false;
    uint64_t v92_toneq_deadline_tx_ = 0;

    double v21_tx_phase_ = 0.0;
    double v21_tx_bit_phase_ = 0.0;
    uint8_t v21_tx_current_bit_ = 1;

    std::vector<int16_t> v21_data_pcm_;
    bool v21_rx_synced_ = false;
    size_t v21_rx_sample_phase_ = 0;
    size_t v21_rx_bit_offset_ = 0;
    size_t v21_rx_frames_emitted_ = 0;

    void queue_silence(size_t samples);
    void queue_linear(const std::vector<int16_t>& pcm);
    void queue_pcmu(const std::vector<uint8_t>& pcmu);
    void set_state(LiveState s, const std::string& event = {});
    void maybe_advance_tx_state();
    void start_v8(bool v90_digital = false);
    void start_v92();
    void start_v92_short_phase2();
    void start_v21();
    void start_v22(int bit_rate, V22LinkMode link_mode);
    void fallback_from_v22(const std::string& why);

    std::optional<std::pair<bool,uint8_t>> find_qc1a();
    bool detect_tone(const std::vector<int16_t>& pcm, double target, double other) const;

    uint8_t next_v90_toneb_ulaw();
    void send_v90_info0d(const std::string& event);
    void send_v90_info1d(const std::string& event);
    void begin_v90_retrain(const std::string& event);
    void feed_v90_phase2_rx(const std::vector<int16_t>& pcm);
    uint8_t next_v21_tx_ulaw();
    void feed_v21_rx(const std::vector<int16_t>& pcm);
};

} // namespace v92
