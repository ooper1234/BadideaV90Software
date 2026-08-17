#include "modem_modes.hpp"
#include <stdexcept>

namespace v92 {

static const ModemModeInfo kModes[] = {
    {ModemMode::V92, "V.92", 56000, 48000, PhyStatus::HandshakePrototype},
    {ModemMode::V90, "V.90", 56000, 33600, PhyStatus::HandshakePrototype},
    {ModemMode::V34, "V.34", 33600, 33600, PhyStatus::Planned},
    {ModemMode::V32bis, "V.32bis", 14400, 14400, PhyStatus::Planned},
    {ModemMode::V32, "V.32", 9600, 9600, PhyStatus::Planned},
    {ModemMode::V22bis, "V.22bis", 2400, 2400, PhyStatus::Planned},
    {ModemMode::V22, "V.22", 1200, 1200, PhyStatus::Planned},
    {ModemMode::V23_1200_75, "V.23 1200/75", 1200, 75, PhyStatus::Working},
    {ModemMode::V23_600_75, "V.23 600/75", 600, 75, PhyStatus::Working},
    {ModemMode::V21_300, "V.21", 300, 300, PhyStatus::Working},
};

const ModemModeInfo& mode_info(ModemMode mode) {
    for (const auto& m : kModes) if (m.mode == mode) return m;
    throw std::runtime_error("unknown modem mode");
}
const char* to_string(ModemMode mode){ return mode_info(mode).name; }
const char* to_string(PhyStatus s){
    switch(s){
        case PhyStatus::Working: return "working";
        case PhyStatus::HandshakePrototype: return "handshake-prototype";
        case PhyStatus::Planned: return "planned";
    }
    return "?";
}

std::vector<ModemMode> default_fallback_order() {
    return {ModemMode::V92, ModemMode::V90, ModemMode::V34, ModemMode::V32bis,
            ModemMode::V32, ModemMode::V22bis, ModemMode::V22,
            ModemMode::V23_1200_75, ModemMode::V23_600_75, ModemMode::V21_300};
}

FallbackController::FallbackController(bool include_incomplete)
    : include_incomplete_(include_incomplete) { reset(); }

void FallbackController::set_remote_capabilities(const std::set<ModemMode>& remote) {
    remote_ = remote; rebuild();
}
void FallbackController::reset(){ index_=0; rebuild(); }
void FallbackController::rebuild(){
    candidates_.clear(); index_=0;
    for(auto m: default_fallback_order()) {
        if (!remote_.empty() && !remote_.count(m)) continue;
        const auto& info=mode_info(m);
        if (!include_incomplete_ && info.phy_status != PhyStatus::Working) continue;
        candidates_.push_back(m);
    }
}
bool FallbackController::has_candidate() const { return index_ < candidates_.size(); }
ModemMode FallbackController::current() const {
    if(!has_candidate()) throw std::runtime_error("no fallback candidate");
    return candidates_[index_];
}
ModemMode FallbackController::fail_and_next() {
    if (index_ < candidates_.size()) ++index_;
    if(!has_candidate()) throw std::runtime_error("fallback exhausted");
    return candidates_[index_];
}

} // namespace v92
