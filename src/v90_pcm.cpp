/*
 * V.90 PCM mapping algebra.
 *
 * Clean C++ integration derived from the public V.90 mapping description and
 * Fabrice Bellard's GPLv2 linmodem v90.c research implementation (1999-2000).
 * This project is GPL-2.0-or-later; see THIRD_PARTY.md.
 */
#include "v90_pcm.hpp"
#include "g711.hpp"
#include "v90_phase2.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace v92 {
namespace {
static constexpr int sign_op[4] = {0, 0x55, 0xff, 0xaa};

uint64_t bits_to_u64_lsb(const std::vector<uint8_t>& bits, size_t off, int n) {
    uint64_t v=0;
    for(int i=0;i<n;++i) if(off+static_cast<size_t>(i)<bits.size()) v |= uint64_t(bits[off+i]&1u)<<i;
    return v;
}

size_t phase4_mapper_delay_samples(const V90PcmParameters& p) {
    int shaping_frame_symbols = 0;
    if (p.S == 5) shaping_frame_symbols = 6;
    else if (p.S == 4) shaping_frame_symbols = 3;
    else if (p.S == 3) shaping_frame_symbols = 2;
    return p.S == 6 ? 0u :
        static_cast<size_t>(shaping_frame_symbols * p.ld);
}

void put_lsb(std::vector<uint8_t>& bits, size_t at, unsigned count,
             unsigned value) {
    if (bits.size() < at + count) bits.resize(at + count, 0u);
    for (unsigned i = 0; i < count; ++i)
        bits[at + i] = static_cast<uint8_t>((value >> i) & 1u);
}
}

V90PcmParameters v90_pcm_lab_parameters() {
    V90PcmParameters p;
    p.alaw=false; p.S=6; p.K=24; p.ld=0;
    for (auto& v : p.allowed_ucodes) {
        for (int j=0;j<16;++j) v.push_back(static_cast<uint8_t>(10+j*6));
    }
    return p;
}

V90PcmMapper::V90PcmMapper(V90PcmParameters p):p_(std::move(p)) {
    if(p_.alaw || p_.S<3 || p_.S>6 || p_.K<1 || p_.K>56 || p_.ld<0 || p_.ld>3) return;
    uint64_t product=1;
    for(int j=0;j<6;++j){
        auto u=p_.allowed_ucodes[j];
        std::sort(u.begin(),u.end());
        u.erase(std::unique(u.begin(),u.end()),u.end());
        u.erase(std::remove_if(u.begin(),u.end(),[](uint8_t x){return x>127;}),u.end());
        if(u.empty()) return;
        // V.90 modulo values are ordered from the largest Ucode down.
        std::sort(u.begin(),u.end(),std::greater<uint8_t>());
        m_to_ucode_[j]=u;
        for(uint8_t x:u)m_to_linear_[j].push_back(static_cast<int16_t>(linear_for_ucode(x)));
        if(product <= (std::numeric_limits<uint64_t>::max()/u.size())) product*=u.size();
    }
    if(p_.K<63 && product < (uint64_t(1)<<p_.K)) return;
    valid_=true;
}

int V90PcmMapper::linear_for_ucode(uint8_t ucode) const {
    // V.90 Ucodes are the positive magnitude index. G.711 sign is supplied by
    // the spectral/sign mapper separately.
    return std::abs(static_cast<int>(ulaw_to_linear(ucode_to_ulaw(ucode,true))));
}

void V90PcmMapper::select_best_signs(int frame_size){
    struct Node{int t=0,x=0,y=0,v=0;int64_t w=0;int prev=0;};
    Node mem[2][6]{};
    int q=enc_Q_;
    int ptr=(ucode_ptr_-(frame_size*p_.ld))&(kRingBuf-1);
    mem[q][0]={enc_t_,enc_x_,enc_y_,enc_v_,0,q};
    const int eff_a1 = p_.a1;
    const int eff_a2 = p_.a2;
    const int eff_b1 = p_.b1;
    const int eff_b2 = p_.b2;
    for(int depth=0;depth<=p_.ld;++depth){
        for(int state=0;state<2;++state){
            int64_t best=std::numeric_limits<int64_t>::max(); Node bn{};
            int lbegin=(depth==0?q:0), lend=(depth==0?q+1:2);
            for(int ls=lbegin;ls<lend;++ls){
                // The shaping input belongs to the spectral-shaping frame
                // currently being evaluated.  With look-ahead enabled this
                // changes at each trellis depth; using the current caller's
                // pp value for every depth corrupts ld>0 sign selection.
                int sg=mem[ls][depth].t ^ pp_[ptr] ^ sign_op[(state<<1)|ls];
                int x1=mem[ls][depth].x,y1=mem[ls][depth].y,v1=mem[ls][depth].v; int64_t w=mem[ls][depth].w;
                for(int i=0;i<frame_size;++i){
                    int x=linear_for_ucode(ucode_[(ptr+i)&(kRingBuf-1)]);
                    if(((sg>>i)&1)==0)x=-x;
                    // V.90 5.4.5.6:
                    //   y[n] = x[n] - b1*x[n-1] + a1*y[n-1]
                    //   v[n] = y[n] - b2*y[n-1] + a2*v[n-1]
                    // Coefficients are signed Q1.6.
                    int y=x+static_cast<int>((
                        int64_t(eff_a1)*y1-int64_t(eff_b1)*x1)>>6);
                    int v=y+static_cast<int>((
                        int64_t(eff_a2)*v1-int64_t(eff_b2)*y1)>>6);
                    w += (int64_t(v)*v)>>4;
                    x1=x;y1=y;v1=v;
                }
                if(w<best){best=w;bn={sg,x1,y1,v1,w,ls};}
            }
            mem[state][depth+1]=bn;
        }
        ptr=(ptr+frame_size)&(kRingBuf-1);
    }
    int state=mem[1][p_.ld+1].w < mem[0][p_.ld+1].w ? 1:0;
    for(int depth=p_.ld;depth>0;--depth)state=mem[state][depth].prev;
    enc_Q_=state;enc_t_=mem[state][1].t;enc_x_=mem[state][1].x;enc_y_=mem[state][1].y;enc_v_=mem[state][1].v;
    ucode_ptr_=(ucode_ptr_+frame_size)&(kRingBuf-1);
}

std::array<int16_t,6> V90PcmMapper::encode(const std::vector<uint8_t>& data){
    std::array<int16_t,6> samples{};
    if(!valid_ || static_cast<int>(data.size())<p_.S+p_.K)return samples;
    uint64_t v=bits_to_u64_lsb(data,p_.S,p_.K);
    for(int i=0;i<6;++i){int m=static_cast<int>(m_to_ucode_[i].size());int k=static_cast<int>(v%uint64_t(m));v/=uint64_t(m);ucode_[(ucode_ptr_+i)&(kRingBuf-1)]=m_to_ucode_[i][k];}
    int signs=0,frame_size=6;
    if(p_.S==6){
        int l=enc_last_sign_;
        for(int i=0;i<6;++i){l=(data[i]&1)^l;signs|=l<<i;}enc_last_sign_=l;ucode_ptr_=(ucode_ptr_+6)&(kRingBuf-1);
    }else{
        int pv[3]={0,0,0};
        if(p_.S==5){int p1=(data[0]&1)^enc_last_sign_,p3=(data[2]&1)^p1,p5=(data[4]&1)^p3;enc_last_sign_=p5;pv[0]=(p1<<1)|((data[1]&1)<<2)|(p3<<3)|((data[3]&1)<<4)|(p5<<5);frame_size=6;}
        else if(p_.S==4){int p1=(data[0]&1)^enc_last_sign_,p4=(data[2]&1)^p1;enc_last_sign_=p4;pv[0]=(p1<<1)|((data[1]&1)<<2);pv[1]=(p4<<1)|((data[3]&1)<<2);frame_size=3;}
        else {int p1=(data[0]&1)^enc_last_sign_,p3=(data[1]&1)^p1,p5=(data[2]&1)^p3;enc_last_sign_=p5;pv[0]=p1<<1;pv[1]=p3<<1;pv[2]=p5<<1;frame_size=2;}
        int nf=6-p_.S;
        for(int i=0,j=0;i<nf;++i,j+=frame_size){pp_[ucode_ptr_]=static_cast<uint8_t>(pv[i]);select_best_signs(frame_size);signs|=(enc_t_&((1<<frame_size)-1))<<j;}
    }
    int ptr=(ucode_ptr_-6-(frame_size*p_.ld))&(kRingBuf-1);
    for(int i=0;i<6;++i){
        int x=linear_for_ucode(ucode_[ptr]);
        if(((signs>>i)&1)==0)x=x==0?-1:-x; // preserve PCMU negative zero
        samples[i]=static_cast<int16_t>(std::clamp(x,-32767,32767));
        ptr=(ptr+1)&(kRingBuf-1);
    }
    return samples;
}

std::vector<uint8_t> V90PcmMapper::decode(const std::array<int16_t,6>& samples){
    std::vector<uint8_t> data(static_cast<size_t>(p_.S+p_.K),0);
    if(!valid_)return data;
    uint64_t bitbuf=0; int signs[6]{};
    for(int j=5;j>=0;--j){
        int val=samples[j]; signs[j]=val>=0?1:0; if(val<0)val=-val;
        const auto& tab=m_to_linear_[j]; size_t best=0; int bd=std::numeric_limits<int>::max();
        for(size_t m=0;m<tab.size();++m){int d=std::abs(int(tab[m])-val);if(d<bd){bd=d;best=m;}}
        bitbuf=bitbuf*tab.size()+best;
    }
    for(int i=0;i<p_.K;++i)data[p_.S+i]=static_cast<uint8_t>((bitbuf>>i)&1u);
    if(p_.S==6){int l=dec_last_sign_;for(int i=0;i<6;++i){data[i]=static_cast<uint8_t>(l^signs[i]);l=signs[i];}dec_last_sign_=l;}
    else if(p_.S==5){int t0=0;for(int i=0;i<6;++i)t0|=signs[i]<<i;int pv=dec_t_^t0;int q1=(pv&1)^dec_Q_;pv^=sign_op[dec_Q_|(q1<<1)]&0x3f;dec_t_=t0;dec_Q_=q1;int p1=(pv>>1)&1,p3=(pv>>3)&1,p5=(pv>>5)&1;data[1]=(pv>>2)&1;data[3]=(pv>>4)&1;data[0]=p1^dec_last_sign_;data[2]=p1^p3;data[4]=p3^p5;dec_last_sign_=p5;}
    else if(p_.S==4){int t0=signs[0]|(signs[1]<<1)|(signs[2]<<2),t1=signs[3]|(signs[4]<<1)|(signs[5]<<2);int pv0=dec_t_^t0,q1=(pv0&1)^dec_Q_;pv0^=sign_op[dec_Q_|(q1<<1)]&7;int pv1=t0^t1,q2=(pv1&1)^q1;pv1^=sign_op[q1|(q2<<1)]&7;dec_t_=t1;dec_Q_=q2;int p1=(pv0>>1)&1,p4=(pv1>>1)&1;data[1]=(pv0>>2)&1;data[3]=(pv1>>2)&1;data[0]=p1^dec_last_sign_;data[2]=p4^p1;dec_last_sign_=p4;}
    else {int t0=signs[0]|(signs[1]<<1),t1=signs[2]|(signs[3]<<1),t2=signs[4]|(signs[5]<<1);int pv0=dec_t_^t0,q1=(pv0&1)^dec_Q_;pv0^=sign_op[dec_Q_|(q1<<1)]&3;int pv1=t0^t1,q2=(pv1&1)^q1;pv1^=sign_op[q1|(q2<<1)]&3;int pv2=t1^t2,q3=(pv2&1)^q2;pv2^=sign_op[q2|(q3<<1)]&3;dec_t_=t2;dec_Q_=q3;int p1=(pv0>>1)&1,p3=(pv1>>1)&1,p5=(pv2>>1)&1;data[0]=p1^dec_last_sign_;data[1]=p3^p1;data[2]=p5^p3;dec_last_sign_=p5;}
    return data;
}

std::vector<uint8_t> v90_trn2d_pcmu(const V90PcmParameters& parameters,
                                    size_t symbols) {
    symbols -= symbols % 6u;
    if (symbols < 2040u) return {};
    V90PcmMapper mapper(parameters);
    if (!mapper.valid()) return {};

    // Digital-modem scrambler GPC = 1 + x^-18 + x^-23. Both the scrambler
    // and all PCM mapping memories are zero at the beginning of TRN2d.
    uint32_t scrambler = 0;
    auto scrambled_one = [&]() {
        const uint8_t out = static_cast<uint8_t>(1u ^
            ((scrambler >> 17) & 1u) ^ ((scrambler >> 22) & 1u));
        scrambler = ((scrambler << 1) | out) & 0x7FFFFFu;
        return out;
    };

    // The streaming mapper has a spectral-shaper latency of ld shaping
    // frames.  V.90 8.6.5 nevertheless requires the *first transmitted*
    // TRN2d data frame to contain the magnitudes obtained from the first D
    // scrambled ones, independent of ld.  Generate through the mapper's
    // zero-initialized look-ahead latency, then discard only those delayed
    // zero-history symbols before placing TRN2d on the line.
    const size_t delay_symbols = phase4_mapper_delay_samples(parameters);
    const size_t generated_symbols = symbols + delay_symbols;
    const size_t generated_frames = (generated_symbols + 5u) / 6u;

    std::vector<uint8_t> delayed;
    delayed.reserve(generated_frames * 6u);
    const int frame_bits = mapper.bits_per_mapping_frame();
    for (size_t frame = 0; frame < generated_frames; ++frame) {
        std::vector<uint8_t> bits(static_cast<size_t>(frame_bits));
        for (auto& bit : bits) bit = scrambled_one();
        const auto pcm = mapper.encode(bits);
        for (int16_t sample : pcm) delayed.push_back(linear_to_ulaw(sample));
    }
    if (delay_symbols + symbols > delayed.size()) return {};
    return std::vector<uint8_t>(delayed.begin() + delay_symbols,
                                delayed.begin() + delay_symbols + symbols);
}

std::vector<uint8_t> build_v90_mp0_bits(int bits_per_data_frame,
                                        bool acknowledge,
                                        uint8_t max_upstream_rate_x2400,
                                        uint16_t upstream_rate_mask) {
    // Table 16/V.90. Type 0 ends at bit 85 and is padded with zeroes until
    // its encoded length ends on a complete six-PCM-symbol data frame.
    if (bits_per_data_frame < 1 || bits_per_data_frame > 62 ||
        max_upstream_rate_x2400 < 1 || max_upstream_rate_x2400 > 14)
        return {};

    std::vector<uint8_t> bits(86u, 0u);
    std::fill(bits.begin(), bits.begin() + 17u, 1u);
    bits[17] = 0u; // start
    bits[18] = 0u; // Type 0
    // 19:23 reserved = 0
    put_lsb(bits, 24u, 4u, max_upstream_rate_x2400);
    bits[28] = 0u;
    put_lsb(bits, 29u, 2u, 0u); // 16-state upstream trellis
    bits[31] = 0u;              // nonlinear encoder theta=0
    bits[32] = 0u;              // minimum constellation shaping
    bits[33] = acknowledge ? 1u : 0u;
    bits[34] = 0u; // start
    bits[35] = 0u; // reserved
    for (unsigned i = 0; i < 13u; ++i)
        bits[36u + i] = static_cast<uint8_t>((upstream_rate_mask >> i) & 1u);
    bits[49] = 0u; // reserved
    bits[50] = 0u; // reserved
    bits[51] = 0u; // start
    // Type-0 bits 52:67 reserved = 0.
    bits[68] = 0u; // start

    // V.34 information CRC excludes frame sync and all start bits.
    std::vector<uint8_t> information;
    information.reserve(48u);
    information.insert(information.end(), bits.begin() + 18u, bits.begin() + 34u);
    information.insert(information.end(), bits.begin() + 35u, bits.begin() + 51u);
    information.insert(information.end(), bits.begin() + 52u, bits.begin() + 68u);
    const uint16_t crc = v34_info_crc(information);
    for (unsigned i = 0; i < 16u; ++i)
        bits[69u + i] = static_cast<uint8_t>((crc >> (15u - i)) & 1u);
    bits[85] = 0u; // mandatory fill
    while (bits.size() % static_cast<size_t>(bits_per_data_frame))
        bits.push_back(0u);
    return bits;
}

V90Phase4DigitalTx::V90Phase4DigitalTx(const V90PcmParameters& parameters)
    : parameters_(parameters), mapper_(parameters) {
    valid_ = mapper_.valid();
    frame_bits_ = parameters_.S + parameters_.K;
    nominal_bit_rate_ = valid_ ? (frame_bits_ * 8000) / 6 : 0;
    initial_discard_samples_ = phase4_mapper_delay_samples(parameters_);
}

uint8_t V90Phase4DigitalTx::scramble(uint8_t input_bit) {
    const uint8_t out = static_cast<uint8_t>((input_bit & 1u) ^
        ((scrambler_ >> 17) & 1u) ^ ((scrambler_ >> 22) & 1u));
    scrambler_ = ((scrambler_ << 1) | out) & 0x7FFFFFu;
    return out;
}

std::vector<uint8_t> V90Phase4DigitalTx::encode_raw_bits(
    const std::vector<uint8_t>& raw_bits) {
    if (!valid_ || raw_bits.empty() || frame_bits_ <= 0 ||
        raw_bits.size() % static_cast<size_t>(frame_bits_) != 0u)
        return {};

    std::vector<uint8_t> generated;
    generated.reserve((raw_bits.size() / static_cast<size_t>(frame_bits_)) * 6u);
    size_t at = 0;
    while (at < raw_bits.size()) {
        std::vector<uint8_t> scrambled(static_cast<size_t>(frame_bits_));
        for (int i = 0; i < frame_bits_; ++i)
            scrambled[static_cast<size_t>(i)] =
                scramble(raw_bits[at + static_cast<size_t>(i)]);
        at += static_cast<size_t>(frame_bits_);
        const auto pcm = mapper_.encode(scrambled);
        for (int16_t sample : pcm) generated.push_back(linear_to_ulaw(sample));
    }

    if (initial_discard_samples_) {
        const size_t discard = std::min(initial_discard_samples_, generated.size());
        generated.erase(generated.begin(), generated.begin() + discard);
        initial_discard_samples_ -= discard;
    }
    return generated;
}

std::vector<uint8_t> V90Phase4DigitalTx::start_trn2d_and_mp(
    size_t trn2d_symbols) {
    if (!valid_) return {};
    trn2d_symbols -= trn2d_symbols % 6u;
    if (trn2d_symbols < 2040u) return {};

    const size_t trn_frames = trn2d_symbols / 6u;
    std::vector<uint8_t> raw(trn_frames * static_cast<size_t>(frame_bits_), 1u);
    const auto mp = build_v90_mp0_bits(frame_bits_, false);
    if (mp.empty()) return {};
    raw.insert(raw.end(), mp.begin(), mp.end());
    return encode_raw_bits(raw);
}

std::vector<uint8_t> V90Phase4DigitalTx::next_mp(bool acknowledge) {
    if (!valid_) return {};
    const auto mp = build_v90_mp0_bits(frame_bits_, acknowledge);
    return encode_raw_bits(mp);
}

std::vector<uint8_t> V90Phase4DigitalTx::start_ed_and_b1d() {
    if (!valid_ || frame_bits_ <= 0) return {};
    // Sequence Ed: 20 data frames of binary ones.
    // Sequence B1d: 48 data frames of binary ones.
    const size_t total_frames = 20u + 48u;
    std::vector<uint8_t> raw(total_frames * static_cast<size_t>(frame_bits_), 1u);
    return encode_raw_bits(raw);
}

void V90Phase4DigitalTx::feed_async_bytes(const std::vector<uint8_t>& bytes) {
    for (uint8_t byte : bytes) {
        // 8-N-1 async framing: start bit 0, 8 data bits LSB first, stop bit 1
        async_tx_bits_.push_back(0u);
        for (int i = 0; i < 8; ++i)
            async_tx_bits_.push_back(static_cast<uint8_t>((byte >> i) & 1u));
        async_tx_bits_.push_back(1u);
    }
}

std::vector<uint8_t> V90Phase4DigitalTx::produce_data_pcmu(size_t samples) {
    if (!valid_ || frame_bits_ <= 0 || samples == 0) return std::vector<uint8_t>(samples, 0xFF);

    std::vector<uint8_t> out;
    out.reserve(samples);

    while (out.size() < samples) {
        if (remaining_data_pcmu_.empty()) {
            // Generate in small chunks to keep latency low.
            // 27 frames * 6 = 162 samples, which covers a typical 160-sample RTP packet.
            const size_t frames_needed = 27;
            std::vector<uint8_t> raw(frames_needed * static_cast<size_t>(frame_bits_), 1u);

            // Drain queued async bits into raw payload; remaining bits remain idle ones (1u)
            for (size_t i = 0; i < raw.size() && !async_tx_bits_.empty(); ++i) {
                raw[i] = async_tx_bits_.front();
                async_tx_bits_.pop_front();
            }

            auto generated = encode_raw_bits(raw);
            remaining_data_pcmu_.insert(remaining_data_pcmu_.end(), generated.begin(), generated.end());
        }
        
        out.push_back(remaining_data_pcmu_.front());
        remaining_data_pcmu_.pop_front();
    }
    
    return out;
}

} // namespace v92

