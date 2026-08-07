#pragma once

#include "MoonrakerPrinterAgent.hpp"

#include <map>
#include <string>

namespace Slic3r {

class SnapmakerPrinterAgent final : public MoonrakerPrinterAgent
{
public:
    explicit SnapmakerPrinterAgent(std::string log_dir);
    ~SnapmakerPrinterAgent() override = default;

    static AgentInfo get_agent_info_static();
    AgentInfo        get_agent_info() override { return get_agent_info_static(); }

    bool fetch_filament_info(std::string dev_id) override;

    std::string get_refined_filament_preset(int slot_index) const override;

private:
    // Combine filament_type + filament_sub_type into a unified type string
    static std::string combine_filament_type(const std::string& type, const std::string& sub_type);

    // Populated by fetch_filament_info() for slots whose best vendor/type/color match cannot be
    // trusted to survive PresetBundle::sync_ams_list()'s generic filament_id-based resolution
    // (see IPrinterAgent::get_refined_filament_preset() for why). Keyed by AMS slot index.
    std::map<int, std::string> m_refined_filament_matches;
};

} // namespace Slic3r
