#include <catch2/catch_all.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/TriangleMesh.hpp"

using namespace Slic3r;

// Orca: on typical Cartesian/CoreXY kinematics the X-axis gantry beam spans the full bed
// width and only moves in Y, so reaching the wipe tower's Y coordinate sweeps the beam
// across that entire row - a tall object anywhere in that row can be hit by the gantry
// even if it is nowhere near the tower's XY footprint (see layered_print_cleareance_valid()
// in Print.cpp). This is a validate()-time (not slice-time) geometry/config check, so these
// tests only need Print::apply(), not a real multi-filament slice.

static Slic3r::DynamicPrintConfig two_filament_config()
{
    Slic3r::DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
    // full_print_config() only carries PrintRegionConfig keys; filament_diameter (PrintConfig)
    // is not among them, so DynamicPrintConfig::set_num_filaments() silently no-ops on it
    // (it resizes via option(key, false), which finds nothing to resize). Materialize it
    // directly instead. nozzle_diameter is deliberately left at its single-nozzle default
    // (single_extruder_multi_material) - that is what needs a wipe tower in the first place.
    config.set_deserialize_strict({
        { "filament_diameter",               "1.75,1.75" },
        { "enable_prime_tower",              "1" },
        { "wipe_tower_no_sparse_layers",     "1" },
        { "wipe_tower_x",                    "10" },
        { "wipe_tower_y",                    "100" },
        { "prime_tower_width",               "30" },
        { "extruder_clearance_height_to_rod","40" },
        { "extruder_clearance_radius",       "40" },
        { "printable_area",                  "0x0,300x0,300x300,0x300" }
    });
    return config;
}

// A tiny object on filament 2, so the plate genuinely references a second filament rather
// than just carrying an unused config slot.
static void add_filament2_object(Model &model, const Vec3d &offset)
{
    ModelObject *object = model.add_object();
    object->name += "filament2.stl";
    object->add_volume(Slic3r::make_cube(5, 5, 5));
    object->config.set_key_value("extruder", new ConfigOptionInt(2));
    object->volumes[0]->config.set_key_value("extruder", new ConfigOptionInt(2));
    object->add_instance()->set_offset(offset);
}

TEST_CASE("Wipe tower collision detection flags a tall object sharing its gantry row", "[WipeTower][Collision]")
{
    Slic3r::DynamicPrintConfig config = two_filament_config();

    Print print;
    Model model;

    // Tall object (60mm > extruder_clearance_height_to_rod), positioned far away in X from
    // the tower's X span ([10, 40]) but at the same Y (100) - same gantry row, no XY overlap.
    // extruder_clearance_radius is small on purpose, so the (already-existing) radius-proximity
    // check does not also trip, isolating the Y-band one.
    ModelObject *object = model.add_object();
    object->name += "tall.stl";
    object->add_volume(Slic3r::make_cube(20, 20, 60));
    object->add_instance()->set_offset(Vec3d(200, 100, 0));

    add_filament2_object(model, Vec3d(50, 250, 0));

    print.apply(model, config);
    REQUIRE(print.has_wipe_tower());

    StringObjectException warning;
    Polygons collison_polygons;
    std::vector<std::pair<Polygon, float>> height_polygons;
    StringObjectException err = print.validate(&warning, &collison_polygons, &height_polygons);

    INFO("err.string: " << err.string);
    REQUIRE_FALSE(err.string.empty());
    REQUIRE(err.type == STRING_EXCEPT_OBJECT_COLLISION_IN_LAYER_PRINT);
    REQUIRE_THAT(err.string, Catch::Matchers::ContainsSubstring("gantry"));
    // The visual keep-out zone (the swept row) must be populated, not just the error text.
    REQUIRE_FALSE(collison_polygons.empty());
}

TEST_CASE("Wipe tower collision detection ignores a tall object in a different row", "[WipeTower][Collision]")
{
    Slic3r::DynamicPrintConfig config = two_filament_config();

    Print print;
    Model model;

    // Same tall object, but far away in Y too (250 vs the tower's 100) and far in X -
    // different gantry row and outside the clearance radius, so neither check should fire.
    ModelObject *object = model.add_object();
    object->name += "tall.stl";
    object->add_volume(Slic3r::make_cube(20, 20, 60));
    object->add_instance()->set_offset(Vec3d(200, 250, 0));

    add_filament2_object(model, Vec3d(150, 50, 0));

    print.apply(model, config);
    REQUIRE(print.has_wipe_tower());

    StringObjectException warning;
    Polygons collison_polygons;
    std::vector<std::pair<Polygon, float>> height_polygons;
    StringObjectException err = print.validate(&warning, &collison_polygons, &height_polygons);

    INFO("err.string: " << err.string);
    REQUIRE(err.string.empty());
}
