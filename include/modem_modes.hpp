#pragma once
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace v92 {

enum class ModemMode {
    V92,
    V90,
    V34,
    V32bis,
    V32,
    V22bis,
    V22,
    V23_1200_75,
    V23_600_75,
    V21_300
};

enum class PhyStatus {
    Working,              // data PHY exists in this prototype
    HandshakePrototype,   // pieces exist, but no complete data PHY yet
    Planned               // fallback slot exists; DSP still to implement
};

struct ModemModeInfo {
    ModemMode mode;
    const char* name;
    int max_down_bps;
    int max_up_bps;
    PhyStatus phy_status;
};

const ModemModeInfo& mode_info(ModemMode mode);
const char* to_string(ModemMode mode);
const char* to_string(PhyStatus status);
std::vector<ModemMode> default_fallback_order();

// Keeps a ranked list of mutually supported modes. A failed training attempt moves
// to the next lower mode. By default it only returns PHYs that are genuinely working.
class FallbackController {
public:
    explicit FallbackController(bool include_incomplete = false);
    void set_remote_capabilities(const std::set<ModemMode>& remote);
    void reset();
    bool has_candidate() const;
    ModemMode current() const;
    ModemMode fail_and_next();
    const std::vector<ModemMode>& candidates() const { return candidates_; }

private:
    bool include_incomplete_ = false;
    std::set<ModemMode> remote_;
    std::vector<ModemMode> candidates_;
    size_t index_ = 0;
    void rebuild();
};

} // namespace v92
