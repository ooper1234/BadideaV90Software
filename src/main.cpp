#include "v92_quickconnect.hpp"
#include "v21.hpp"
#include "v23.hpp"
#include "g711.hpp"
#include "async_serial.hpp"
#include "answer_tones.hpp"
#include "modem_modes.hpp"
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

static bool write_file(const std::string& p, const std::vector<uint8_t>& d){
    std::ofstream f(p, std::ios::binary); if(!f) return false;
    f.write(reinterpret_cast<const char*>(d.data()), static_cast<std::streamsize>(d.size())); return true;
}
static bool write_s16le(const std::string& p, const std::vector<int16_t>& d){
    std::ofstream f(p, std::ios::binary); if(!f) return false;
    f.write(reinterpret_cast<const char*>(d.data()), static_cast<std::streamsize>(d.size()*sizeof(int16_t))); return true;
}

int main(int argc, char** argv){
    using namespace v92;
    if(argc < 2){
        std::cout << "v92lab commands:\n"
                  << "  qts <ucode> <out.pcmu>\n"
                  << "  anspcm <-12|-15|-18|-9> <seconds> <out.pcmu>\n"
                  << "  qca1d <-12|-15|-18|-9> <out.s16le>\n"
                  << "  ansam <seconds> <out.s16le>\n"
                  << "  ans <seconds> <out.s16le>\n"
                  << "  v21-text <low|high> <text> <out.s16le>\n"
                  << "  v23-text <600|1200> <text> <out.s16le>\n"
                  << "  modes\n"
                  << "  demo-fallback [all]\n"
                  << "  demo-sm\n";
        return 0;
    }
    std::string cmd=argv[1];
    if(cmd=="qts" && argc==4){
        int u=std::stoi(argv[2]); auto x=build_qts_ulaw(static_cast<uint8_t>(u));
        if(!write_file(argv[3],x)) return 2;
        std::cout << "wrote "<<x.size()<<" PCMU symbols\n"; return 0;
    }
    if(cmd=="anspcm" && argc==5){
        int lev=std::stoi(argv[2]); double sec=std::stod(argv[3]); auto x=build_anspcm_ulaw(lev,sec);
        if(!write_file(argv[4],x)) return 2;
        std::cout << "wrote "<<x.size()<<" PCMU symbols\n"; return 0;
    }
    if(cmd=="qca1d" && argc==4){
        int lev=std::stoi(argv[2]); QCA1d q{true,lev}; auto bits=build_qca1d_bits(q);
        auto pcm=v21_modulate(bits,V21Band::High);
        if(!write_s16le(argv[3],pcm)) return 2;
        std::cout << "wrote "<<bits.size()<<" bits / "<<pcm.size()<<" PCM samples\n"; return 0;
    }
    if(cmd=="ansam" && argc==4){
        auto pcm=build_ansam(std::stod(argv[2]));
        if(!write_s16le(argv[3],pcm)) return 2;
        std::cout << "wrote V.8 ANSam: "<<pcm.size()<<" samples\n"; return 0;
    }
    if(cmd=="ans" && argc==4){
        auto pcm=build_ans_2100(std::stod(argv[2]),true);
        if(!write_s16le(argv[3],pcm)) return 2;
        std::cout << "wrote V.25-style 2100-Hz ANS: "<<pcm.size()<<" samples\n"; return 0;
    }
    if(cmd=="v21-text" && argc==5){
        V21Band band = std::string(argv[2])=="low" ? V21Band::Low : V21Band::High;
        std::string text=argv[3]; std::vector<uint8_t> bytes(text.begin(),text.end());
        auto bits=async_encode(bytes); auto pcm=v21_modulate(bits,band);
        if(!write_s16le(argv[4],pcm)) return 2;
        std::cout << "wrote V.21 300-bps async stream: "<<bits.size()<<" bits\n"; return 0;
    }
    if(cmd=="v23-text" && argc==5){
        V23ForwardMode mode=std::string(argv[2])=="600" ? V23ForwardMode::Baud600 : V23ForwardMode::Baud1200;
        std::string text=argv[3]; std::vector<uint8_t> bytes(text.begin(),text.end());
        auto bits=async_encode(bytes); auto pcm=v23_modulate_forward(bits,mode);
        if(!write_s16le(argv[4],pcm)) return 2;
        std::cout << "wrote V.23 async forward stream: "<<bits.size()<<" bits\n"; return 0;
    }
    if(cmd=="modes"){
        std::cout << "Fallback ladder (highest first):\n";
        for(auto m: default_fallback_order()){
            const auto& i=mode_info(m);
            std::cout << "  "<<i.name<<"  down="<<i.max_down_bps<<" up="<<i.max_up_bps
                      <<"  ["<<to_string(i.phy_status)<<"]\n";
        }
        return 0;
    }
    if(cmd=="demo-fallback"){
        bool all = argc>=3 && std::string(argv[2])=="all";
        FallbackController fb(all);
        std::set<ModemMode> remote;
        for(auto m: default_fallback_order()) remote.insert(m);
        fb.set_remote_capabilities(remote);
        std::cout << (all ? "full planned ladder" : "currently working PHY ladder") << ":\n";
        for(auto m: fb.candidates()) std::cout << "  try "<<to_string(m)<<"\n";
        return 0;
    }
    if(cmd=="demo-sm"){
        QuickConnectAnswerSM sm;
        auto show=[&]{std::cout<<to_string(sm.state())<<"\n";};
        show(); sm.on_call_answered(); show(); sm.on_200ms_elapsed(); show();
        sm.on_qc1a(QC1a{true,61}); show(); sm.on_qca1d_sent(); show(); sm.on_75ms_elapsed(); show();
        sm.on_qts_sent(); show(); sm.on_toneq_detected(); show(); sm.on_75ms_elapsed(); show();
        return 0;
    }
    std::cerr<<"bad arguments\n"; return 1;
}
