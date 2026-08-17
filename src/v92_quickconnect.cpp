#include "v92_quickconnect.hpp"
#include "g711.hpp"
#include <algorithm>
#include <cmath>
#include <map>

namespace v92 {

static const std::map<uint8_t,uint8_t> kUqtsMap = {
    {0x0,61},{0x1,62},{0x2,63},{0x3,66},{0x4,67},{0x5,70},{0x6,71},{0x7,74},
    {0x8,75},{0x9,78},{0xA,79},{0xB,82},{0xC,83},{0xD,86},{0xE,87}
};

std::optional<QC1a> parse_qc1a(const std::vector<uint8_t>& bits) {
    if (bits.size() < 60) return std::nullopt;
    auto eq = [&](size_t a, const char* s) {
        for (size_t i=0; s[i]; ++i) if ((bits[a+i] & 1) != static_cast<uint8_t>(s[i]-'0')) return false;
        return true;
    };
    if (!eq(0,"1111111111") || !eq(10,"0101010101") ||
        !eq(30,"1111111111") || !eq(40,"0101010101")) return std::nullopt;
    if (bits[20] != 0 || bits[21] != 0 || bits[22] != 0 ||
        bits[25] != 0 || bits[29] != 1) return std::nullopt;
    // Table 2/V.92 repeats the complete data frame in bits 50:59. Checking
    // both copies prevents a random V.21 fragment from being accepted as a
    // saved Quick-Connect profile.
    for (size_t i=0;i<10;++i) if ((bits[20+i]&1)!=(bits[50+i]&1)) return std::nullopt;
    uint8_t wxyz = static_cast<uint8_t>((bits[24]<<3) | (bits[26]<<2) | (bits[27]<<1) | bits[28]);
    auto it = kUqtsMap.find(wxyz);
    if (it == kUqtsMap.end()) return std::nullopt; // 1111 is cleardown, not QC startup.
    QC1a q;
    q.lapm = bits[23] != 0;
    q.uqts = it->second;
    return q;
}

static uint8_t level_to_lm(int dbm0) {
    switch (dbm0) {
        case -9: case -10: return 0; // -9.5 dBm0 represented by LM=00
        case -12: return 1;
        case -15: return 2;
        case -18: return 3;
        default: return 1;
    }
}

std::vector<uint8_t> build_qca1d_bits(const QCA1d& qca) {
    std::vector<uint8_t> b;
    auto append = [&](const char* s){ while (*s) b.push_back(static_cast<uint8_t>(*s++ - '0')); };
    append("1111111111");
    append("0101010101");
    uint8_t lm = level_to_lm(qca.anspcm_level_dbm0);
    b.push_back(0); // start
    b.push_back(1); // digital modem
    b.push_back(1); // QCA
    b.push_back(qca.lapm ? 1 : 0);
    b.push_back(0); b.push_back(0); b.push_back(0);
    b.push_back((lm >> 1) & 1); b.push_back(lm & 1);
    b.push_back(1);
    append("1111111111");
    append("0101010101");
    // Bits 20:29 repeated.
    b.push_back(0); b.push_back(1); b.push_back(1); b.push_back(qca.lapm ? 1 : 0);
    b.push_back(0); b.push_back(0); b.push_back(0);
    b.push_back((lm >> 1) & 1); b.push_back(lm & 1); b.push_back(1);
    append("1111111111");
    return b;
}

std::vector<uint8_t> build_qts_ulaw(uint8_t uqts) {
    uint8_t vp = ucode_to_ulaw(uqts, true);
    uint8_t vn = ucode_to_ulaw(uqts, false);
    uint8_t zp = ucode_to_ulaw(0, true);
    uint8_t zn = ucode_to_ulaw(0, false);
    std::vector<uint8_t> out;
    out.reserve(128*6 + 8*6);
    for (int i=0;i<128;++i) {
        out.insert(out.end(), {vp,zp,vp,vn,zn,vn});
    }
    for (int i=0;i<8;++i) {
        out.insert(out.end(), {vn,zn,vn,vp,zp,vp});
    }
    return out;
}

std::vector<uint8_t> build_anspcm_ulaw(int level_dbm0, double seconds) {
    double scl;
    switch (level_dbm0) {
        case -9: case -10: scl = 1334.0; break;
        case -12: scl = 1000.0; break;
        case -15: scl = 708.0; break;
        case -18: scl = 500.0; break;
        default: scl = 1000.0; break;
    }
    constexpr double pi = 3.14159265358979323846;
    const double theta = 0.25 * pi / 301.0;
    std::vector<uint8_t> base(301);
    for (int k=0;k<301;++k) {
        double x = scl * std::sqrt(2.0) * std::cos(2.0*pi*k*79.0/301.0 + theta);
        // V.92 Table 6 expresses x in the 14-bit linear domain used by the
        // G.711 quantizer. linear_to_ulaw() accepts conventional 16-bit PCM,
        // so expand by four before encoding. Omitting this conversion made
        // ANSpcm 12 dB too quiet (C7 instead of Table 8's first codeword A9
        // at the -12 dBm0 setting).
        int16_t pcm = static_cast<int16_t>(4.0 * std::floor(x + 0.5));
        base[k] = linear_to_ulaw(pcm);
    }
    size_t n = static_cast<size_t>(std::ceil(seconds * 8000.0));
    std::vector<uint8_t> out;
    out.reserve(n);
    for (size_t i=0;i<n;++i) {
        uint8_t c = base[i % 301];
        // Phase reversal every 3612 symbols (12 * 301): invert G.711 polarity.
        if ((i / 3612) & 1) c ^= 0x80;
        out.push_back(c);
    }
    return out;
}

bool detect_toneq_980(const std::vector<int16_t>& pcm, int sample_rate) {
    // V.92 specifies at least 50 ms of TONEq. Requiring the whole interval
    // avoids advancing on a short speech/noise burst near 980 Hz.
    if (sample_rate <= 0 || pcm.size() < static_cast<size_t>(sample_rate * 0.05)) return false;
    auto energy = [&](double f) {
        double c=0,s=0;
        for (size_t i=0;i<pcm.size();++i) {
            double a = 2.0*M_PI*f*i/sample_rate;
            c += pcm[i]*std::cos(a); s += pcm[i]*std::sin(a);
        }
        return c*c+s*s;
    };
    double e980 = energy(980.0);
    double e900 = energy(900.0);
    double e1060 = energy(1060.0);
    return e980 > 4.0 * std::max(e900, e1060);
}

const char* to_string(QCState s) {
    switch(s) {
        case QCState::Idle: return "Idle";
        case QCState::AnswerSilence: return "AnswerSilence";
        case QCState::SendANSam: return "SendANSam";
        case QCState::WaitQC1aOrCM: return "WaitQC1aOrCM";
        case QCState::SendQCA1d: return "SendQCA1d";
        case QCState::Send75msSilence: return "Send75msSilence";
        case QCState::SendQTS: return "SendQTS";
        case QCState::SendANSpcm: return "SendANSpcm";
        case QCState::WaitTONEq: return "WaitTONEq";
        case QCState::SendPostTONEqSilence: return "SendPostTONEqSilence";
        case QCState::ShortPhase2: return "ShortPhase2";
        case QCState::FullV8Fallback: return "FullV8Fallback";
    }
    return "?";
}

void QuickConnectAnswerSM::on_call_answered(){ state_=QCState::AnswerSilence; }
void QuickConnectAnswerSM::on_200ms_elapsed(){ if(state_==QCState::AnswerSilence) state_=QCState::SendANSam; }
void QuickConnectAnswerSM::on_qc1a(const QC1a& qc){ if(state_==QCState::WaitQC1aOrCM || state_==QCState::SendANSam){ uqts_=qc.uqts; lapm_=qc.lapm; state_=QCState::SendQCA1d; } }
void QuickConnectAnswerSM::on_cm_detected(){ state_=QCState::FullV8Fallback; }
void QuickConnectAnswerSM::on_qca1d_sent(){ if(state_==QCState::SendQCA1d) state_=QCState::Send75msSilence; }
void QuickConnectAnswerSM::on_75ms_elapsed(){ if(state_==QCState::Send75msSilence) state_=QCState::SendQTS; else if(state_==QCState::SendPostTONEqSilence) state_=QCState::ShortPhase2; }
void QuickConnectAnswerSM::on_qts_sent(){ if(state_==QCState::SendQTS) state_=QCState::SendANSpcm; }
void QuickConnectAnswerSM::on_toneq_detected(){ if(state_==QCState::SendANSpcm || state_==QCState::WaitTONEq) state_=QCState::SendPostTONEqSilence; }
void QuickConnectAnswerSM::on_timeout(){ state_=QCState::FullV8Fallback; }

} // namespace v92
