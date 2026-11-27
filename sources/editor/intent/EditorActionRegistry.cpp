#include "editor/intent/EditorActionRegistry.h"
#if WITH_EDITOR

#include "editor/intent/EditorActionBuilders.h" // R10.3
#include "editor/EditorContext.h"
#include "editor/commands/SetVesselTuneCommand.h"
#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

#include "core/logging/Log.h"
#include "rendering/view/Camera.h"
#include "app/scene/Scene.h"
#include "editor/EditorFraming.h"
#include "editor/intent/EditorIntentResolver.h"
#include "editor/assets/AssetRegistry.h"
#include "app/scene/TimeOfDay.h"              // T4: setTime / setDate
#include "editor/commands/EditorCommandStack.h"
#include "editor/scene/EditorZone.h"
#include "editor/scene/EditorPlayerStart.h"
#include "editor/assets/MeshRestPose.h"
#include "materials/MaterialDataManager.h"

// R10.3: the builders the table names live in EditorActions*.cpp (EditorActionBuilders.h).
using namespace editoractions;

// ----------------------------------------------------------------- the registry

EditorActionRegistry::EditorActionRegistry()
{
    // Descriptions are written for a reader who has never seen this editor -- that reader is
    // the model. "bury" without its sentence gets confused with hiding; "select" without its
    // sentence gets picked for "delete the palms" because selecting them is a step towards it.
    actions_.push_back({
        "select",
        "Select the matching objects and change nothing else. Use it when the user only wants to "
        "find or highlight objects, never as a step towards another action.",
        EditorActionEffect::SelectionOnly,
        EditorTargetKind::Objects,
        {},
        nullptr,
    });

    actions_.push_back({
        "count",
        "Answer how many objects match, and which assets they are, WITHOUT changing "
        "anything. Use it for questions -- 'how many palms are there', 'what is on the "
        "island', 'are there any rocks near the camera'. Never use it when the designer "
        "asked for something to happen.",
        EditorActionEffect::ReadOnly,
        EditorTargetKind::Objects,
        {},
        nullptr,
    });

    actions_.push_back({
        "setEnabled",
        "Show or hide the matching objects. Hidden objects stay in the level at their position and "
        "can be shown again; this does NOT move or remove anything.",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::Objects,
        { { "enabled", EditorParamKind::Bool, true, "true shows the objects, false hides them" } },
        &BuildSetEnabled,
        false,   // filterFromSelection
        nullptr, // refinePreview
        false,   // takesDestinationAsset
        false,   // wholeLevelIsFine
        nullptr, // wholeLevelNeedsParam
        true,    // reachesLights: a group of lamps is its meshes AND its lights, as the Outliner's eye shows it
    });

    actions_.push_back({
        "setBuoyant",
        "Float the matching meshes on the ocean -- heave, pitch and roll on its waves -- or stop them "
        "floating. Position and heading stay where they are; how each floats comes from its mesh's "
        "buoyancy settings.",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::Objects,
        { { "enabled", EditorParamKind::Bool, true, "true floats the meshes, false puts them back where the level placed them" } },
        &BuildSetBuoyant,
    });

    actions_.push_back({
        "setRaceGate",
        "Make the matching meshes (buoys) marks of a gate of the level's race course, or take them out of it. Two "
        "marks with the same gate number are one gate: a boat passes between them. The gates are run in the order "
        "of their numbers and the last leads back to gate 1, the start and the finish. Moving a mark moves the "
        "course; the bots read it when Play starts.",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::Objects,
        { { "gate", EditorParamKind::Number, true, "the gate's number, 1 and up; 0 takes the meshes out of the race" },
          { "track", EditorParamKind::String, false, "the course's name: a level may hold several, each with its gates 1, 2, 3 ... and its own start zone; \"\" = the level's first" } },
        &BuildSetRaceGate,
    });

    actions_.push_back({
        "setRaceBot",
        "Make the matching boats' KIND one the bots race in, or not: a race (the level's course, the gates marked with "
        "setRaceGate) makes its own copies of such kinds on the grid, a bot at each helm; the boats themselves stay "
        "where they are for the player, and the race's copies go when it is over.",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::Objects,
        { { "enabled", EditorParamKind::Bool, true, "true: its kind races as a bot (copies made for a race), false: not" } },
        &BuildSetRaceBot,
    });

    {
        std::vector<std::string_view> vesselKeys;
        for (const physics::VesselKey& key : physics::VesselKeys())
        {
            if (physics::VesselGroupPerCopy(key.group)) { vesselKeys.push_back(key.key); }
        }
        actions_.push_back({
            "setVessel",
            "Tune how the matching boats race -- one number of theirs, over their asset's: thrust (N, with surgeDrag the top "
            "speed), mass (kg), throttleRate, rudderForce (how tight it turns), rudderDeg, rudderRate, wash, the drags and "
            "the sea's dampings. The level keeps it for those copies alone; the asset's numbers stay. A race bot drives by the "
            "boat's numbers. reset: true puts the asset's back (with a key that one, without one every one).",
            EditorActionEffect::DocumentEdit,
            EditorTargetKind::Objects,
            { { "key", EditorParamKind::Enum, false, "which number", vesselKeys },
              { "value", EditorParamKind::Number, false, "its value, in the key's units" },
              { "reset", EditorParamKind::Bool, false, "true puts the asset's number back (every one when no key is given)" } },
            &BuildSetVessel,
        });
    }

    actions_.push_back({
        "setBoatCard",
        "Tune the matching boats by their card, the four bars a player reads over a boat in Play: topSpeedKn (flat out), "
        "secondsTo20Kn (acceleration), circleM (its tightest circle across), seaKeeping (%). Name any of them; the others "
        "stay where they are, and the boat's numbers (mass, thrust, drag ahead, rudder force) are found to match. The level "
        "keeps them for those copies alone; a race bot drives by them.",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::Objects,
        { { "topSpeedKn", EditorParamKind::Number, false, "knots flat out" },
          { "secondsTo20Kn", EditorParamKind::Number, false, "seconds from rest to 20 knots, full ahead" },
          { "circleM", EditorParamKind::Number, false, "metres across its tightest circle, hard over" },
          { "seaKeeping", EditorParamKind::Number, false, "percent: half its draft against 0.8 m, half its weight against 8 t" } },
        &BuildSetBoatCard,
    });

    actions_.push_back({
        "setCastShadow",
        "Make the matching meshes cast shadows, or not (UE's Cast Shadow): off keeps them out of every light's "
        "shadow -- the sun's, the lamps' and ray-traced ones -- while they stay visible and lit. Use it for grass, "
        "small clutter or a bulb inside its own lamp.",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::Objects,
        { { "enabled", EditorParamKind::Bool, true, "true lets the meshes cast shadows, false stops them" } },
        &BuildSetCastShadow,
    });

    actions_.push_back({
        "setPhysics",
        "Give the matching meshes their place in the physics world: \"static\" (the default: things collide with it), \"dynamic\" (it falls and is pushed while the simulation runs, as a convex hull of its solid slots) or \"none\" (no body). mass is kilograms, 0 = from the asset's density. A body's pose never reaches the level until keepSimulation.",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::Objects,
        { { "motion", EditorParamKind::Enum, true, "none, static or dynamic", { "none", "static", "dynamic" } },
          { "mass", EditorParamKind::Number, false, "kilograms; 0 or absent = from the density" } },
        &BuildSetPhysics,
    });

    actions_.push_back({
        "keepSimulation",
        "UE's Keep Simulation Changes: the poses the dynamic bodies have reached become the transforms of the matching objects (of every moved one with no target), one undo entry. Only while the level plays (the tool play): in the editor nothing moves.",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::Objects,
        {},
        &BuildKeepSimulation,
        false,   // filterFromSelection
        nullptr, // refinePreview
        false,   // takesDestinationAsset
        true,    // wholeLevelIsFine
    });

    actions_.push_back({
        "placePlayerStart",
        "Put the level's Player Start: where the player stands when the level plays, its feet at the point, facing the yaw. "
        "A level has one: a second call moves it. Without one, Play starts from the camera.",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::None,
        {
            { "at", EditorParamKind::Vec3, false, "where the feet stand. Omit to use the ground the camera is looking at" },
            { "yawDeg", EditorParamKind::Number, false, "which way it faces, in degrees. Omit to face as the camera does" },
        },
        &BuildPlacePlayerStart,
    });

    actions_.push_back({
        "setOnAt",
        "Make the matching lamps come on at night (or only by day, or always): point and spot lights, and meshes "
        "that glow (a bulb, a lit window -- their emissive). They switch when the sun goes down and comes up, "
        "warming up over a second or two, each a little after the next. Lights and meshes are reached by name, or "
        "through the selection (target scope \"selected\").",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::Objects,
        { { "onAt", EditorParamKind::Enum, true, "night = on after sunset, day = on after sunrise, always = never switched",
            { "night", "day", "always" } } },
        &BuildSetOnAt,
        false,   // filterFromSelection
        nullptr, // refinePreview
        false,   // takesDestinationAsset
        false,   // wholeLevelIsFine
        nullptr, // wholeLevelNeedsParam
        true,    // reachesLights
    });

    actions_.push_back({
        "setLight",
        "Change the matching point and spot lights that already exist: how bright (lumens -- a candle ~12, a household "
        "bulb ~800 -- or scale, a multiplier of each light's own brightness: 0.5 = half), how far a point light reaches "
        "(radius in metres; a spot's range), its colour, whether it casts shadows, a point light's flicker (a "
        "candle ~0.15 at ~3 Hz; 0 = steady), and dayBrightness, the light's own multiplier by day (1 = none, 0 = dark "
        "by day; it fades out as the night comes in), and sourceRadius, how big the light is for VSM's rays (m; 0 = a hard "
        "edge). Anything left out stays. Lights are reached by name or group "
        "(\"Bathroom Candles\"), or through the selection (target scope \"selected\").",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::Objects,
        {
            { "lumens", EditorParamKind::Number, false, "luminous flux in lumens, the same for every matching light" },
            { "scale", EditorParamKind::Number, false, "multiplies each light's own lumens (0.5 = half as bright)" },
            { "radius", EditorParamKind::Number, false, "metres a point light reaches (a spot's range)" },
            { "color", EditorParamKind::Vec3, false, "[r, g, b] linear, e.g. a candle [1, 0.55, 0.2]" },
            { "shadows", EditorParamKind::Bool, false, "true = casts shadows, false = none" },
            { "flicker", EditorParamKind::Number, false, "point lights: +/- this share of the flux, 0..1 (a candle ~0.15)" },
            { "flickerHz", EditorParamKind::Number, false, "point lights: the flicker's base frequency (a candle ~3)" },
            { "dayBrightness", EditorParamKind::Number, false, "the light's own multiplier by day, 1 = none, 0 = dark by day" },
            { "sourceRadius", EditorParamKind::Number, false, "metres, how big the light is for VSM's rays: a candle ~0.01; 0 = a hard edge" },
        },
        &BuildSetLight,
        false,   // filterFromSelection
        nullptr, // refinePreview
        false,   // takesDestinationAsset
        false,   // wholeLevelIsFine
        nullptr, // wholeLevelNeedsParam
        true,    // reachesLights
    });

    actions_.push_back({
        "delete",
        "Remove the matching objects from the level entirely.",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::Objects,
        {},
        &BuildDelete,
    });

    actions_.push_back({
        "duplicate",
        "Create a copy of each matching object in place; the copies become the new selection.",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::Objects,
        {},
        &BuildDuplicate,
    });

    actions_.push_back({
        "bury",
        "Move each matching object straight DOWN until the bottom of its geometry is under the "
        "surface below it (terrain, sand, rock). Fixes objects floating above uneven ground or "
        "standing on it with a visible gap. Only changes height, never the horizontal position.",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::Objects,
        // THE CEILING IS PART OF THE DESCRIPTION, because the value is clamped to it. Said
        // as "default 1" with no upper bound, "зарой в два раза глубже" came back as 200
        // and silently became 25 -- a control that accepts a number and does something
        // else with it.
        { { "depthPercent", EditorParamKind::Number, false,
            "how far past contact it ends up, as a percent of the object's height "
            "(default 1, and anything over 25 is treated as 25 -- it is the footing that "
            "gets buried, not the whole mesh)" } },
        &BuildBury,
    });

    actions_.push_back({
        "spawn",
        "Create COUNT new copies, scattered over the ground around the point the camera is "
        "looking at (or around the selection). Each copy is dropped onto the surface (lying in "
        "its asset's rest pose if it has one), given "
        "a random yaw and a slightly random size, and kept clear of water, steep slopes and "
        "whatever is already standing there. This is the action for planting, scattering or "
        "adding new objects that are not in the level yet.\n"
        "For SEVERAL KINDS AT ONCE put them all in target.assets -- \"palms of different "
        "types\" is one spawn with three assets, not three spawns -- and the count is shared "
        "out evenly between them.\n"
        "The spacing is already EVEN rather than clumped: minSeparation is a hard floor "
        "between any two, so raising it spreads them out and lowering it lets them cluster. "
        "Cover a wide area by raising radius; there is no separate 'distribute evenly' mode "
        "to ask for, because that is what this already does.\n"
        "When the level has ZONES and the phrase says where -- \"on the beach\", \"in the "
        "north zone\" -- pass that zone's name as the `zone` parameter and leave radius "
        "alone: the zone decides the shape and the area, and nothing lands outside it. "
        "When the phrase says where but NO zone covers it -- \"by that rock\", \"around the "
        "island\" -- ask for the bounds and pass the point as `at`. Do not invent a zone "
        "name: only the zones listed in the prompt exist.\n"
        "FOR \"в выделенной зоне\" / \"the selected zone\", where the phrase names the place "
        "without naming it, leave `zone` out and set target.scope to \"selected\": the editor "
        "knows which zone is selected and fills it in. Guessing a name instead is how you get "
        "a refusal for a zone that does not exist.\n"
        "NOT OVERLAPPING IS ALREADY THE RULE. \"чтобы не пересекались меж собой и с другими "
        "мешами\" needs no parameter and is not a reason for needs_api: every object's real "
        "size is counted, and minSeparation is clear air ON TOP of it. Pass 0 for \"touching "
        "but not inside each other\".\n"
        "GROUPING IS AUTOMATIC: what you spawn joins the group its kind already uses, or "
        "starts that group when the kind has none. Do not follow a spawn with a `group` "
        "command to tidy it up -- it is already tidy.",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::Asset,
        {
            { "count", EditorParamKind::Number, true, "how many to create (1-200)" },
            { "radius", EditorParamKind::Number, false, "radius of the scatter disc in metres (default 25)" },
            { "minSeparation", EditorParamKind::Number, false,
              "clear gap between EDGES, in metres (default 1.5). Each object's own size is "
              "already counted, so this is air on top of it: raise it to thin things out, "
              "and on a crowded level it is the knob that decides whether anything fits" },
            { "alignToGround", EditorParamKind::Bool, false,
              "drop each onto the surface below (default true)" },
            { "minGroundNormalY", EditorParamKind::Number, false,
              "reject ground steeper than this, 1 = flat only (default 0.82)" },
            { "minHeight", EditorParamKind::Number, false,
              "reject ground below this world height (defaults to just above the waterline)" },
            { "yawRange", EditorParamKind::Range, false,
              "random yaw in degrees (default [0, 360]). \"вращение -180+180\" is [-180, 180]" },
            { "scaleRange", EditorParamKind::Range, false,
              "random size multiplier on the asset's own scale (default [0.9, 1.1]). A signed "
              "spread is a DELTA around 1: \"скейл -0.5+0.1\" is [0.5, 1.1]. Pass it either "
              "way -- a negative low end is read as the delta it must be" },
            { "seed", EditorParamKind::Number, false, "fixes the layout; omit for a fresh one" },
            { "zone", EditorParamKind::String, false,
              "name of a zone to fill instead of a disc around the camera" },
            // The one that makes asking worth it. Without it a model that has just been
            // told where the island's north edge is has nowhere to PUT that answer: it can
            // scatter around the camera or inside a named zone, and neither is "there".
            // Asked to place a rock on the north shore of a level with no zones, it
            // invented a zone name -- not a guess about the world, a guess about the API.
            { "at", EditorParamKind::Vec3, false,
              "world point to scatter around, instead of the camera. Y is ignored when "
              "alignToGround is on, which it is by default" },
        },
        &BuildSpawn,
    });

    actions_.push_back({
        "place",
        "Create new objects at EXACT positions, one per item of params.items: each item is "
        "{asset, position [x, y, z]} with an optional yawDeg, rotationDeg [pitch, yaw, roll] "
        "and scale (a number, or [x, y, z]; it multiplies the asset's own size), physics (\"dynamic\" falls when "
        "the simulation runs, \"none\" collides with nothing), mass (kg), buoyant (true floats it) and raceGate (a "
        "number: the item is a mark of that gate of the level's race, two marks a gate; raceTrack names the course when the level has more than one). Nothing is "
        "scattered, dropped onto the ground or kept apart -- the positions are used as given, "
        "height included. This is for a layout that was worked out already: numbers the "
        "designer typed, or points computed from the terrain. For \"plant twenty palms here\" "
        "use spawn, which finds the ground and the spacing itself.\n"
        "An asset with a rest pose (one that lies rather than stands, like a shell) is laid in "
        "it when the item has no rotationDeg: yawDeg turns it about the vertical and the "
        "position's height is where its lowest point touches. A rotationDeg is taken as given.\n"
        "Each object joins the group its kind already uses, unless params.group names one. "
        "Up to 500 items, all of them one undo entry.",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::None,
        {
            { "items", EditorParamKind::Placements, true,
              "the objects: [{asset, position, yawDeg?, rotationDeg?, scale?}, ...], 1-500" },
            { "group", EditorParamKind::String, false,
              "put every placed object into this group instead of its kind's own" },
        },
        &BuildPlace,
    });

    actions_.push_back({
        "addLights",
        "Create point or spot LIGHTS at exact positions, one per item of params.items: each item "
        "is {position [x, y, z]} with an optional type (point, the default, or spot), name, "
        "lumens (a candle ~12, a household bulb ~800, the default 3000; a spot 12000), radius "
        "(metres the point light reaches, default 6), color [r, g, b] (linear, e.g. a candle "
        "[1, 0.55, 0.2]), shadows (true/false; a point light defaults to none, a spot to on) and "
        "onAt (night, day or always, the default -- night lamps follow the time of day), and for "
        "a spot direction [x, y, z], range, innerAngleDeg and outerAngleDeg. Positions are used "
        "as given: put a candle's light at its flame, not at its foot. params.group puts every "
        "light into that group. Up to 100 items, all of them one undo entry.",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::None,
        {
            { "items", EditorParamKind::LightPlacements, true,
              "the lights: [{position, type?, name?, lumens?, radius?, color?, shadows?, onAt?, "
              "direction?, range?, innerAngleDeg?, outerAngleDeg?}, ...], 1-100" },
            { "group", EditorParamKind::String, false, "put every light into this group" },
        },
        &BuildAddLights,
    });

    // Both of these were REFUSALS first. The editor could already do them -- RenameObject
    // and SetMaterial commands have existed all along, undoable and tested through the
    // Inspector -- but an action the registry does not name is an action the model cannot
    // reach, so it correctly answered needs_api and wrote a note. The gap was in the
    // vocabulary, not the engine. "rename" was asked three separate times.
    actions_.push_back({
        "rename",
        "Give the matching objects a new name. Renaming several numbers them -- \"spheres\" "
        "becomes \"spheres 001\", \"spheres 002\" -- because the outliner and every filter "
        "match on name, and two hundred objects sharing one is a level you cannot search. "
        "This changes the LABEL only; nothing moves and no mesh changes.\n"
        "FOR \"назови объекты нормальными именами\" -- naming the level's things after WHAT "
        "THEY ARE rather than giving them all one name -- pass perAsset and no name at all, "
        "with scope \"all\" and no filter. Each object is then called after its own asset "
        "and numbered within its kind: coconut_palm 001..222, rock_boulder 001, tent 001. "
        "That is one command and one undo entry for the whole level.",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::Objects,
        {
            { "name", EditorParamKind::String, false,
              "the new name. Leave it out when using perAsset, or give it to prefix every "
              "generated name" },
            { "perAsset", EditorParamKind::Bool, false,
              "name each object after its own asset instead of giving them all one name" },
        },
        &BuildRename,
        false,             // filterFromSelection
        &RefineRenamePreview,
        false,             // takesDestinationAsset
        false,             // wholeLevelIsFine -- only with perAsset, below
        "perAsset",
    });

    actions_.push_back({
        "setMaterial",
        "Give the matching objects a different material -- the thing that decides how they "
        "look: colour, roughness, metalness. Use it for 'make the rocks look wet' or 'put "
        "the sand material on this'. Name a material that exists in the level; an object "
        "with several material slots takes an optional slot number, and without one the "
        "whole object changes.",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::Objects,
        {
            { "material", EditorParamKind::String, true, "material asset name" },
            { "slot", EditorParamKind::Number, false,
              "which material slot to change; omit to change the whole object" },
        },
        &BuildSetMaterial,
    });

    actions_.push_back({
        "replace",
        "Swap the mesh of each matching object for a different mesh asset, keeping its position, "
        "rotation and scale. Use it to change WHICH model existing objects use.",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::Objects,
        {},
        &BuildReplace,
        false,   // filterFromSelection
        nullptr, // refinePreview
        true,    // takesDestinationAsset -- the one verb with two nouns
    });

    actions_.push_back({
        "move",
        "Move the matching objects -- or point and spot lights, named in the filter. With relative=true (the "
        "default) the value is metres to add to the current position; with relative=false it is the absolute "
        "world position to put them at.",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::Objects,
        {
            { "value", EditorParamKind::Vec3, true, "metres as [x, y, z]; a single number means all three" },
            { "relative", EditorParamKind::Bool, false, "add to the current position (default true)" },
        },
        &BuildMove,
        false,   // filterFromSelection
        nullptr, // refinePreview
        false,   // takesDestinationAsset
        false,   // wholeLevelIsFine
        nullptr, // wholeLevelNeedsParam
        true,    // reachesLights -- a lamp is moved by its name
    });

    actions_.push_back({
        "rotate",
        "Rotate the matching objects. The value is degrees as [pitch, yaw, roll]; yaw is the middle "
        "one and is the one that turns an upright object on the spot.",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::Objects,
        {
            { "value", EditorParamKind::Vec3, true, "degrees as [pitch, yaw, roll]" },
            { "relative", EditorParamKind::Bool, false, "add to the current rotation (default true)" },
        },
        &BuildRotate,
    });

    actions_.push_back({
        "scale",
        "Resize the matching objects. With relative=true (the default) the value MULTIPLIES the "
        "current scale, so 1.2 means 20% bigger; with relative=false it replaces it outright.",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::Objects,
        {
            { "value", EditorParamKind::Vec3, true, "multiplier as [x, y, z]; a single number scales uniformly" },
            { "relative", EditorParamKind::Bool, false, "multiply the current scale (default true)" },
        },
        &BuildScale,
    });

    actions_.push_back({
        "randomizeRotation",
        "Give each matching object its own random rotation, so a group of copies stops looking like "
        "copies. Yaw only by default, which is what upright things like trees and posts want.",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::Objects,
        {
            { "range", EditorParamKind::Range, false, "degrees to pick from (default [0, 360])" },
            { "axis", EditorParamKind::Enum, false, "yaw = turn on the spot, all = any orientation",
              { "yaw", "all" } },
            { "seed", EditorParamKind::Number, false, "fixes the result; omit for a fresh one" },
        },
        &BuildRandomizeRotation,
    });

    actions_.push_back({
        "isolate",
        "Show ONLY the matching objects: hide everything else in the level, and make sure "
        "the matching ones are visible. Use it for 'show me just the palms' or 'hide "
        "everything except the rocks'. Undo restores what was visible before.",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::Objects,
        {},
        &BuildIsolate,
    });

    actions_.push_back({
        "traceZone",
        "Draw a spline zone around the ABOVE-WATER part of whatever the target names -- the "
        "shoreline of an island, the dry part of a sandbank. The editor probes the ground on "
        "a grid, finds where it crosses the waterline, and makes a closed zone from that "
        "contour, filling INSIDE it. This is the action for \"обведи остров\", \"сделай "
        "зону по контуру берега\", \"зона по урезу воды\". Afterwards the zone's name works "
        "like any other: spawn can fill it, and target.where.zone narrows to what is in it.\n"
        "TO REDO AN EXISTING ZONE, pass its name: naming a zone this level already has "
        "replaces that zone instead of laying a second contour on top of it. That is how to "
        "answer \"больше точек в этой зоне\" or \"обведи поточнее\" -- same name, new "
        "`points` or `resolution`.",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::Objects,
        {
            { "margin", EditorParamKind::Number, false,
              "metres above the waterline to draw the line at (default 0); raise it to stay "
              "clear of the surf" },
            { "height", EditorParamKind::Number, false,
              "trace this world height instead of the waterline" },
            { "resolution", EditorParamKind::Number, false,
              "probe grid across the target, 24-160 (default 96). Finer follows the coast "
              "more closely and costs more" },
            { "points", EditorParamKind::Number, false,
              "how many control points to keep, 4-120 (default 32)" },
            { "name", EditorParamKind::String, false,
              "what to call the zone -- an existing zone's name re-traces THAT zone" },
        },
        &BuildTraceZone,
    });

    actions_.push_back({
        "setColor",
        "THE ACTION FOR \"покрась\", \"перекрась\", \"сделай <цвет>\", \"paint\", \"tint\". "
        "It sets the object's colour and leaves everything else alone -- the same tree, red. "
        "Use it whenever a phrase names a colour for objects that already exist; it is not a "
        "near-miss for those phrases, it is the answer to them, so do NOT reach for "
        "needs_api. (It is not replace, which swaps the mesh, and not setMaterial, which "
        "swaps the whole material.)\n"
        "Colour as [r, g, b], each 0..1, and a plain colour word is a plain value: red "
        "[1, 0, 0], bright red [1, 0, 0], dark red [0.4, 0, 0], green [0, 1, 0], blue "
        "[0, 0, 1], warm sand [0.9, 0.8, 0.6]. White [1, 1, 1] puts them back to normal. "
        "A textured object keeps its texture and takes the colour over it, which is what "
        "painting something means -- that is not a limitation to warn about.",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::Objects,
        {
            { "color", EditorParamKind::Vec3, true, "[r, g, b], each 0..1" },
        },
        &BuildSetColor,
    });

    actions_.push_back({
        "setSurface",
        "How shiny the matching objects are: \"сделай гладким/зеркальным/матовым\", \"make it "
        "chrome\", \"metallic\". Metalness and roughness as flat values, each 0..1; either may be "
        "left out and keeps the current value. Chrome or a mirror: metal 1, roughness 0.02. "
        "Glossy paint or wet plastic: metal 0, roughness 0.05. Matte: roughness 0.8. The "
        "object's metal/rough texture is switched off (flat values), and its colour and "
        "material stay as they are. normalStrength scales the material's normal map (0 = a "
        "perfectly smooth shape -- what a mirror needs, or its bumps break the reflection up). "
        "albedo [r, g, b] replaces the albedo TEXTURE with a flat colour -- a metal reflects in "
        "that colour, so chrome needs a light grey like [0.95, 0.93, 0.88].",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::Objects,
        {
            { "metal", EditorParamKind::Number, false, "0 = dielectric, 1 = metal" },
            { "roughness", EditorParamKind::Number, false, "0 = mirror, 1 = fully rough" },
            { "normalStrength", EditorParamKind::Number, false, "normal map strength, 0 = none, 1 = as authored" },
            { "albedo", EditorParamKind::Vec3, false, "[r, g, b] 0..1, a flat colour instead of the albedo texture" },
            { "tint", EditorParamKind::Vec3, false, "[r, g, b] 0..1 multiplied into the albedo texture, its detail kept: a white-albedo mesh such as ocean_buoy_white takes the colour" },
        },
        &BuildSetSurface,
    });

    // WHAT THE PREVIEW SAYS WHEN THE ANSWER IS "into what". "group 621 objects" is true
    // and useless: it names the verb and the count and leaves out the only thing being
    // decided -- how many folders, called what. The person is looking at that line to
    // choose whether to press Run.
    //
    // The breakdown under it is per-KIND, and with perAsset each kind IS a group, so the
    // sentence only has to say so. Without perAsset they all go into one named folder,
    // which is a different sentence and just as short.
    actions_.push_back({
        "group",
        "Put the matching objects into a named GROUP -- a label they carry, shown as a "
        "folder in the outliner. Once grouped, the group's name works as a filter "
        "everywhere: \"спрячь северную рощу\" is setEnabled with that name in the filter, "
        "\"удали её\" is a delete. Use it for \"сгруппируй\", \"собери в группу\", "
        "\"назови это\". An EMPTY name takes them out of whatever group they were in.\n"
        "FOR \"a group for each kind\" pass perAsset instead of naming one, and do it in a "
        "SINGLE command: perAsset splits whatever it is given, and the whole thing is one "
        "undo entry.\n"
        "TO TIDY THE WHOLE OUTLINER -- \"наведи порядок\", \"разложи по папкам\", "
        "\"сгруппируй объекты как надо\" -- send perAsset with scope \"all\" and NO filter "
        "at all. That is the one case where an empty filter is right, and it is the only "
        "way to catch every kind: the level summary lists the commonest assets and says "
        "`otherAssetKinds` for the rest, so a filter you type out by hand will silently "
        "miss the ones it never showed you -- the island, the rocks, the tents.",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::Objects,
        {
            { "name", EditorParamKind::String, false,
              "the group's name; empty removes them from their group" },
            { "perAsset", EditorParamKind::Bool, false,
              "one group per KIND instead of one group for all of them, each named after "
              "its asset. This is what \"группы для каждого типа\" means -- use it instead "
              "of sending one command per kind" },
        },
        &BuildGroup,
        false,             // filterFromSelection
        &RefineGroupPreview,
        false,             // takesDestinationAsset
        true,              // wholeLevelIsFine -- "наведи порядок в аутлайнере" IS the level
    });

    actions_.push_back({
        "thin",
        "Thin OUT objects that are already in the level, by deleting some of them. Two ways "
        "to say how much: minSeparation removes whatever stands closer together than that "
        "many metres, keepPercent keeps roughly that share and drops the rest evenly. This "
        "is the answer to \"проредь\", \"слишком густо\", \"убери половину\" -- it is the "
        "undo spawn does not have, and it keeps the arrangement rather than replacing it.",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::Objects,
        {
            { "minSeparation", EditorParamKind::Number, false,
              "closest two may stand, in metres; anything nearer loses the newer one" },
            { "keepPercent", EditorParamKind::Number, false,
              "keep about this share, 50 = half. Use one of these two, not both" },
        },
        &BuildThin,
        false,
        &RefineThinPreview,
    });

    actions_.push_back({
        "createZone",
        "Draw a named region on the ground -- a circle or a rectangle -- around the point "
        "the camera is looking at, or around an explicit `at`. A zone is how the editor says "
        "WHERE: once one exists, spawn can fill it with params.zone, and every other action "
        "can narrow to what is inside it with target.where.zone. Make one when the designer "
        "names a place the level does not have yet (\"заведи зону на пляже\", \"сделай "
        "область вокруг того камня\"), and when a later command will need to refer to it.",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::None,
        {
            { "shape", EditorParamKind::Enum, false,
              "circle (default) or rect", { "circle", "rect" } },
            { "size", EditorParamKind::Number, false,
              "radius for a circle, half-extent for a rect, in metres (default 25)" },
            { "name", EditorParamKind::String, false,
              "what to call it; numbered automatically when omitted" },
            { "at", EditorParamKind::Vec3, false,
              "world centre. Omit to use what the camera is looking at" },
        },
        &BuildCreateZone,
    });

    actions_.push_back({
        "frame",
        "Point the camera at the matching objects so they all fit on screen, without "
        "changing anything in the level. Use it for 'show me', 'take me to', 'where are'. "
        "It only moves the camera; it does not select, hide or alter anything.",
        EditorActionEffect::ViewChange,
        EditorTargetKind::Objects,
        {},
        nullptr,
    });

    actions_.push_back({
        "selectSimilar",
        "Select everything that uses the same asset as what is selected right now. The "
        "designer clicks one palm and asks for 'all the ones like this'; you do NOT need to "
        "know which one that is -- leave the filter empty and the editor works it out.",
        EditorActionEffect::SelectionOnly,
        EditorTargetKind::Objects,
        {},
        nullptr,
        true,
    });

    actions_.push_back({
        "align",
        "Line the matching objects up: give them all the same position on ONE axis, leaving "
        "the other two alone. Use it for 'line these up', 'put them at the same height', "
        "'align them on X'.",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::Objects,
        {
            { "axis", EditorParamKind::Enum, true,
              "which coordinate they should share; y is height", { "x", "y", "z" } },
            { "to", EditorParamKind::Enum, false,
              "which value to use (default average)",
              { "min", "max", "center", "average", "first" } },
        },
        &BuildAlign,
    });

    actions_.push_back({
        "distribute",
        "Space the matching objects evenly along ONE axis. Without a spacing the two "
        "outermost stay where they are and everything between them is evened out; with a "
        "spacing they are walked out from the first one at that interval.",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::Objects,
        {
            { "axis", EditorParamKind::Enum, true, "the axis to space them along",
              { "x", "y", "z" } },
            { "spacing", EditorParamKind::Number, false,
              "metres between neighbours; omit to even out what is already there" },
        },
        &BuildDistribute,
    });

    actions_.push_back({
        "snap",
        "Round the matching objects' positions onto a grid. By default only the horizontal "
        "plane is snapped, because rounding height on uneven ground lifts things off it; "
        "ask for axes 'all' if that is really wanted. Can also round the yaw to a step.",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::Objects,
        {
            { "grid", EditorParamKind::Number, false, "grid size in metres (default 1)" },
            { "axes", EditorParamKind::Enum, false,
              "which axes to round (default xz -- the ground plane)",
              { "xz", "all", "x", "y", "z" } },
            { "rotationStep", EditorParamKind::Number, false,
              "round the yaw to this many degrees too; 0 leaves rotation alone" },
        },
        &BuildSnap,
    });

    actions_.push_back({
        "setEnvironment",
        "Change one setting of the world's look: fog, sun, wind, water, exposure or colour "
        "grading. target.setting names it, e.g. \"wind.strength\" or \"gtao.intensity\". Give "
        "params.value for an absolute new value, or params.scale to multiply what it is now "
        "(scale 1.5 = half again as much, 0.5 = half). This is the action for 'thicker fog', "
        "'stronger wind', 'brighter sun' -- anything about how the level LOOKS rather than "
        "about particular objects.",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::Environment,
        {
            { "value", EditorParamKind::Any, false,
              "the new value; must match the setting's own type" },
            { "scale", EditorParamKind::Number, false,
              "multiply the current value instead (numbers only)" },
        },
        &BuildSetEnvironment,
    });

    // Time-of-day plan T4. Their own verbs rather than setEnvironment on `timeOfDay.time`: a time is
    // said as "18:30", "закат", "+2 часа", and the hours-as-a-float that the setting holds is not how
    // anybody says it -- nor is a sunset, which depends on the date and the place.
    actions_.push_back({
        "setTime",
        "Set the level's time of day; the sun and the moon move with it (the sun lights the day, the "
        "moon the night). params.time is a clock time \"HH:MM\" (\"06:15\", \"18:30\"), a moment "
        "(\"sunrise\", \"sunset\", \"noon\", \"midnight\" -- with real positions the date's own), or a "
        "shift from the current time (\"+2h\", \"-30m\", \"+1:30\"). This is the action for \"сделай "
        "закат\", \"поставь утро\", \"на два часа позже\". One undo entry.",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::None,
        {
            { "time", EditorParamKind::String, true,
              "\"HH:MM\", sunrise / sunset / noon / midnight, or a shift like \"+2h\"" },
        },
        &BuildSetTime,
        false,
        &RefineSetTime,
    });

    actions_.push_back({
        "setDate",
        "Set the date the sun and the moon are computed for (params.date \"YYYY-MM-DD\"). Switches "
        "the level to REAL positions -- the sun's path and the moon's phase for that day at the "
        "level's latitude and longitude -- since a date means nothing to the simple path. For "
        "\"сделай зимнее солнце\", \"какая луна будет 1 января\".",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::None,
        {
            { "date", EditorParamKind::String, true, "\"YYYY-MM-DD\"" },
        },
        &BuildSetDate,
        false,
        &RefineSetDate,
    });

    actions_.push_back({
        "randomizeScale",
        "Vary the size of each matching object by a random multiplier on its current scale, so a "
        "group of copies reads as individuals rather than as one model repeated.",
        EditorActionEffect::DocumentEdit,
        EditorTargetKind::Objects,
        {
            { "range", EditorParamKind::Range, false, "multiplier to pick from (default [0.9, 1.1])" },
            { "uniform", EditorParamKind::Bool, false,
              "same factor on all three axes (default true); false stretches" },
            { "seed", EditorParamKind::Number, false, "fixes the result; omit for a fresh one" },
        },
        &BuildRandomizeScale,
    });
}

const EditorActionRegistry& EditorActionRegistry::Builtin()
{
    static const EditorActionRegistry registry;
    return registry;
}

const EditorActionDesc* EditorActionRegistry::Find(std::string_view id) const
{
    for (const EditorActionDesc& action : actions_)
    {
        if (action.id == id)
        {
            return &action;
        }
    }
    return nullptr;
}

bool ValidateParams(const EditorActionDesc& action, nlohmann::json& params, std::string& outError)
{
    if (!params.is_object())
    {
        outError = "parameters must be an object";
        return false;
    }

    // A PARAMETER THE ACTION DOES NOT DECLARE IS A REFUSAL, NOT A SHRUG. This loop used to
    // be absent, and an unknown key was dropped without a word -- which is worse than a bad
    // value, because the command then runs and does something plausible.
    //
    // The prompt makes the collision likely on purpose: `spawn` says WHERE with params.zone
    // while every other action says it with target.where.zone. "удали пальмы в зоне Beach"
    // answered with params.zone lost the zone silently, and the preview said "delete 183
    // objects" -- honest, plausible, and the whole level's palms instead of the twelve on
    // the beach. Naming the permitted keys is what turns that into a fixable message.
    for (const auto& entry : params.items())
    {
        const auto known = std::find_if(action.params.begin(), action.params.end(),
            [&entry](const EditorActionParam& param) { return param.name == entry.key(); });
        if (known != action.params.end())
        {
            continue;
        }
        outError = "'" + std::string(action.id) + "' has no parameter '" + entry.key() + "'";
        if (action.params.empty())
        {
            outError += "; it takes none";
        }
        else
        {
            outError += "; it takes ";
            for (std::size_t i = 0; i < action.params.size(); ++i)
            {
                outError += (i ? ", " : "") + std::string(action.params[i].name);
            }
        }
        return false;
    }

    for (const EditorActionParam& param : action.params)
    {
        const std::string name(param.name);
        auto it = params.find(name);
        if (it == params.end())
        {
            if (param.required)
            {
                outError = "'" + std::string(action.id) + "' needs a '" + name + "'";
                return false;
            }
            continue;
        }

        switch (param.kind)
        {
        case EditorParamKind::Number:
            if (!it->is_number())
            {
                outError = "'" + name + "' must be a number";
                return false;
            }
            break;
        case EditorParamKind::Bool:
            if (!it->is_boolean())
            {
                outError = "'" + name + "' must be true or false";
                return false;
            }
            break;
        case EditorParamKind::String:
            if (!it->is_string())
            {
                outError = "'" + name + "' must be text";
                return false;
            }
            break;
        case EditorParamKind::Range:
            // A lone number is a legal way to say "exactly this", so broaden it here rather
            // than making every source remember to write [n, n].
            if (it->is_number())
            {
                const float value = it->get<float>();
                *it = nlohmann::json::array({ value, value });
            }
            if (!it->is_array() || it->size() != 2 || !(*it)[0].is_number() || !(*it)[1].is_number())
            {
                outError = "'" + name + "' must be two numbers, [low, high]";
                return false;
            }
            break;
        case EditorParamKind::Vec3:
            // Likewise: "scale by 2" is a vec3 of twos, and refusing it would be pedantry
            // that the model would then have to be told about in the prompt.
            if (it->is_number())
            {
                const float value = it->get<float>();
                *it = nlohmann::json::array({ value, value, value });
            }
            if (!it->is_array() || it->size() != 3 ||
                !(*it)[0].is_number() || !(*it)[1].is_number() || !(*it)[2].is_number())
            {
                outError = "'" + name + "' must be three numbers, [x, y, z]";
                return false;
            }
            break;
        case EditorParamKind::Any:
            break;   // checked later, by something that knows the real type
        case EditorParamKind::Placements:
        {
            // Checked HERE, item by item and by number, because a list is where "half of it
            // was fine" hides: a builder that met item 37's missing position would have
            // already built 36 objects' worth of command before it could say so.
            constexpr std::size_t kMaxItems = 500;
            if (!it->is_array() || it->empty() || it->size() > kMaxItems)
            {
                outError = "'" + name + "' must be a list of 1-" + std::to_string(kMaxItems) +
                    " items, each {asset, position}";
                return false;
            }
            const auto isVec3 = [](const nlohmann::json& v)
            {
                return v.is_array() && v.size() == 3 && v[0].is_number() && v[1].is_number() &&
                    v[2].is_number();
            };
            for (std::size_t index = 0; index < it->size(); ++index)
            {
                const nlohmann::json& item = (*it)[index];
                const std::string at = "'" + name + "' item " + std::to_string(index + 1);
                if (!item.is_object())
                {
                    outError = at + " must be an object {asset, position}";
                    return false;
                }
                for (const auto& field : item.items())
                {
                    const std::string& key = field.key();
                    const nlohmann::json& v = field.value();
                    const bool ok =
                        (key == "asset" && v.is_string() && !v.get<std::string>().empty()) ||
                        (key == "position" && isVec3(v)) ||
                        (key == "yawDeg" && v.is_number()) ||
                        (key == "rotationDeg" && isVec3(v)) ||
                        (key == "scale" && ((v.is_number() && v.get<float>() > 0.0f) || isVec3(v))) ||
                        (key == "physics" && v.is_string()) ||
                        (key == "mass" && v.is_number()) ||
                        (key == "buoyant" && v.is_boolean()) ||
                        (key == "raceGate" && v.is_number()) ||
                        (key == "raceTrack" && v.is_string()) ||
                        (key == "collision" && v.is_object());
                    if (!ok)
                    {
                        const bool known = key == "asset" || key == "position" || key == "yawDeg" ||
                            key == "rotationDeg" || key == "scale";
                        outError = at + (known ? " has a malformed '" + key + "'"
                                               : " has no field '" + key + "'") +
                            "; an item is {asset, position [x, y, z], yawDeg?, rotationDeg? [p, y, r], "
                            "scale? (number or [x, y, z])}";
                        return false;
                    }
                }
                if (!item.contains("asset") || !item.contains("position"))
                {
                    outError = at + " needs both 'asset' and 'position'";
                    return false;
                }
            }
            break;
        }
        case EditorParamKind::LightPlacements:
        {
            // Item by item and by number, like Placements: a lamp list half right is a level half lit.
            constexpr std::size_t kMaxLights = 100;
            if (!it->is_array() || it->empty() || it->size() > kMaxLights)
            {
                outError = "'" + name + "' must be a list of 1-" + std::to_string(kMaxLights) +
                    " lights, each {position, ...}";
                return false;
            }
            const auto isVec3 = [](const nlohmann::json& v)
            {
                return v.is_array() && v.size() == 3 && v[0].is_number() && v[1].is_number() &&
                    v[2].is_number();
            };
            const auto positive = [](const nlohmann::json& v) { return v.is_number() && v.get<float>() > 0.0f; };
            for (std::size_t index = 0; index < it->size(); ++index)
            {
                const nlohmann::json& item = (*it)[index];
                const std::string at = "'" + name + "' light " + std::to_string(index + 1);
                if (!item.is_object())
                {
                    outError = at + " must be an object {position, ...}";
                    return false;
                }
                for (const auto& field : item.items())
                {
                    const std::string& key = field.key();
                    const nlohmann::json& v = field.value();
                    const std::string text = v.is_string() ? v.get<std::string>() : std::string();
                    const bool ok =
                        (key == "position" && isVec3(v)) ||
                        (key == "type" && (text == "point" || text == "spot")) ||
                        (key == "name" && !text.empty()) ||
                        (key == "lumens" && positive(v)) ||
                        (key == "radius" && positive(v)) ||
                        (key == "range" && positive(v)) ||
                        (key == "color" && isVec3(v)) ||
                        (key == "shadows" && v.is_boolean()) ||
                        (key == "onAt" && (text == "night" || text == "day" || text == "always")) ||
                        (key == "direction" && isVec3(v)) ||
                        (key == "innerAngleDeg" && positive(v)) ||
                        (key == "outerAngleDeg" && positive(v));
                    if (!ok)
                    {
                        const bool known = key == "position" || key == "type" || key == "name" ||
                            key == "lumens" || key == "radius" || key == "range" || key == "color" ||
                            key == "shadows" || key == "onAt" || key == "direction" ||
                            key == "innerAngleDeg" || key == "outerAngleDeg";
                        outError = at + (known ? " has a malformed '" + key + "'"
                                               : " has no field '" + key + "'") +
                            "; a light is {position [x, y, z], type? point|spot, name?, lumens?, radius?, "
                            "color? [r, g, b], shadows? true/false, onAt? night|day|always, and for a spot "
                            "direction? [x, y, z], range?, innerAngleDeg?, outerAngleDeg?}";
                        return false;
                    }
                }
                if (!item.contains("position"))
                {
                    outError = at + " needs a 'position'";
                    return false;
                }
            }
            break;
        }
        case EditorParamKind::Enum:
        {
            if (!it->is_string())
            {
                outError = "'" + name + "' must be text";
                return false;
            }
            const std::string value = it->get<std::string>();
            bool known = false;
            std::string permitted;
            for (const std::string_view candidate : param.values)
            {
                if (!permitted.empty())
                {
                    permitted += ", ";
                }
                permitted += std::string(candidate);
                known = known || value == candidate;
            }
            if (!known)
            {
                outError = "'" + name + "' must be one of: " + permitted;
                return false;
            }
            break;
        }
        }
    }

    return true;
}

EditorIntent MakeActionIntent(std::string_view actionId, nlohmann::json params)
{
    EditorIntent intent;
    intent.kind = EditorIntentKind::Command;
    intent.action = std::string(actionId);
    intent.params = std::move(params);
    intent.sourceLabel = "editor";
    return intent;
}

static bool RunEditorActionUnchecked(const EditorActionContext& actionCtx, EditorCommandStack& commandStack,
    const EditorIntent& intent, const std::vector<EditorObjectId>& targets, std::string& outStatus);

// Architecture plan R9.6d: an action's parameters come from the model (the command bar, in the frame), from MCP or from
// the editor itself, and its readers take some with value(): a parameter of another type threw from deep inside one --
// out of the frame that is std::terminate with the unsaved level. It is the caller's mistake and comes back as the
// status, as MCP answered it already (EditorMcpServer's catch around RunDocumentTool).
bool RunEditorAction(const EditorActionContext& actionCtx,
    EditorCommandStack& commandStack,
    const EditorIntent& intent,
    const std::vector<EditorObjectId>& targets,
    std::string& outStatus)
{
    try
    {
        return RunEditorActionUnchecked(actionCtx, commandStack, intent, targets, outStatus);
    }
    catch (const nlohmann::json::exception& e)
    {
        outStatus = "'" + intent.action + "' was not run: a parameter has a value of another type (" + e.what() + ")";
        LOG_WARNING(logging::LogCategory::Editor, "action {} refused: {}", intent.action, e.what());
        return false;
    }
}

static bool RunEditorActionUnchecked(const EditorActionContext& actionCtx,
    EditorCommandStack& commandStack,
    const EditorIntent& intent,
    const std::vector<EditorObjectId>& targets,
    std::string& outStatus)
{
    const EditorActionDesc* action = EditorActionRegistry::Builtin().Find(intent.action);
    if (!action)
    {
        outStatus = "Unknown editor action '" + intent.action + "'";
        return false;
    }

    if (action->effect == EditorActionEffect::ReadOnly)
    {
        // Nothing to do: the answer was computed by the resolver and is already on screen.
        // Reaching here at all means something asked to "run" a question.
        outStatus = "Nothing to run -- that was a question, and it is already answered";
        return false;
    }

    if (action->effect == EditorActionEffect::ViewChange)
    {
        if (targets.empty())
        {
            outStatus = "Nothing to look at";
            return false;
        }
        // Framing wants an EditorSelection, but it must NOT disturb the real one: "show me
        // the palms" is a request to look, not to select.
        EditorSelection framed;
        for (const EditorObjectId id : targets)
        {
            framed.Add(id, false);
        }
        if (!editorframing::FrameObjects(actionCtx.editor.renderer, actionCtx.editor.scene,
                actionCtx.editor.document, framed))
        {
            outStatus = "Could not work out where those are";
            return false;
        }
        outStatus = "Framed " + CountedObjects(targets.size());
        return true;
    }

    if (action->effect == EditorActionEffect::SelectionOnly)
    {
        if (targets.empty())
        {
            outStatus = "Nothing matched";
            return false;
        }
        actionCtx.editor.selection.SetOrdered(targets, targets.front());
        outStatus = "Selected " + CountedObjects(targets.size());
        return true;
    }

    std::string buildStatus;
    std::unique_ptr<EditorCommand> command = action->build(actionCtx, targets, intent, buildStatus);
    if (!command)
    {
        outStatus = buildStatus;
        return false;
    }

    if (!commandStack.Execute(actionCtx.editor, std::move(command)))
    {
        outStatus = buildStatus.empty() ? "Action failed" : buildStatus + " -- failed";
        return false;
    }
    outStatus = buildStatus;
    return true;
}

std::size_t editornames::HighestNameOrdinal(const EditorSceneDocument& document, const std::string& stem)
{
    std::size_t highest = 0;
    const std::string prefix = stem + " ";
    for (const EditorObject& object : document.Objects())
    {
        if (object.name.size() <= prefix.size() || object.name.compare(0, prefix.size(), prefix) != 0)
        {
            continue;
        }
        const std::string tail = object.name.substr(prefix.size());
        const bool digits = tail.size() <= 9 && std::all_of(tail.begin(), tail.end(),
            [](unsigned char c) { return std::isdigit(c) != 0; });
        if (digits) { highest = std::max<std::size_t>(highest, std::stoul(tail)); }
    }
    return highest;
}

#endif // WITH_EDITOR
