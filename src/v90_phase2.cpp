#include "v90_phase2.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace v92 {
namespace {
constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr double kInfoBitRate = 600.0;

void put_bits(std::vector<uint8_t>& out, unsigned n, uint32_t value) {
    // Literal sequences (fill, frame sync, and the already bit-reversed CRC)
    // are written left-most first in time.
    for (int i = static_cast<int>(n) - 1; i >= 0; --i)
        out.push_back(static_cast<uint8_t>((value >> i) & 1u));
}

void put_uint_lsb(std::vector<uint8_t>& out, unsigned n, uint32_t value) {
    // V.34/V.90 INFO tables label numeric ranges LSB:MSB. The lowest-numbered
    // bit is transmitted first, so numeric fields are little-endian on wire.
    for (unsigned i = 0; i < n; ++i)
        out.push_back(static_cast<uint8_t>((value >> i) & 1u));
}

unsigned get_uint_lsb(const std::vector<uint8_t>& bits, size_t first, size_t last) {
    unsigned value = 0;
    for (size_t i = first; i < last; ++i)
        value |= static_cast<unsigned>(bits[i] & 1u) << (i - first);
    return value;
}

uint32_t clamp_u(int v, int lo, int hi) {
    return static_cast<uint32_t>(std::max(lo, std::min(hi, v)));
}

struct Phasor { double re=0.0, im=0.0, power=0.0; };

Phasor coherent(const std::vector<int16_t>& pcm, size_t a, size_t z,
                double hz, int sample_rate) {
    Phasor p;
    z = std::min(z, pcm.size());
    if (a >= z || sample_rate <= 0) return p;
    for (size_t n=a; n<z; ++n) {
        const double ph = 2.0*kPi*hz*static_cast<double>(n)/sample_rate;
        const double x = pcm[n];
        p.re += x*std::sin(ph);
        p.im += x*std::cos(ph);
        p.power += x*x;
    }
    return p;
}

bool phasor_present(const Phasor& p, size_t n) {
    if (!n || p.power <= 1.0) return false;
    const double coherent_energy = p.re*p.re + p.im*p.im;
    return coherent_energy > p.power*static_cast<double>(n)*0.07;
}

std::vector<Phasor> info_symbols(const std::vector<int16_t>& pcm,
                                 size_t first_sample, size_t count,
                                 double carrier_hz, int sample_rate) {
    std::vector<Phasor> s;
    s.reserve(count);
    for (size_t k=0; k<count; ++k) {
        const size_t a = first_sample + static_cast<size_t>(std::llround(k*sample_rate/kInfoBitRate));
        const size_t z = first_sample + static_cast<size_t>(std::llround((k+1)*sample_rate/kInfoBitRate));
        if (z > pcm.size()) return {};
        s.push_back(coherent(pcm,a,z,carrier_hz,sample_rate));
    }
    return s;
}

std::vector<uint8_t> differential_bits(const std::vector<Phasor>& sym) {
    if (sym.size()<2) return {};
    std::vector<uint8_t> b(sym.size()-1);
    for (size_t i=1;i<sym.size();++i) {
        const double dot=sym[i-1].re*sym[i].re + sym[i-1].im*sym[i].im;
        b[i-1]=dot<0.0?1:0;
    }
    return b;
}

bool info0a_shape_ok(const std::vector<uint8_t>& b) {
    if (b.size()!=49) return false;
    static const uint8_t sync[8]={0,1,1,1,0,0,1,0};
    for(int i=0;i<4;++i) if(b[i]!=1) return false;
    for(int i=0;i<8;++i) if(b[4+i]!=sync[i]) return false;
    for(int i=45;i<49;++i) if(b[i]!=1) return false;
    return true;
}

bool info1a_phase2_shape_ok(const std::vector<uint8_t>& b) {
    if (b.size()!=70) return false;
    static const uint8_t sync[8]={0,1,1,1,0,0,1,0};
    for(int i=0;i<4;++i) if(b[i]!=1) return false;
    for(int i=0;i<8;++i) if(b[4+i]!=sync[i]) return false;
    for(int i=66;i<70;++i) if(b[i]!=1) return false;
    return true;
}

constexpr double kProbeFreqs[] = {
    150,300,450,600,750,1050,1350,1500,1650,1950,2100,2250,
    2550,2700,2850,3000,3150,3300,3450,3600,3750
};
constexpr int kProbePhase180[] = {
    0,1,0,0,0,0,0,0,1,0,0,1,0,1,0,1,1,1,1,0,0
};

double sine_peak_for_dbm0_local(double dbm0) {
    return 32768.0*std::pow(10.0,(dbm0-3.14)/20.0);
}
}

uint16_t v34_info_crc(const std::vector<uint8_t>& information_bits) {
    // ITU-T V.34 10.1.2.3.2: x^16+x^12+x^5+1; register starts at
    // all ones. Bits are entered in transmitted order and CRC bit 0 is
    // transmitted first. The final reversal makes put_bits(...,16,crc)
    // emit that required register-bit order.
    uint16_t crc = 0xFFFFu;
    for (uint8_t bit : information_bits) {
        const uint16_t b = static_cast<uint16_t>((crc & 1u) ^ (bit & 1u));
        crc = static_cast<uint16_t>((crc >> 1) ^
              (b ? static_cast<uint16_t>((1u << 15) | (1u << 10) | (1u << 3)) : 0u));
    }
    uint16_t reversed = 0;
    for (int i = 0; i < 16; ++i)
        reversed |= static_cast<uint16_t>(((crc >> i) & 1u) << (15 - i));
    return reversed;
}

std::vector<uint8_t> build_v90_info0d_bits(const V90Info0dConfig& c) {
    std::vector<uint8_t> b;
    b.reserve(62);
    put_bits(b, 4, 0xF);      // bits 0:3 fill
    put_bits(b, 8, 0x72);     // bits 4:11 frame sync, 01110010 in time

    b.push_back(0);            // bit 12: reserved (0) in V.90
    b.push_back(0);            // bit 13: reserved (0) in V.90
    b.push_back(c.sr_3429);    // bit 14: 3429
    b.push_back(c.sr_3000_low);
    b.push_back(c.sr_3000_high);
    b.push_back(c.sr_3200_low);
    b.push_back(c.sr_3200_high);
    // V.90 bit 19: ZERO means 3429 transmission is disallowed.
    // v4 accidentally wrote disallow_3429 directly, reversing this meaning.
    b.push_back(!c.disallow_3429);
    b.push_back(c.power_reduction);
    put_uint_lsb(b, 3, std::min<unsigned>(5, c.max_symbol_rate_difference));
    b.push_back(c.cme_modem);
    b.push_back(c.constellation_1664);
    b.push_back(c.request_short_phase2);
    b.push_back(c.v92_capable);
    b.push_back(c.acknowledge_info0a);

    put_uint_lsb(b, 4, clamp_u((-c.nominal_tx_power_dbm0) - 6, 0, 15));
    put_uint_lsb(b, 5, clamp_u((-c.max_digital_tx_power_dbm0_x2) - 1, 0, 31));
    b.push_back(c.codec_output_power_measured);
    b.push_back(c.alaw);
    b.push_back(c.v90_upstream_3429);
    b.push_back(0);            // reserved

    // CRC covers information bits 12..41 only.
    std::vector<uint8_t> info(b.begin() + 12, b.end());
    const uint16_t crc = v34_info_crc(info);
    put_bits(b, 16, crc);
    put_bits(b, 4, 0xF);
    return b;
}

bool check_v90_info0d_crc(const std::vector<uint8_t>& bits) {
    if (bits.size() != 62) return false;
    std::vector<uint8_t> info(bits.begin() + 12, bits.begin() + 42);
    const uint16_t crc = v34_info_crc(info);
    uint16_t got = 0;
    for (size_t i = 42; i < 58; ++i) got = static_cast<uint16_t>((got << 1) | (bits[i] & 1u));
    return got == crc;
}

std::vector<uint8_t> build_v90_info0a_bits(bool acknowledge_info0d,
                                           bool v92_capable,
                                           bool request_short_phase2) {
    std::vector<uint8_t> b;
    b.reserve(49);
    put_bits(b,4,0xF);
    put_bits(b,8,0x72);
    // Reasonable V.90-capable analogue/V.34 capabilities for a test frame.
    b.push_back(0); // 2743 not required by V.90 analogue side
    b.push_back(0); // 2800 not required
    b.push_back(1); // 3429 capability
    b.push_back(1); // 3000 low
    b.push_back(1); // 3000 high
    b.push_back(1); // 3200 low
    b.push_back(1); // 3200 high
    b.push_back(1); // bit 19: 3429 not disallowed
    b.push_back(1); // can reduce power
    put_uint_lsb(b,3,5);
    b.push_back(0); // CME
    b.push_back(1); // 1664-point constellation
    b.push_back(v92_capable);
    b.push_back(request_short_phase2);
    b.push_back(acknowledge_info0d);
    std::vector<uint8_t> info(b.begin()+12,b.end()); // bits 12..28
    put_bits(b,16,v34_info_crc(info));
    put_bits(b,4,0xF);
    return b;
}

bool check_v90_info0a_crc(const std::vector<uint8_t>& bits) {
    if (!info0a_shape_ok(bits)) return false;
    std::vector<uint8_t> info(bits.begin()+12,bits.begin()+29);
    const uint16_t want=v34_info_crc(info);
    uint16_t got=0;
    for(size_t i=29;i<45;++i) got=static_cast<uint16_t>((got<<1)|(bits[i]&1u));
    return got==want;
}

V90Info1dConfig v90_info1d_config_from_info0a(const std::vector<uint8_t>& bits) {
    V90Info1dConfig c;
    c.projected_rate_x2400.fill(0);
    // ITU-T V.90 Table 3: 2400, 2743, 2800 are not supported for V.90 upstream (indices 0, 1, 2 = 0).
    // 3200 symbols/s is mandatory for V.90 upstream. 3000 and 3429 are optional.
    c.projected_rate_x2400[4] = 4; // 3200 symbols/s mandatory
    if(bits.size()<20 || !check_v90_info0a_crc(bits)) return c;
    if(bits[15] || bits[16]){
        c.projected_rate_x2400[3]=4;          // 3000
        c.high_carrier[3]=!bits[15] && bits[16];
    }
    if(bits[17] || bits[18]){
        c.projected_rate_x2400[4]=4;          // 3200
        c.high_carrier[4]=!bits[17] && bits[18];
    }
    if(bits[14] && bits[19]) c.projected_rate_x2400[5]=4; // 3429 allowed
    return c;
}


std::vector<uint8_t> build_v90_info1d_bits(const V90Info1dConfig& c) {
    std::vector<uint8_t> b;
    b.reserve(109);
    put_bits(b,4,0xF);
    put_bits(b,8,0x72);
    put_uint_lsb(b,3,std::min<unsigned>(7,c.min_power_reduction_db));
    put_uint_lsb(b,3,std::min<unsigned>(7,c.additional_power_reduction_db));
    put_uint_lsb(b,7,std::min<unsigned>(127,c.md_length_35ms));
    for(size_t i=0;i<6;++i){
        b.push_back(c.high_carrier[i]?1:0);
        put_uint_lsb(b,4,std::min<unsigned>(10,c.preemphasis[i]));
        put_uint_lsb(b,4,std::min<unsigned>(14,c.projected_rate_x2400[i]));
    }
    int fo=std::max(-512,std::min(511,c.frequency_offset_x002_hz));
    put_uint_lsb(b,10,static_cast<uint16_t>(fo)&0x03FFu);
    std::vector<uint8_t> info(b.begin()+12,b.end()); // bits 12..88
    put_bits(b,16,v34_info_crc(info));
    put_bits(b,4,0xF);
    return b;
}

bool check_v90_info1d_crc(const std::vector<uint8_t>& bits) {
    if(bits.size()!=109) return false;
    std::vector<uint8_t> info(bits.begin()+12,bits.begin()+89);
    const uint16_t want=v34_info_crc(info);
    uint16_t got=0;
    for(size_t i=89;i<105;++i) got=static_cast<uint16_t>((got<<1)|(bits[i]&1u));
    return got==want;
}

std::vector<uint8_t> build_v90_info1a_phase2_bits(bool request_v90,
                                                  uint8_t upstream_symbol_rate_index,
                                                  uint8_t uinfo,
                                                  uint8_t md_length_35ms) {
    std::vector<uint8_t> b;
    b.reserve(70);
    put_bits(b,4,0xF);
    put_bits(b,8,0x72);
    put_bits(b,6,0);             // reserved 12:17
    put_uint_lsb(b,7,md_length_35ms); // optional manufacturer-defined Phase-3 signal
    put_uint_lsb(b,7,std::max<unsigned>(67,std::min<unsigned>(127,uinfo)));
    put_bits(b,2,0);             // reserved 32:33
    const uint8_t us=std::max<uint8_t>(3,std::min<uint8_t>(6,upstream_symbol_rate_index));
    put_uint_lsb(b,3,us);        // V.34 symbol rate or V.92 PCM upstream
    // Downstream value 6 selects the PCM/V.90 family. For the V.34 test case,
    // clamp the selector to a valid V.34 symbol-rate value.
    put_uint_lsb(b,3,request_v90?6:std::min<uint8_t>(5,us));
    put_uint_lsb(b,10,0x200);    // -512: frequency-offset measurement unavailable
    std::vector<uint8_t> info(b.begin()+12,b.end()); // bits 12..49
    put_bits(b,16,v34_info_crc(info));
    put_bits(b,4,0xF);
    return b;
}

bool check_v90_info1a_phase2_crc(const std::vector<uint8_t>& bits) {
    if(!info1a_phase2_shape_ok(bits)) return false;
    std::vector<uint8_t> info(bits.begin()+12,bits.begin()+50);
    const uint16_t want=v34_info_crc(info);
    uint16_t got=0;
    for(size_t i=50;i<66;++i) got=static_cast<uint16_t>((got<<1)|(bits[i]&1u));
    return got==want;
}

std::optional<V90Info1aPhase2Frame> find_v90_info1a_phase2(const std::vector<int16_t>& pcm,
                                                           int sample_rate) {
    if(sample_rate<=0) return std::nullopt;
    constexpr size_t kDataBits=70, kSymbols=71;
    const size_t need=static_cast<size_t>(std::ceil(kSymbols*sample_rate/kInfoBitRate));
    if(pcm.size()<need) return std::nullopt;
    const size_t last=pcm.size()-need;
    for(size_t start=0;start<=last;++start){
        auto sym=info_symbols(pcm,start,kSymbols,2400.0,sample_rate);
        if(sym.size()!=kSymbols) continue;
        size_t good=0;
        const size_t sn=static_cast<size_t>(std::llround(sample_rate/kInfoBitRate));
        for(const auto& ph:sym) if(phasor_present(ph,sn)) ++good;
        if(good<kSymbols*3/4) continue;
        auto bits=differential_bits(sym);
        if(!info1a_phase2_shape_ok(bits)||!check_v90_info1a_phase2_crc(bits)) continue;
        V90Info1aPhase2Frame f;
        f.valid=true;
        f.md_length_35ms=static_cast<uint8_t>(get_uint_lsb(bits,18,25));
        f.uinfo=static_cast<uint8_t>(get_uint_lsb(bits,25,32));
        f.upstream_symbol_rate_index=static_cast<uint8_t>(get_uint_lsb(bits,34,37));
        f.pcm_upstream=(f.upstream_symbol_rate_index==6);
        f.requests_v90=(get_uint_lsb(bits,37,40)==6);
        unsigned raw=get_uint_lsb(bits,40,50);
        f.frequency_offset_x002_hz=(raw&0x200u)?static_cast<int>(raw)-1024:static_cast<int>(raw);
        f.bits=std::move(bits);
        f.first_sample=start+static_cast<size_t>(std::llround(sample_rate/kInfoBitRate));
        return f;
    }
    return std::nullopt;
}

std::vector<int16_t> v34_line_probe(double seconds,double total_power_dbm0,int sample_rate){
    if(seconds<=0.0||sample_rate<=0) return {};
    constexpr size_t N=sizeof(kProbeFreqs)/sizeof(kProbeFreqs[0]);
    const size_t ns=static_cast<size_t>(std::llround(seconds*sample_rate));
    const double per_tone_peak=sine_peak_for_dbm0_local(total_power_dbm0)/std::sqrt(static_cast<double>(N));
    std::vector<int16_t> out(ns);
    for(size_t n=0;n<ns;++n){
        const double t=static_cast<double>(n)/sample_rate;
        double y=0.0;
        for(size_t k=0;k<N;++k){
            const double ph=kProbePhase180[k]?kPi:0.0;
            y += per_tone_peak*std::cos(2.0*kPi*kProbeFreqs[k]*t+ph);
        }
        y=std::max(-32767.0,std::min(32767.0,y));
        out[n]=static_cast<int16_t>(std::lround(y));
    }
    return out;
}

double v34_line_probe_metric_dbfs(const std::vector<int16_t>& pcm,int sample_rate){
    if(pcm.size()<80||sample_rate<=0) return -120.0;
    constexpr size_t N=sizeof(kProbeFreqs)/sizeof(kProbeFreqs[0]);
    long double sum_peak2=0.0;
    for(size_t k=0;k<N;++k){
        auto ph=coherent(pcm,0,pcm.size(),kProbeFreqs[k],sample_rate);
        const double peak=2.0*std::hypot(ph.re,ph.im)/static_cast<double>(pcm.size());
        sum_peak2 += peak*peak;
    }
    const double rms=std::sqrt(static_cast<double>(sum_peak2/2.0L));
    return rms>1e-9?20.0*std::log10(rms/32768.0):-120.0;
}

bool v34_line_probe_present(const std::vector<int16_t>& pcm,int sample_rate){
    if(pcm.size()<80||sample_rate<=0) return false;
    constexpr size_t N=sizeof(kProbeFreqs)/sizeof(kProbeFreqs[0]);
    unsigned visible=0;
    for(size_t k=0;k<N;++k){
        auto ph=coherent(pcm,0,pcm.size(),kProbeFreqs[k],sample_rate);
        const double peak=2.0*std::hypot(ph.re,ph.im)/static_cast<double>(pcm.size());
        const double db=peak>1e-9?20.0*std::log10(peak/32768.0):-120.0;
        if(db>-48.0) ++visible;
    }
    return visible>=5 && v34_line_probe_metric_dbfs(pcm,sample_rate)>-52.0;
}

std::vector<int16_t> v90_info_dbpsk_modulate(const std::vector<uint8_t>& bits,
                                             double carrier_hz,
                                             double amplitude,
                                             int sample_rate,
                                             bool prepend_reference_point) {
    if (bits.empty() || sample_rate <= 0) return {};
    const size_t symbols=bits.size()+(prepend_reference_point?1u:0u);
    const size_t ns=static_cast<size_t>(std::llround(symbols*sample_rate/kInfoBitRate));
    std::vector<int> polarity(symbols,1);
    int p=1;
    size_t si=0;
    if(prepend_reference_point) polarity[si++]=p; // arbitrary reference phase
    for(uint8_t bit:bits){if(bit&1u)p=-p;polarity[si++]=p;}

    std::vector<int16_t> out(ns);
    for(size_t n=0;n<ns;++n){
        size_t sym=static_cast<size_t>(std::floor(n*kInfoBitRate/sample_rate));
        if(sym>=polarity.size())sym=polarity.size()-1;
        const double ph=2.0*kPi*carrier_hz*static_cast<double>(n)/sample_rate;
        double y=polarity[sym]*amplitude*std::sin(ph);
        y=std::max(-32767.0,std::min(32767.0,y));
        out[n]=static_cast<int16_t>(std::lround(y));
    }
    return out;
}

std::vector<uint8_t> v90_info_dbpsk_demodulate(const std::vector<int16_t>& pcm,
                                               double carrier_hz,
                                               int sample_rate,
                                               bool has_reference_point) {
    if(pcm.size()<20||sample_rate<=0)return{};
    const size_t nsym=static_cast<size_t>(std::floor(pcm.size()*kInfoBitRate/sample_rate));
    if(nsym<2)return{};
    auto sym=info_symbols(pcm,0,nsym,carrier_hz,sample_rate);
    if(sym.size()<2)return{};
    auto d=differential_bits(sym);
    if(has_reference_point)return d; // every data bit has its reference
    // Without a reference point, the first differential result corresponds to
    // the second transmitted data bit. Return it as a shortened stream.
    return d;
}

std::vector<int16_t> v90_info0a_waveform(const std::vector<uint8_t>& bits,
                                         int sample_rate) {
    // INFO0a: 2400-Hz DBPSK is 1 dB below nominal; 1800-Hz guard is
    // 7 dB below nominal. Use 6500 as an arbitrary nominal lab amplitude.
    const double nominal=6500.0;
    const double data_amp=nominal*std::pow(10.0,-1.0/20.0);
    const double guard_amp=nominal*std::pow(10.0,-7.0/20.0);
    auto out=v90_info_dbpsk_modulate(bits,2400.0,data_amp,sample_rate,true);
    for(size_t n=0;n<out.size();++n){
        double y=static_cast<double>(out[n])+guard_amp*std::sin(2.0*kPi*1800.0*n/sample_rate);
        y=std::max(-32767.0,std::min(32767.0,y));
        out[n]=static_cast<int16_t>(std::lround(y));
    }
    return out;
}

std::optional<V90Info0aFrame> find_v90_info0a(const std::vector<int16_t>& pcm,
                                              int sample_rate) {
    if(sample_rate<=0)return std::nullopt;
    constexpr size_t kDataBits=49;
    constexpr size_t kSymbols=1+kDataBits; // arbitrary reference point + frame
    const size_t need=static_cast<size_t>(std::ceil(kSymbols*sample_rate/kInfoBitRate));
    if(pcm.size()<need)return std::nullopt;

    // Brute-force one-sample timing acquisition. Phase is differential, so no
    // carrier phase search is required. A complete CRC-valid frame is a very
    // strong discriminator against speech/noise/guard-tone false positives.
    const size_t last=pcm.size()-need;
    for(size_t start=0;start<=last;++start){
        auto sym=info_symbols(pcm,start,kSymbols,2400.0,sample_rate);
        if(sym.size()!=kSymbols)continue;
        // Reject candidates with no coherent 2400-Hz INFO carrier.
        size_t good=0;
        for(const auto& p:sym)if(phasor_present(p,static_cast<size_t>(std::llround(sample_rate/kInfoBitRate))))++good;
        if(good<kSymbols*3/4)continue;
        auto bits=differential_bits(sym);
        if(!info0a_shape_ok(bits) || !check_v90_info0a_crc(bits))continue;
        V90Info0aFrame f;f.valid=true;
        f.v92_capable=bits[26]!=0;
        f.requests_short_phase2=bits[27]!=0;
        f.acknowledge_info0d=bits[28]!=0;f.bits=std::move(bits);
        f.first_sample=start+static_cast<size_t>(std::llround(sample_rate/kInfoBitRate));
        return f;
    }
    return std::nullopt;
}

std::vector<int16_t> v90_tone(double hz, double seconds, bool reversed,
                              double amplitude, int sample_rate) {
    if(seconds<=0||sample_rate<=0)return{};
    size_t n=static_cast<size_t>(std::llround(seconds*sample_rate));
    std::vector<int16_t> out(n);
    const double sign=reversed?-1.0:1.0;
    for(size_t i=0;i<n;++i){double y=sign*amplitude*std::sin(2.0*kPi*hz*i/sample_rate);out[i]=static_cast<int16_t>(std::lround(y));}
    return out;
}

V90ToneObservation observe_v90_tone(const std::vector<int16_t>& pcm,
                                    double hz,int sample_rate) {
    V90ToneObservation r;
    if(pcm.size()<40||sample_rate<=0)return r;
    auto p=coherent(pcm,0,pcm.size(),hz,sample_rate);
    r.re=p.re;r.im=p.im;r.energy=p.re*p.re+p.im*p.im;
    if(p.power>1.0)
        r.coherence=r.energy/(p.power*static_cast<double>(pcm.size()));
    r.present=phasor_present(p,pcm.size());
    return r;
}

bool v90_retrain_tone_present(const std::vector<int16_t>& pcm,
                              int sample_rate) {
    const auto tone = observe_v90_tone(pcm, 2400.0, sample_rate);
    // Theoretical coherence for a pure single tone is 0.50. Requiring >= 0.40
    // accepts clean Tone A while rejecting wideband modulated signal/noise (< 0.15).
    return tone.present && tone.coherence >= 0.40;
}

bool v90_infomarksa_present(const std::vector<int16_t>& pcm,int sample_rate) {
    if(sample_rate<=0) return false;
    const size_t symbol_samples=static_cast<size_t>(std::ceil(sample_rate/kInfoBitRate));
    if(pcm.size()<9*symbol_samples) return false;
    // Search one nominal symbol interval because RTP packet boundaries have no
    // relationship to the 600-bit/s clock. Eight consecutive phase changes
    // discriminate INFOMARKSa from the unmodulated recovery Tone A.
    for(size_t start=0;start<symbol_samples && start<pcm.size();++start){
        const size_t count=static_cast<size_t>(
            std::floor((pcm.size()-start)*kInfoBitRate/sample_rate));
        if(count<9) continue;
        auto sym=info_symbols(pcm,start,count,2400.0,sample_rate);
        if(sym.size()!=count) continue;
        auto bits=differential_bits(sym);
        size_t run=0;
        for(size_t i=0;i<bits.size();++i){
            const size_t a=start+static_cast<size_t>(std::llround(i*sample_rate/kInfoBitRate));
            const size_t z=start+static_cast<size_t>(std::llround((i+1)*sample_rate/kInfoBitRate));
            const size_t n=z>a?z-a:symbol_samples;
            const bool strong=phasor_present(sym[i],n) && phasor_present(sym[i+1],n);
            run=(strong && bits[i]) ? run+1 : 0;
            if(run>=8) return true;
        }
    }
    return false;
}

bool v90_phase_reversed(const V90ToneObservation& a,const V90ToneObservation& b) {
    if(!a.present||!b.present)return false;
    const double dot=a.re*b.re+a.im*b.im;
    const double ma=std::hypot(a.re,a.im),mb=std::hypot(b.re,b.im);
    return ma>0&&mb>0&&dot<-0.55*ma*mb;
}

std::optional<size_t> find_v90_phase_reversal(const std::vector<int16_t>& pcm,
                                              double hz,int sample_rate,
                                              size_t window_samples,
                                              size_t min_split) {
    if(sample_rate<=0||window_samples<24||pcm.size()<2*window_samples+1)return std::nullopt;
    std::optional<size_t> best;
    double best_score=0.0;
    const size_t start_split=std::max(window_samples,min_split);
    // Search every sample; the 10-ms default windows reject the unmodulated
    // 1800-Hz guard while remaining short enough for V.90 ranging timing.
    for(size_t split=start_split;split+window_samples<=pcm.size();++split){
        auto a=coherent(pcm,split-window_samples,split,hz,sample_rate);
        auto b=coherent(pcm,split,split+window_samples,hz,sample_rate);
        if(!phasor_present(a,window_samples)||!phasor_present(b,window_samples))continue;
        const double ma=std::hypot(a.re,a.im),mb=std::hypot(b.re,b.im);
        if(ma<=0||mb<=0)continue;
        const double c=(a.re*b.re+a.im*b.im)/(ma*mb);
        if(c<-0.70){const double score=-c*std::min(ma,mb);if(score>best_score){best_score=score;best=split;}}
    }
    return best;
}

} // namespace v92
