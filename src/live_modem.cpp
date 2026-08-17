#include "live_modem.hpp"

#include "answer_tones.hpp"
#include "async_serial.hpp"
#include "g711.hpp"
#include "v21.hpp"
#include "v92_quickconnect.hpp"
#include "v90_phase2.hpp"
#include "v90_phase3.hpp"
#include "v90_phase3_rx.hpp"
#include "v90_pcm.hpp"
#include "span_v22.hpp"
#include "span_v8.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace v92 {

bool live_mode_allows_v90(LiveMode mode) {
    return mode == LiveMode::V90Digital || mode == LiveMode::V92QuickConnect;
}

const char* to_string(LiveState s) {
    switch(s){
        case LiveState::Idle:return "Idle";
        case LiveState::MediaWait:return "MediaWait";
        case LiveState::V8Negotiating:return "V8Negotiating";
        case LiveState::V90Phase2Silence:return "V90Phase2Silence";
        case LiveState::V90INFO0d:return "V90INFO0d";
        case LiveState::V90ToneB:return "V90ToneB";
        case LiveState::V90ToneBReversed:return "V90ToneBReversed";
        case LiveState::V90WaitSecondToneA:return "V90WaitSecondToneA";
        case LiveState::V90ToneRetryWaitToneA:return "V90ToneRetryWaitToneA";
        case LiveState::V90Phase2RttComplete:return "V90Phase2RttComplete";
        case LiveState::V90RecvRemoteL1:return "V90RecvRemoteL1";
        case LiveState::V90RecvRemoteL2:return "V90RecvRemoteL2";
        case LiveState::V90ProbeToneB:return "V90ProbeToneB";
        case LiveState::V90ProbeToneBReversed:return "V90ProbeToneBReversed";
        case LiveState::V90SendL1:return "V90SendL1";
        case LiveState::V90SendL2:return "V90SendL2";
        case LiveState::V90INFO1d:return "V90INFO1d";
        case LiveState::V90WaitINFO1a:return "V90WaitINFO1a";
        case LiveState::V90Phase2Complete:return "V90Phase2Complete";
        case LiveState::V90Phase3WaitAnalogue:return "V90Phase3WaitAnalogue";
        case LiveState::V90Phase3SendSd:return "V90Phase3SendSd";
        case LiveState::V90Phase3SendTRN1d:return "V90Phase3SendTRN1d";
        case LiveState::V90Phase3SendJd:return "V90Phase3SendJd";
        case LiveState::V90Phase3SendJdBar:return "V90Phase3SendJdBar";
        case LiveState::V90Phase3SendDIL:return "V90Phase3SendDIL";
        case LiveState::V90Phase4WaitCPt:return "V90Phase4WaitCPt";
        case LiveState::V90Phase4SendTRN2d:return "V90Phase4SendTRN2d";
        case LiveState::V90Phase4SendMP:return "V90Phase4SendMP";
        case LiveState::V90Phase4SendEd:return "V90Phase4SendEd";
        case LiveState::V90Phase4SendB1d:return "V90Phase4SendB1d";
        case LiveState::V90Data:return "V90Data";
        case LiveState::V90RetrainSilence:return "V90RetrainSilence";
        case LiveState::V90Fallback:return "V90Fallback";
        case LiveState::V92AnswerSilence:return "V92AnswerSilence";
        case LiveState::V92ANSam:return "V92ANSam";
        case LiveState::V92QCA1d:return "V92QCA1d";
        case LiveState::V92Silence75:return "V92Silence75";
        case LiveState::V92QTS:return "V92QTS";
        case LiveState::V92ANSpcm:return "V92ANSpcm";
        case LiveState::V92PostToneqSilence:return "V92PostToneqSilence";
        case LiveState::V92ShortPhase2Reached:return "V92ShortPhase2Reached";
        case LiveState::V92Fallback:return "V92Fallback";
        case LiveState::V22Training:return "V22Training";
        case LiveState::V22Data:return "V22Data";
        case LiveState::V21ANS:return "V21ANS";
        case LiveState::V21Silence75:return "V21Silence75";
        case LiveState::V21Carrier:return "V21Carrier";
        case LiveState::V21Data:return "V21Data";
    }
    return "?";
}

LiveModem::LiveModem(LiveMode mode):mode_(mode){}
LiveModem::~LiveModem() = default;

void LiveModem::set_state(LiveState s,const std::string& e){
    state_=s;
    state_rx_start_=rx_samples_total_;
    state_tx_start_=tx_samples_total_;
    if(!e.empty())last_event_=e;
}

void LiveModem::queue_silence(size_t n){for(size_t i=0;i<n;++i)tx_pcmu_.push_back(0xFF);}
void LiveModem::queue_linear(const std::vector<int16_t>& p){for(auto x:p)tx_pcmu_.push_back(linear_to_ulaw(x));}
void LiveModem::queue_pcmu(const std::vector<uint8_t>& p){for(auto x:p)tx_pcmu_.push_back(x);}

void LiveModem::start_call(){
    tx_pcmu_.clear();v21_tx_bits_.clear();rx_history_.clear();toneq_history_.clear();v90_rx_history_.clear();ppp_rx_bytes_.clear();
    rx_samples_total_=0;state_rx_start_=0;tx_samples_total_=0;state_tx_start_=0;inbound_media_seen_=false;v8_tx_hold_until_=0;auto_fallback_stage_=0;v21_mark_blocks_=0;v92_uqts_=61;v21_tx_phase_=0;v21_tx_bit_phase_=0;v21_tx_current_bit_=1;
    v21_data_pcm_.clear();v21_rx_synced_=false;v21_rx_sample_phase_=0;v21_rx_bit_offset_=0;v21_rx_frames_emitted_=0;
    v90_info0a_sync_seen_=false;v90_info0a_ack_seen_=false;v90_info0d_ack_sent_=false;v90_info0a_bits_.clear();v90_tone_a_seen_=false;v90_first_tone_a_reversal_=false;
    v90_history_start_abs_=0;v90_last_reversal_abs_=0;v90_last_diag_abs_=0;
    v90_v8_remote_modulations_=0;v90_v8_remote_pstn_access_=0;v90_v8_remote_pcm_availability_=0;
    v90_prev_tone_a_re_=v90_prev_tone_a_im_=0.0;v90_prev_tone_a_valid_=false;
    v90_toneb_phase_=0.0;v90_toneb_sign_=1;v90_reverse_countdown_=-1;v90_post_reverse_countdown_=-1;v90_toneb_samples_sent_=0;
    v90_second_ranging_round_=false;v90_remote_probe_seen_=false;v90_remote_l1_level_dbfs_=v90_remote_l2_level_dbfs_=-120.0;v90_remote_probe_start_abs_=0;v90_second_tone_a_seen_=false;v90_local_l2_tone_a_seen_=false;v90_info1a_seen_=false;v90_info1a_recovery_armed_=false;v90_info1d_retry_count_=0;v90_info1a_tone_samples_=0;v90_tone_retry_count_=0;
    v90_phase3_uinfo_=0;v90_phase3_md_length_35ms_=0;v90_phase3_upstream_symbol_rate_index_=0;v90_phase3_analogue_signal_seen_=false;v90_phase3_tone_a_samples_=0;v90_phase3_tone_a_history_.clear();v90_phase3_tx_.reset();v90_phase3_rx_.reset();v90_phase4_tx_.reset();v90_dil_descriptor_.reset();v90_mp_acknowledge_=false;v90_cp_prime_seen_=false;v90_dil_next_segment_=0;v90_dil_s_detected_abs_=0;v90_dil_stop_after_segment_=false;
    v92_short_phase2_active_=false;v92_short_phase2_reached_=false;v92_qc_lapm_=false;v92_toneq_deadline_tx_=0;
    span_v22_.reset(); span_v8_.reset();
    if(mode_==LiveMode::V21_300) start_v21();
    else if(mode_==LiveMode::V90Digital) start_v8(true);
    else if(mode_==LiveMode::V22bis_2400) start_v22(2400, V22LinkMode::V42Detect);
    else if(mode_==LiveMode::V22_1200) start_v22(1200, V22LinkMode::V42Detect);
    else if(mode_==LiveMode::V92QuickConnect) start_v92();
    else start_v8();
}
void LiveModem::end_call(){tx_pcmu_.clear();v21_tx_bits_.clear();span_v22_.reset();span_v8_.reset();v90_phase3_tx_.reset();v90_phase3_rx_.reset();v90_phase4_tx_.reset();v90_dil_descriptor_.reset();state_=LiveState::Idle;last_event_="call ended";}

void LiveModem::start_v8(bool v90_digital){
    tx_pcmu_.clear();
    if(!span_v8_available()){
        last_event_="auto: SpanDSP V.8 unavailable; using built-in V.21 300 fallback";
        start_v21();
        return;
    }
    span_v8_ = std::make_unique<SpanV8Answerer>(v90_digital ? V8Profile::V90Digital : V8Profile::V22Only);
    if(!span_v8_->start()){
        span_v8_.reset();
        last_event_="auto: V.8 startup failed; using V.21 300 fallback";
        start_v21();
        return;
    }
    v8_tx_hold_until_ = tx_samples_total_ + 2000; // 250 ms at 8 kHz
    if(v90_digital)
        set_state(LiveState::V8Negotiating,"V.90: 250 ms answer silence, V.8 ANSam; advertising DIGITAL V.90 plus truthful V.22 fallback");
    else
        set_state(LiveState::V8Negotiating,"auto: 250 ms answer silence, then V.8 ANSam; advertising V.22/V.22bis with V.42 LAPM and V.42bis compression");
}

void LiveModem::start_v90_phase2(){
    span_v8_.reset();
    v90_phase3_tx_.reset();
    v90_phase3_rx_.reset();
    v90_phase4_tx_.reset();
    v90_dil_descriptor_.reset();
    v90_mp_acknowledge_=false;v90_cp_prime_seen_=false;
    tx_pcmu_.clear();
    v90_rx_history_.clear();
    v92_short_phase2_active_=false;
    v90_info0a_sync_seen_=false;v90_info0a_ack_seen_=false;v90_info0d_ack_sent_=false;v90_info0a_bits_.clear();v90_tone_a_seen_=false;v90_first_tone_a_reversal_=false;
    v90_history_start_abs_=rx_samples_total_;v90_last_reversal_abs_=0;v90_last_diag_abs_=rx_samples_total_;
    v90_prev_tone_a_valid_=false;v90_toneb_phase_=0.0;v90_toneb_sign_=1;
    v90_reverse_countdown_=-1;v90_post_reverse_countdown_=-1;v90_toneb_samples_sent_=0;
    v90_second_ranging_round_=false;v90_remote_probe_seen_=false;v90_remote_l1_level_dbfs_=v90_remote_l2_level_dbfs_=-120.0;v90_remote_probe_start_abs_=0;v90_second_tone_a_seen_=false;v90_local_l2_tone_a_seen_=false;v90_info1a_seen_=false;v90_info1a_recovery_armed_=false;v90_info1d_retry_count_=0;v90_info1a_tone_samples_=0;v90_tone_retry_count_=0;
    v90_phase3_uinfo_=0;v90_phase3_md_length_35ms_=0;v90_phase3_upstream_symbol_rate_index_=0;v90_phase3_analogue_signal_seen_=false;v90_phase3_tone_a_samples_=0;v90_phase3_tone_a_history_.clear();v90_dil_next_segment_=0;v90_dil_s_detected_abs_=0;v90_dil_stop_after_segment_=false;
    // Do NOT add another 75-ms guard here. SpanDSP's answer-side V.8 state
    // machine already waits the post-CJ 75 ms before reporting V8_STATUS_V8_CALL.
    // V.90 Phase 2 starts immediately after that single guard interval.
    std::ostringstream ev;
    ev << "V.90 selected by V.8; post-CJ 75 ms guard already complete; remote CM pstn=0x"
       << std::hex << v90_v8_remote_pstn_access_ << " pcm=0x" << v90_v8_remote_pcm_availability_
       << std::dec << "; starting INFO0d immediately while listening for analogue INFO0a";
    set_state(LiveState::V90Phase2Silence,ev.str());
}

void LiveModem::start_v90_phase3(uint8_t uinfo, uint8_t upstream_symbol_rate_index, uint8_t md_length_35ms){
    tx_pcmu_.clear();
    span_v8_.reset();
    span_v22_.reset();
    v90_phase4_tx_.reset();
    v90_dil_descriptor_.reset();
    v90_mp_acknowledge_=false;
    v90_cp_prime_seen_=false;
    v90_phase3_tx_=std::make_unique<V90Phase3DigitalTx>(uinfo);
    v90_phase3_rx_=std::make_unique<V90Phase3AnalogueRx>(
        upstream_symbol_rate_index,md_length_35ms,-512);
    v90_phase3_uinfo_=uinfo;
    v90_phase3_md_length_35ms_=md_length_35ms;
    v90_phase3_upstream_symbol_rate_index_=upstream_symbol_rate_index;
    v90_phase3_analogue_signal_seen_=false;
    v90_phase3_tone_a_samples_=0;
    v90_phase3_tone_a_history_.clear();
    v90_dil_next_segment_=0;
    v90_dil_s_detected_abs_=0;
    v90_dil_stop_after_segment_=false;
    set_state(LiveState::V90Phase3WaitAnalogue,
              "V.90 Phase 3: started; upstream TRN equalizer/Ja decoder armed; downstream remains silent until Ja validates");
}

void LiveModem::start_v92(){
    v92_short_phase2_active_=false;v92_short_phase2_reached_=false;v92_qc_lapm_=false;v92_toneq_deadline_tx_=0;
    set_state(LiveState::V92AnswerSilence,"V.92 Quick Connect: 200 ms answer silence"); queue_silence(1600);
}

void LiveModem::start_v92_short_phase2(){
    tx_pcmu_.clear();v90_rx_history_.clear();v90_phase3_tx_.reset();
    v90_phase3_rx_.reset();v90_phase4_tx_.reset();v90_dil_descriptor_.reset();
    v90_mp_acknowledge_=false;v90_cp_prime_seen_=false;
    v92_short_phase2_active_=true;
    v90_info0a_sync_seen_=false;v90_info0a_ack_seen_=false;v90_info0d_ack_sent_=false;v90_info0a_bits_.clear();
    v90_tone_a_seen_=false;v90_first_tone_a_reversal_=false;
    v90_history_start_abs_=rx_samples_total_;v90_last_reversal_abs_=0;v90_last_diag_abs_=rx_samples_total_;
    v90_prev_tone_a_valid_=false;v90_toneb_phase_=0.0;v90_toneb_sign_=1;
    v90_reverse_countdown_=-1;v90_post_reverse_countdown_=-1;v90_toneb_samples_sent_=0;
    v90_second_ranging_round_=false;v90_second_tone_a_seen_=false;v90_info1a_seen_=false;v90_info1a_recovery_armed_=false;v90_info1d_retry_count_=0;v90_info1a_tone_samples_=0;v90_tone_retry_count_=0;
    v90_phase3_uinfo_=0;v90_phase3_md_length_35ms_=0;v90_phase3_upstream_symbol_rate_index_=0;v90_phase3_analogue_signal_seen_=false;v90_phase3_tone_a_samples_=0;v90_phase3_tone_a_history_.clear();v90_dil_next_segment_=0;v90_dil_s_detected_abs_=0;v90_dil_stop_after_segment_=false;
    set_state(LiveState::V90Phase2Silence,
              "V.92: TONEq guard complete; starting real short Phase 2 with V.92-capable INFO0d");
}
void LiveModem::start_v21(){
    auto_fallback_stage_=3;
    set_state(LiveState::V21ANS,"V.21: sending legacy ANS for 2.5 s"); queue_linear(build_ans_2100(2.5,true));
}

void LiveModem::start_v22(int bit_rate, V22LinkMode link_mode){
    tx_pcmu_.clear();
    v22_wait_for_lapm_ = (link_mode != V22LinkMode::TransparentAsync);
    v22_carrier_announced_ = false;
    v22_last_link_status_.clear();
    if(mode_==LiveMode::Auto || live_mode_allows_v90(mode_)) auto_fallback_stage_ = bit_rate==2400 ? 1 : 2;
    span_v8_.reset();
    span_v22_ = std::make_unique<SpanV22Modem>(bit_rate);
    if(!span_v22_available() || !span_v22_->start_answer(link_mode)){
        set_state(LiveState::V92Fallback,"V.22/V.22bis requested but SpanDSP support is unavailable");
        return;
    }
    std::string link = link_mode==V22LinkMode::V42Lapm ? "V.42 LAPM + V.42bis (negotiated by V.8)" :
                       link_mode==V22LinkMode::V42Detect ? "V.42 auto-detect + negotiated V.42bis" : "transparent async";
    set_state(LiveState::V22Training,
              std::string(bit_rate==2400 ? "V.22bis" : "V.22") +
              ": answer training, " + link);
}

void LiveModem::fallback_from_v22(const std::string& why){
    span_v22_.reset();
    if(mode_==LiveMode::Auto || live_mode_allows_v90(mode_)){
        if(auto_fallback_stage_ <= 1){
            start_v22(1200, V22LinkMode::V42Detect);
            last_event_=why+"; retrying as V.22 1200";
        } else {
            start_v21();
            last_event_=why+"; falling back to built-in V.21 300";
        }
    } else {
        set_state(LiveState::V92Fallback, why);
    }
}

static double tone_energy(const int16_t* x,size_t n,double f,int fs=8000){
    double c=0,s=0; for(size_t i=0;i<n;++i){double a=2*M_PI*f*i/fs;c+=x[i]*std::cos(a);s+=x[i]*std::sin(a);}return c*c+s*s;
}

bool LiveModem::detect_tone(const std::vector<int16_t>& p,double t,double o) const {
    if(p.size()<160)return false; double et=tone_energy(p.data(),p.size(),t), eo=tone_energy(p.data(),p.size(),o); return et>3.0*eo;
}

std::optional<std::pair<bool,uint8_t>> LiveModem::find_qc1a(){
    // QC1a is 60 bits at 300 bit/s = 1600 samples. Search all integer sample
    // phases and bit offsets in a short rolling history. This is intentionally
    // more tolerant than the nominal lab demodulator.
    if(rx_history_.size()<1700)return std::nullopt;
    const size_t keep=5200; if(rx_history_.size()>keep)rx_history_.erase(rx_history_.begin(),rx_history_.end()-keep);
    for(size_t sample_phase=0;sample_phase<27;++sample_phase){
        if(sample_phase>=rx_history_.size())break;
        std::vector<int16_t> v(rx_history_.begin()+sample_phase,rx_history_.end());
        auto bits=v21_demodulate_nominal(v,V21Band::Low);
        for(size_t bo=0;bo+60<=bits.size();++bo){
            std::vector<uint8_t> w(bits.begin()+bo,bits.begin()+bo+60);
            auto q=parse_qc1a(w); if(q)return std::make_pair(q->lapm,q->uqts);
        }
    }
    return std::nullopt;
}

void LiveModem::receive_pcm(const std::vector<int16_t>& pcm){
    if(pcm.empty()) return;
    const bool first_media = !inbound_media_seen_;
    inbound_media_seen_ = true;
    rx_samples_total_+=pcm.size();
    if(state_==LiveState::MediaWait){
        // We deliberately refused to guess a fallback while the return RTP path
        // was absent. As soon as genuine caller media appears, start the
        // standards negotiation from the beginning and feed this first block.
        start_v8(live_mode_allows_v90(mode_));
        if(state_==LiveState::V8Negotiating && span_v8_) span_v8_->receive_pcm(pcm);
        last_event_ = first_media ? "caller RTP appeared; restarting V.8 negotiation from real inbound audio" : "restarting V.8 negotiation";
        return;
    }
    if(state_==LiveState::V8Negotiating){
        if(!span_v8_){ fallback_from_v22("V.8 context vanished"); return; }
        size_t rem=span_v8_->receive_pcm(pcm);
        const auto result=span_v8_->result();
        if(result.done){
            std::vector<int16_t> tail;
            if(rem && rem<=pcm.size()) tail.assign(pcm.end()-rem,pcm.end());
            // MODE=v92 first attempts the shortened V.92 startup. If the
            // caller has no reusable Quick-Connect profile it must fall back
            // to the same full V.8/V.90 path as MODE=v90. Previously this
            // check accepted V.90 only for MODE=v90, so a valid V.90 result in
            // MODE=v92 was accidentally discarded in favour of V.22bis.
            if(live_mode_allows_v90(mode_) && result.v90 && !result.failed){
                v90_v8_remote_modulations_=result.remote_modulations;
                v90_v8_remote_pstn_access_=result.remote_pstn_access;
                v90_v8_remote_pcm_availability_=result.remote_pcm_modem_availability;
                if(!result.v90_pair_valid){
                    start_v22(2400, V22LinkMode::V42Detect);
                    std::ostringstream ev;
                    ev << "V.8 remote CM set V.90 but did not advertise analogue PCM-modem availability "
                       << "(pstn=0x" << std::hex << result.remote_pstn_access
                       << ", pcm=0x" << result.remote_pcm_modem_availability << std::dec
                       << "); V.90 analogue/digital pair is not established; using V.22bis fallback";
                    last_event_=ev.str();
                    if(!tail.empty() && span_v22_) span_v22_->receive_pcm(tail);
                } else {
                    start_v90_phase2();
                    if(!tail.empty()) feed_v90_phase2_rx(tail);
                }
            } else if((result.v22 || result.v34) && !result.failed){
                start_v22(2400, result.lapm ? V22LinkMode::V42Lapm
                                            : V22LinkMode::TransparentAsync);
                if(!tail.empty() && span_v22_) span_v22_->receive_pcm(tail);
                if(live_mode_allows_v90(mode_)) {
                    std::ostringstream ev;
                    ev << "peer negotiated " << (result.v34 ? "V.34" : "V.22bis/V.22")
                       << " in V.8 (remote CM modulations=0x" << std::hex
                       << result.remote_modulations << ", pstn=0x"
                       << result.remote_pstn_access << ", pcm=0x"
                       << result.remote_pcm_modem_availability << std::dec
                       << "); active fallback connection online";
                    last_event_=ev.str();
                }
            } else {
                span_v8_.reset();
                if((mode_==LiveMode::Auto || live_mode_allows_v90(mode_)) && span_v22_available()){
                    start_v22(2400, V22LinkMode::V42Detect);
                    last_event_="V.8 did not complete; trying direct V.22bis 2400 with V.42 detection";
                } else {
                    start_v21();
                    last_event_="V.8 did not complete; trying V.21 300 fallback";
                }
            }
        }
    } else if(state_==LiveState::V90Phase2Silence || state_==LiveState::V90INFO0d ||
              state_==LiveState::V90ToneB || state_==LiveState::V90ToneBReversed ||
              state_==LiveState::V90WaitSecondToneA || state_==LiveState::V90ToneRetryWaitToneA ||
              state_==LiveState::V90Phase2RttComplete ||
              state_==LiveState::V90RecvRemoteL1 || state_==LiveState::V90RecvRemoteL2 ||
              state_==LiveState::V90ProbeToneB || state_==LiveState::V90ProbeToneBReversed ||
              state_==LiveState::V90SendL1 || state_==LiveState::V90SendL2 ||
              state_==LiveState::V90INFO1d || state_==LiveState::V90WaitINFO1a){
        feed_v90_phase2_rx(pcm);
    } else if(state_==LiveState::V90Phase3WaitAnalogue ||
              state_==LiveState::V90Phase3SendSd ||
              state_==LiveState::V90Phase3SendTRN1d ||
              state_==LiveState::V90Phase3SendJd ||
              state_==LiveState::V90Phase3SendJdBar ||
              state_==LiveState::V90Phase3SendDIL ||
              state_==LiveState::V90Phase4WaitCPt ||
              state_==LiveState::V90Phase4SendTRN2d ||
              state_==LiveState::V90Phase4SendMP){
        // Keep the trained V.34 receiver alive after Ja. The analogue modem's
        // post-Jd S signal is the positive instruction to finish Jd and send
        // Jd-bar; listening only for Tone A here used to miss that instruction
        // and inevitably provoke a retrain.
        bool accepted_phase_transition=false;
        if(v90_phase3_rx_){
            const auto phase3=v90_phase3_rx_->feed(pcm);
            if(phase3.training_lock_new){
                v90_phase3_analogue_signal_seen_=true;
                std::ostringstream ev;ev<<std::fixed<<std::setprecision(2)
                    <<"V.90 Phase 3 RX: locked and equalizing client GPA-scrambled TRN (correlation="
                    <<phase3.training_correlation<<"); tracking client symbol clock until Ja is decoded";
                last_event_=ev.str();
            }
            if(phase3.ja_detected_new && v90_phase3_tx_){
                if(!phase3.dil_descriptor.valid){
                    set_state(LiveState::V90Fallback,
                              "V.90 Phase 3: Ja CRC passed but its DIL parameters could not be retained; refusing malformed training");
                    return;
                }
                v90_dil_descriptor_=std::make_unique<V90DILDescriptor>(
                    phase3.dil_descriptor);
                // V.90 9.3.1.3 permits up to 500 ms after receiving Ja. Use
                // most of that interoperability window now that the entire
                // CRC-valid descriptor (not merely its prefix) is available.
                queue_silence(3200);
                queue_pcmu(v90_phase3_tx_->sd_and_sbar_pcmu());
                std::ostringstream ev;
                ev<<"V.90 Phase 3 RX: complete CRC-valid "<<phase3.ja_descriptor_bits
                  <<"-bit Ja descriptor received at TRN symbol "<<phase3.ja_symbol
                  <<" (N="<<unsigned(phase3.dil_segment_count)
                  <<", symbol clock="<<std::showpos<<std::fixed<<std::setprecision(0)
                  <<phase3.symbol_clock_ppm<<std::noshowpos
                  <<" ppm); holding a 400-ms compatibility guard, then sending 384T Sd and 48T S-bar";
                set_state(LiveState::V90Phase3SendSd,ev.str());
            }
            if(phase3.s_detected_new && state_==LiveState::V90Phase3SendJd &&
               v90_phase3_tx_){
                accepted_phase_transition=true;
                // tx_pcmu_ contains at most the remainder of the current Jd;
                // append Jd-bar so the current frame is completed exactly as
                // required before the 12T terminating sequence.
                queue_pcmu(v90_phase3_tx_->jd_bar_pcmu());
                std::ostringstream ev;ev<<std::fixed<<std::setprecision(2)
                    <<"V.90 Phase 3: analogue S heard after Jd (correlation="
                    <<phase3.s_correlation
                    <<"); completing current Jd and sending 12T Jd-bar";
                set_state(LiveState::V90Phase3SendJdBar,ev.str());
            }else if(phase3.s_detected_new &&
                     state_==LiveState::V90Phase3SendDIL){
                accepted_phase_transition=true;
                // Transition to Ri immediately (no later than 24T after S-bar).
                // To preserve the V.90 downstream 6-sample data frame alignment, we must only
                // discard a multiple of 6 samples from the pending transmission buffer.
                const size_t keep = tx_pcmu_.size() % 6u;
                tx_pcmu_.erase(tx_pcmu_.begin() + keep, tx_pcmu_.end());
                if(v90_phase3_rx_)v90_phase3_rx_->arm_for_cpt();
                v90_last_diag_abs_=rx_samples_total_;
                queue_pcmu(v90_phase3_tx_->ri_pcmu(192,false));
                std::ostringstream ev;ev<<std::fixed<<std::setprecision(2)
                    <<"V.90 Phase 3: analogue S/S-bar detected (correlation="
                    <<phase3.s_correlation
                    <<"); aborting DIL, sending Phase-4 Ri, and waiting for CPt";
                set_state(LiveState::V90Phase4WaitCPt,ev.str());
            }
            if(phase3.cpt_detected_new &&
               state_==LiveState::V90Phase4WaitCPt && v90_phase3_tx_ &&
               !phase3.cpt_ordinary){
                v90_phase4_tx_=std::make_unique<V90Phase4DigitalTx>(
                    phase3.cpt_parameters);
                if(!v90_phase4_tx_->valid()){
                    set_state(LiveState::V90Fallback,
                              "V.90 Phase 4: CRC-valid CPt requested parameters that cannot be emitted on the PCMU path");
                    return;
                }
                const auto trn_and_mp=v90_phase4_tx_->start_trn2d_and_mp(2400u);
                if(trn_and_mp.empty()){
                    set_state(LiveState::V90Fallback,
                              "V.90 Phase 4: failed to build continuous TRN2d/MP stream from CRC-valid CPt");
                    return;
                }
                accepted_phase_transition=true;
                v90_mp_acknowledge_=false;
                v90_cp_prime_seen_=false;
                // CPt is the positive transition.  Complete R with 24T Ri-bar,
                // then transmit 2400T TRN2d and the first MP using one
                // continuous GPC/PCM encoder.  V.90 9.4.1.3 requires MP within
                // 2000 ms of the beginning of TRN2d.
                queue_pcmu(v90_phase3_tx_->ri_pcmu(24u,true));
                queue_pcmu(trn_and_mp);
                if(v90_phase3_rx_) v90_phase3_rx_->arm_for_cp();
                std::ostringstream ev;
                ev<<"[V90][RX] detected CPt: CRC-valid "<<phase3.cpt_bits
                  <<"-bit sequence (S="<<phase3.cpt_parameters.S
                  <<", K="<<phase3.cpt_parameters.K
                  <<", ld="<<phase3.cpt_parameters.ld
                  <<", a1="<<phase3.cpt_parameters.a1
                  <<", a2="<<phase3.cpt_parameters.a2
                  <<", b1="<<phase3.cpt_parameters.b1
                  <<", b2="<<phase3.cpt_parameters.b2
                  <<"); [V90][TX] sending 24T Ri-bar, 2400T TRN2d, then continuous CRC-valid Type-0 MP at "
                  <<v90_phase4_tx_->nominal_bit_rate()<<" bit/s";
                set_state(LiveState::V90Phase4SendTRN2d,ev.str());
            }
            if(phase3.cpt_detected_new && phase3.cpt_ordinary &&
               (state_==LiveState::V90Phase4SendTRN2d ||
                state_==LiveState::V90Phase4SendMP)){
                accepted_phase_transition=true;
                if(!phase3.cpt_acknowledge){
                    if(!v90_mp_acknowledge_){
                        v90_mp_acknowledge_=true;
                        last_event_="[V90][RX] detected ordinary CP: CRC-valid (ack=0); [V90][TX] completing current MP sequence and switching to MP' (ack=1)";
                    }
                    // Keep listening for the caller's CP' after it hears MP'.
                    if(v90_phase3_rx_) v90_phase3_rx_->arm_for_cp();
                }else{
                    v90_cp_prime_seen_=true;
                    if(v90_phase4_tx_){
                        const auto ed_b1d=v90_phase4_tx_->start_ed_and_b1d();
                        queue_pcmu(ed_b1d);
                        std::ostringstream ev;
                        ev<<"[V90][RX] detected CP': CRC-valid (ack=1); MP/CP exchange complete; [V90][TX] transmitting Sequence Ed (20 frames) and Sequence B1d (48 frames) at "
                          <<v90_phase4_tx_->nominal_bit_rate()<<" bit/s";
                        set_state(LiveState::V90Phase4SendEd,ev.str());
                    }
                }
            }
            if(state_==LiveState::V90Phase3WaitAnalogue &&
               rx_samples_total_>=v90_last_diag_abs_+8000u){
                v90_last_diag_abs_=rx_samples_total_;
                std::ostringstream ev;ev<<std::fixed<<std::setprecision(2)
                    <<"V.90 Phase 3: still equalizing client TRN (correlation="
                    <<phase3.training_correlation<<", clock="
                    <<(phase3.symbol_clock_ppm>=0?"+":"")<<phase3.symbol_clock_ppm
                    <<" ppm) and scanning for Ja";
                last_event_=ev.str();
            }
            if(state_==LiveState::V90Phase3SendJd &&
               rx_samples_total_>=v90_last_diag_abs_+8000u){
                v90_last_diag_abs_=rx_samples_total_;
                std::ostringstream ev;ev<<std::fixed<<std::setprecision(2)
                    <<"V.90 Phase 3: still repeating Jd and listening for the client's 128T S; best alternating-phase correlation="
                    <<phase3.s_correlation<<" (accept >=0.55)";
                last_event_=ev.str();
            }
            if(state_==LiveState::V90Phase4WaitCPt &&
               rx_samples_total_>=v90_last_diag_abs_+8000u){
                v90_last_diag_abs_=rx_samples_total_;
                std::ostringstream ev;ev<<std::fixed<<std::setprecision(2)
                    <<"V.90 Phase 4: still sending Ri and decoding CPt with retained TRN equalizer="
                    <<(phase3.cpt_equalizer_active?"yes":"no")
                    <<"; adaptive CPt re-equalizer=enabled"
                    <<"; raw CPt equalizer-bypass=enabled"
                    <<"; fixed CPt header="<<(phase3.cpt_header_seen?"seen":"not seen")
                    <<"; best mean QPSK decision error="
                    <<phase3.cpt_decision_error<<" rad";
                last_event_=ev.str();
            }
            if((state_==LiveState::V90Phase4SendTRN2d ||
                state_==LiveState::V90Phase4SendMP) &&
               rx_samples_total_>=v90_last_diag_abs_+8000u){
                v90_last_diag_abs_=rx_samples_total_;
                std::ostringstream ev;ev<<std::fixed<<std::setprecision(2)
                    <<"V.90 Phase 4: waiting for CRC-valid CP/CP'; header="
                    <<(phase3.cpt_header_seen?"seen":"not seen")
                    <<"; best mean QPSK decision error="
                    <<phase3.cpt_decision_error
                    <<" rad; Tone-A retrain detector=armed";
                last_event_=ev.str();
            }
            // Actively decoding or receiving valid Phase 3/4 sequences suppresses false Tone A retrains.
            if((state_==LiveState::V90Phase4WaitCPt && phase3.cpt_header_seen) ||
               (state_==LiveState::V90Phase3WaitAnalogue && phase3.training_locked) ||
               phase3.cpt_detected_new || phase3.ja_detected_new || phase3.s_detected_new)
                accepted_phase_transition=true;
        }
        // V.90 9.3.1/9.5.1.2: Tone A held for at least 50 ms is a retrain
        // request. First give the trained V.34 receiver the same samples: S
        // and CPt are valid transitions and can contain a strong 2400-Hz bin.
        if(accepted_phase_transition){
            v90_phase3_tone_a_samples_=0;
            v90_phase3_tone_a_history_.clear();
        }else{
            // The Recommendation requires one Tone A held for 50 ms, not
            // three unrelated 20-ms blocks which happen to have energy in
            // the 2400-Hz bin. Preserve phase across RTP packet boundaries
            // and test the complete 400-sample interval coherently.
            v90_phase3_tone_a_history_.insert(
                v90_phase3_tone_a_history_.end(),pcm.begin(),pcm.end());
            if(v90_phase3_tone_a_history_.size()>400u)
                v90_phase3_tone_a_history_.erase(
                    v90_phase3_tone_a_history_.begin(),
                    v90_phase3_tone_a_history_.end()-400u);
            const bool held=v90_phase3_tone_a_history_.size()==400u &&
                v90_retrain_tone_present(v90_phase3_tone_a_history_);
            v90_phase3_tone_a_samples_=held?400u:0u;
        }
        if(v90_phase3_tone_a_samples_>=400){
            v90_tone_retry_count_=0;
            begin_v90_retrain(
                "V.90 Phase 3/4: client Tone A retrain request held for 50 ms; transmitting the required 70 ms silence before Tone B");
            return;
        }
    } else if(state_==LiveState::V92ANSam){
        rx_history_.insert(rx_history_.end(),pcm.begin(),pcm.end());
        // V.92 9.2.4.1 requires the digital answer modem to detect QC1a,
        // QC1d, or ordinary V.8 CM while ANSam is on the line. Keep a real
        // V.8 answerer running in parallel with the QC1a detector so a caller
        // without a saved profile can continue its already-started CM/JM/CJ
        // exchange instead of having CM discarded by a five-second timeout.
        if(span_v8_) (void)span_v8_->receive_pcm(pcm);
        if(auto q=find_qc1a()){
            span_v8_.reset();tx_pcmu_.clear(); v92_qc_lapm_=q->first;v92_uqts_=q->second; QCA1d a{q->first,-12}; auto bits=build_qca1d_bits(a); queue_linear(v21_modulate(bits,V21Band::High));
            set_state(LiveState::V92QCA1d,"V.92: QC1a detected; sending QCA1d, UQTS="+std::to_string(q->second));
        } else if(span_v8_ && span_v8_->result().cm_detected){
            // Do not call start_v8(): it would throw away the CM which was
            // just decoded and begin another answer-tone cycle. SpanDSP has
            // already queued the matching JM in this same V.8 context.
            tx_pcmu_.clear();v8_tx_hold_until_=0;
            set_state(LiveState::V8Negotiating,
                      "V.92: ordinary CM detected (no usable QC profile); continuing the same live V.8 exchange with JM, not restarting ANSam");
        }
    } else if(state_==LiveState::V92ANSpcm){
        toneq_history_.insert(toneq_history_.end(),pcm.begin(),pcm.end());
        if(toneq_history_.size()>800)toneq_history_.erase(toneq_history_.begin(),toneq_history_.end()-800);
        if(detect_toneq_980(toneq_history_)){
            tx_pcmu_.clear(); queue_silence(600); set_state(LiveState::V92PostToneqSilence,"V.92: valid >=50 ms TONEq 980 Hz detected; sending 75 ms guard");
        }
    } else if(state_==LiveState::V22Training || state_==LiveState::V22Data){
        if(span_v22_){
            span_v22_->receive_pcm(pcm);
            const std::string link_status=span_v22_->link_status();
            if(state_==LiveState::V22Training && link_status!=v22_last_link_status_){
                v22_last_link_status_=link_status;
                if(link_status.find("ODP detected")!=std::string::npos)
                    last_event_=link_status;
                else if(link_status.find("LAPM flags")!=std::string::npos)
                    last_event_=link_status;
                else if(link_status.find("LAPM XID")!=std::string::npos ||
                        link_status.find("LAPM SABME")!=std::string::npos ||
                        link_status.find("bad CRC")!=std::string::npos)
                    last_event_=link_status;
            }
            auto b=span_v22_->take_bytes();
            if(!b.empty())ppp_rx_bytes_.insert(ppp_rx_bytes_.end(),b.begin(),b.end());
            if(span_v22_->failed()) {
                fallback_from_v22("V.22/V.22bis training failed");
            } else if(state_==LiveState::V22Training && span_v22_->connected()) {
                // The V.22 carrier can be trained before V.42/LAPM finishes.
                // Starting PPP at carrier-up races LAPM and can make the peer look
                // completely silent even though the modem reports CONNECT.
                const bool link_ready = !v22_wait_for_lapm_ ||
                                        span_v22_->lapm_connected() ||
                                        span_v22_->transparent_mode();
                if(link_ready) {
                    set_state(LiveState::V22Data,"V.22/V.22bis CONNECT "+std::to_string(span_v22_->current_bit_rate())+
                              ": "+span_v22_->link_status()+"; data link ready; PPP byte mode active");
                } else if(!v22_carrier_announced_) {
                    v22_carrier_announced_ = true;
                    last_event_ = "V.22/V.22bis carrier trained at "+
                                  std::to_string(span_v22_->current_bit_rate())+
                                  "; waiting for V.42/LAPM data link";
                }
            } else if(state_==LiveState::V22Training && tx_samples_total_-state_tx_start_>20*8000ULL) {
                fallback_from_v22(v22_carrier_announced_ ?
                                  "V.22 carrier up but V.42/LAPM did not reach data state" :
                                  "V.22/V.22bis training timeout");
            }
        }
    } else if(state_==LiveState::V21Carrier){
        // Answering side waits for the calling modem's low-channel mark at 980 Hz.
        if(detect_tone(pcm,980.0,1180.0))++v21_mark_blocks_; else v21_mark_blocks_=0;
        if(v21_mark_blocks_>=10){set_state(LiveState::V21Data,"V.21 CONNECT 300: carrier established; PPP byte mode active");}
    } else if(state_==LiveState::V21Data){
        feed_v21_rx(pcm);
    } else if(state_==LiveState::V90Data){
        if(v90_phase3_rx_){
            auto bytes=v90_phase3_rx_->demodulate_data(pcm);
            if(!bytes.empty()){
                ppp_rx_bytes_.insert(ppp_rx_bytes_.end(),bytes.begin(),bytes.end());
            }
        }
    }
}

static double v90_sine_peak_for_dbm0(double dbm0);

void LiveModem::send_v90_info0d(const std::string& event){
    tx_pcmu_.clear();
    V90Info0dConfig c;
    c.acknowledge_info0a=v90_info0a_sync_seen_;
    c.request_short_phase2=v92_short_phase2_active_;
    c.v92_capable=v92_short_phase2_active_;
    auto bits=build_v90_info0d_bits(c);
    queue_linear(v90_info_dbpsk_modulate(bits,1200.0,
                                         v90_sine_peak_for_dbm0(c.nominal_tx_power_dbm0)));
    if(c.acknowledge_info0a)v90_info0d_ack_sent_=true;
    set_state(LiveState::V90INFO0d,event);
}

void LiveModem::send_v90_info1d(const std::string& event){
    tx_pcmu_.clear();
    const auto c=v90_info1d_config_from_info0a(v90_info0a_bits_);
    queue_linear(v90_info_dbpsk_modulate(build_v90_info1d_bits(c),1200.0,
                                         v90_sine_peak_for_dbm0(-12.0)));
    v90_info1a_recovery_armed_=false;
    v90_info1a_tone_samples_=0;
    set_state(LiveState::V90INFO1d,event);
}

void LiveModem::begin_v90_retrain(const std::string& event){
    tx_pcmu_.clear();
    v90_phase3_tx_.reset();v90_phase3_rx_.reset();v90_phase4_tx_.reset();
    v90_mp_acknowledge_=false;v90_cp_prime_seen_=false;
    v90_rx_history_.clear();v90_history_start_abs_=rx_samples_total_;
    v90_tone_a_seen_=true;v90_first_tone_a_reversal_=false;
    v90_prev_tone_a_valid_=false;v90_last_reversal_abs_=rx_samples_total_;
    // A retrain starts with the first Tone-B exchange.  Carrying the old
    // second-ranging flag caused the observed jump straight to L1/INFO1d.
    v90_second_ranging_round_=false;v90_second_tone_a_seen_=false;
    v90_remote_probe_seen_=false;v90_remote_probe_start_abs_=0;
    v90_remote_l1_level_dbfs_=v90_remote_l2_level_dbfs_=-120.0;
    v90_local_l2_tone_a_seen_=false;v90_info1a_seen_=false;
    v90_info1a_recovery_armed_=false;v90_info1d_retry_count_=0;
    v90_info1a_tone_samples_=0;v90_phase3_tone_a_samples_=0;v90_phase3_tone_a_history_.clear();
    v90_toneb_phase_=0.0;v90_toneb_sign_=1;v90_toneb_samples_sent_=0;
    v90_reverse_countdown_=-1;v90_post_reverse_countdown_=-1;
    v92_short_phase2_active_=false;
    queue_silence(560);
    set_state(LiveState::V90RetrainSilence,event);
}

void LiveModem::maybe_advance_tx_state(){
    if(!tx_pcmu_.empty())return;
    switch(state_){
        case LiveState::V90Phase2Silence:
            send_v90_info0d(v92_short_phase2_active_ ?
                "V.92 short Phase 2: transmitting INFO0d with V.92 and short-phase capability bits" :
                "V.90: transmitting standards-shaped INFO0d at 600 bit/s DBPSK / 1200 Hz"); break;
        case LiveState::V90INFO0d:
            if(v90_info0a_sync_seen_ && !v90_info0d_ack_sent_){
                send_v90_info0d(v92_short_phase2_active_ ?
                    "V.92 short Phase 2: repeating INFO0d to acknowledge CRC-valid INFO0a" :
                    "V.90: repeating INFO0d with INFO0a acknowledgement set");
            }else{
                v90_toneb_samples_sent_=0;
                set_state(LiveState::V90ToneB,v92_short_phase2_active_ ?
                    "V.92 short Phase 2: INFO0d complete; transmitting Tone B, waiting for Tone A" :
                    "V.90: INFO0d complete; transmitting 1200-Hz Tone B, waiting for analogue Tone A reversal");
            }
            break;
        case LiveState::V90SendL1:
            queue_linear(v34_line_probe(0.500,-12.0));
            set_state(LiveState::V90SendL2,"V.90: local L1 complete; transmitting V.34 L2 at nominal Phase-2 power while waiting for Tone A"); break;
        case LiveState::V90INFO1d:
            v90_rx_history_.clear();v90_history_start_abs_=rx_samples_total_;
            set_state(LiveState::V90WaitINFO1a,"V.90: INFO1d complete; sending silence and waiting for CRC-valid analogue INFO1a"); break;
        case LiveState::V90Phase3SendSd:
            if(v90_phase3_tx_){
                // MD is optional; we omit it. PP is the two-symbol phase
                // reference for TRN1d/Jd. TRN1d follows PP.
                queue_pcmu(v90_phase3_tx_->pp_and_trn1d_pcmu(4800));
                set_state(LiveState::V90Phase3SendTRN1d,
                          "V.90 Phase 3: S/S-bar complete; sending PP and 4800T TRN1d (2040T minimum plus VoIP receiver-training margin)");
            }
            break;
        case LiveState::V90Phase3SendTRN1d:
            if(v90_phase3_tx_){
                queue_pcmu(v90_phase3_tx_->jd_frame_pcmu());
                if(v90_phase3_rx_)v90_phase3_rx_->arm_for_s();
                v90_last_diag_abs_=rx_samples_total_;
                set_state(LiveState::V90Phase3SendJd,
                          "V.90 Phase 3: TRN1d complete; repeating CRC-valid Jd capability frames; trained V.34 receiver armed for analogue S");
            }
            break;
        case LiveState::V90Phase3SendJd:
            if(v90_phase3_tx_)queue_pcmu(v90_phase3_tx_->jd_frame_pcmu());
            break;
        case LiveState::V90Phase3SendJdBar:
            if(!v90_phase3_tx_ || !v90_dil_descriptor_ ||
               !v90_dil_descriptor_->valid){
                set_state(LiveState::V90Fallback,
                          "V.90 Phase 3: missing retained Ja parameters after Jd-bar");
                break;
            }
            if(v90_dil_descriptor_->segment_count==0){
                if(v90_phase3_rx_)v90_phase3_rx_->arm_for_cpt();
                v90_last_diag_abs_=rx_samples_total_;
                queue_pcmu(v90_phase3_tx_->ri_pcmu(192,false));
                set_state(LiveState::V90Phase4WaitCPt,
                          "V.90 Phase 3 complete with N=0; sending Phase-4 Ri and waiting for analogue CPt");
            }else{
                v90_dil_next_segment_=0;
                v90_dil_s_detected_abs_=0;
                v90_dil_stop_after_segment_=false;
                // Skip the immediate 16T S-bar response to Jd-bar. The next S
                // after this guard is the caller's positive DIL-completion
                // signal from 9.3.2.10.
                if(v90_phase3_rx_)v90_phase3_rx_->arm_for_s(320,true);
                const auto segment=v90_phase3_tx_->dil_segment_pcmu(
                    *v90_dil_descriptor_,v90_dil_next_segment_++);
                if(segment.empty()){
                    set_state(LiveState::V90Fallback,
                              "V.90 Phase 3: retained Ja requested an invalid DIL segment");
                }else{
                    queue_pcmu(segment);
                    std::ostringstream ev;
                    ev<<"V.90 Phase 3: Jd-bar complete; transmitting the requested "
                      <<unsigned(v90_dil_descriptor_->segment_count)
                      <<"-segment DIL and listening for the subsequent analogue S/S-bar";
                    set_state(LiveState::V90Phase3SendDIL,ev.str());
                }
            }
            break;
        case LiveState::V90Phase3SendDIL:
            if(!v90_phase3_tx_ || !v90_dil_descriptor_){
                set_state(LiveState::V90Fallback,"V.90 Phase 3: DIL state lost");
                break;
            }
            {
                const size_t index=v90_dil_next_segment_++ %
                                   v90_dil_descriptor_->segment_count;
                const auto segment=v90_phase3_tx_->dil_segment_pcmu(
                    *v90_dil_descriptor_,index);
                if(segment.empty())
                    set_state(LiveState::V90Fallback,
                              "V.90 Phase 3: invalid DIL segment while repeating the requested sequence");
                else
                    queue_pcmu(segment);
            }
            break;
        case LiveState::V90Phase4WaitCPt:
            // Ri must continue for at least 192T and until CPt is received.
            if(v90_phase3_tx_)queue_pcmu(v90_phase3_tx_->ri_pcmu(192,false));
            break;
        case LiveState::V90Phase4SendTRN2d:
            if(v90_phase4_tx_){
                const auto mp=v90_phase4_tx_->next_mp(v90_mp_acknowledge_);
                if(mp.empty()){
                    set_state(LiveState::V90Fallback,
                              "V.90 Phase 4: continuous MP encoder stopped after TRN2d");
                }else{
                    queue_pcmu(mp);
                    set_state(LiveState::V90Phase4SendMP,
                              "V.90 Phase 4: TRN2d complete; repeating Type-0 MP while receiving ordinary CP/CP'");
                }
            }
            break;
        case LiveState::V90Phase4SendMP:
            if(v90_phase4_tx_){
                const auto mp=v90_phase4_tx_->next_mp(v90_mp_acknowledge_);
                if(mp.empty())
                    set_state(LiveState::V90Fallback,
                              "V.90 Phase 4: continuous MP/MP' encoder failed");
                else
                    queue_pcmu(mp);
            }
            break;
        case LiveState::V90Phase4SendEd:
            if(v90_phase4_tx_){
                std::ostringstream ev;
                ev << "[V90] entering V90Data: Sequence Ed/B1d complete; PCM downstream "
                   << v90_phase4_tx_->nominal_bit_rate()
                   << " bit/s; PPP data mode ACTIVE";
                set_state(LiveState::V90Data,ev.str());
            }
            break;
        case LiveState::V90RetrainSilence:
            v90_toneb_phase_=0.0;v90_toneb_sign_=1;v90_toneb_samples_sent_=0;
            v90_reverse_countdown_=-1;v90_post_reverse_countdown_=-1;
            set_state(LiveState::V90ToneB,
                      "V.90 retrain: 70 ms silence complete; transmitting Tone B and waiting for the client Tone A reversal");
            break;
        case LiveState::V92AnswerSilence:
            // Use the same mature V.8 state machine for ANSam and ordinary CM
            // fallback. The separate QC1a detector observes the same RX audio.
            // Portable builds without SpanDSP retain the waveform-only QC lab
            // path, but shipping Windows builds use the live dual detector.
            if(span_v8_available()){
                span_v8_=std::make_unique<SpanV8Answerer>(V8Profile::V90Digital);
                if(!span_v8_->start())span_v8_.reset();
            }
            if(!span_v8_)queue_linear(build_ansam(5.0));
            set_state(LiveState::V92ANSam,
                      span_v8_ ?
                      "V.92: ANSam active; listening concurrently for QC1a or ordinary V.8 CM" :
                      "V.92: ANSam active; waiting for QC1a (SpanDSP CM detector unavailable)");break;
        case LiveState::V92QCA1d:
            v92_toneq_deadline_tx_=tx_samples_total_+2*8000ULL;
            queue_silence(600);set_state(LiveState::V92Silence75,"V.92: 75 ms silence after QCA1d; TONEq recovery timer armed");break;
        case LiveState::V92Silence75:
            {auto q=build_qts_ulaw(v92_uqts_);for(auto x:q)tx_pcmu_.push_back(x);set_state(LiveState::V92QTS,"V.92: sending QTS/QTS-bar");}break;
        case LiveState::V92QTS:
            {auto a=build_anspcm_ulaw(-12,2.0);for(auto x:a)tx_pcmu_.push_back(x);toneq_history_.clear();set_state(LiveState::V92ANSpcm,"V.92: ANSpcm active; waiting up to 2 seconds after QCA1d for TONEq");}break;
        case LiveState::V92PostToneqSilence:
            start_v92_short_phase2();break;
        case LiveState::V21ANS:
            queue_silence(600);set_state(LiveState::V21Silence75,"V.21: 75 ms silence after ANS");break;
        case LiveState::V21Silence75:
            set_state(LiveState::V21Carrier,"V.21: transmitting 1650-Hz answer-channel mark; waiting for 980-Hz caller mark");break;
        default:break;
    }
}

static double v90_sine_peak_for_dbm0(double dbm0){
    // SpanDSP/G.711 convention: a full-scale sine is about +3.14 dBm0.
    return 32768.0*std::pow(10.0,(dbm0-3.14)/20.0);
}

static double coherent_peak_dbfs(const std::vector<int16_t>& pcm,double hz){
    if(pcm.empty())return -120.0;
    double re=0.0,im=0.0;
    for(size_t i=0;i<pcm.size();++i){
        const double ph=2.0*M_PI*hz*static_cast<double>(i)/8000.0;
        re+=static_cast<double>(pcm[i])*std::cos(ph);
        im-=static_cast<double>(pcm[i])*std::sin(ph);
    }
    const double peak=2.0*std::hypot(re,im)/static_cast<double>(pcm.size());
    return peak>1e-9 ? 20.0*std::log10(peak/32768.0) : -120.0;
}

static double rms_dbfs(const std::vector<int16_t>& pcm){
    if(pcm.empty())return -120.0;
    long double sum=0.0;for(auto x:pcm){const long double v=x;sum+=v*v;}
    const double rms=std::sqrt(static_cast<double>(sum/pcm.size()));
    return rms>1e-9 ? 20.0*std::log10(rms/32768.0) : -120.0;
}

uint8_t LiveModem::next_v90_toneb_ulaw(){
    // Digital/server side Phase 2 Tone B. In both ranging exchanges the
    // digital modem reverses B 40 ms after the received A reversal and
    // continues B for another 10 ms.
    // V.92 9.4 short Phase 2 omits the first A/B round-trip measurement: once
    // Tone A is present and B has lasted at least 50 ms, reverse B directly.
    if(v92_short_phase2_active_ && state_==LiveState::V90ToneB &&
       !v90_second_ranging_round_ &&
       v90_tone_a_seen_ && v90_reverse_countdown_<0 &&
       v90_toneb_samples_sent_>=400){
        v90_reverse_countdown_=0;
    }
    if(v90_reverse_countdown_==0){
        v90_toneb_sign_=-v90_toneb_sign_;v90_reverse_countdown_=-1;v90_post_reverse_countdown_=80;
        if(v90_second_ranging_round_)
            set_state(LiveState::V90ProbeToneBReversed,"V.90: second Tone-B phase reversal transmitted after 40 ms; holding 10 ms before local L1/L2");
        else
            set_state(LiveState::V90ToneBReversed,v92_short_phase2_active_ ?
                "V.92 short Phase 2: reversed Tone B after at least 50 ms; holding 10 ms" :
                "V.90: transmitted Tone B phase reversal after 40 ms; holding 10 ms");
    }
    if(v90_reverse_countdown_>0)--v90_reverse_countdown_;
    if(v90_post_reverse_countdown_==0){
        v90_post_reverse_countdown_=-1;
        if(v90_second_ranging_round_){
            tx_pcmu_.clear();
            queue_linear(v34_line_probe(0.160,-6.0));
            set_state(LiveState::V90SendL1,"V.90: second ranging exchange complete; transmitting exact V.34 L1 for 160 ms at +6 dB");
        }else{
            if(v92_short_phase2_active_){
                // Discard pre-reversal Tone A so the following search cannot
                // rediscover an old phase transition as the client's response.
                v90_rx_history_.clear();v90_history_start_abs_=rx_samples_total_;
                v90_last_reversal_abs_=rx_samples_total_;v90_first_tone_a_reversal_=true;
                set_state(LiveState::V90WaitSecondToneA,
                          "V.92 short Phase 2: Tone B reversed and stopped after 10 ms; waiting for client Tone A reversal");
            }else if(v90_second_tone_a_seen_){
                set_state(LiveState::V90Phase2RttComplete,
                          "V.90: SECOND Tone A phase reversal detected during Tone-B tail; RTT/ranging exchange complete; receiving remote L1/L2 next");
            }else{
                set_state(LiveState::V90WaitSecondToneA,"V.90: Tone B stopped after reversal; waiting for second analogue Tone A reversal (RTT)");
            }
        }
        return 0xFF;
    }
    if(v90_post_reverse_countdown_>0)--v90_post_reverse_countdown_;
    const double phase2_peak=v90_sine_peak_for_dbm0(-12.0);
    int16_t x=static_cast<int16_t>(std::lround(v90_toneb_sign_*phase2_peak*std::sin(v90_toneb_phase_)));
    v90_toneb_phase_+=2*M_PI*1200.0/8000.0; if(v90_toneb_phase_>2*M_PI)v90_toneb_phase_=std::fmod(v90_toneb_phase_,2*M_PI);
    ++v90_toneb_samples_sent_;
    return linear_to_ulaw(x);
}

void LiveModem::feed_v90_phase2_rx(const std::vector<int16_t>& pcm){
    if(pcm.empty())return;
    v90_rx_history_.insert(v90_rx_history_.end(),pcm.begin(),pcm.end());
    constexpr size_t kKeep=20000; // 2.5 s at 8 kHz
    if(v90_rx_history_.size()>kKeep){
        const size_t drop=v90_rx_history_.size()-kKeep;
        v90_rx_history_.erase(v90_rx_history_.begin(),v90_rx_history_.begin()+drop);
        v90_history_start_abs_+=drop;
    }

    // V.90 9.2.1.2.3 recovery: if the second Tone-A reversal was lost,
    // remain silent until the analogue/client modem presents Tone A again,
    // then restart the Tone-B ranging exchange. Keep this bounded so a dead
    // endpoint cannot hold the call in a retry loop forever.
    if(state_==LiveState::V90ToneRetryWaitToneA){
        if(observe_v90_tone(pcm,2400.0).present){
            tx_pcmu_.clear();
            v90_rx_history_.clear();v90_history_start_abs_=rx_samples_total_;
            v90_last_reversal_abs_=rx_samples_total_;
            v90_tone_a_seen_=true;v90_first_tone_a_reversal_=false;
            v90_prev_tone_a_valid_=false;
            v90_second_ranging_round_=false;
            v90_toneb_phase_=0.0;v90_toneb_sign_=1;
            v90_reverse_countdown_=-1;v90_post_reverse_countdown_=-1;
            std::ostringstream ev;
            ev << "V.90: client Tone A reacquired; retrying Tone-B ranging exchange ("
               << v90_tone_retry_count_ << "/2)";
            set_state(LiveState::V90ToneB,ev.str());
        }
        return;
    }

    // First INFO exchange: the analogue modem sends INFO0a while we send INFO0d.
    if(!v90_info0a_sync_seen_){
        if(auto f=find_v90_info0a(v90_rx_history_)){
            v90_info0a_sync_seen_=true;
            v90_info0a_ack_seen_=f->acknowledge_info0d;
            v90_info0a_bits_=f->bits;
            if(v92_short_phase2_active_ && !(f->v92_capable && f->requests_short_phase2)){
                v92_short_phase2_active_=false;
                last_event_="V.92: peer INFO0a did not confirm short Phase 2; continuing with full V.90 Phase 2";
            }else{
                last_event_=std::string(v92_short_phase2_active_ ?
                            "V.92: CRC-valid INFO0a confirmed short Phase 2" :
                            "V.90: CRC-valid analogue INFO0a received")+
                            (v90_info0a_ack_seen_?"; peer ACKed INFO0d":"; ACK bit not yet set");
            }
            // The digital modem must acknowledge a correct INFO0a in INFO0d.
            // If the initial frame was already sent, repeat it now with bit 28.
            if(!v90_info0d_ack_sent_ &&
               (state_==LiveState::V90ToneB || state_==LiveState::V90ToneBReversed)){
                send_v90_info0d(v92_short_phase2_active_ ?
                    "V.92 short Phase 2: repeating INFO0d to acknowledge CRC-valid INFO0a" :
                    "V.90: repeating INFO0d with INFO0a acknowledgement set");
                return;
            }
        }
    }

    if(state_==LiveState::V90WaitSecondToneA && v34_line_probe_present(pcm)){
        v90_second_tone_a_seen_=true;
        v90_remote_probe_seen_=true;
        v90_remote_probe_start_abs_=rx_samples_total_-pcm.size();
        v90_remote_l1_level_dbfs_=v34_line_probe_metric_dbfs(pcm);
        set_state(LiveState::V90RecvRemoteL1,
                  "V.90: remote V.34 L1 probe arrived during ranging wait; advancing to receive remote L1/L2");
        return;
    }

    // After the first ranging exchange the analogue modem sends V.34 L1 for
    // 160 ms and then L2. Measure the actual multitone instead of guessing from
    // elapsed time alone.
    if(state_==LiveState::V90Phase2RttComplete){
        v90_remote_probe_seen_=false;
        v90_remote_l1_level_dbfs_=v90_remote_l2_level_dbfs_=-120.0;
        v90_remote_probe_start_abs_=0;
        set_state(LiveState::V90RecvRemoteL1,
                  "V.90: ranging complete; conditioned receiver for remote V.34 L1 (160 ms) then L2 line probe");
    }
    if(state_==LiveState::V90RecvRemoteL1){
        const bool probe=v34_line_probe_present(pcm);
        const double level=v34_line_probe_metric_dbfs(pcm);
        if(probe){
            if(!v90_remote_probe_seen_){
                v90_remote_probe_seen_=true;
                v90_remote_probe_start_abs_=rx_samples_total_-pcm.size();
                v90_remote_l1_level_dbfs_=level;
                std::ostringstream ev;ev<<std::fixed<<std::setprecision(1)
                    <<"V.90: remote V.34 L1 probe detected at "<<level<<" dBFS; measuring 160-ms L1→L2 level step";
                last_event_=ev.str();
            }else{
                v90_remote_l1_level_dbfs_=std::max(v90_remote_l1_level_dbfs_,level);
            }
        }
        if(v90_remote_probe_seen_){
            const uint64_t elapsed=rx_samples_total_-v90_remote_probe_start_abs_;
            // L1 is exactly 160 ms (1280 samples). Once 160 ms elapsed or level stepped, advance to L2.
            if(elapsed>=1200 && (level<=v90_remote_l1_level_dbfs_-3.0 || elapsed>=1600 || !probe)){
                v90_remote_l2_level_dbfs_=level;
                std::ostringstream ev;ev<<std::fixed<<std::setprecision(1)
                    <<"V.90: remote L2 detected after L1; L1="<<v90_remote_l1_level_dbfs_
                    <<" dBFS L2="<<v90_remote_l2_level_dbfs_<<" dBFS; preparing second Tone-B exchange";
                set_state(LiveState::V90RecvRemoteL2,ev.str());
            }
        } else if(rx_samples_total_-state_rx_start_>=3200){ // 400 ms timeout
            v90_second_ranging_round_=true;
            v90_second_tone_a_seen_=false;
            v90_toneb_phase_=0.0;v90_toneb_sign_=1;v90_reverse_countdown_=-1;v90_post_reverse_countdown_=-1;
            v90_rx_history_.clear();v90_history_start_abs_=rx_samples_total_;v90_last_reversal_abs_=rx_samples_total_;
            set_state(LiveState::V90ProbeToneB,
                      "V.90: remote probing interval elapsed; transmitting Tone B for second ranging exchange and waiting for Tone A reversal");
        }
        return;
    }
    if(state_==LiveState::V90RecvRemoteL2){
        if(v34_line_probe_present(pcm)) v90_remote_l2_level_dbfs_=v34_line_probe_metric_dbfs(pcm);
        if(rx_samples_total_-state_rx_start_>=800){ // >=100 ms of L2 is enough to measure
            v90_second_ranging_round_=true;
            v90_second_tone_a_seen_=false;
            v90_toneb_phase_=0.0;v90_toneb_sign_=1;v90_reverse_countdown_=-1;v90_post_reverse_countdown_=-1;
            v90_rx_history_.clear();v90_history_start_abs_=rx_samples_total_;v90_last_reversal_abs_=rx_samples_total_;
            set_state(LiveState::V90ProbeToneB,
                      "V.90: remote L1/L2 received; transmitting Tone B for second ranging exchange and waiting for Tone A reversal");
        }
        return;
    }

    // Second Tone A/B reversal exchange. This time, after B is reversed and
    // held for 10 ms, the digital modem sends its own L1 followed by L2.
    if(state_==LiveState::V90ProbeToneB || state_==LiveState::V90ProbeToneBReversed){
        auto obs=observe_v90_tone(pcm,2400.0);
        if(obs.present && !v90_second_tone_a_seen_){
            v90_second_tone_a_seen_=true;
            last_event_="V.90: second-exchange analogue Tone A detected; waiting for its phase reversal";
        }
        if(auto rel=find_v90_phase_reversal(v90_rx_history_,2400.0,8000,80)){
            const uint64_t abs=v90_history_start_abs_+*rel;
            if(abs>state_rx_start_+80 && abs>v90_last_reversal_abs_+160){
                v90_last_reversal_abs_=abs;
                v90_reverse_countdown_=320;
                last_event_="V.90: second-exchange Tone A phase reversal detected; scheduling Tone B reversal in 40 ms";
            }
        }
        return;
    }

    // During our local L1/L2 the analogue modem returns to Tone A. Once L1 is
    // complete and L2 has been observed for a useful interval, transmit INFO1d.
    if(state_==LiveState::V90SendL1 || state_==LiveState::V90SendL2){
        auto obs=observe_v90_tone(pcm,2400.0);
        if(obs.present && !v90_local_l2_tone_a_seen_){
            v90_local_l2_tone_a_seen_=true;
            last_event_="V.90: analogue Tone A detected during local L1/L2 probe";
        }
        if(state_==LiveState::V90SendL2 && v90_local_l2_tone_a_seen_ && rx_samples_total_-state_rx_start_>=800){
            send_v90_info1d(
                "V.90: local L1/L2 probing complete; transmitting CRC-valid 109-bit INFO1d at 1200 Hz with only INFO0a-supported V.34 symbol rates");
        }
        return;
    }

    if(state_==LiveState::V90WaitINFO1a){
        if(!v90_info1a_seen_){
            if(auto f=find_v90_info1a_phase2(v90_rx_history_)){
                v90_info1a_seen_=true;
                static const char* sr_name[]={"2400 (unsupported)","2743 (unsupported)","2800 (unsupported)","3000","3200","3429"};
                const unsigned raw_sr=f->upstream_symbol_rate_index;
                const unsigned effective_sr = raw_sr < 3 ? 4 : (raw_sr > 5 ? 4 : raw_sr);
                const bool was_short=v92_short_phase2_active_;
                v92_short_phase2_active_=false;
                if(was_short)v92_short_phase2_reached_=true;

                std::ostringstream ev;
                ev << "[MODEM] INFO1a decoded: modulation=" << (f->requests_v90 ? "V.90 downstream PCM" : "V.34 downstream QAM")
                   << ", upstream symbol rate=" << (raw_sr < 6 ? sr_name[raw_sr] : "unknown")
                   << " (index " << raw_sr << " -> effective index " << effective_sr << ")"
                   << ", UINFO=" << unsigned(f->uinfo)
                   << ", MD length=" << unsigned(f->md_length_35ms)*35 << " ms"
                   << ", PCM upstream=" << (f->pcm_upstream ? "yes" : "no")
                   << "; entering V.34/V.90 Phase 3 with trained V34QamDemodulator";
                last_event_ = ev.str();

                if(f->pcm_upstream){
                    std::ostringstream ev_pcm;
                    ev_pcm<<"V.92: CRC-valid INFO1a completed short Phase 2; upstream=PCM, downstream=PCM, UINFO="
                          <<unsigned(f->uinfo)<<(v92_qc_lapm_?", client requested LAPM":"")
                          <<"; PCM-upstream Phase 3 DSP is not implemented";
                    set_state(LiveState::V92ShortPhase2Reached,ev_pcm.str());
                }else{
                    v90_phase3_tx_=std::make_unique<V90Phase3DigitalTx>(f->uinfo);
                    v90_phase3_rx_=std::make_unique<V90Phase3AnalogueRx>(
                        effective_sr, f->md_length_35ms,
                        f->frequency_offset_x002_hz);
                    if(!v90_phase3_tx_->valid() || !v90_phase3_rx_->valid()){
                        v90_phase3_tx_.reset();
                        v90_phase3_rx_.reset();
                        set_state(LiveState::V90Fallback,
                                  "INFO1a requested unsupported Phase-3 parameters; refusing to emit malformed training");
                        return;
                    }
                    v90_phase3_uinfo_=f->uinfo;
                    v90_phase3_md_length_35ms_=f->md_length_35ms;
                    v90_phase3_upstream_symbol_rate_index_=effective_sr;
                    v90_phase3_analogue_signal_seen_=false;
                    v90_phase3_tone_a_samples_=0;
                    v90_phase3_tone_a_history_.clear();

                    std::ostringstream ev_p3;
                    ev_p3<<(was_short?"V.92 short Phase 2":"V.90 Phase 2")
                         <<" complete; receiving analogue Phase-3 S/PP/TRN/Ja at "
                         <<(effective_sr<6?sr_name[effective_sr]:"unknown")<<" symbols/s, MD="
                         <<unsigned(f->md_length_35ms)*35<<" ms, UINFO="<<unsigned(f->uinfo)
                         <<"; upstream TRN equalizer/Ja decoder armed; downstream remains silent until Ja validates; V34QamDemodulator armed for data";
                    set_state(LiveState::V90Phase3WaitAnalogue,ev_p3.str());
                }
            }
        }
        if(state_==LiveState::V90WaitINFO1a && v90_info1a_recovery_armed_){
            if(v90_infomarksa_present(pcm)){
                v90_info1a_tone_samples_=0;
                if(v90_info1d_retry_count_<2){
                    ++v90_info1d_retry_count_;
                    std::ostringstream ev;
                    ev<<"V.90 recovery: INFOMARKSa received; repeating INFO1d ("
                      <<v90_info1d_retry_count_<<"/2)";
                    send_v90_info1d(ev.str());
                }else if(v90_tone_retry_count_<2){
                    ++v90_tone_retry_count_;
                    begin_v90_retrain(
                        "V.90 recovery: repeated INFOMARKSa after two INFO1d repeats; sending 70 ms silence and restarting the full Tone-B ranging exchange");
                }else{
                    set_state(LiveState::V90Fallback,
                              "V.90 INFO1a recovery exhausted after two INFO1d repeats and two retrains; stopping cleanly");
                }
                return;
            }
            if(v90_retrain_tone_present(pcm))
                v90_info1a_tone_samples_+=pcm.size();
            else
                v90_info1a_tone_samples_=0;
            if(v90_info1a_tone_samples_>=400){
                if(v90_tone_retry_count_<2){
                    ++v90_tone_retry_count_;
                    begin_v90_retrain(
                        "V.90 recovery: client Tone A retrain request held for 50 ms; transmitting 70 ms silence before a clean first Tone-B exchange");
                }else{
                    set_state(LiveState::V90Fallback,
                              "V.90 retrain request repeated after two clean retries; stopping this handshake cleanly");
                }
                return;
            }
        }
        return;
    }

    // First Tone A presence and ranging exchange.
    auto obs=observe_v90_tone(pcm,2400.0);
    if(obs.present && !v90_tone_a_seen_){
        v90_tone_a_seen_=true;
        last_event_="V.90: analogue Tone A detected (2400 Hz + expected guard)";
        if(!v90_info0a_sync_seen_ && state_==LiveState::V90ToneB){
            send_v90_info0d(v92_short_phase2_active_ ?
                "V.92 short Phase 2: Tone A arrived before valid INFO0a; repeating INFO0d" :
                "V.90: Tone A arrived before valid INFO0a; repeating INFO0d per recovery procedure");
            return;
        }
    }

    if(rx_samples_total_ >= v90_last_diag_abs_ + 4000 && last_event_.empty()){
        v90_last_diag_abs_=rx_samples_total_;
        std::ostringstream ev;
        ev << std::fixed << std::setprecision(1)
           << "V.90 RX spectrum: rms=" << rms_dbfs(pcm) << " dBFS"
           << ", 1200=" << coherent_peak_dbfs(pcm,1200.0) << " dBFS"
           << ", 1800=" << coherent_peak_dbfs(pcm,1800.0) << " dBFS"
           << ", 2400=" << coherent_peak_dbfs(pcm,2400.0) << " dBFS"
           << (v90_info0a_sync_seen_ ? "; INFO0a=CRC-OK" : "; INFO0a=not locked");
        last_event_=ev.str();
    }

    size_t min_rel=0;
    if(v90_last_reversal_abs_>v90_history_start_abs_){
        const uint64_t diff=v90_last_reversal_abs_-v90_history_start_abs_+80;
        if(diff<v90_rx_history_.size()) min_rel=static_cast<size_t>(diff);
    }
    if(auto rel=find_v90_phase_reversal(v90_rx_history_,2400.0,8000,80,min_rel)){
        const uint64_t abs=v90_history_start_abs_+*rel;
        const uint64_t min_separation=80;
        if(abs>v90_last_reversal_abs_+min_separation){
            if(!v92_short_phase2_active_ && !v90_first_tone_a_reversal_ &&
               (state_==LiveState::V90ToneB || state_==LiveState::V90INFO0d)){
                v90_last_reversal_abs_=abs;
                v90_first_tone_a_reversal_=true;
                v90_reverse_countdown_=320;
                last_event_="V.90: FIRST Tone A phase reversal detected; scheduling Tone B reversal in 40 ms";
            }else if(v90_first_tone_a_reversal_){
                v90_last_reversal_abs_=abs;
                v90_second_tone_a_seen_=true;
                if(state_==LiveState::V90WaitSecondToneA){
                    if(v92_short_phase2_active_){
                        v90_rx_history_.clear();v90_history_start_abs_=rx_samples_total_;
                        set_state(LiveState::V90WaitINFO1a,
                                  "V.92 short Phase 2: client Tone A reversal detected; waiting directly for INFO1a (L1/L2 and INFO1d are omitted)");
                    }else{
                        set_state(LiveState::V90Phase2RttComplete,
                                  "V.90: SECOND Tone A phase reversal detected; RTT/ranging exchange complete; receiving remote L1/L2 next");
                    }
                }
            }
        }
    }
}

uint8_t LiveModem::next_v21_tx_ulaw(){
    if(v21_tx_bit_phase_>=1.0){v21_tx_bit_phase_-=1.0;if(!v21_tx_bits_.empty()){v21_tx_current_bit_=v21_tx_bits_.front();v21_tx_bits_.pop_front();}else v21_tx_current_bit_=1;}
    const double f=v21_tx_current_bit_?1650.0:1850.0;
    int16_t s=static_cast<int16_t>(std::lround(6500.0*std::sin(v21_tx_phase_)));
    v21_tx_phase_+=2*M_PI*f/8000.0;if(v21_tx_phase_>2*M_PI)v21_tx_phase_=std::fmod(v21_tx_phase_,2*M_PI);
    v21_tx_bit_phase_+=300.0/8000.0;
    return linear_to_ulaw(s);
}

std::vector<uint8_t> LiveModem::next_tx_pcmu(size_t n){
    tx_samples_total_ += n;
    if(state_==LiveState::V8Negotiating && span_v8_){
        // Give the hardware modem a clean post-answer quiet interval before ANSam.
        // We still feed RX audio during this time, so CI/calling tones are not lost.
        if(tx_samples_total_ <= v8_tx_hold_until_) return std::vector<uint8_t>(n, 0xFF);
        // SpanDSP's answer-side V.8 CM timer advances in v8_rx(). If RTP is
        // one-way, v8_rx() never runs. Enforce the same timeout from our own
        // continuously-running 8 kHz TX clock so ANSam can never hang forever.
        if(tx_samples_total_-state_tx_start_ > 6*8000ULL){
            span_v8_.reset();
            if(!inbound_media_seen_){
                // Never infer a remote modem standard from silence / missing
                // packets. Blind fallback was the cause of misleading mode
                // changes on one-way RTP calls. Go quiet and wait until an
                // actual RTP packet from the caller is observed.
                tx_pcmu_.clear();
                set_state(LiveState::MediaWait,
                          "no caller RTP received; refusing blind modem fallback and waiting for return audio");
                return std::vector<uint8_t>(n, 0xFF);
            }
            if((mode_==LiveMode::Auto || live_mode_allows_v90(mode_)) && span_v22_available()){
                start_v22(2400, V22LinkMode::V42Detect);
                last_event_="V.8 timed out after real caller RTP; trying V.22bis 2400 with V.42 detection";
                tx_samples_total_-=n;
                return next_tx_pcmu(n);
            }
            start_v21();
            last_event_="V.8 timed out after real caller RTP; trying V.21 300 fallback";
            tx_samples_total_-=n;
            return next_tx_pcmu(n);
        }
        auto pcm=span_v8_->next_tx_pcm(n); std::vector<uint8_t> u;u.reserve(pcm.size());
        for(auto x:pcm)u.push_back(linear_to_ulaw(x)); return u;
    }
    if(state_==LiveState::MediaWait) return std::vector<uint8_t>(n, 0xFF);
    if(state_==LiveState::V92ANSam && tx_samples_total_-state_tx_start_>5*8000ULL){
        span_v8_.reset();
        if(!inbound_media_seen_){
            tx_pcmu_.clear();
            set_state(LiveState::MediaWait,
                      "V.92: neither QC1a nor caller RTP/CM arrived; waiting silently for return audio");
        }else if(span_v22_available()){
            start_v22(2400,V22LinkMode::V42Detect);
            last_event_="V.92: neither QC1a nor a decodable ordinary CM arrived during ANSam; trying direct V.22bis";
        }else{
            start_v21();
            last_event_="V.92: neither QC1a nor a decodable ordinary CM arrived during ANSam; trying V.21";
        }
        tx_samples_total_-=n;
        return next_tx_pcmu(n);
    }
    if(state_==LiveState::V92ANSam && span_v8_){
        auto pcm=span_v8_->next_tx_pcm(n);std::vector<uint8_t> u;u.reserve(pcm.size());
        for(auto x:pcm)u.push_back(linear_to_ulaw(x));return u;
    }
    if(state_==LiveState::V92ANSpcm && v92_toneq_deadline_tx_ &&
       tx_samples_total_>=v92_toneq_deadline_tx_){
        start_v8(true);
        last_event_="V.92: no valid TONEq within 2 seconds after QCA1d; transmitting ANSam and falling back to full V.8/V.90 startup";
        return std::vector<uint8_t>(n,0xFF);
    }
    if(state_==LiveState::V90ToneB || state_==LiveState::V90ToneBReversed ||
       state_==LiveState::V90ProbeToneB || state_==LiveState::V90ProbeToneBReversed){
        // V.90 9.2.1.2.2 explicitly requires the digital modem to continue
        // Tone B until the first Tone-A reversal is detected. Do not invent an
        // 8-second fallback timeout in the explicit V.90 lab mode.
        // In the second exchange the standard supplies a recovery path when
        // the client's Tone-A reversal is missed: after about 900 ms plus RTT,
        // schedule our B reversal anyway, then continue with L1/L2. We do not
        // yet retain a precise RTD estimate, so one second is a conservative
        // bound for the local SIP/RTP path.
        if(state_==LiveState::V90ProbeToneB && v90_reverse_countdown_<0 &&
           tx_samples_total_-state_tx_start_ > 8000ULL){
            v90_reverse_countdown_=320;
            last_event_="V.90: client second-exchange Tone A reversal timed out; applying recovery and scheduling Tone B reversal in 40 ms";
        }
        std::vector<uint8_t> u;u.reserve(n);
        for(size_t i=0;i<n;++i){
            if(state_==LiveState::V90ToneB || state_==LiveState::V90ToneBReversed ||
               state_==LiveState::V90ProbeToneB || state_==LiveState::V90ProbeToneBReversed){
                u.push_back(next_v90_toneb_ulaw());
            }else if(!tx_pcmu_.empty()){
                u.push_back(tx_pcmu_.front());tx_pcmu_.pop_front();
            }else{
                u.push_back(0xFF);
            }
        }
        return u;
    }
    if(state_==LiveState::V90WaitSecondToneA){
        const uint64_t limit=v92_short_phase2_active_ ? 2500ULL*8ULL : 2*8000ULL;
        if(tx_samples_total_-state_tx_start_ > limit){
            if(v90_tone_retry_count_<2){
                ++v90_tone_retry_count_;
                const bool was_short=v92_short_phase2_active_;
                v92_short_phase2_active_=false;
                tx_pcmu_.clear();v90_rx_history_.clear();v90_history_start_abs_=rx_samples_total_;
                v90_tone_a_seen_=false;v90_prev_tone_a_valid_=false;
                std::ostringstream ev;
                ev << (was_short ? "V.92 short Phase 2" : "V.90")
                   << ": client Tone A reversal timed out; waiting for Tone A, then retrying full Tone-B ranging ("
                   << v90_tone_retry_count_ << "/2)";
                set_state(LiveState::V90ToneRetryWaitToneA,ev.str());
                return std::vector<uint8_t>(n,0xFF);
            }
            v92_short_phase2_active_=false;
            set_state(LiveState::V90Fallback,
                      "client Tone A ranging failed after 2 retries; stopping this handshake cleanly so a new call can renegotiate its modem mode");
            return std::vector<uint8_t>(n,0xFF);
        }
        return std::vector<uint8_t>(n,0xFF);
    }
    if(state_==LiveState::V90ToneRetryWaitToneA){
        if(tx_samples_total_-state_tx_start_ > 5*8000ULL){
            set_state(LiveState::V90Fallback,
                      "retry timed out waiting for client Tone A; stopping this handshake cleanly so a new call can renegotiate");
            return std::vector<uint8_t>(n,0xFF);
        }
        return std::vector<uint8_t>(n,0xFF);
    }
    if(state_==LiveState::V90Phase3SendJd &&
       tx_samples_total_-state_tx_start_ > 6*8000ULL){
        double correlation=0.0;
        if(v90_phase3_rx_)correlation=v90_phase3_rx_->observation().s_correlation;
        std::ostringstream ev;ev<<std::fixed<<std::setprecision(2)
            <<"V.90 Phase 3: no valid client S within the Jd response window (best correlation="
            <<correlation<<"); performing the required full retrain instead of repeating Jd forever";
        begin_v90_retrain(ev.str());
        tx_samples_total_-=n;
        return next_tx_pcmu(n);
    }
    if(state_==LiveState::V90Phase3SendDIL &&
       tx_samples_total_-state_tx_start_ > 6*8000ULL){
        begin_v90_retrain(
            "V.90 Phase 3: no subsequent S/S-bar within the DIL completion window; performing a full retrain");
        tx_samples_total_-=n;
        return next_tx_pcmu(n);
    }
    if(state_==LiveState::V90Phase4WaitCPt &&
       tx_samples_total_-state_tx_start_ > 5000ULL*8ULL){
        std::ostringstream ev;
        ev<<std::fixed<<std::setprecision(2)
          <<"V.90 Phase 4: no CRC-valid CPt within 5 seconds";
        if(v90_phase3_rx_){
            const auto cpt=v90_phase3_rx_->observation();
            ev<<" (retained equalizer="<<(cpt.cpt_equalizer_active?"yes":"no")
              <<", fixed header="<<(cpt.cpt_header_seen?"seen":"not seen")
              <<", best decision error="<<cpt.cpt_decision_error<<" rad)";
        }
        ev<<"; initiating immediate clean retrain";
        begin_v90_retrain(ev.str());
        tx_samples_total_-=n;
        return next_tx_pcmu(n);
    }
    // Echo bypass for CP/CP'
    // Without a digital echo canceller, the server's strong MP transmission often
    // drowns out the client's upstream CP/CP', causing the server to wait forever.
    // Instead of waiting, we proactively sequence the state machine.
    if(state_==LiveState::V90Phase4SendMP) {
        const uint64_t elapsed = tx_samples_total_ - state_tx_start_;
        if(!v90_mp_acknowledge_ && elapsed > 1200ULL * 8ULL) {
            v90_mp_acknowledge_ = true;
            last_event_ = "[V90][TX] bypassing CP reception due to echo; unilaterally switching to MP' (ack=1)";
        } else if (v90_mp_acknowledge_ && elapsed > 2400ULL * 8ULL) {
            v90_cp_prime_seen_ = true;
            if(v90_phase4_tx_) {
                const auto ed_b1d = v90_phase4_tx_->start_ed_and_b1d();
                // NEVER clear tx_pcmu_ here! It truncates the active MP' sequence and
                // misaligns the strict 6-sample downstream PCM data framing boundary,
                // causing the client modem to immediately retrain!
                queue_pcmu(ed_b1d);
                std::ostringstream ev;
                ev << "[V90][TX] bypassing CP' reception due to echo; MP/CP exchange complete; transmitting Sequence Ed and Sequence B1d at "
                   << v90_phase4_tx_->nominal_bit_rate() << " bit/s";
                set_state(LiveState::V90Phase4SendEd, ev.str());
            }
            tx_samples_total_ -= n;
            return next_tx_pcmu(n);
        }
    }
    if((state_==LiveState::V90Phase4SendTRN2d || state_==LiveState::V90Phase4SendMP) &&
       tx_samples_total_-state_tx_start_ > 6000ULL*8ULL){
        std::ostringstream ev;
        ev<<"V.90 Phase 4: CP/CP' parameter exchange timed out after 6 seconds; performing clean retrain";
        begin_v90_retrain(ev.str());
        tx_samples_total_-=n;
        return next_tx_pcmu(n);
    }
    if(state_==LiveState::V90Phase3WaitAnalogue){
        // Receive-driven gate: elapsed time or random line energy can never
        // start the digital transmitter. receive_pcm() moves state only after
        // the client TRN has trained the equalizer and Ja has validated.
        return std::vector<uint8_t>(n,0xFF);
    }
    if(state_==LiveState::V90Phase2RttComplete || state_==LiveState::V90RecvRemoteL1 ||
       state_==LiveState::V90RecvRemoteL2 || state_==LiveState::V90WaitINFO1a ||
       state_==LiveState::V90Phase2Complete){
        if((state_==LiveState::V90RecvRemoteL1 || state_==LiveState::V90RecvRemoteL2) &&
           tx_samples_total_-state_tx_start_ > 4*8000ULL){
            set_state(LiveState::V90Fallback,
                      "V.90 timed out waiting for remote L1/L2 line probe; a fresh call is required for lower-speed renegotiation");
            return std::vector<uint8_t>(n,0xFF);
        }
        if(state_==LiveState::V90WaitINFO1a){
            const uint64_t elapsed=tx_samples_total_-state_tx_start_;
            if(v92_short_phase2_active_ && elapsed>2500ULL*8ULL){
                if(v90_tone_retry_count_<2){
                    ++v90_tone_retry_count_;
                    begin_v90_retrain(
                        "V.92 short Phase 2: INFO1a timeout; sending 70 ms silence and retrying a clean full V.90 Tone-B ranging exchange");
                }else{
                    v92_short_phase2_active_=false;
                    set_state(LiveState::V90Fallback,
                              "V.92 short Phase-2 INFO1a failed after two clean full-Phase-2 retries; stopping cleanly");
                }
                return std::vector<uint8_t>(n,0xFF);
            }
            if(!v92_short_phase2_active_ && !v90_info1a_recovery_armed_ &&
               elapsed>2*8000ULL){
                // V.90 9.2.1.2.6: after the INFO1a timer expires the digital
                // modem conditions its receiver for Tone A or INFOMARKSa. It
                // must not blindly transmit another Tone B at this boundary.
                v90_info1a_recovery_armed_=true;
                v90_info1a_tone_samples_=0;
                v90_rx_history_.clear();v90_history_start_abs_=rx_samples_total_;
                set_state(LiveState::V90WaitINFO1a,
                          "V.90: INFO1a recovery timer expired; remaining silent and listening for client Tone A or INFOMARKSa");
                return std::vector<uint8_t>(n,0xFF);
            }
            if(!v92_short_phase2_active_ && v90_info1a_recovery_armed_ &&
               elapsed>5*8000ULL){
                if(v90_tone_retry_count_<2){
                    ++v90_tone_retry_count_;
                    begin_v90_retrain(
                        "V.90 recovery: neither Tone A nor INFOMARKSa arrived within 5 seconds; sending 70 ms silence and restarting a clean Tone-B exchange");
                }else{
                    set_state(LiveState::V90Fallback,
                              "V.90 INFO1a recovery remained silent after two retrains; stopping cleanly");
                }
                return std::vector<uint8_t>(n,0xFF);
            }
        }
        return std::vector<uint8_t>(n,0xFF);
    }
    if(state_==LiveState::V90SendL2 && tx_pcmu_.empty() && tx_samples_total_-state_tx_start_ > 5600ULL){
        set_state(LiveState::V90Fallback,
                  "V.90 local L2 reached its Phase-2 limit without return Tone A; a fresh call is required for lower-speed renegotiation");
        return std::vector<uint8_t>(n,0xFF);
    }
    if(state_==LiveState::V90Fallback) return std::vector<uint8_t>(n,0xFF);
    if(state_==LiveState::V90Data && v90_phase4_tx_){
        return v90_phase4_tx_->produce_data_pcmu(n);
    }
    if((state_==LiveState::V22Training||state_==LiveState::V22Data) && span_v22_){
        auto pcm=span_v22_->next_tx_pcm(n); std::vector<uint8_t> u;u.reserve(pcm.size());
        for(auto x:pcm)u.push_back(linear_to_ulaw(x)); return u;
    }
    maybe_advance_tx_state(); std::vector<uint8_t> out;out.reserve(n);
    for(size_t i=0;i<n;++i){
        if(state_==LiveState::V21Carrier||state_==LiveState::V21Data) out.push_back(next_v21_tx_ulaw());
        else { if(tx_pcmu_.empty())maybe_advance_tx_state(); if(!tx_pcmu_.empty()){out.push_back(tx_pcmu_.front());tx_pcmu_.pop_front();}else out.push_back(0xFF); }
    }
    return out;
}

void LiveModem::feed_ppp_bytes(const std::vector<uint8_t>& bytes){
    if(bytes.empty())return;
    if(state_==LiveState::V90Data && v90_phase4_tx_){v90_phase4_tx_->feed_async_bytes(bytes);return;}
    if(state_==LiveState::V22Data && span_v22_){span_v22_->feed_bytes(bytes);return;}
    if(state_!=LiveState::V21Data)return; auto bits=async_encode(bytes); for(auto b:bits)v21_tx_bits_.push_back(b);
}

static bool decode_fixed_async_frame(const std::vector<uint8_t>& bits,size_t i,uint8_t& ch){
    if(i+10>bits.size() || bits[i]!=0 || bits[i+9]!=1)return false;
    ch=0;for(int b=0;b<8;++b)ch|=static_cast<uint8_t>((bits[i+1+b]&1)<<b);return true;
}

void LiveModem::feed_v21_rx(const std::vector<int16_t>& pcm){
    v21_data_pcm_.insert(v21_data_pcm_.end(),pcm.begin(),pcm.end());
    if(v21_data_pcm_.size()<320)return;

    if(!v21_rx_synced_){
        size_t best_score=0,best_phase=0,best_off=0,best_frames=0;
        for(size_t phase=0;phase<27 && phase<v21_data_pcm_.size();++phase){
            std::vector<int16_t> x(v21_data_pcm_.begin()+phase,v21_data_pcm_.end());
            auto bits=v21_demodulate_nominal(x,V21Band::Low);
            for(size_t off=0;off<10;++off){
                size_t valid=0,total=0;
                for(size_t i=off;i+10<=bits.size();i+=10){uint8_t ch=0;++total;if(decode_fixed_async_frame(bits,i,ch))++valid;}
                // Need multiple frames before locking; weight valid frames and
                // penalize invalid framing so idle bits don't win by accident.
                size_t score=valid*4-(total>valid?std::min<size_t>(valid*3,total-valid):0);
                if(valid>=2 && score>best_score){best_score=score;best_phase=phase;best_off=off;best_frames=total;}
            }
        }
        if(best_score==0)return;
        v21_rx_synced_=true;v21_rx_sample_phase_=best_phase;v21_rx_bit_offset_=best_off;v21_rx_frames_emitted_=0;
    }

    std::vector<int16_t> x(v21_data_pcm_.begin()+v21_rx_sample_phase_,v21_data_pcm_.end());
    auto bits=v21_demodulate_nominal(x,V21Band::Low);
    size_t frame_no=0;
    for(size_t i=v21_rx_bit_offset_;i+10<=bits.size();i+=10,++frame_no){
        if(frame_no<v21_rx_frames_emitted_)continue;
        uint8_t ch=0;if(decode_fixed_async_frame(bits,i,ch))ppp_rx_bytes_.push_back(ch);
        v21_rx_frames_emitted_=frame_no+1;
    }

    // Periodically compact only at an exact 10-bit frame boundary. Keep a few
    // frames of history so a noisy packet does not force an immediate relock.
    if(v21_rx_frames_emitted_>512){
        size_t drop_frames=v21_rx_frames_emitted_-32;
        double drop_samples_d=drop_frames*10.0*8000.0/300.0;
        size_t drop_samples=static_cast<size_t>(std::llround(drop_samples_d));
        if(drop_samples<v21_data_pcm_.size()){
            v21_data_pcm_.erase(v21_data_pcm_.begin(),v21_data_pcm_.begin()+drop_samples);
            v21_rx_frames_emitted_-=drop_frames;
            v21_rx_sample_phase_=0; // boundary is rounded to nearest sample
        }
    }
}

std::vector<uint8_t> LiveModem::take_ppp_bytes(){auto r=std::move(ppp_rx_bytes_);ppp_rx_bytes_.clear();return r;}

} // namespace v92
