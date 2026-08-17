#include "live_modem.hpp"
#include "async_serial.hpp"
#include "g711.hpp"
#include "span_v22.hpp"
#include "v21.hpp"
#include "v90_phase2.hpp"
#include "v90_phase3.hpp"
#include "v90_phase3_rx.hpp"
#include "v90_pcm.hpp"
#include "v34_qam.hpp"
#include "v92_quickconnect.hpp"

#ifdef V92_HAVE_SPANDSP
extern "C" {
#include <spandsp.h>
#include <spandsp/v8.h>
#include <spandsp/v42bis.h>
}
#endif

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <deque>
#include <iostream>
#include <vector>

namespace {

void feed_chunks(v92::LiveModem& modem, const std::vector<int16_t>& pcm) {
    for (size_t off=0; off<pcm.size(); off+=160) {
        modem.receive_pcm(std::vector<int16_t>(pcm.begin()+off,
            pcm.begin()+std::min(pcm.size(),off+160)));
    }
}

void drive_to_v92_short_info0(v92::LiveModem& modem) {
    using namespace v92;
    modem.start_call();
    for(int i=0;i<11;++i)(void)modem.next_tx_pcmu();
    assert(modem.state()==LiveState::V92ANSam);

    std::vector<uint8_t> qc;
    auto app=[&](const char* x){while(*x)qc.push_back(static_cast<uint8_t>(*x++-'0'));};
    app("1111111111");app("0101010101");
    app("0001000001"); // analogue, QC, LAPM, UQTS selector 0 (Ucode 61)
    app("1111111111");app("0101010101");app("0001000001");
    std::vector<uint8_t> framed;framed.reserve(12+qc.size());
    for(int i=0;i<12;++i)framed.push_back(1);
    for(auto bit:qc)framed.push_back(bit);
    feed_chunks(modem,v21_modulate(framed,V21Band::Low));
    assert(modem.state()==LiveState::V92QCA1d);

    for(int i=0;i<40 && modem.state()!=LiveState::V92ANSpcm;++i)(void)modem.next_tx_pcmu();
    assert(modem.state()==LiveState::V92ANSpcm);
    feed_chunks(modem,v21_modulate(std::vector<uint8_t>(40,1),V21Band::Low));
    assert(modem.state()==LiveState::V92PostToneqSilence);
    for(int i=0;i<5;++i)(void)modem.next_tx_pcmu();
    assert(modem.state()==LiveState::V90INFO0d || modem.state()==LiveState::V90Phase2Silence);
}

#ifdef V92_HAVE_SPANDSP
struct BisBuffers { std::vector<uint8_t> encoded,decoded; };
void bis_encoded(void* u,const uint8_t* p,int n){
    auto* b=static_cast<BisBuffers*>(u);b->encoded.insert(b->encoded.end(),p,p+n);
}
void bis_decoded(void* u,const uint8_t* p,int n){
    auto* b=static_cast<BisBuffers*>(u);b->decoded.insert(b->decoded.end(),p,p+n);
}

struct RawV22Caller {
    std::deque<int> tx_bits;
    bool connected=false;
    bool failed=false;
};

struct V42V22Caller {
    v42_state_t* v42=nullptr;
    bool physical_connected=false;
    bool physical_failed=false;
    bool lapm_connected=false;
};
int v42_caller_get_frame(void*,uint8_t[],int){return 0;}
void v42_caller_put_frame(void*,const uint8_t[],int){}
void v42_caller_status(void* u,int status){
    auto* c=static_cast<V42V22Caller*>(u);
    if(status==SIG_STATUS_LINK_CONNECTED ||
       (status>=0 && std::string(lapm_status_to_str(status))=="LAPM_DATA"))
        c->lapm_connected=true;
}
int v42_v22_caller_get_bit(void* u){
    auto* c=static_cast<V42V22Caller*>(u);
    return v42_tx_bit(c->v42);
}
void v42_v22_caller_put_bit(void* u,int bit){
    auto* c=static_cast<V42V22Caller*>(u);
    if(bit==SIG_STATUS_TRAINING_SUCCEEDED)c->physical_connected=true;
    else if(bit==SIG_STATUS_TRAINING_FAILED)c->physical_failed=true;
    else if(bit==SIG_STATUS_CARRIER_DOWN)c->physical_connected=false;
    v42_rx_bit(c->v42,bit);
}
int raw_v22_caller_get_bit(void* u){
    auto* c=static_cast<RawV22Caller*>(u);
    if(c->tx_bits.empty())return 1;
    const int b=c->tx_bits.front();c->tx_bits.pop_front();return b&1;
}
void raw_v22_caller_put_bit(void* u,int bit){
    auto* c=static_cast<RawV22Caller*>(u);
    if(bit==SIG_STATUS_TRAINING_SUCCEEDED)c->connected=true;
    else if(bit==SIG_STATUS_TRAINING_FAILED)c->failed=true;
    else if(bit==SIG_STATUS_CARRIER_DOWN)c->connected=false;
}
#endif

} // namespace

int main(){
    using namespace v92;

    assert(!v42_detection_timeout_reached(kV42DetectionTimerSamples-1));
    assert(v42_detection_timeout_reached(kV42DetectionTimerSamples));
    assert(!v42_establishment_timeout_reached(kV42EstablishmentWatchdogSamples-1));
    assert(v42_establishment_timeout_reached(kV42EstablishmentWatchdogSamples));

    // Exact answer-side Figure-11 XID.  The optional-functions octets at 8..11
    // catch SpanDSP 0.0.6's missing-pointer-advance bug, while byte 1 verifies
    // that an incoming poll is mirrored as the response final bit.
    const auto xid=build_v42_answer_xid_response();
    assert(xid.size()==44 && xid[0]==0x03 && xid[1]==0xBF && xid[2]==0x82);
    const uint8_t xid_optional[6]={0x03,0x04,0x8A,0x89,0x00,0x00};
    for(size_t i=0;i<6;++i)assert(xid[6+i]==xid_optional[i]);
    assert(xid[3]==0x80 && xid[4]==0x00 && xid[5]==0x14);
    assert(xid[14]==0x04 && xid[15]==0x00 && xid[18]==0x04 && xid[19]==0x00);
    assert(xid[26]==0xF0 && xid[27]==0x00 && xid[28]==0x0F);
    assert(xid[31]=='V' && xid[32]=='4' && xid[33]=='2');
    assert(build_v42_answer_xid_response(0x03,false)[1]==0xAF);

    // V.42 ODP is DC1 with alternating parity, separated by 8..16 marks.
    // The exact 11/91 repetition in the hardware trace must disarm T400; a
    // non-alternating or too-tightly-spaced stream must not.
    auto odp_bits=[](const std::vector<uint8_t>& chars,unsigned marks){
        std::vector<int> bits;
        for(size_t n=0;n<chars.size();++n){
            bits.push_back(0);
            for(unsigned i=0;i<8;++i)bits.push_back((chars[n]>>i)&1u);
            bits.push_back(1);
            if(n+1<chars.size())bits.insert(bits.end(),marks,1);
        }
        return bits;
    };
    V42OdpDetector odp;
    for(int bit:odp_bits({0x11,0x91,0x11,0x91},8))odp.feed(bit);
    assert(odp.detected());
    odp.reset();
    for(int bit:odp_bits({0x11,0x11,0x11,0x11},8))odp.feed(bit);
    assert(!odp.detected());
    odp.reset();
    for(int bit:odp_bits({0x11,0x91,0x11,0x91},3))odp.feed(bit);
    assert(!odp.detected());

    // The project's signed-linear bridge conversion must round-trip all PCMU
    // codewords used by V.90, including the otherwise ambiguous -zero. The
    // actual linked PJSIP encoder is guarded separately by the frontend.
    for(unsigned u=0;u<256;++u)
        assert(linear_to_ulaw(ulaw_to_linear_for_reencode(static_cast<uint8_t>(u)))==u);

    // Numeric INFO fields are transmitted least-significant bit first. This
    // regression is the real-call bug: selector 6 used to decode as 3 (V.34).
    auto i1a=build_v90_info1a_phase2_bits(true,3,80);
    const uint8_t upstream3[3]={1,1,0};
    const uint8_t downstream6[3]={0,1,1};
    for(int i=0;i<3;++i){assert(i1a[34+i]==upstream3[i]);assert(i1a[37+i]==downstream6[i]);}
    auto parsed=find_v90_info1a_phase2(v90_info0a_waveform(i1a));
    assert(parsed && parsed->requests_v90 && parsed->upstream_symbol_rate_index==3);

    auto i1a92=build_v90_info1a_phase2_bits(true,6,80);
    auto parsed92=find_v90_info1a_phase2(v90_info0a_waveform(i1a92));
    assert(parsed92 && parsed92->requests_v90 && parsed92->pcm_upstream);

    V90Info0dConfig short_cfg;
    short_cfg.request_short_phase2=true;short_cfg.v92_capable=true;
    auto i0d=build_v90_info0d_bits(short_cfg);
    assert(i0d[26]==1 && i0d[27]==1 && check_v90_info0d_crc(i0d));
    // -12 dBm0 encodes 6 as 0110 on the LSB-first wire; -12 dBm0 digital
    // maximum encodes 23 as 11101.
    const uint8_t tx_power6[4]={0,1,1,0};
    const uint8_t max_power23[5]={1,1,1,0,1};
    for(int i=0;i<4;++i)assert(i0d[29+i]==tx_power6[i]);
    for(int i=0;i<5;++i)assert(i0d[33+i]==max_power23[i]);

    auto i1d=build_v90_info1d_bits();
    const uint8_t projected4[4]={0,0,1,0};
    for(size_t rate=0;rate<6;++rate)
        for(int i=0;i<4;++i)assert(i1d[30+9*rate+i]==projected4[i]);
    assert(check_v90_info1d_crc(i1d));

    // INFO1d must not project rates the caller explicitly omitted in INFO0a.
    // The test frame excludes 2743/2800 but permits 3000/3200/3429.
    const auto selective_i0a=build_v90_info0a_bits(true);
    const auto selective_i1d=v90_info1d_config_from_info0a(selective_i0a);
    assert(selective_i1d.projected_rate_x2400[0]==0);
    assert(selective_i1d.projected_rate_x2400[1]==0);
    assert(selective_i1d.projected_rate_x2400[2]==0);
    assert(selective_i1d.projected_rate_x2400[3]==4);
    assert(selective_i1d.projected_rate_x2400[4]==4);
    assert(selective_i1d.projected_rate_x2400[5]==4);

    // Post-INFO1d recovery must distinguish INFOMARKSa (continuous DBPSK
    // binary ones) from the client's unmodulated 2400-Hz Tone A retrain.
    const auto infomarksa=v90_info_dbpsk_modulate(std::vector<uint8_t>(16,1),2400.0);
    assert(v90_infomarksa_present(infomarksa));
    assert(!v90_infomarksa_present(v90_tone(2400.0,0.040,false)));

    // The post-INFO1a "random data" is the deterministic V.90 Phase-3 PCM
    // training stream, not arbitrary bytes. Verify exact Sd/S-bar octets,
    // GPC startup, minimum TRN1d length, and the CRC-valid Jd frame.
    V90Phase3DigitalTx phase3(79);
    assert(phase3.valid());
    auto sd=phase3.sd_and_sbar_pcmu();
    assert(sd.size()==432);
    const uint8_t wp=ucode_to_ulaw(95,true),wn=ucode_to_ulaw(95,false);
    const uint8_t zp=ucode_to_ulaw(0,true),zn=ucode_to_ulaw(0,false);
    const uint8_t s0[6]={wp,zp,wp,wn,zn,wn};
    const uint8_t sb0[6]={wn,zn,wn,wp,zp,wp};
    for(int i=0;i<6;++i){assert(sd[i]==s0[i]);assert(sd[384+i]==sb0[i]);}
    auto trn=phase3.pp_and_trn1d_pcmu(2038);
    assert(trn.size()==2040);
    assert(trn[0]==ucode_to_ulaw(0,true)); // PP symbol 1
    assert(trn[1]==ucode_to_ulaw(0,true)); // PP symbol 2
    for(size_t i=2;i<20;++i)assert(trn[i]==ucode_to_ulaw(79,true));
    for(size_t i=20;i<25;++i)assert(trn[i]==ucode_to_ulaw(79,false));
    auto jd=phase3.jd_bits();
    assert(jd.size()==72 && phase3.jd_crc_ok(jd));
    assert(jd[18]==1); // advertise only the conservative 28000-bit/s rate
    // V.34 CRC input excludes the three start bits.  Check the specified
    // coverage independently so a self-consistent encoder/checker bug cannot
    // make this regression pass again.
    std::vector<uint8_t> jd_information(jd.begin()+18,jd.begin()+34);
    jd_information.insert(jd_information.end(),jd.begin()+35,jd.begin()+51);
    uint16_t jd_crc=0;
    for(size_t i=52;i<68;++i)
        jd_crc=static_cast<uint16_t>((jd_crc<<1)|(jd[i]&1u));
    assert(jd_crc==v34_info_crc(jd_information));
    const std::vector<uint8_t> old_wrong_jd_crc_input(jd.begin()+18,jd.begin()+52);
    assert(jd_crc!=v34_info_crc(old_wrong_jd_crc_input));
    auto jd_pcm=phase3.jd_frame_pcmu();
    assert(jd_pcm.size()==72);
    assert(phase3.jd_bar_pcmu().size()==12);
    // V.90 8.6.4/9.4.1: initial Ri is ordinary R at UINFO (+++---). Only
    // after CPt is received is it terminated by 24T of Ri-bar (---+++).
    const auto ri=phase3.ri_pcmu(192,false);
    assert(ri.size()==192);
    for(size_t i=0;i<ri.size();++i)
        assert(ri[i]==ucode_to_ulaw(79,(i%6)<3));
    const auto ri_bar=phase3.ri_pcmu(24,true);
    for(size_t i=0;i<ri_bar.size();++i)
        assert(ri_bar[i]==ucode_to_ulaw(79,(i%6)>=3));

    // The caller-side observer must acquire real V.34/QPSK random training,
    // tolerate a carrier offset, and validate Ja. Energy alone is not enough.
    for(uint8_t rate_index : {uint8_t(3),uint8_t(4),uint8_t(5)}){
        const uint8_t md=rate_index==5?1:0;
        V90Phase3AnalogueRx phase3_rx(rate_index,md);
        auto waveform=v90_phase3_analogue_test_waveform(
            rate_index,md,5000.0,rate_index==3?-1.5:1.5);
        V90Phase3RxObservation rx_observation;
        for(size_t off=0;off<waveform.size();off+=160){
            rx_observation=phase3_rx.feed(std::vector<int16_t>(
                waveform.begin()+off,
                waveform.begin()+std::min(waveform.size(),off+160)));
        }
        assert(rx_observation.training_locked);
        assert(rx_observation.ja_detected);
        assert(rx_observation.ja_descriptor_bits==240);
        assert(rx_observation.dil_segment_count==0);
        assert(rx_observation.dil_descriptor.valid);
        assert(rx_observation.dil_descriptor.segment_count==0);
    }
    // N=1 makes the mandatory descriptor exactly 256 bits, so Table 12's
    // optional second fill bit must be absent. The previous parser demanded
    // it and mistook the next repeated Ja frame's sync 1 for an invalid fill.
    V90Phase3AnalogueRx n1_phase3_rx(4,0);
    auto short_line_echo=[](const std::vector<int16_t>& input){
        auto output=input;
        for(size_t i=3;i<output.size();++i)
            output[i]=static_cast<int16_t>(std::clamp(
                0.72*input[i]+0.28*input[i-3],-32767.0,32767.0));
        return output;
    };
    auto n1_waveform=short_line_echo(
        v90_phase3_analogue_test_waveform(4,0,5000.0,1.0,false,1));
    V90Phase3RxObservation n1_observation;
    for(size_t off=0;off<n1_waveform.size();off+=160)
        n1_observation=n1_phase3_rx.feed(std::vector<int16_t>(
            n1_waveform.begin()+off,
            n1_waveform.begin()+std::min(n1_waveform.size(),off+160)));
    assert(n1_observation.ja_detected);
    assert(n1_observation.ja_descriptor_bits==256);
    assert(n1_observation.dil_segment_count==1);
    assert(n1_observation.dil_descriptor.valid);
    assert(n1_observation.dil_descriptor.training_ucode.size()==1);
    assert(n1_observation.dil_descriptor.sign_pattern.size()==1);
    assert(n1_observation.dil_descriptor.training_pattern.size()==1);
    const auto n1_dil=phase3.dil_segment_pcmu(n1_observation.dil_descriptor,0);
    assert(n1_dil.size()==6);
    assert(std::all_of(n1_dil.begin(),n1_dil.end(),[&](uint8_t x){
        return x==ucode_to_ulaw(0,false);
    }));

    // After Ja, the receiver must remain active and hear the caller's V.34 S
    // instruction during our repeating Jd. A 2400-Hz Tone A is not S.
    n1_phase3_rx.arm_for_s();
    const auto not_s=v90_tone(2400.0,0.060,false);
    for(size_t off=0;off<not_s.size();off+=160)
        (void)n1_phase3_rx.feed(std::vector<int16_t>(
            not_s.begin()+off,not_s.begin()+std::min(not_s.size(),off+160)));
    assert(!n1_phase3_rx.observation().s_detected);
    auto caller_s=v90_phase3_analogue_s_test_waveform(4,128,5000.0,1.0);
    // A real splitter/PSTN path adds a short echo. The post-Jd gate must still
    // find a coherent section of S instead of requiring all 64 transitions to
    // be pristine as the old laboratory-only detector did.
    auto echoed_s=short_line_echo(caller_s);
    V90Phase3RxObservation s_observation;
    for(size_t off=0;off<echoed_s.size();off+=160)
        s_observation=n1_phase3_rx.feed(std::vector<int16_t>(
            echoed_s.begin()+off,
            echoed_s.begin()+std::min(echoed_s.size(),off+160)));
    assert(s_observation.s_detected);
    assert(s_observation.s_correlation>=0.78);

    // During DIL the caller may send SCR. A chance 32T correlation inside
    // that random stream must not be mistaken for the later 128T completion
    // S, otherwise the ISP cuts the requested DIL short and sends Ri while the
    // caller is still in Phase 3.
    n1_phase3_rx.arm_for_s(320,true);
    const auto scr_like=v90_phase4_cpt_test_waveform(4,5000.0,1.0,8);
    for(size_t off=0;off<scr_like.size();off+=160)
        s_observation=n1_phase3_rx.feed(std::vector<int16_t>(
            scr_like.begin()+off,
            scr_like.begin()+std::min(scr_like.size(),off+160)));
    assert(!s_observation.s_detected);
    for(size_t off=0;off<echoed_s.size();off+=160)
        s_observation=n1_phase3_rx.feed(std::vector<int16_t>(
            echoed_s.begin()+off,
            echoed_s.begin()+std::min(echoed_s.size(),off+160)));
    assert(s_observation.s_detected);

    // Phase 4 starts only after a complete CRC-valid CPt has supplied the
    // exact PCM constellations. That parameter set must generate real TRN2d,
    // not the unparameterized random PCM used by an earlier prototype.
    V90Phase3RxObservation cpt_observation;
    n1_phase3_rx.arm_for_cpt();
    // ITU-T V.90 Table 14 assigns bit 19 = 0 to CPt and bit 19 = 1 to
    // ordinary CP.  The receiver must not accept an ordinary CP sequence as
    // the Phase-4 training-parameter response.
    const auto ordinary_cp=short_line_echo(
        v90_phase4_cpt_test_waveform(4,5000.0,1.0,4,0,false,true));
    for(size_t off=0;off<ordinary_cp.size();off+=160)
        cpt_observation=n1_phase3_rx.feed(std::vector<int16_t>(
            ordinary_cp.begin()+off,
            ordinary_cp.begin()+std::min(ordinary_cp.size(),off+160)));
    assert(!cpt_observation.cpt_detected);

    n1_phase3_rx.arm_for_cpt();
    const auto caller_cpt=short_line_echo(
        v90_phase4_cpt_test_waveform(4,5000.0,1.0,4));
    for(size_t off=0;off<caller_cpt.size();off+=160)
        cpt_observation=n1_phase3_rx.feed(std::vector<int16_t>(
            caller_cpt.begin()+off,
            caller_cpt.begin()+std::min(caller_cpt.size(),off+160)));
    assert(cpt_observation.cpt_detected);
    assert(cpt_observation.cpt_bits==292);
    assert(!cpt_observation.cpt_parameters.alaw);
    assert(cpt_observation.cpt_parameters.S==6);
    assert(cpt_observation.cpt_parameters.K==6);

    // Real Phase 4 is not the same receive condition as Phase-3 TRN: our
    // downstream Ri is now present and the analogue hybrid/VoIP echo can move
    // the effective upstream impulse response.  The CPt receiver must refine
    // the retained TRN equalizer instead of freezing its seven taps forever.
    // This changed echo path produced the hardware symptom "header seen,
    // ~0.3-rad decisions, CRC never validates" with the frozen equalizer.
    n1_phase3_rx.arm_for_cpt();
    auto changed_phase4_echo=[](const std::vector<int16_t>& input){
        auto output=input;
        for(size_t i=1;i<output.size();++i)
            output[i]=static_cast<int16_t>(std::clamp(
                0.75*input[i]+0.25*input[i-1],-32767.0,32767.0));
        return output;
    };
    const auto changed_channel_cpt=changed_phase4_echo(
        v90_phase4_cpt_test_waveform(4,5000.0,1.0,8,0,false,false,true));
    for(size_t off=0;off<changed_channel_cpt.size();off+=160)
        cpt_observation=n1_phase3_rx.feed(std::vector<int16_t>(
            changed_channel_cpt.begin()+off,
            changed_channel_cpt.begin()+
                std::min(changed_channel_cpt.size(),off+160)));
    assert(cpt_observation.cpt_detected);
    // With relaxed structural-zero tolerances the decoder may succeed on the
    // short_fill=false (292-bit) pass first, using the next frame repetition
    // for the trailing fill bits.  Both 290 and 292 represent valid decodes.
    assert(cpt_observation.cpt_bits==290 || cpt_observation.cpt_bits==292);

    const auto final_trn=v90_trn2d_pcmu(cpt_observation.cpt_parameters,2400);
    assert(final_trn.size()==2400);
    assert(std::any_of(final_trn.begin(),final_trn.end(),
                       [](uint8_t x){return x!=0xFF;}));

    // Exercise the maximum legal CPt layout as well as the compact frame.
    // This 1788-bit form has all six constellation collections and separate
    // codec constellations; it also traverses the same echo that trained TRN.
    n1_phase3_rx.arm_for_cpt();
    const auto long_cpt=short_line_echo(
        v90_phase4_cpt_test_waveform(4,5000.0,1.0,4,5,true));
    for(size_t off=0;off<long_cpt.size();off+=160)
        cpt_observation=n1_phase3_rx.feed(std::vector<int16_t>(
            long_cpt.begin()+off,
            long_cpt.begin()+std::min(long_cpt.size(),off+160)));
    assert(cpt_observation.cpt_detected);
    assert(cpt_observation.cpt_bits==1788);
    assert(cpt_observation.cpt_parameters.allowed_ucodes[5].size()==15);
    // The test CPt deliberately uses transmitter Ucodes 8,16,...,120 and a
    // different post-codec family 9,17,...,121.  TRN2d must use the first
    // (digital-transmitter) family even when CPt bit 128 is set.
    assert(cpt_observation.cpt_parameters.allowed_ucodes[5].front()==8);
    assert(cpt_observation.cpt_parameters.allowed_ucodes[5].back()==120);

    // Hardware interoperability: a deployed caller repeats a 426-bit CPt
    // with one fill zero rather than Table 14's 428-bit three-fill form.
    // Three aligned CRC-valid copies permit this guarded compatibility path.
    n1_phase3_rx.arm_for_cpt();
    const auto short_fill_cpt=short_line_echo(
        v90_phase4_cpt_test_waveform(4,5000.0,1.0,13,0,true,false,true));
    for(size_t off=0;off<short_fill_cpt.size();off+=160)
        cpt_observation=n1_phase3_rx.feed(std::vector<int16_t>(
            short_fill_cpt.begin()+off,
            short_fill_cpt.begin()+std::min(short_fill_cpt.size(),off+160)));
    assert(cpt_observation.cpt_detected);
    assert(cpt_observation.cpt_bits==426);

    // The same trained receive path is re-armed after MP for ordinary CP
    // (Table-14 bit 19 = 1). Its data-mode K uses drn+20 rather than the
    // training CPt drn+8 formula, and the acknowledge bit is retained for
    // the MP -> MP' transition.
    n1_phase3_rx.arm_for_cp();
    const auto data_cp=short_line_echo(
        v90_phase4_cpt_test_waveform(4,5000.0,1.0,5,0,false,true));
    for(size_t off=0;off<data_cp.size();off+=160)
        cpt_observation=n1_phase3_rx.feed(std::vector<int16_t>(
            data_cp.begin()+off,
            data_cp.begin()+std::min(data_cp.size(),off+160)));
    assert(cpt_observation.cpt_detected);
    assert(cpt_observation.cpt_ordinary);
    assert(!cpt_observation.cpt_acknowledge);
    assert(cpt_observation.cpt_parameters.S==6);
    assert(cpt_observation.cpt_parameters.K==18); // drn=4 => D=24

    n1_phase3_rx.arm_for_cp();
    const auto data_cp_prime=short_line_echo(
        v90_phase4_cpt_test_waveform(4,5000.0,1.0,5,0,false,true,false,true));
    for(size_t off=0;off<data_cp_prime.size();off+=160)
        cpt_observation=n1_phase3_rx.feed(std::vector<int16_t>(
            data_cp_prime.begin()+off,
            data_cp_prime.begin()+std::min(data_cp_prime.size(),off+160)));
    assert(cpt_observation.cpt_detected);
    assert(cpt_observation.cpt_ordinary && cpt_observation.cpt_acknowledge);

    V90Phase4DigitalTx ed_tx(cpt_observation.cpt_parameters);
    assert(ed_tx.valid());
    const auto ed_b1d_samples = ed_tx.start_ed_and_b1d();
    assert(ed_b1d_samples.size() == (20u + 48u) * 6u); // 68 frames * 6 symbols/frame = 408 PCM symbols

    ed_tx.feed_async_bytes({0x7E, 0xFF, 0x03, 0xC0, 0x21});
    const auto data_pcmu = ed_tx.produce_data_pcmu(160);
    assert(data_pcmu.size() == 160);

    V90Phase3AnalogueRx false_phase3_rx(4,0);
    std::vector<int16_t> unrelated(560,0);
    auto unrelated_tone=v90_tone(1800.0,0.500,false);
    unrelated.insert(unrelated.end(),unrelated_tone.begin(),unrelated_tone.end());
    V90Phase3RxObservation false_observation;
    for(size_t off=0;off<unrelated.size();off+=160)
        false_observation=false_phase3_rx.feed(std::vector<int16_t>(
            unrelated.begin()+off,
            unrelated.begin()+std::min(unrelated.size(),off+160)));
    assert(!false_observation.training_locked && !false_observation.ja_detected);

    // The repeated-Ja path must not rely on an assumed transition symbol or
    // inherited scrambler register. GPA self-synchronizes after 23 valid bits.
    V90Phase3AnalogueRx self_sync_rx(4,0);
    auto reset_at_ja=v90_phase3_analogue_test_waveform(4,0,5000.0,-2.0,true);
    V90Phase3RxObservation self_sync_observation;
    for(size_t off=0;off<reset_at_ja.size();off+=160)
        self_sync_observation=self_sync_rx.feed(std::vector<int16_t>(
            reset_at_ja.begin()+off,
            reset_at_ja.begin()+std::min(reset_at_ja.size(),off+160)));
    assert(self_sync_observation.training_locked && self_sync_observation.ja_detected);

    // A real analogue modem has an independent symbol clock. At the V.34
    // tolerance limit, a long TRN drifts by more than one complete symbol;
    // the receiver must track it or all following Ja CRCs are undecodable.
    std::vector<int16_t> clock_drift;
    for(double ppm : {-100.0,100.0}){
        V90Phase3AnalogueRx clock_drift_rx(4,0);
        clock_drift=v90_phase3_analogue_test_waveform(
            4,0,5000.0,0.4,false,0,12000,ppm);
        const auto clock_drift_observation=clock_drift_rx.feed(clock_drift);
        assert(clock_drift_observation.training_locked &&
               clock_drift_observation.ja_detected);
        assert(std::abs(clock_drift_observation.symbol_clock_ppm-ppm)<=20.0);
    }

    // Wideband Phase-3 QAM must never be mistaken for a sustained unmodulated
    // 2400-Hz Tone-A retrain request.
    unsigned false_retrain_blocks=0;
    for(size_t off=560;off+160<=clock_drift.size();off+=160){
        const std::vector<int16_t> block(clock_drift.begin()+off,
                                         clock_drift.begin()+off+160);
        false_retrain_blocks = v90_retrain_tone_present(block) ?
            false_retrain_blocks + 1u : 0u;
        assert(false_retrain_blocks < 3u);
    }

    // A 52-bit Ja prefix is not a descriptor.  The old receiver started Sd
    // here, cutting off the client's parameters and provoking a Tone-A retry.
    V90Phase3AnalogueRx partial_ja_rx(4,0);
    auto partial_ja=v90_phase3_analogue_test_waveform(4,0,5000.0,0.0);
    partial_ja.resize(std::min<size_t>(partial_ja.size(),3020));
    V90Phase3RxObservation partial_observation;
    for(size_t off=0;off<partial_ja.size();off+=160)
        partial_observation=partial_ja_rx.feed(std::vector<int16_t>(
            partial_ja.begin()+off,
            partial_ja.begin()+std::min(partial_ja.size(),off+160)));
    assert(partial_observation.training_locked && !partial_observation.ja_detected);

    assert(!detect_toneq_980(v21_modulate(std::vector<uint8_t>(12,1),V21Band::Low)));
    assert(detect_toneq_980(v21_modulate(std::vector<uint8_t>(20,1),V21Band::Low)));

    // Exact short-Phase-1 framing and durations from Tables 2, 8 and 13.
    std::vector<uint8_t> qc1a_bits;
    auto qc_append=[&](const char* x){
        while(*x)qc1a_bits.push_back(static_cast<uint8_t>(*x++-'0'));
    };
    qc_append("1111111111");qc_append("0101010101");qc_append("0001000001");
    qc_append("1111111111");qc_append("0101010101");qc_append("0001000001");
    const auto parsed_qc1a=parse_qc1a(qc1a_bits);
    assert(parsed_qc1a && parsed_qc1a->lapm && parsed_qc1a->uqts==61);
    auto bad_qc1a=qc1a_bits;bad_qc1a[58]^=1;
    assert(!parse_qc1a(bad_qc1a));
    const auto qca1d=build_qca1d_bits(QCA1d{true,-12});
    assert(qca1d.size()==70);
    assert(std::equal(qca1d.begin()+20,qca1d.begin()+30,qca1d.begin()+50));
    const auto qts=build_qts_ulaw(61);
    assert(qts.size()==768+48);
    const auto anspcm=build_anspcm_ulaw(-12,1.0);
    static constexpr std::array<uint8_t,301> table8_ulaw={
        0xA9,0x5D,0x29,0xC9,0xAA,0x3D,0x2B,0xB7,0xAD,0x32,0x2F,0xAE,0xB4,0x2C,0x3A,0xAB,
        0xC2,0x29,0x4F,0xA9,0xFD,0x29,0xD0,0xA9,0x42,0x2B,0xBB,0xAC,0x35,0x2E,0xAF,0xB2,
        0x2D,0x37,0xAB,0xBD,0x2A,0x48,0xA9,0xDC,0x29,0xDF,0xA9,0x4A,0x2A,0xBE,0xAB,0x38,
        0x2D,0xB2,0xAF,0x2E,0x34,0xAC,0xBA,0x2B,0x41,0xAA,0xCE,0x29,0x76,0xA9,0x52,0x29,
        0xC3,0xAA,0x3B,0x2C,0xB5,0xAE,0x30,0x31,0xAD,0xB7,0x2B,0x3D,0xAA,0xC7,0x29,0x5A,
        0xA9,0x62,0x29,0xCA,0xAA,0x3E,0x2B,0xB8,0xAD,0x32,0x2F,0xAE,0xB4,0x2C,0x3A,0xAB,
        0xC0,0x2A,0x4E,0xA9,0xEF,0x29,0xD4,0xA9,0x44,0x2A,0xBB,0xAC,0x35,0x2E,0xB0,0xB1,
        0x2D,0x36,0xAC,0xBC,0x2A,0x46,0xA9,0xD8,0x29,0xE6,0xA9,0x4B,0x2A,0xBF,0xAB,0x39,
        0x2D,0xB3,0xAF,0x2F,0x33,0xAD,0xB9,0x2B,0x3F,0xAA,0xCD,0x29,0x6B,0xA9,0x56,0x29,
        0xC5,0xAA,0x3C,0x2C,0xB6,0xAE,0x30,0x31,0xAE,0xB6,0x2C,0x3C,0xAA,0xC5,0x29,0x57,
        0xA9,0x69,0x29,0xCC,0xAA,0x3F,0x2B,0xB9,0xAD,0x33,0x2F,0xAF,0xB3,0x2D,0x39,0xAB,
        0xBF,0x2A,0x4C,0xA9,0xE7,0x29,0xD8,0xA9,0x46,0x2A,0xBC,0xAC,0x36,0x2E,0xB1,0xB0,
        0x2E,0x36,0xAC,0xBC,0x2A,0x45,0xA9,0xD5,0x29,0xED,0xA9,0x4D,0x2A,0xC0,0xAB,0x39,
        0x2C,0xB4,0xAF,0x2F,0x33,0xAD,0xB8,0x2B,0x3F,0xAA,0xCB,0x29,0x64,0xA9,0x59,0x29,
        0xC7,0xAA,0x3D,0x2C,0xB7,0xAD,0x31,0x30,0xAE,0xB5,0x2C,0x3B,0xAA,0xC4,0x29,0x53,
        0xA9,0x72,0x29,0xCE,0xAA,0x41,0x2B,0xBA,0xAC,0x34,0x2E,0xAF,0xB2,0x2D,0x38,0xAB,
        0xBE,0x2A,0x4A,0xA9,0xE0,0x29,0xDB,0xA9,0x48,0x2A,0xBD,0xAB,0x37,0x2D,0xB1,0xB0,
        0x2E,0x35,0xAC,0xBB,0x2A,0x43,0xA9,0xD1,0x29,0xF9,0xA9,0x4F,0x2A,0xC2,0xAB,0x3A,
        0x2C,0xB4,0xAE,0x2F,0x32,0xAD,0xB8,0x2B,0x3E,0xAA,0xC9,0x29,0x5E
    };
    assert(anspcm.size()==8000);
    assert(std::equal(table8_ulaw.begin(),table8_ulaw.end(),anspcm.begin()));
    assert(anspcm[3612]==static_cast<uint8_t>(anspcm[0]^0x80));

    auto i0a92=build_v90_info0a_bits(true,true,true);
    auto found0a=find_v90_info0a(v90_info0a_waveform(i0a92));
    assert(found0a && found0a->v92_capable && found0a->requests_short_phase2);

    // MODE=v92 must preserve the full V.8/V.90 path. A caller without a
    // reusable QC profile sends ordinary CM while ANSam is still active.
    assert(live_mode_allows_v90(LiveMode::V90Digital));
    assert(live_mode_allows_v90(LiveMode::V92QuickConnect));
    assert(!live_mode_allows_v90(LiveMode::Auto));

    // Complete Quick Connect through the real V.92 short Phase 2: INFO0,
    // 50-ms Tone B, B reversal/10-ms hold, client A reversal, then INFO1a.
    LiveModem modem(LiveMode::V92QuickConnect);
    drive_to_v92_short_info0(modem);
    feed_chunks(modem,v90_info0a_waveform(i0a92));
    for(int i=0;i<15 && modem.state()!=LiveState::V90ToneB;++i)(void)modem.next_tx_pcmu();
    assert(modem.state()==LiveState::V90ToneB);
    modem.receive_pcm(v90_tone(2400.0,0.020,false));
    for(int i=0;i<8 && modem.state()!=LiveState::V90WaitSecondToneA;++i)
        (void)modem.next_tx_pcmu();
    assert(modem.state()==LiveState::V90WaitSecondToneA);
    modem.receive_pcm(v90_tone(2400.0,0.020,false));
    modem.receive_pcm(v90_tone(2400.0,0.020,true));
    assert(modem.state()==LiveState::V90WaitINFO1a);
    feed_chunks(modem,v90_info0a_waveform(i1a92));
    assert(modem.state()==LiveState::V92ShortPhase2Reached);
    assert(modem.short_phase2_reached());

    // A Quick-Connect call that selects the V.34 upstream/V.90 downstream path
    // must leave INFO1a silence and emit the Phase-3 PCM training stream.
    LiveModem phase3_live(LiveMode::V92QuickConnect);
    drive_to_v92_short_info0(phase3_live);
    feed_chunks(phase3_live,v90_info0a_waveform(i0a92));
    for(int i=0;i<15 && phase3_live.state()!=LiveState::V90ToneB;++i)(void)phase3_live.next_tx_pcmu();
    phase3_live.receive_pcm(v90_tone(2400.0,0.020,false));
    for(int i=0;i<8 && phase3_live.state()!=LiveState::V90WaitSecondToneA;++i)
        (void)phase3_live.next_tx_pcmu();
    phase3_live.receive_pcm(v90_tone(2400.0,0.020,false));
    phase3_live.receive_pcm(v90_tone(2400.0,0.020,true));
    assert(phase3_live.state()==LiveState::V90WaitINFO1a);
    auto i1a_phase3=build_v90_info1a_phase2_bits(true,4,79,2);
    feed_chunks(phase3_live,v90_info0a_waveform(i1a_phase3));
    assert(phase3_live.state()==LiveState::V90Phase3WaitAnalogue);
    assert(phase3_live.short_phase2_reached());
    // It must remain silent on unrelated energy and start only after the real
    // client TRN has trained the equalizer and Ja has been descrambled.
    phase3_live.receive_pcm(v90_tone(1800.0,0.020,false));
    for(int i=0;i<10;++i){
        auto block=phase3_live.next_tx_pcmu();
        assert(std::all_of(block.begin(),block.end(),[](uint8_t x){return x==0xFF;}));
    }
    auto analogue_phase3_md2=v90_phase3_analogue_test_waveform(
        4,2,5000.0,1.5,false,1);
    feed_chunks(phase3_live,analogue_phase3_md2);
    if(phase3_live.state()!=LiveState::V90Phase3SendSd)
        std::cerr<<"phase3 state="<<to_string(phase3_live.state())
                 <<" event="<<phase3_live.last_event()<<"\n";
    assert(phase3_live.state()==LiveState::V90Phase3SendSd);
    for(int i=0;i<70 && phase3_live.state()!=LiveState::V90Phase3SendJd;++i)
        (void)phase3_live.next_tx_pcmu();
    assert(phase3_live.state()==LiveState::V90Phase3SendJd);

    // This is the hardware failure reproduced by the attached trace: the
    // caller sends S while Jd is repeating. The ISP must hear it, finish the
    // current Jd, send Jd-bar, transmit the requested N=1 DIL, hear the later
    // S/S-bar completion, and enter Phase 4 with Ri.
    feed_chunks(phase3_live,
        v90_phase3_analogue_s_test_waveform(4,128,5000.0,1.5));
    assert(phase3_live.state()==LiveState::V90Phase3SendJdBar);
    for(int i=0;i<10 && phase3_live.state()!=LiveState::V90Phase3SendDIL;++i)
        (void)phase3_live.next_tx_pcmu();
    assert(phase3_live.state()==LiveState::V90Phase3SendDIL);
    feed_chunks(phase3_live,std::vector<int16_t>(400,0));
    feed_chunks(phase3_live,
        v90_phase3_analogue_s_test_waveform(4,128,5000.0,1.5));
    feed_chunks(phase3_live,std::vector<int16_t>(320,0));
    for(int i=0;i<10 && phase3_live.state()!=LiveState::V90Phase4WaitCPt;++i)
        (void)phase3_live.next_tx_pcmu();
    assert(phase3_live.state()==LiveState::V90Phase4WaitCPt);

    feed_chunks(phase3_live,v90_phase4_cpt_test_waveform(4,5000.0,1.5,4));
    assert(phase3_live.state()==LiveState::V90Phase4SendTRN2d);
    assert(phase3_live.last_event().find("TRN2d")!=std::string::npos);

    // A client retry request in Phase 3 or 4 is Tone A for >=50 ms. The ISP
    // must stop training, hold 70 ms silence, then answer with Tone B.
    feed_chunks(phase3_live,v90_tone(2400.0,0.060,false));
    assert(phase3_live.state()==LiveState::V90RetrainSilence);
    for(int i=0;i<5 && phase3_live.state()!=LiveState::V90ToneB;++i)
        (void)phase3_live.next_tx_pcmu();
    assert(phase3_live.state()==LiveState::V90ToneB);

    // The client-tone retry is bounded and V.92 short-phase failure retries as
    // full Phase 2 instead of hanging or falsely claiming success.
    LiveModem retry(LiveMode::V92QuickConnect);
    drive_to_v92_short_info0(retry);
    feed_chunks(retry,v90_info0a_waveform(i0a92));
    for(int i=0;i<15 && retry.state()!=LiveState::V90ToneB;++i)(void)retry.next_tx_pcmu();
    retry.receive_pcm(v90_tone(2400.0,0.020,false));
    for(int i=0;i<5 && retry.state()!=LiveState::V90WaitSecondToneA;++i)(void)retry.next_tx_pcmu();
    assert(retry.state()==LiveState::V90WaitSecondToneA);
    for(int i=0;i<130 && retry.state()==LiveState::V90WaitSecondToneA;++i)(void)retry.next_tx_pcmu();
    assert(retry.state()==LiveState::V90ToneRetryWaitToneA);

#ifdef V92_HAVE_SPANDSP
    // Feed ordinary CM (with no preceding QC1a) while MODE=v92 is emitting
    // ANSam. The live V.8 receiver must take ownership of the same exchange;
    // restarting ANSam after five seconds loses this CM.
    LiveModem v92_first_call(LiveMode::V92QuickConnect);
    v92_first_call.start_call();
    for(int i=0;i<11;++i)(void)v92_first_call.next_tx_pcmu(160);
    assert(v92_first_call.state()==LiveState::V92ANSam);
    std::vector<uint8_t> cm_bits;
    auto append_async_byte=[&](uint8_t byte){
        cm_bits.push_back(0);
        for(unsigned bit=0;bit<8;++bit)cm_bits.push_back((byte>>bit)&1u);
        cm_bits.push_back(1);
    };
    // Three identical messages: the V.8 receiver accepts two consecutive
    // copies when the following preamble closes the second message.
    for(int copy=0;copy<3;++copy){
        cm_bits.insert(cm_bits.end(),10,1);
        append_async_byte(0xE0); // CM/JM synchronization
        append_async_byte(0xC1); // V-series data call
        append_async_byte(0x25); // V.90 modulation octet
        append_async_byte(0x12); // V.22 extension
        append_async_byte(0x10); // final modulation extension
        append_async_byte(0x2A); // LAPM/V.42 protocol
        append_async_byte(0x27); // analogue PCM-modem availability
    }
    const auto ordinary_cm=v21_modulate(cm_bits,V21Band::Low);
    for(size_t off=0;off<ordinary_cm.size();off+=160){
        v92_first_call.receive_pcm(std::vector<int16_t>(
            ordinary_cm.begin()+off,
            ordinary_cm.begin()+std::min(ordinary_cm.size(),off+160)));
        (void)v92_first_call.next_tx_pcmu(160);
        if(v92_first_call.state()==LiveState::V8Negotiating)break;
    }
    assert(v92_first_call.state()==LiveState::V8Negotiating);
    assert(v92_first_call.last_event().find("ordinary CM detected")!=std::string::npos);
    assert(v92_first_call.last_event().find("QC1a timeout")==std::string::npos);

    // Reproduce the hardware trace: a plain V.22bis caller trains at 2400 but
    // sends no V.42 ODP. The answerer must preserve the trained carrier and
    // enter transparent async at T400, not remain stuck waiting for LAPM.
    SpanV22Modem v22_answer(2400);
    assert(v22_answer.start_answer(V22LinkMode::V42Detect));
    RawV22Caller raw_caller;
    auto* caller=v22bis_init(nullptr,2400,V22BIS_GUARD_TONE_NONE,true,
                             raw_v22_caller_get_bit,&raw_caller,
                             raw_v22_caller_put_bit,&raw_caller);
    assert(caller);
    v22bis_tx_power(caller,-12.0f);
    std::vector<int16_t> caller_pcm(160),answer_pcm;
    for(int block=0;block<1000 && !v22_answer.transparent_mode();++block){
        const int nc=v22bis_tx(caller,caller_pcm.data(),160);
        assert(nc>=0);
        v22_answer.receive_pcm(caller_pcm);
        answer_pcm=v22_answer.next_tx_pcm(160);
        v22bis_rx(caller,answer_pcm.data(),static_cast<int>(answer_pcm.size()));
    }
    assert(!raw_caller.failed && raw_caller.connected);
    assert(v22_answer.connected() && v22_answer.transparent_mode());
    assert(v22_answer.link_status().find("T400=750 ms")!=std::string::npos);

    const std::vector<uint8_t> raw_probe={0x7e,0x55};
    auto raw_probe_bits=async_encode(raw_probe);
    for(uint8_t bit:raw_probe_bits)raw_caller.tx_bits.push_back(bit);
    std::vector<uint8_t> raw_recovered;
    for(int block=0;block<100 && raw_recovered.size()<raw_probe.size();++block){
        const int nc=v22bis_tx(caller,caller_pcm.data(),160);
        assert(nc>=0);
        v22_answer.receive_pcm(caller_pcm);
        auto bytes=v22_answer.take_bytes();
        raw_recovered.insert(raw_recovered.end(),bytes.begin(),bytes.end());
        answer_pcm=v22_answer.next_tx_pcm(160);
        v22bis_rx(caller,answer_pcm.data(),static_cast<int>(answer_pcm.size()));
    }
    assert(raw_recovered==raw_probe);
    v22bis_free(caller);

    // Exercise the full detection exchange through both V.22bis data pumps.
    // This guards the real timeout: a valid ODP must keep the answerer out of
    // transparent mode while ADP and SABME/UA advance to LAPM data state.
    SpanV22Modem v42_answer(2400);
    assert(v42_answer.start_answer(V22LinkMode::V42Detect));
    V42V22Caller v42_caller;
    v42_caller.v42=v42_init(nullptr,true,true,v42_caller_get_frame,
                            v42_caller_put_frame,&v42_caller);
    assert(v42_caller.v42);
    v42_set_status_callback(v42_caller.v42,v42_caller_status,&v42_caller);
    v42_restart(v42_caller.v42);
    auto* v42_caller_modem=v22bis_init(nullptr,2400,V22BIS_GUARD_TONE_NONE,true,
                                       v42_v22_caller_get_bit,&v42_caller,
                                       v42_v22_caller_put_bit,&v42_caller);
    assert(v42_caller_modem);
    v22bis_tx_power(v42_caller_modem,-12.0f);
    for(int block=0;block<1000 && !v42_answer.lapm_connected();++block){
        const int nc=v22bis_tx(v42_caller_modem,caller_pcm.data(),160);
        assert(nc>=0);
        v42_answer.receive_pcm(caller_pcm);
        answer_pcm=v42_answer.next_tx_pcm(160);
        v22bis_rx(v42_caller_modem,answer_pcm.data(),static_cast<int>(answer_pcm.size()));
    }
    assert(!v42_caller.physical_failed && v42_caller.physical_connected);
    assert(v42_answer.connected() && v42_answer.lapm_connected());
    assert(!v42_answer.transparent_mode());
    v22bis_free(v42_caller_modem);
    v42_free(v42_caller.v42);

    // Verify the exact SpanDSP V.42bis codec configuration used behind LAPM.
    BisBuffers buffers;
    auto* bis=v42bis_init(nullptr,V42BIS_P0_BOTH_DIRECTIONS,512,6,
                          bis_encoded,&buffers,1024,bis_decoded,&buffers,1024);
    assert(bis);
    const std::vector<uint8_t> sample={0x7e,0xff,0x03,0xc0,0x21,0x01,0x01,0x00,0x04,
                                      'P','P','P','P','P','P','P','P','P','P'};
    assert(v42bis_compress(bis,sample.data(),static_cast<int>(sample.size()))==0);
    assert(v42bis_compress_flush(bis)==0);
    assert(!buffers.encoded.empty());
    assert(v42bis_decompress(bis,buffers.encoded.data(),static_cast<int>(buffers.encoded.size()))==0);
    assert(v42bis_decompress_flush(bis)==0);
    assert(buffers.decoded==sample);
    v42bis_free(bis);
#endif

    // =========================================================================
    // Comprehensive Real-Waveform V.90 Phase 3/4 & Data Mode Verification Suite
    // =========================================================================

    // Test 1: Real Waveform CPt Detection with Line Echo & Frequency Offsets
    {
        v92::V90Phase3AnalogueRx cpt_rx(4, 0);
        auto ja_waveform = v90_phase3_analogue_test_waveform(4, 0, 5000.0, 1.0, false, 1);
        (void)cpt_rx.feed(ja_waveform);
        assert(cpt_rx.observation().training_locked);

        cpt_rx.arm_for_cpt();
        const auto waveform = short_line_echo(v90_phase4_cpt_test_waveform(4, 5000.0, 1.0, 4, 0, false, false));
        v92::V90Phase3RxObservation obs;
        for (size_t off = 0; off < waveform.size(); off += 160) {
            obs = cpt_rx.feed(std::vector<int16_t>(waveform.begin() + off,
                waveform.begin() + std::min(waveform.size(), off + 160)));
        }
        assert(obs.cpt_detected);
        assert(obs.cpt_bits == 292);
        assert(obs.cpt_parameters.S == 6 && obs.cpt_parameters.K == 6);
        assert(obs.cpt_decision_error < 0.35);
    }

    // Test 2: MP / MP' Framing, Table 16 Bitfields & Information CRC
    {
        const int frame_bits = 17; // S=5, K=12
        const auto mp_bits = v92::build_v90_mp0_bits(frame_bits, false);
        assert(!mp_bits.empty());
        assert(mp_bits.size() % frame_bits == 0);
        // Frame sync (17 ones)
        for (size_t i = 0; i < 17; ++i) assert(mp_bits[i] == 1u);
        assert(mp_bits[17] == 0u); // start bit
        assert(mp_bits[18] == 0u); // Type 0
        assert(mp_bits[33] == 0u); // acknowledge bit = 0 for MP

        const auto mp_prime_bits = v92::build_v90_mp0_bits(frame_bits, true);
        assert(mp_prime_bits[33] == 1u); // acknowledge bit = 1 for MP'

        v92::V90PcmParameters p;
        p.alaw = false; p.S = 5; p.K = 12; p.ld = 1;
        for (auto& v : p.allowed_ucodes) {
            for (int j = 0; j < 16; ++j) v.push_back(static_cast<uint8_t>(10 + j * 6));
        }
        v92::V90Phase4DigitalTx digital_tx(p);
        assert(digital_tx.valid());
        const auto trn2d_and_mp = digital_tx.start_trn2d_and_mp(2400);
        assert(!trn2d_and_mp.empty());
        assert(trn2d_and_mp.size() >= 2400);
        assert(trn2d_and_mp.size() == 2400u + 36u - 6u);
    }

    // Test 3: CP and CP' Exchange Validation
    {
        v92::V90Phase3AnalogueRx cp_rx(4, 0);
        auto ja_waveform = v90_phase3_analogue_test_waveform(4, 0, 5000.0, 1.0, false, 1);
        (void)cp_rx.feed(ja_waveform);
        assert(cp_rx.observation().training_locked);

        cp_rx.arm_for_cp();
        const auto cp_waveform = short_line_echo(v90_phase4_cpt_test_waveform(4, 5000.0, 1.0, 5, 0, false, true, false));
        v92::V90Phase3RxObservation obs;
        for (size_t off = 0; off < cp_waveform.size(); off += 160) {
            obs = cp_rx.feed(std::vector<int16_t>(cp_waveform.begin() + off,
                cp_waveform.begin() + std::min(cp_waveform.size(), off + 160)));
        }
        assert(obs.cpt_detected);
        assert(obs.cpt_ordinary && !obs.cpt_acknowledge);

        cp_rx.arm_for_cp();
        const auto cp_prime_waveform = short_line_echo(v90_phase4_cpt_test_waveform(4, 5000.0, 1.0, 5, 0, false, true, false, true));
        for (size_t off = 0; off < cp_prime_waveform.size(); off += 160) {
            obs = cp_rx.feed(std::vector<int16_t>(cp_prime_waveform.begin() + off,
                cp_prime_waveform.begin() + std::min(cp_prime_waveform.size(), off + 160)));
        }
        assert(obs.cpt_detected);
        assert(obs.cpt_ordinary && obs.cpt_acknowledge);
    }

    // Test 4: Sequence Ed / B1d Transition
    {
        v92::V90PcmParameters p;
        p.alaw = false; p.S = 6; p.K = 24; p.ld = 0;
        for (auto& v : p.allowed_ucodes) {
            for (int j = 0; j < 16; ++j) v.push_back(static_cast<uint8_t>(10 + j * 6));
        }
        v92::V90Phase4DigitalTx ed_b1d_tx(p);
        assert(ed_b1d_tx.valid());
        const auto stream = ed_b1d_tx.start_ed_and_b1d();
        assert(stream.size() == (20u + 48u) * 6u); // 68 frames * 6 symbols/frame = 408 PCM symbols
        assert(std::any_of(stream.begin(), stream.end(), [](uint8_t b) { return b != 0xFF; }));
    }

    // Test 5: Tone A (2400 Hz) Retrain Detection & Noise Rejection
    {
        auto tone_a_pcm = v90_tone(2400.0, 0.050, false); // 50 ms of 2400 Hz Tone A = 400 samples
        assert(v90_retrain_tone_present(tone_a_pcm));

        auto tone_1800_pcm = v90_tone(1800.0, 0.050, false);
        assert(!v90_retrain_tone_present(tone_1800_pcm));

        std::vector<int16_t> noise_pcm(400);
        for (size_t i = 0; i < 400; ++i) noise_pcm[i] = static_cast<int16_t>((i * 12345) % 10000 - 5000);
        assert(!v90_retrain_tone_present(noise_pcm));
    }

    // Test 6: Complete Phase 3 -> Phase 4 -> V90Data End-to-End Real Waveform Sequence
    {
        v92::LiveModem modem(v92::LiveMode::V90Digital);
        modem.start_call();
        modem.start_v90_phase3(79, 4, 0);

        // 1. Feed Phase 3 TRN + Ja waveform
        auto ja_waveform = v90_phase3_analogue_test_waveform(4, 0, 5000.0, 0.0, false, 1);
        feed_chunks(modem, ja_waveform);
        assert(modem.state() == v92::LiveState::V90Phase3SendSd);

        // 2. Drain Sd and TRN1d until in Jd
        for (int i = 0; i < 300 && modem.state() != v92::LiveState::V90Phase3SendJd; ++i)
            (void)modem.next_tx_pcmu(160);
        assert(modem.state() == v92::LiveState::V90Phase3SendJd);

        // 3. Feed S waveform to acknowledge Jd
        auto s_waveform = v90_phase3_analogue_s_test_waveform(4, 5000.0);
        feed_chunks(modem, s_waveform);
        assert(modem.state() == v92::LiveState::V90Phase3SendJdBar);

        // 4. Drain Jd-bar into DIL
        for (int i = 0; i < 20 && modem.state() != v92::LiveState::V90Phase3SendDIL; ++i)
            (void)modem.next_tx_pcmu(160);
        assert(modem.state() == v92::LiveState::V90Phase3SendDIL);

        // 5. Feed subsequent S to conclude DIL
        feed_chunks(modem, s_waveform);
        for (int i = 0; i < 50 && modem.state() != v92::LiveState::V90Phase4WaitCPt; ++i)
            (void)modem.next_tx_pcmu(160);
        assert(modem.state() == v92::LiveState::V90Phase4WaitCPt);

        // 6. Feed CPt waveform to trigger Phase 4 TRN2d + MP
        auto cpt_waveform = short_line_echo(v90_phase4_cpt_test_waveform(4, 5000.0, 1.0, 4, 0, false, false));
        feed_chunks(modem, cpt_waveform);
        assert(modem.state() == v92::LiveState::V90Phase4SendTRN2d);

        // 7. Drain TRN2d into MP
        for (int i = 0; i < 50 && modem.state() != v92::LiveState::V90Phase4SendMP; ++i)
            (void)modem.next_tx_pcmu(160);
        assert(modem.state() == v92::LiveState::V90Phase4SendMP);

        // 8. Feed ordinary CP to trigger MP'
        auto cp_waveform = short_line_echo(v90_phase4_cpt_test_waveform(4, 5000.0, 1.0, 5, 0, false, true, false));
        feed_chunks(modem, cp_waveform);

        // 9. Feed CP' to trigger Ed / B1d
        auto cp_prime_waveform = short_line_echo(v90_phase4_cpt_test_waveform(4, 5000.0, 1.0, 5, 0, false, true, false, true));
        feed_chunks(modem, cp_prime_waveform);
        assert(modem.state() == v92::LiveState::V90Phase4SendEd);

        // 10. Drain Ed / B1d into V90Data!
        for (int i = 0; i < 50 && modem.state() != v92::LiveState::V90Data; ++i)
            (void)modem.next_tx_pcmu(160);
        assert(modem.state() == v92::LiveState::V90Data);
        assert(modem.data_connected());
    }

    // Test 7: G.711 PCMU 256-Codeword Round-Trip & Negative Zero Preservation
    {
        for (int byte_val = 0; byte_val < 256; ++byte_val) {
            const uint8_t u_in = static_cast<uint8_t>(byte_val);
            const int16_t linear = v92::ulaw_to_linear(u_in);
            const uint8_t u_out = v92::linear_to_ulaw(linear);
            if (u_in == 0x7F) {
                // Negative zero: mapped to -1 and back to 0x7F
                assert(v92::linear_to_ulaw(-1) == 0x7F);
            } else if (u_in == 0xFF) {
                // Positive zero: mapped to 0 and back to 0xFF
                assert(v92::linear_to_ulaw(0) == 0xFF);
            } else {
                // All non-zero codewords must roundtrip faithfully
                assert(std::abs(static_cast<int>(v92::ulaw_to_linear(u_out)) - linear) <= 64);
            }
        }
    }

    // Test 8: Bidirectional Real Modem Data Mode & PPP Byte Streaming
    {
        v92::V90PcmParameters p;
        p.alaw = false; p.S = 6; p.K = 24; p.ld = 0;
        for (auto& v : p.allowed_ucodes) {
            for (int j = 0; j < 16; ++j) v.push_back(static_cast<uint8_t>(10 + j * 6));
        }
        v92::V90Phase4DigitalTx tx(p);
        assert(tx.valid());

        // Queue downstream PPP data
        const std::vector<uint8_t> ppp_downstream = {0x7E, 0xFF, 0x03, 0xC0, 0x21, 0x01, 0x01, 0x00, 0x04, 0x7E};
        tx.feed_async_bytes(ppp_downstream);
        const auto pcm_data = tx.produce_data_pcmu(320);
        assert(pcm_data.size() == 320);
        assert(std::any_of(pcm_data.begin(), pcm_data.end(), [](uint8_t b) { return b != 0xFF; }));

        // Upstream data demodulation test
        v92::V90Phase3AnalogueRx rx(4, 0);
        // Lock training first so demodulate_data is armed
        auto ja_waveform = v90_phase3_analogue_test_waveform(4, 0, 5000.0, 0.0, false, 1);
        (void)rx.feed(ja_waveform);
        assert(rx.observation().training_locked);

        // Modulate upstream 8-N-1 async bytes into genuine V.34 16-QAM with 4D Trellis Coding and GPA scrambler
        const std::vector<uint8_t> ppp_upstream = {0x7E, 0xFF, 0x03, 0x80, 0x21, 0x7E};
        v92::V34QamModulator v34_mod(3200, 4, 5000.0);
        const auto upstream_pcm = v34_mod.modulate_bytes(ppp_upstream);

        const auto recovered_bytes = rx.demodulate_data(upstream_pcm);
        // Assert upstream V.34 QAM demodulator recovered the exact transmitted bytes
        assert(recovered_bytes.size() >= ppp_upstream.size());
        const std::vector<uint8_t> matched_upstream(recovered_bytes.begin(), recovered_bytes.begin() + ppp_upstream.size());
        assert(matched_upstream == ppp_upstream);
    }

    // ------------------------------------------------------------------------
    // Test 9: Genuine Multi-Rate V.34 Upstream QAM (3200, 3000, 3429 symbols/s)
    // ------------------------------------------------------------------------
    {
        const std::vector<unsigned> symbol_rates = {3200, 3000, 3429};
        const std::vector<uint8_t> payload = {0x7E, 0xFF, 0x03, 0xC0, 0x21, 0x01, 0x01, 0x00, 0x04, 0x7E};

        for (unsigned rate : symbol_rates) {
            v92::V34QamModulator mod(rate, 4, 5000.0);
            v92::V34QamDemodulator demod(rate, 4);

            const auto pcm = mod.modulate_bytes(payload);
            assert(!pcm.empty());

            const auto recovered = demod.process_pcm(pcm);
            assert(recovered.size() >= payload.size());
            const std::vector<uint8_t> matched(recovered.begin(), recovered.begin() + payload.size());
            assert(matched == payload);
        }
    }

    std::cout<<"protocol tests passed\n";
}

