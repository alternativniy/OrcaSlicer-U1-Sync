#include "SnapmakerPrinterAgent.hpp"
#include "Http.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/GUI_App.hpp"

#include "nlohmann/json.hpp"
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cctype>

namespace Slic3r {

namespace {

constexpr const char* SNAPMAKER_AGENT_VERSION = "0.0.1";

// Safely access a parallel array by index, returning a fallback if out of bounds.
template<typename T>
T safe_at(const std::vector<T>& vec, int index, const T& fallback)
{
    return (index >= 0 && index < static_cast<int>(vec.size())) ? vec[index] : fallback;
}

// combine_filament_type() folds cosmetic finishes (SnapSpeed, Silk, Wood, Matte, Marble) into the
// type string as "<base> <FINISH>" (e.g. "PLA HIGH SPEED"), but Orca/Snapmaker profiles never
// store that in filament_type -- only composite materials like "PLA-CF"/"PLA-GF" get a real,
// distinct filament_type. The finish is only ever spelled out in the preset *name*. Mirrors the
// same "strip after first space" convention PresetCollection::first_visible_idx_by_type() already
// applies for its own generic (non-vendor) fallback.
bool filament_type_matches(const std::string& preset_type, const std::string& target_type)
{
    if (preset_type == target_type)
        return true;
    auto sep = target_type.find(' ');
    return sep != std::string::npos && preset_type == target_type.substr(0, sep);
}

bool name_contains_ci(const std::string& name, const std::string& needle)
{
    if (needle.empty())
        return false;
    auto it = std::search(name.begin(), name.end(), needle.begin(), needle.end(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
    return it != name.end();
}

// Finds the visible, compatible filament preset whose vendor and type match, breaking ties by
// closest color. Unlike the generic AMS filament_id resolution in PresetBundle::sync_ams_list(),
// this considers presets regardless of "inherits": filament_vendor/filament_type are always
// fully resolved in Preset::config at load time (Preset.cpp copies the parent's config into the
// child, then applies the child's own overrides on top), so a plain "Save as" profile that never
// detached from its parent is just as valid a match candidate as a root preset.
//
// name_hint, when non-empty (e.g. "SnapSpeed"), is a marketing finish name that isn't part of
// filament_type; among candidates that otherwise tie, one whose name contains it is preferred
// over one that doesn't, so e.g. a SnapSpeed-tagged spool doesn't get matched to a same-vendor
// regular-finish preset purely because both share the plain "PLA" filament_type.
const Preset* find_closest_color_preset_by_vendor_and_type(const PresetCollection& filaments,
                                                             const std::string&      vendor_name,
                                                             const std::string&      filament_type,
                                                             const std::string&      color_rgba,
                                                             const std::string&      name_hint)
{
    const Preset* best_match         = nullptr;
    unsigned int  best_distance      = 0xffffffff;
    const Preset* best_hint_match    = nullptr;
    unsigned int  best_hint_distance = 0xffffffff;

    for (const auto& p : filaments.get_presets()) {
        if (p.is_visible && p.is_compatible && p.config.opt_string("filament_vendor", 0u) == vendor_name &&
            filament_type_matches(p.config.opt_string("filament_type", 0u), filament_type)) {
            // The printer returns RGBA in the format RRGGBBAA, but profiles store color as #RRGGBB,
            // so we must remove # and ignore alpha channel for distance calculation
            unsigned int target_color_value = std::stoul(color_rgba.substr(0, color_rgba.length() - 2), nullptr, 16);

            std::string  p_color = p.config.opt_string("default_filament_colour", 0u);
            unsigned int p_color_value;
            if (!p_color.empty()) {
                unsigned int hash_pos = p_color.find("#");
                p_color_value         = std::stoul(p_color.substr(hash_pos != std::string::npos ? hash_pos + 1 : 0), nullptr, 16);
            } else {
                // Default to black if no color specified in profile. Assume other profiles might be a closer color match.
                // Could be a problem if the target color is also black and there exist a specific profile for that type, vendor and color
                // combination.
                p_color_value = 0;
            }

            // Calculate Euclidean color distance in RGB space
            int dr = ((target_color_value & 0xff) - (p_color_value & 0xff));
            int dg = (((target_color_value >> 8) & 0xff) - ((p_color_value >> 8) & 0xff));
            int db = (((target_color_value >> 16) & 0xff) - ((p_color_value >> 16) & 0xff));
            unsigned int distance = dr * dr + dg * dg + db * db;

            if (distance < best_distance) {
                best_distance = distance;
                best_match    = &p;
            }
            if (name_contains_ci(p.name, name_hint) && distance < best_hint_distance) {
                best_hint_distance = distance;
                best_hint_match    = &p;
            }
        }
    }
    return best_hint_match ? best_hint_match : best_match;
}

// Marketing finish name for sub-types that combine_filament_type() folds into the type string but
// that never appear in any preset's filament_type field (see filament_type_matches() above) --
// only ever in the preset name, e.g. "Snapmaker PLA SnapSpeed @U1". CF/GF need no hint: those get
// a real, distinct filament_type ("PLA-CF") that already disambiguates on its own.
// `sub` must already be trimmed and upper-cased (see MoonrakerPrinterAgent::trim_and_upper()).
std::string sub_type_name_hint(const std::string& sub)
{
    if (sub == "SNAPSPEED" || sub == "HS")
        return "SnapSpeed";
    if (sub == "SILK")
        return "Silk";
    if (sub == "WOOD")
        return "Wood";
    if (sub == "MATTE")
        return "Matte";
    if (sub == "MARBLE")
        return "Marble";
    if (sub == "HF")
        return "HF";
    return "";
}

// Mirrors the eligibility test PresetBundle::sync_ams_list()/get_ams_cobox_infos() apply when
// resolving a filament_id back to a preset: only a *root* preset (no "inherits") is considered,
// and among those sharing an id the pick is a name-sort tie-break, not necessarily this one. If
// either holds, the generic resolution cannot be trusted to land back on `match`.
bool generic_resolution_may_miss(const PresetCollection& filaments, const Preset& match)
{
    if (filaments.get_preset_base(match) != &match)
        return true;
    for (const auto& other : filaments.get_presets())
        if (&other != &match && other.is_compatible && other.filament_id == match.filament_id &&
            filaments.get_preset_base(other) == &other)
            return true;
    return false;
}

} // anonymous namespace

SnapmakerPrinterAgent::SnapmakerPrinterAgent(std::string log_dir) : MoonrakerPrinterAgent(std::move(log_dir)) {}

AgentInfo SnapmakerPrinterAgent::get_agent_info_static()
{
    return AgentInfo{"snapmaker", "Snapmaker", SNAPMAKER_AGENT_VERSION, "Snapmaker printer agent"};
}

std::string SnapmakerPrinterAgent::combine_filament_type(const std::string& type, const std::string& sub_type)
{
    const std::string base = trim_and_upper(type);
    const std::string sub  = trim_and_upper(sub_type);

    if (base.empty())
        return "PLA";

    if (sub.empty() || sub == "NONE")
        return base;

    if (sub == "CF")
        return base + "-CF";
    if (sub == "GF")
        return base + "-GF";
    if (sub == "SNAPSPEED" || sub == "HS")
        return base + " HIGH SPEED";
    if (sub == "SILK")
        return base + " SILK";
    if (sub == "WOOD")
        return base + " WOOD";
    if (sub == "MATTE")
        return base + " MATTE";
    if (sub == "MARBLE")
        return base + " MARBLE";

    // Unrecognized sub-type (brand names like Polylite, Basic, etc.) -- use base type only
    return base;
}

bool SnapmakerPrinterAgent::fetch_filament_info(std::string dev_id)
{
    std::string url = join_url(device_info.base_url, "/printer/objects/query?print_task_config&filament_detect");

    std::string response_body;
    bool        success = false;
    std::string http_error;

    auto http = Http::get(url);
    if (!device_info.api_key.empty()) {
        http.header("X-Api-Key", device_info.api_key);
    }
    http.timeout_connect(5)
        .timeout_max(10)
        .on_complete([&](std::string body, unsigned status) {
            if (status == 200) {
                response_body = body;
                success       = true;
            } else {
                http_error = "HTTP error: " + std::to_string(status);
            }
        })
        .on_error([&](std::string body, std::string err, unsigned status) {
            http_error = err;
            if (status > 0) {
                http_error += " (HTTP " + std::to_string(status) + ")";
            }
        })
        .perform_sync();

    if (!success) {
        BOOST_LOG_TRIVIAL(warning) << "SnapmakerPrinterAgent::fetch_filament_info: HTTP request failed: " << http_error;
        return false;
    }

    auto json = nlohmann::json::parse(response_body, nullptr, false, true);
    if (json.is_discarded()) {
        BOOST_LOG_TRIVIAL(warning) << "SnapmakerPrinterAgent::fetch_filament_info: Invalid JSON response";
        return false;
    }

    // Navigate to result.status.print_task_config
    if (!json.contains("result") || !json["result"].contains("status") ||
        !json["result"]["status"].contains("print_task_config")) {
        BOOST_LOG_TRIVIAL(warning) << "SnapmakerPrinterAgent::fetch_filament_info: Missing print_task_config in response";
        return false;
    }

    auto& ptc = json["result"]["status"]["print_task_config"];

    // Read parallel arrays from print_task_config
    auto filament_exist    = ptc.value("filament_exist", std::vector<bool>{});
    auto filament_type     = ptc.value("filament_type", std::vector<std::string>{});
    auto filament_sub_type = ptc.value("filament_sub_type", std::vector<std::string>{});
    auto filament_color    = ptc.value("filament_color_rgba", std::vector<std::string>{});
    auto filament_vendor   = ptc.value("filament_vendor", std::vector<std::string>{});

    const int slot_count = static_cast<int>(filament_exist.size());
    if (slot_count == 0) {
        BOOST_LOG_TRIVIAL(info) << "SnapmakerPrinterAgent::fetch_filament_info: No filament slots reported";
        return false;
    }

    // Read NFC filament_detect data for temperature info (optional)
    nlohmann::json nfc_info;
    if (json["result"]["status"].contains("filament_detect") &&
        json["result"]["status"]["filament_detect"].contains("info")) {
        nfc_info = json["result"]["status"]["filament_detect"]["info"];
    }

    static const std::string empty_str;
    static const std::string default_color = "FFFFFFFF";

    std::vector<AmsTrayData> trays;
    trays.reserve(slot_count);
    m_refined_filament_matches.clear();

    for (int i = 0; i < slot_count; ++i) {
        AmsTrayData tray;
        tray.slot_index   = i;
        tray.has_filament = filament_exist[i];

        if (tray.has_filament) {
            tray.tray_type     = combine_filament_type(safe_at(filament_type, i, empty_str),
                                                       safe_at(filament_sub_type, i, empty_str));
            tray.tray_color    = safe_at(filament_color, i, default_color);

            auto* bundle = GUI::wxGetApp().preset_bundle;
            // Try to find a matching preset for this filament based on vendor, type and color.
            // If not found, default to traditional search by type only or generic type mapping.
            if (bundle) {
                std::string   vendor    = safe_at(filament_vendor, i, empty_str);
                std::string   name_hint = sub_type_name_hint(trim_and_upper(safe_at(filament_sub_type, i, empty_str)));
                const Preset* match = find_closest_color_preset_by_vendor_and_type(bundle->filaments, vendor, tray.tray_type,
                                                                                    tray.tray_color, name_hint);

                if (match) {
                    tray.tray_info_idx = match->filament_id;
                    BOOST_LOG_TRIVIAL(warning) << "Filament sync: Found manufacturer-specific profile for slot " << i << ": "
                                               << match->name;
                    // The generic filament_id resolution downstream may not land back on `match`
                    // (e.g. it's a "Save as" profile that never detached, or shares its id with a
                    // sibling); get_refined_filament_preset() lets the GUI substitute it in by name.
                    if (generic_resolution_may_miss(bundle->filaments, *match))
                        m_refined_filament_matches[i] = match->name;
                } else {
                    tray.tray_info_idx = bundle->filaments.filament_id_by_type(tray.tray_type);
                }
            } else {
                tray.tray_info_idx = map_filament_type_to_generic_id(tray.tray_type);
            }

            // Extract NFC temperature data if available
            if (nfc_info.is_array() && i < static_cast<int>(nfc_info.size()) && nfc_info[i].is_object()) {
                auto& nfc_slot = nfc_info[i];
                std::string vendor = nfc_slot.value("VENDOR", "NONE");
                if (vendor != "NONE" && !vendor.empty()) {
                    tray.bed_temp    = nfc_slot.value("BED_TEMP", 0);
                    tray.nozzle_temp = nfc_slot.value("FIRST_LAYER_TEMP", 0);
                }
            }
        }

        trays.emplace_back(std::move(tray));
    }

    build_ams_payload(1, slot_count - 1, trays);
    return true;
}

std::string SnapmakerPrinterAgent::get_refined_filament_preset(int slot_index) const
{
    auto it = m_refined_filament_matches.find(slot_index);
    return it != m_refined_filament_matches.end() ? it->second : std::string();
}

} // namespace Slic3r
