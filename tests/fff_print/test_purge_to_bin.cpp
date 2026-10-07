#include <catch2/catch_all.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/GCodeReader.hpp"

#include "test_data.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <sstream>

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

// Infill on the second filament gives a tool change on every layer. One nozzle keeps the test away from
// the multi-nozzle filament grouping, set_extruder() still runs a regular tool change. Purge to bin is for
// tool changers, not for single extruder MM.
DynamicPrintConfig two_filament_config(bool prime_tower)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "single_extruder_multi_material", 0 },
        { "filament_diameter",         "1.75,1.75" },
        { "filament_colour",           "#FF0000;#00FF00" },
        { "flush_volumes_matrix",      "0,100,100,0" },
        { "sparse_infill_filament_id", 2 },
        { "sparse_infill_density",     "20%" },
        { "enable_prime_tower",        prime_tower },
        { "ooze_prevention",           1 },
        { "standby_temperature_delta", -5 },
        { "enable_pressure_advance",   "1,1" },
        { "change_filament_gcode",     "; CHANGE prev=[previous_extruder]\n" },
    });
    return config;
}

const char *purge_bin_gcode =
    "M118 PURGE first=[purge_bin_first_use] next=[next_extruder] refill=[purge_bin_refill] volume=[purge_bin_volume] "
    "wipe=[purge_bin_wipe_retract] dwell=[purge_bin_dwell_ms]\n"
    "G1 E{purge_bin_refill + purge_bin_length} F{purge_bin_feedrate}\n"
    "G1 E-{purge_bin_retract + purge_bin_wipe_retract} F2400\n";

void enable_purge_to_bin(DynamicPrintConfig &config)
{
    config.set_deserialize_strict({
        { "purge_to_bin",    1 },
        { "purge_bin_gcode", purge_bin_gcode },
    });
}

std::vector<std::string> lines_of(const std::string &gcode)
{
    std::vector<std::string> lines;
    std::istringstream       in(gcode);
    for (std::string line; std::getline(in, line);)
        lines.emplace_back(line);
    return lines;
}

std::string slice_cube(const DynamicPrintConfig &config, bool comments = false)
{
    Print print;
    Model model;
    init_print({ TestMesh::cube_20x20x20 }, print, model, config, comments);
    print.is_BBL_printer() = false; // not initialized by Print
    return gcode(print);
}

// Two cubes on different filaments. With the infill filament alone, normalize_fdm_2() sees one used filament
// and switches the prime tower off.
std::string slice_two_cubes(const DynamicPrintConfig &config_in)
{
    Print print;
    Model model;
    init_print({ TestMesh::cube_20x20x20, TestMesh::cube_20x20x20 }, print, model, config_in, true);
    model.objects[1]->config.set_key_value("extruder", new ConfigOptionInt(2));
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.apply(config_in);
    print.apply(model, config);
    print.is_BBL_printer() = false;
    return gcode(print);
}

// G-code without comments: drops the config dump and the object labels with run-dependent ids.
std::string strip_comments(const std::string &gcode)
{
    std::string out;
    for (const std::string &line : lines_of(gcode))
        if (!boost::starts_with(line, ";"))
            out += line + "\n";
    return out;
}

} // namespace

TEST_CASE("Purge to bin is off by default and when disabled", "[PurgeToBin]")
{
    DynamicPrintConfig config = two_filament_config(false);
    const std::string  reference = slice_cube(config);

    // Every purge setting changed, but purge_to_bin off: the G-code must not change.
    config.set_deserialize_strict({
        { "purge_bin_gcode",           purge_bin_gcode },
        { "purge_bin_first_volume",    30 },
        { "purge_bin_depart_retract",  7 },
        { "purge_bin_restart_trim",    1 },
        { "purge_bin_wait_temp",       0 },
        { "filament_purge_bin_volume", "33,33" },
    });
    const std::string gcode = slice_cube(config);

    CHECK(strip_comments(gcode).find("PURGE") == std::string::npos);
    CHECK(strip_comments(gcode) == strip_comments(reference));
}

TEST_CASE("Purge to bin runs at the end of every tool change", "[PurgeToBin]")
{
    DynamicPrintConfig config = two_filament_config(false);
    enable_purge_to_bin(config);
    const std::vector<std::string> lines = lines_of(slice_cube(config, true));

    size_t changes = 0, purges = 0, first_uses = 0, purge_tags = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        // Filament usage and time tags for the G-code processor.
        if (boost::starts_with(lines[i], "; EXTERNAL_PURGE "))
            ++purge_tags;
        // Skip the first pick in the start G-code: nothing to purge there.
        if (boost::starts_with(lines[i], "; CHANGE") && !boost::starts_with(lines[i], "; CHANGE prev=-1"))
            ++changes;
        if (!boost::starts_with(lines[i], "M118 PURGE"))
            continue;
        ++purges;
        if (lines[i].find("first=1") != std::string::npos) {
            ++first_uses;
            // Only the second tool is new, the first one starts the print.
            CHECK(lines[i].find("next=1") != std::string::npos);
            CHECK(lines[i].find("volume=48") != std::string::npos);
        } else
            CHECK(lines[i].find("volume=20") != std::string::npos);
        CHECK(lines[i].find("wipe=9.2") != std::string::npos);
        CHECK(lines[i].find("dwell=2000") != std::string::npos);

        // Nothing parks the nozzle between the purge and the print: no temperature wait, no PA change,
        // and the first deretraction gives back the depart retraction minus the restart trim.
        bool deretracted = false;
        for (size_t j = i + 1; j < lines.size() && !deretracted; ++j) {
            const std::string &line = lines[j];
            CHECK_FALSE(boost::starts_with(line, "M109"));
            CHECK_FALSE(boost::starts_with(line, "M900"));
            CHECK_FALSE(boost::starts_with(line, "T"));
            if (boost::starts_with(line, "G1 E") && line.find("E-") == std::string::npos && j > i + 2) {
                CHECK(boost::starts_with(line, "G1 E9.5 "));
                deretracted = true;
            }
        }
        CHECK(deretracted);
    }
    CHECK(changes > 1);
    CHECK(purges == changes);
    CHECK(purge_tags == purges);
    CHECK(first_uses == 1);
}

TEST_CASE("Purge to bin refills what was retracted when the tool was put away", "[PurgeToBin]")
{
    DynamicPrintConfig config = two_filament_config(false);
    enable_purge_to_bin(config);
    const std::vector<std::string> lines = lines_of(slice_cube(config, true));

    std::vector<std::string> refills;
    for (const std::string &line : lines)
        if (boost::starts_with(line, "M118 PURGE")) {
            const size_t pos = line.find("refill=");
            refills.emplace_back(line.substr(pos, line.find(' ', pos) - pos));
        }
    REQUIRE(refills.size() > 2);
    // Tool 1 on its first use was never retracted. Afterwards each tool was left with the depart retraction
    // minus the restart trim, then retracted again for the tool change.
    CHECK(refills[0] == "refill=0");
    for (size_t i = 1; i < refills.size(); ++i)
        CHECK(refills[i] != "refill=0");
}

TEST_CASE("Purge to bin does nothing with the prime tower enabled", "[PurgeToBin]")
{
    DynamicPrintConfig config = two_filament_config(true);
    const std::string  reference = slice_two_cubes(config);
    enable_purge_to_bin(config);
    const std::string gcode = slice_two_cubes(config);

    REQUIRE(gcode.find("; CHANGE prev=0") != std::string::npos);
    REQUIRE(gcode.find(";TYPE:Prime tower") != std::string::npos);
    CHECK(strip_comments(gcode).find("PURGE") == std::string::npos);
    CHECK(strip_comments(gcode) == strip_comments(reference));
}
