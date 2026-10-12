#include <catch2/catch_all.hpp>

#include "libslic3r/libslic3r.h"

#include "test_data.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <cstdlib>
#include <sstream>

using namespace Slic3r;
using namespace Slic3r::Test;
using Catch::Matchers::WithinAbs;

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

std::string slice_cube(const DynamicPrintConfig &config)
{
    Print print;
    Model model;
    init_print({ TestMesh::cube_20x20x20 }, print, model, config, true);
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

// Commands of every tool change, from the change G-code to the first move, without comments. Two slicings of the
// same model don't always give exactly the same paths, and the retraction before a travel, the fan speed and the
// progress depend on them, so the tests compare what a tool change emits rather than the whole G-code.
std::vector<std::string> tool_changes(const std::string &gcode)
{
    std::vector<std::string> changes;
    bool                     in_change = false;
    for (const std::string &line : lines_of(gcode)) {
        if (boost::starts_with(line, "; CHANGE prev=")) {
            changes.emplace_back();
            in_change = true;
        } else if (in_change && line.size() > 1 && line[0] == 'G' && line[1] >= '0' && line[1] <= '3' && (line.size() == 2 || line[2] == ' '))
            in_change = false;
        else if (in_change && !boost::starts_with(line, ";") && !boost::starts_with(line, "M106") && !boost::starts_with(line, "M107") &&
                 !boost::starts_with(line, "M73 "))
            changes.back() += line + "\n";
    }
    return changes;
}

// Value of "key=" in the marker line of the template.
double value_of(const std::string &line, const std::string &key)
{
    const size_t pos = line.find(key + "=");
    REQUIRE(pos != std::string::npos);
    return std::atof(line.c_str() + pos + key.size() + 1);
}

bool is_purge(const std::string &line) { return boost::starts_with(line, "M118 PURGE"); }

double float_at(const DynamicPrintConfig &config, const char *key) { return config.option<ConfigOptionFloats>(key)->get_at(0); }

// The first deretraction on the print after the purge at line i (the template's own E moves are the next two lines).
double deretraction_after(const std::vector<std::string> &lines, size_t i)
{
    for (size_t j = i + 3; j < lines.size(); ++j)
        if (boost::starts_with(lines[j], "G1 E") && lines[j].find("E-") == std::string::npos)
            return std::atof(lines[j].c_str() + 4);
    FAIL("no deretraction after the purge");
    return 0.;
}

} // namespace

TEST_CASE("Purge to bin is off by default and when disabled", "[PurgeToBin]")
{
    DynamicPrintConfig config    = two_filament_config(false);
    const std::string  reference = slice_cube(config);

    // Every purge setting changed, but purge_to_bin off: tool changes must not change.
    config.set_deserialize_strict({
        { "purge_bin_gcode",                 purge_bin_gcode },
        { "purge_bin_depart_retract",        7 },
        { "purge_bin_restart_extra",         -1 },
        { "filament_purge_bin_volume",       "33,33" },
        { "filament_purge_bin_first_volume", "30,30" },
        { "filament_purge_bin_dwell",        "5,5" },
    });
    const std::vector<std::string> changes = tool_changes(slice_cube(config));

    CHECK(changes.size() > 1);
    CHECK(changes == tool_changes(reference));
}

TEST_CASE("Purge to bin runs at the end of every tool change", "[PurgeToBin]")
{
    DynamicPrintConfig config = two_filament_config(false);
    enable_purge_to_bin(config);
    const std::vector<std::string> lines = lines_of(slice_cube(config));

    const double area    = 0.25 * M_PI * 1.75 * 1.75;
    const double retract = float_at(config, "retraction_length");
    const double depart  = float_at(config, "purge_bin_depart_retract");

    size_t changes = 0, purges = 0, first_uses = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        // Skip the first pick in the start G-code: nothing to purge there.
        if (boost::starts_with(lines[i], "; CHANGE prev=") && !boost::starts_with(lines[i], "; CHANGE prev=-1"))
            ++changes;
        if (!is_purge(lines[i]))
            continue;
        ++purges;
        const double volume = value_of(lines[i], "volume");
        if (value_of(lines[i], "first") == 1.) {
            ++first_uses;
            // Only the second filament is new, the first one starts the print.
            CHECK(value_of(lines[i], "next") == 1.);
            CHECK_THAT(volume, WithinAbs(float_at(config, "filament_purge_bin_first_volume"), 1e-6));
        } else
            CHECK_THAT(volume, WithinAbs(float_at(config, "filament_purge_bin_volume"), 1e-6));
        CHECK_THAT(value_of(lines[i], "wipe"), WithinAbs(depart - retract, 1e-6));
        CHECK_THAT(value_of(lines[i], "dwell"), WithinAbs(1000. * float_at(config, "filament_purge_bin_dwell"), 1e-6));

        // After the template: modes and feedrate restored, filament usage tag, then straight to the print: no temperature
        // wait, no PA change, no tool change, and the deretraction gives back depart_retract + restart_extra.
        bool g90 = false, m83 = false, usage_tag = false;
        for (size_t j = i + 3; j < lines.size() && !(boost::starts_with(lines[j], "G1 X") && lines[j].find(" E") != std::string::npos); ++j) {
            const std::string &line = lines[j];
            g90 |= line == "G90";
            m83 |= line == "M83";
            if (boost::starts_with(line, "; EXTERNAL_PURGE ")) {
                usage_tag = true;
                CHECK_THAT(std::atof(line.c_str() + 17), WithinAbs(volume / area, 1e-4));
            }
            CHECK_FALSE(boost::starts_with(line, "M109"));
            CHECK_FALSE(boost::starts_with(line, "M900"));
            CHECK_FALSE(boost::starts_with(line, "T"));
        }
        CHECK(g90);
        CHECK(m83);
        CHECK(usage_tag);
        CHECK_THAT(deretraction_after(lines, i), WithinAbs(depart + float_at(config, "purge_bin_restart_extra"), 1e-6));
    }
    CHECK(changes > 1);
    CHECK(purges == changes);
    CHECK(first_uses == 1);
}

TEST_CASE("Purge to bin refills what was retracted when the tool was put away", "[PurgeToBin]")
{
    DynamicPrintConfig config = two_filament_config(false);
    enable_purge_to_bin(config);

    std::vector<double> refills;
    for (const std::string &line : lines_of(slice_cube(config)))
        if (is_purge(line))
            refills.emplace_back(value_of(line, "refill"));
    REQUIRE(refills.size() > 2);
    // The second filament on its first use was never retracted. Afterwards each tool was put away with the tool change retraction.
    CHECK(refills[0] == 0.);
    const double toolchange_retract = float_at(config, "retract_length_toolchange") + float_at(config, "retract_restart_extra_toolchange");
    for (size_t i = 1; i < refills.size(); ++i)
        CHECK_THAT(refills[i], WithinAbs(toolchange_retract, 1e-6));
}

TEST_CASE("Purge to bin retraction from the printer, overridden by the filament", "[PurgeToBin]")
{
    DynamicPrintConfig config = two_filament_config(false);
    enable_purge_to_bin(config);
    config.set_deserialize_strict({
        { "purge_bin_depart_retract",          5 },
        { "purge_bin_restart_extra",           -0.5 },
        { "filament_purge_bin_depart_retract", "nil,7" },
    });
    const std::vector<std::string> lines = lines_of(slice_cube(config));

    size_t checked = 0;
    for (size_t i = 0; i < lines.size(); ++i)
        if (is_purge(lines[i])) {
            // The first filament takes the printer's value, the second one its own.
            const double depart = value_of(lines[i], "next") == 0. ? 5. : 7.;
            CHECK_THAT(deretraction_after(lines, i), WithinAbs(depart - 0.5, 1e-6));
            ++checked;
        }
    CHECK(checked > 2);
}

TEST_CASE("Purge to bin restores the fan where the template asks for it", "[PurgeToBin]")
{
    DynamicPrintConfig config = two_filament_config(false);
    enable_purge_to_bin(config);
    config.set_deserialize_strict({
        { "purge_bin_gcode", "M118 PURGE\nG1 E{purge_bin_refill + purge_bin_length} F{purge_bin_feedrate}\n"
                             ";_FORCE_RESUME_FAN_SPEED\nM118 LEAVE\nG1 E-{purge_bin_retract + purge_bin_wipe_retract} F2400\n" },
    });
    const std::vector<std::string> lines = lines_of(slice_cube(config));

    // The template restores the fan before leaving the bin: no fan command between leaving and the travel to the print.
    size_t checked = 0;
    for (size_t i = 0; i < lines.size(); ++i)
        if (lines[i] == "M118 LEAVE") {
            for (size_t j = i + 1; j < lines.size() && !boost::starts_with(lines[j], "G1 X"); ++j) {
                CHECK_FALSE(boost::starts_with(lines[j], "M106"));
                CHECK_FALSE(boost::starts_with(lines[j], "M107"));
            }
            ++checked;
        }
    CHECK(checked > 2);
}

TEST_CASE("Purge to bin with absolute extrusion distances", "[PurgeToBin]")
{
    DynamicPrintConfig config = two_filament_config(false);
    enable_purge_to_bin(config);
    config.set_deserialize_strict({ { "use_relative_e_distances", 0 } });
    const std::vector<std::string> lines = lines_of(slice_cube(config));

    size_t purges = 0;
    for (size_t i = 0; i < lines.size(); ++i)
        if (is_purge(lines[i])) {
            ++purges;
            // Absolute mode restored and E counted from zero again, the template's E moves are unknown to the writer.
            bool m82 = false, reset = false;
            for (size_t j = i + 3; j < lines.size() && !boost::starts_with(lines[j], "G1 X"); ++j) {
                m82 |= lines[j] == "M82";
                reset |= boost::starts_with(lines[j], "G92 E0");
            }
            CHECK(m82);
            CHECK(reset);
        }
    CHECK(purges > 0);
}

TEST_CASE("Purge to bin does nothing where it is not supported", "[PurgeToBin]")
{
    SECTION("prime tower") {
        DynamicPrintConfig config    = two_filament_config(true);
        const std::string  reference = slice_two_cubes(config);
        enable_purge_to_bin(config);
        const std::string gcode = slice_two_cubes(config);

        REQUIRE(gcode.find(";TYPE:Prime tower") != std::string::npos);
        const std::vector<std::string> changes = tool_changes(gcode);
        CHECK(changes.size() > 1);
        CHECK(changes == tool_changes(reference));
    }
    SECTION("firmware retraction") {
        DynamicPrintConfig config = two_filament_config(false);
        config.set_deserialize_strict({ { "use_firmware_retraction", 1 } });
        const std::string reference = slice_cube(config);
        enable_purge_to_bin(config);
        const std::vector<std::string> changes = tool_changes(slice_cube(config));

        CHECK(changes.size() > 1);
        CHECK(changes == tool_changes(reference));
    }
}
