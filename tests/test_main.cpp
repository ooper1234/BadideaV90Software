#include "v92_quickconnect.hpp"
#include "v90_phase2.hpp"
#include "v90_pcm.hpp"
#include "v21.hpp"
#include "v23.hpp"
#include "g711.hpp"
#include "async_serial.hpp"
#include "answer_tones.hpp"
#include "modem_modes.hpp"
#include "ppp_backend.hpp"
#include "live_modem.hpp"
#include "span_v22.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <iostream>
#include <set>
#include <algorithm>
#include <cmath>

int main(){
    using namespace v92;
    assert(ucode_to_ulaw(0,true)==0xFF);
    assert(ucode_to_ulaw(61,true)==0xC2);
    assert(ucode_to_ulaw(127,true)==0x80);
    assert(ucode_to_ulaw(61,false)==0x42);

    // Real V.90 Phase-2 primitives: INFO0d is the exact 62-bit frame shape,
    // its V.34 CRC validates, and 600-bps DBPSK round-trips at the digital
    // modem's 1200-Hz INFO carrier.
    auto i0d=build_v90_info0d_bits();
    assert(i0d.size()==62);
    assert(i0d[0]==1 && i0d[1]==1 && i0d[2]==1 && i0d[3]==1);
    const uint8_t i0sync[8]={0,1,1,1,0,0,1,0};
    for(int i=0;i<8;++i)assert(i0d[4+i]==i0sync[i]);
    assert(check_v90_info0d_crc(i0d));
    auto i0pcm=v90_info_dbpsk_modulate(i0d,1200.0);
    auto i0rx=v90_info_dbpsk_demodulate(i0pcm,1200.0);
    assert(i0rx.size()>=i0d.size());
    for(size_t i=0;i<i0d.size();++i)assert(i0rx[i]==i0d[i]);
    // Bit 19 is one when 3429 transmission is allowed (zero disallows it).
    assert(i0d[19]==1);

    // Real INFO0a receiver: arbitrary RTP-relative start plus the mandatory
    // 1800-Hz guard tone must still produce a complete CRC-valid frame.
    auto i0a=build_v90_info0a_bits(true);
    assert(i0a.size()==49 && check_v90_info0a_crc(i0a));
    auto i0aw=v90_info0a_waveform(i0a);
    std::vector<int16_t> shifted(137,0); shifted.insert(shifted.end(),i0aw.begin(),i0aw.end()); shifted.insert(shifted.end(),211,0);
    auto found_i0a=find_v90_info0a(shifted);
    assert(found_i0a && found_i0a->valid && found_i0a->acknowledge_info0d);

    auto ta=v90_tone(2400.0,0.040,false), tar=v90_tone(2400.0,0.040,true);
    auto oa=observe_v90_tone(ta,2400.0), oar=observe_v90_tone(tar,2400.0);
    assert(oa.present && oar.present && v90_phase_reversed(oa,oar));
    assert(v90_retrain_tone_present(ta));
    // Sliding reversal finder must work even when the transition is not on an
    // RTP/160-sample boundary and Tone A carries its 1800-Hz guard.
    std::vector<int16_t> rev;
    for(size_t n=0;n<777;++n){
        double sign=n<413?1.0:-1.0;
        double y=sign*5800.0*std::sin(2.0*3.14159265358979323846*2400.0*n/8000.0)+6500.0*std::sin(2.0*3.14159265358979323846*1800.0*n/8000.0);
        rev.push_back(static_cast<int16_t>(std::lround(y)));
    }
    auto rp=find_v90_phase_reversal(rev);
    assert(rp && *rp>390 && *rp<435);

    // Phase-2 continuation: exact INFO1 frame sizes/CRCs and V.34 L1/L2
    // line-probe waveform. INFO1a must be found at an arbitrary RTP offset.
    auto i1d=build_v90_info1d_bits();
    assert(i1d.size()==109 && check_v90_info1d_crc(i1d));
    auto i1dpcm=v90_info_dbpsk_modulate(i1d,1200.0);
    auto i1drx=v90_info_dbpsk_demodulate(i1dpcm,1200.0);
    assert(i1drx.size()>=i1d.size());
    for(size_t i=0;i<i1d.size();++i)assert(i1drx[i]==i1d[i]);

    auto i1a=build_v90_info1a_phase2_bits(true,3,80);
    assert(i1a.size()==70 && check_v90_info1a_phase2_crc(i1a));
    auto i1aw=v90_info0a_waveform(i1a);
    std::vector<int16_t> i1shift(91,0);i1shift.insert(i1shift.end(),i1aw.begin(),i1aw.end());i1shift.insert(i1shift.end(),127,0);
    auto found_i1a=find_v90_info1a_phase2(i1shift);
    assert(found_i1a && found_i1a->valid && found_i1a->requests_v90);
    assert(found_i1a->upstream_symbol_rate_index==3 && found_i1a->uinfo==80);

    auto l1=v34_line_probe(0.160,-6.0);
    auto l2=v34_line_probe(0.200,-12.0);
    assert(l1.size()==1280 && l2.size()==1600);
    assert(v34_line_probe_present(l1) && v34_line_probe_present(l2));
    const double l1db=v34_line_probe_metric_dbfs(l1), l2db=v34_line_probe_metric_dbfs(l2);
    assert(l1db-l2db>5.0 && l1db-l2db<7.0);

    // Bellard/ITU V.90 PCM algebra foundation: 30 data bits map to six G.711
    // PCM amplitudes and recover exactly for an error-free lab constellation.
    V90PcmMapper pcmmap(v90_pcm_lab_parameters());
    assert(pcmmap.valid());
    assert(pcmmap.bits_per_mapping_frame()==30);
    assert(pcmmap.nominal_bit_rate()==40000);
    std::vector<uint8_t> mbits(30); for(size_t i=0;i<mbits.size();++i)mbits[i]=static_cast<uint8_t>((i*7+3)&1);
    auto msamp=pcmmap.encode(mbits);
    auto mback=pcmmap.decode(msamp);
    assert(mback==mbits);

    // Real Phase-4 parameter shape seen from hardware: S=5/K=12 with one
    // spectral-shaping look-ahead frame.  V.90 requires ld to affect signs,
    // not the mapped magnitudes of the first transmitted TRN2d data frame.
    // This also catches accidental emission of the mapper's zero-history
    // latency as the beginning of TRN2d.
    V90PcmParameters shaped;
    shaped.S=5; shaped.K=12; shaped.ld=1;
    shaped.a1=64; shaped.a2=0; shaped.b1=0; shaped.b2=0;
    for(auto& c: shaped.allowed_ucodes) c={33,54,63,72};
    auto shaped_trn=v90_trn2d_pcmu(shaped,2040);
    assert(shaped_trn.size()==2040);
    auto unshaped=shaped; unshaped.ld=0;
    auto unshaped_trn=v90_trn2d_pcmu(unshaped,2040);
    assert(unshaped_trn.size()==2040);
    for(size_t i=0;i<6;++i)
        assert((shaped_trn[i]|0x80u)==(unshaped_trn[i]|0x80u));

    // Phase 4 must not stop after TRN2d. Table-16 Type-0 MP is padded to a
    // whole six-symbol data frame and carries the V.34 information CRC. For
    // the hardware CPt shape D=S+K=17, the 86 fixed bits pad to 102 bits =
    // six data frames = 36 PCM symbols.
    const auto mp0=build_v90_mp0_bits(17,false,4,0x0007);
    assert(mp0.size()==102);
    assert(std::all_of(mp0.begin(),mp0.begin()+17,[](uint8_t b){return b==1;}));
    assert(mp0[17]==0 && mp0[18]==0 && mp0[33]==0 && mp0[34]==0 &&
           mp0[51]==0 && mp0[68]==0 && mp0[85]==0);
    std::vector<uint8_t> mp_info;
    mp_info.insert(mp_info.end(),mp0.begin()+18,mp0.begin()+34);
    mp_info.insert(mp_info.end(),mp0.begin()+35,mp0.begin()+51);
    mp_info.insert(mp_info.end(),mp0.begin()+52,mp0.begin()+68);
    uint16_t mp_crc=0;
    for(size_t i=69;i<=84;++i) mp_crc=static_cast<uint16_t>((mp_crc<<1)|mp0[i]);
    assert(mp_crc==v34_info_crc(mp_info));
    const auto mp1=build_v90_mp0_bits(17,true,4,0x0007);
    assert(mp1.size()==mp0.size() && mp1[33]==1);

    V90Phase4DigitalTx phase4_stream(shaped);
    assert(phase4_stream.valid());
    const auto trn_mp=phase4_stream.start_trn2d_and_mp(2400);
    // ld=1/S=5 has a six-sample mapper look-ahead. The stream removes that
    // delay once, then later MP calls return a full continuous sequence.
    assert(trn_mp.size()==2400+36-6);
    const auto mp_more=phase4_stream.next_mp(false);
    const auto mp_prime=phase4_stream.next_mp(true);
    assert(mp_more.size()==36 && mp_prime.size()==36);

    auto qts=build_qts_ulaw(61);
    assert(qts.size()==(128+8)*6);
    assert(qts[0]==0xC2);
    assert(qts[1]==0xFF);
    assert(qts[3]==0x42);

    QCA1d q{true,-12};
    auto bits=build_qca1d_bits(q);
    assert(bits.size()==70);
    auto pcm=v21_modulate(bits,V21Band::High);
    auto rx=v21_demodulate_nominal(pcm,V21Band::High);
    assert(rx.size()>=bits.size()-1);
    for(size_t i=0;i<rx.size() && i<bits.size();++i) assert(rx[i]==bits[i]);

    // 8-N-1 async framing round-trip.
    std::vector<uint8_t> msg={'H','i','!'};
    auto abits=async_encode(msg);
    assert(async_decode(abits)==msg);

    // V.21 data PHY lab round-trip.
    auto v21pcm=v21_modulate(abits,V21Band::Low);
    auto v21bits=v21_demodulate_nominal(v21pcm,V21Band::Low);
    auto v21bytes=async_decode(v21bits);
    assert(v21bytes==msg);

    // V.23 forward channels, both standardized baud modes.
    for(auto mode: {V23ForwardMode::Baud600,V23ForwardMode::Baud1200}){
        auto p=v23_modulate_forward(abits,mode);
        auto b=v23_demodulate_forward_nominal(p,mode);
        assert(async_decode(b)==msg);
    }
    // V.23 75-baud backward channel.
    auto p75=v23_modulate_backward75(abits);
    auto b75=v23_demodulate_backward75_nominal(p75);
    assert(async_decode(b75)==msg);

    auto ans=build_anspcm_ulaw(-12,1.0);
    assert(ans.size()==8000);
    assert(build_ansam(1.0).size()==8000);
    assert(build_ans_2100(1.0).size()==8000);

    QuickConnectAnswerSM sm;
    sm.on_call_answered(); assert(sm.state()==QCState::AnswerSilence);
    sm.on_200ms_elapsed(); assert(sm.state()==QCState::SendANSam);
    sm.on_qc1a(QC1a{true,61}); assert(sm.state()==QCState::SendQCA1d);
    sm.on_qca1d_sent(); assert(sm.state()==QCState::Send75msSilence);
    sm.on_75ms_elapsed(); assert(sm.state()==QCState::SendQTS);
    sm.on_qts_sent(); assert(sm.state()==QCState::SendANSpcm);
    sm.on_toneq_detected(); assert(sm.state()==QCState::SendPostTONEqSilence);
    sm.on_75ms_elapsed(); assert(sm.state()==QCState::ShortPhase2);

    // Production-mode fallback only advertises PHYs that are implemented.
    FallbackController fb(false);
    fb.set_remote_capabilities({ModemMode::V92,ModemMode::V34,ModemMode::V23_1200_75,ModemMode::V21_300});
    assert(fb.current()==ModemMode::V23_1200_75);
    assert(fb.fail_and_next()==ModemMode::V21_300);

    // Development ladder keeps every requested V-series slot in priority order.
    FallbackController dev(true);
    dev.set_remote_capabilities({ModemMode::V92,ModemMode::V90,ModemMode::V34,ModemMode::V32bis,
                                 ModemMode::V32,ModemMode::V22bis,ModemMode::V22,
                                 ModemMode::V23_1200_75,ModemMode::V23_600_75,ModemMode::V21_300});
    assert(dev.current()==ModemMode::V92);
    assert(dev.candidates().size()==10);


    // PPP backend argv: server/peer addresses, Windows DNS, and lab no-auth mode.
    PppConfig pc;
    auto pa=build_pppd_args(pc,"/dev/pts/42");
    auto has=[&](const std::string& x){ return std::find(pa.begin(),pa.end(),x)!=pa.end(); };
    assert(has("/dev/pts/42"));
    assert(has("10.77.0.1:10.77.0.2"));
    assert(has("ms-dns"));
    assert(has("1.1.1.1"));
    assert(has("noauth"));
    assert(has("296"));
    assert(has("ipparam"));
    assert(has("v92isp"));



    // Live V.92 Quick Connect path through the short-Phase-2 boundary using a
    // synthetic standards-shaped QC1a and TONEq.
    LiveModem qlm(LiveMode::V92QuickConnect);
    qlm.start_call();
    for(int i=0;i<11;++i) (void)qlm.next_tx_pcmu();
    assert(qlm.state()==LiveState::V92ANSam);
    std::vector<uint8_t> qc;
    auto app=[&](const char* x){while(*x)qc.push_back(static_cast<uint8_t>(*x++-'0'));};
    app("1111111111"); app("0101010101");
    app("0001000001"); // analogue, QC, LAPM=1, UQTS selector 0000 => Ucode 61
    app("1111111111"); app("0101010101"); app("0001000001");
    std::vector<uint8_t> qc_with_idle(12,1); qc_with_idle.insert(qc_with_idle.end(),qc.begin(),qc.end());
    auto qc_pcm=v21_modulate(qc_with_idle,V21Band::Low);
    for(size_t o=0;o<qc_pcm.size();o+=160){
        std::vector<int16_t> c(qc_pcm.begin()+o,qc_pcm.begin()+std::min(qc_pcm.size(),o+160)); qlm.receive_pcm(c);
    }
    assert(qlm.state()==LiveState::V92QCA1d);
    for(int i=0;i<40 && qlm.state()!=LiveState::V92ANSpcm;++i)(void)qlm.next_tx_pcmu();
    assert(qlm.state()==LiveState::V92ANSpcm);
    auto toneq=v21_modulate(std::vector<uint8_t>(40,1),V21Band::Low); // 980-Hz mark
    qlm.receive_pcm(toneq);
    assert(qlm.state()==LiveState::V92PostToneqSilence);
    for(int i=0;i<5;++i)(void)qlm.next_tx_pcmu();
    assert(qlm.state()==LiveState::V90INFO0d || qlm.state()==LiveState::V90Phase2Silence);
    auto short_i0a=build_v90_info0a_bits(true,true,true);
    auto short_i0aw=v90_info0a_waveform(short_i0a);
    for(size_t o=0;o<short_i0aw.size();o+=160){
        qlm.receive_pcm(std::vector<int16_t>(short_i0aw.begin()+o,
            short_i0aw.begin()+std::min(short_i0aw.size(),o+160)));
    }
    for(int i=0;i<15 && qlm.state()!=LiveState::V90ToneB;++i)(void)qlm.next_tx_pcmu();
    assert(qlm.state()==LiveState::V90ToneB);
    qlm.receive_pcm(v90_tone(2400.0,0.020,false));
    for(int i=0;i<5 && qlm.state()!=LiveState::V90WaitSecondToneA;++i)(void)qlm.next_tx_pcmu();
    assert(qlm.state()==LiveState::V90WaitSecondToneA);
    qlm.receive_pcm(v90_tone(2400.0,0.020,false));
    qlm.receive_pcm(v90_tone(2400.0,0.020,true));
    assert(qlm.state()==LiveState::V90WaitINFO1a);
    auto short_i1a=build_v90_info1a_phase2_bits(true,6,80);
    auto short_i1aw=v90_info0a_waveform(short_i1a);
    for(size_t o=0;o<short_i1aw.size();o+=160){
        qlm.receive_pcm(std::vector<int16_t>(short_i1aw.begin()+o,
            short_i1aw.begin()+std::min(short_i1aw.size(),o+160)));
    }
    assert(qlm.state()==LiveState::V92ShortPhase2Reached);

    // V.21 standard polarity: idle/mark on the originate low channel is 980 Hz.
    // A one-bit mark generated by the corrected PHY must demodulate as binary 1.
    auto mark_pcm=v21_modulate(std::vector<uint8_t>{1,1,1},V21Band::Low);
    auto mark_bits=v21_demodulate_nominal(mark_pcm,V21Band::Low);
    assert(!mark_bits.empty() && mark_bits[0]==1);

    // Live V.21 answer sequence progresses from ANS -> 75 ms -> carrier.
    LiveModem lm(LiveMode::V21_300);
    lm.start_call(); assert(lm.state()==LiveState::V21ANS);
    for(int i=0;i<130;++i) (void)lm.next_tx_pcmu();
    assert(lm.state()==LiveState::V21Carrier || lm.state()==LiveState::V21Silence75);


    // Exercise the live V.21 receive path far enough to establish carrier and
    // recover async bytes that would be forwarded to pppd.
    LiveModem lv(LiveMode::V21_300);
    lv.start_call();
    for(int i=0;i<140;++i) (void)lv.next_tx_pcmu();
    assert(lv.state()==LiveState::V21Carrier);
    auto caller_mark=v21_modulate(std::vector<uint8_t>(120,1),V21Band::Low);
    for(size_t o=0;o<caller_mark.size();o+=160){
        std::vector<int16_t> c(caller_mark.begin()+o,caller_mark.begin()+std::min(caller_mark.size(),o+160));
        lv.receive_pcm(c);
        if(lv.data_connected()) break;
    }
    assert(lv.data_connected());
    std::vector<uint8_t> ppp_probe={0x7e,0xff,0x03,0xc0,0x21,0x7e};
    auto probe_bits=async_encode(ppp_probe);
    std::vector<uint8_t> with_idle(12,1); with_idle.insert(with_idle.end(),probe_bits.begin(),probe_bits.end());
    auto probe_pcm=v21_modulate(with_idle,V21Band::Low);
    for(size_t o=0;o<probe_pcm.size();o+=160){
        std::vector<int16_t> c(probe_pcm.begin()+o,probe_pcm.begin()+std::min(probe_pcm.size(),o+160));
        lv.receive_pcm(c);
    }
    auto recovered=lv.take_ppp_bytes();
    assert(recovered==ppp_probe);

    // SpanDSP is optional; the base build must remain valid without it.
    if(!span_v22_available()){ SpanV22Modem sv(2400); assert(!sv.start_answer()); }

    std::cout << "all tests passed\n";
}
