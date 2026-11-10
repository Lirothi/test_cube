#pragma once

// The knobs of a boat's wake FOAM and of its bow wave's two arms, in ONE table (2026-11-10, owner: "bring out everything
// that can be tuned, with hints one can understand"): the key (the ocean preset's JSON key and `--set ocean.<key>`), the
// field of OceanRenderConfig, its range, and the label and hint both windows show (the Ocean window, the Inspector's
// "Boat wake"). A knob added here is saved, loaded, settable and drawn; nothing else lists these keys.
// A hint says what the eye sees and which way to turn -- not how the term is computed.

#include <array>

#include "ocean/OceanSimulationConfig.h"

namespace ocean
{
struct WakeKnob
{
    const char* key;
    float OceanRenderConfig::* field;
    float min;
    float max;
    float speed;         // the drag's, a pixel
    const char* label;
    const char* hint;
    const char* section; // not null: a heading drawn before this knob
};

inline const std::array<WakeKnob, 27>& WakeKnobs()
{
    static const std::array<WakeKnob, 27> knobs = { {
        // ---- how much foam, and for how long ----
        { "wakeFoam", &OceanRenderConfig::wakeFoam, 0.0f, 2.0f, 0.01f, "Foam amount",
          "All of the wake's foam on the water at once.\n0 = no foam at all, 1 = as simulated, 2 = twice as dense.",
          "Wake foam: how much, how long" },
        { "wakeWashFoam", &OceanRenderConfig::wakeWashFoam, 0.0f, 20.0f, 0.05f, "Centre trail",
          "The white water the hull leaves straight behind its stern.\nHigher = a whiter centre trail that stays solid longer. 0 = no centre trail.",
          nullptr },
        { "wakeWashBow", &OceanRenderConfig::wakeWashBow, 0.0f, 1.0f, 0.005f, "Stem collar",
          "White water laid where the bow meets the water, as a collar round the stem.\n0 = none (a planing boat's stem is in the air; the arms carry the bow's foam). 1 = as much as behind the stern.\nIt steps forward with the wake's 30 steps a second: at speed it flickers.",
          nullptr },
        { "wakeFoamFadeDense", &OceanRenderConfig::wakeFoamFadeDense, 0.0f, 10.0f, 0.02f, "Solid white fade",
          "How fast solid white water thins out into lace.\nHigher = the solid white patch behind the boat is shorter. 0 = it stays solid as long as the lace lives.",
          nullptr },
        { "wakeFoamFade", &OceanRenderConfig::wakeFoamFade, 0.02f, 5.0f, 0.005f, "Lace fade",
          "How fast the thin lace disappears.\n0.1 = lace lives some 20-30 seconds (a long trail); 0.5 = about 5 seconds (a short one).",
          nullptr },
        { "wakeFoamSpread", &OceanRenderConfig::wakeFoamSpread, 0.0f, 1.0f, 0.005f, "Foam spreading",
          "How fast foam smears sideways once it is laid (m2/s).\n0 = it stays exactly where it was laid, crisp edges; higher = a wider, softer trail.",
          nullptr },
        { "wakeAeration", &OceanRenderConfig::wakeAeration, 0.0f, 1.0f, 0.005f, "Bubble scar",
          "The light milky-turquoise water under and around the foam (bubbles below the surface).\n0 = none: dark water between the strands. Higher = a brighter scar that marks the whole wake.",
          nullptr },
        { "wakeCrestFoam", &OceanRenderConfig::wakeCrestFoam, 0.0f, 5.0f, 0.01f, "Steep crest foam",
          "Foam on the wake's own steep wave crests (mostly the two waves right behind the stern).\n0 = none.",
          nullptr },
        { "wakeCrestCoverage", &OceanRenderConfig::wakeCrestCoverage, 0.0f, 1.0f, 0.005f, "Steep crest threshold",
          "How steep a wake wave must be to foam.\n0.55 = only waves about to break; higher = gentler waves foam too (0.8 puts a band over the trail).",
          nullptr },
        // ---- what the foam looks like ----
        { "wakeFoamTile", &OceanRenderConfig::wakeFoamTile, 0.2f, 16.0f, 0.02f, "Pattern size (m)",
          "Metres of water one tile of the foam texture covers.\nSmaller = fine feathery lace (2-3 looks right from the air). Bigger = large cells and thick strands\n(4 reads well from the helm, 8+ are slabs). No visible repeat at any size.",
          "Wake foam: the pattern" },
        { "wakeFoamTileVary", &OceanRenderConfig::wakeFoamTileVary, 0.0f, 0.4f, 0.002f, "Pattern size variation",
          "How much the pattern's size differs from patch to patch of the wake (each patch about half a tile across).\n0 = one size everywhere. 0.15 = each patch somewhere between 15 % smaller and 15 % bigger. 0.4 = fine lace\nnext to coarse.",
          nullptr },
        { "wakeFoamStretch", &OceanRenderConfig::wakeFoamStretch, 1.0f, 8.0f, 0.02f, "Streak stretch",
          "How much the pattern is drawn out along the way the boat went.\n1 = round cells, 3.5 = streaks, 8 = long thin threads.",
          nullptr },
        { "wakeFoamSoftness", &OceanRenderConfig::wakeFoamSoftness, 0.02f, 1.0f, 0.005f, "Edge softness",
          "How soft the border between foam and water is.\nLow = crisp, hard-edged strands. High = blurry, half-transparent foam.",
          nullptr },
        { "wakeFoamBoil", &OceanRenderConfig::wakeFoamBoil, 0.0f, 0.5f, 0.002f, "Boil speed (m/s)",
          "How fast patches of the pattern slide against each other, so the foam seems to boil and crawl.\n0 = a frozen pattern.",
          nullptr },
        { "wakeFoamCap", &OceanRenderConfig::wakeFoamCap, 0.5f, 1.0f, 0.002f, "Solid white limit",
          "The most of the water the densest foam may cover.\n1 = fresh white water is a solid white slab. 0.93 = even the densest foam keeps small holes\nwith milky water in them (froth, not paint). Lower = more and bigger holes.",
          nullptr },
        { "wakeChurn", &OceanRenderConfig::wakeChurn, 0.0f, 1.0f, 0.01f, "White water relief",
          "How strongly fresh solid white water is shaded as lumps (lit by the sun from one side, grey creases).\n0 = a flat white sheet, 1 = full relief.",
          nullptr },
        { "wakeChurnSize", &OceanRenderConfig::wakeChurnSize, 0.5f, 16.0f, 0.02f, "Relief size (m)",
          "The scale of those lumps: metres of water one tile of the relief covers (its big boils are about\na seventh of it). Small = fine grain, reads as plaster. 4-6 = boils of half a metre, as behind a propeller.\nNot smaller than Pattern size.",
          nullptr },
        // ---- the bow wave's two arms ----
        { "wakeBowFoam", &OceanRenderConfig::wakeBowFoam, 0.0f, 4.0f, 0.01f, "Arms amount",
          "The two foam arms that run back from the bow's shoulders.\n0 = no arms. 1 = a white breaker on a planing hull. Higher = whiter, and visible at lower speeds.",
          "Bow wave arms" },
        { "wakeBowSpread", &OceanRenderConfig::wakeBowSpread, 0.25f, 4.0f, 0.01f, "Arms opening",
          "How fast the arms run outward: the angle of the V.\n1 = the real wave's speed (the foam rides its crest). 2 = a V twice as wide.",
          nullptr },
        { "wakeBowThrow", &OceanRenderConfig::wakeBowThrow, 0.0f, 0.3f, 0.002f, "Arms shoulder",
          "How far the spray sheet throws the arms off the hull right at the bow.\n0 = the arms start touching the hull. 0.09 = 1.4 m off it at full speed. Higher = a wide shoulder beside the boat.",
          nullptr },
        { "wakeBowLife", &OceanRenderConfig::wakeBowLife, 0.2f, 10.0f, 0.02f, "Arms length (s)",
          "Seconds until an arm's white breaker has faded to about a third.\n2.5 = white for some four boat lengths at full speed. Higher = longer arms.",
          nullptr },
        { "wakeBowFadeIn", &OceanRenderConfig::wakeBowFadeIn, 0.0f, 2.0f, 0.005f, "Arms start (s)",
          "How gradually an arm's white band comes in where it leaves the hull.\n0 = full white right at the hull (a hard white cap beside the bow). 0.3 = it grows over the first 5 m at full speed.\nHigher = the arms start further back.",
          nullptr },
        { "wakeBowWidth", &OceanRenderConfig::wakeBowWidth, 0.3f, 4.0f, 0.01f, "Arms width (m)",
          "Half the width of an arm's white band where it leaves the hull.\nHigher = fatter arms.",
          nullptr },
        { "wakeBowWiden", &OceanRenderConfig::wakeBowWiden, 0.0f, 2.0f, 0.005f, "Arms widening (m/s)",
          "How fast an arm's band gets wider as it runs away from the boat.\n0 = the same width all along.",
          nullptr },
        { "wakeBowLace", &OceanRenderConfig::wakeBowLace, 0.0f, 1.0f, 0.005f, "Lace behind arms",
          "How much foam an arm leaves behind it: the lace between the arms and the centre trail.\n0 = clear water there (three separate stripes). 1 = one solid wedge of foam.",
          nullptr },
        { "wakeBowScallop", &OceanRenderConfig::wakeBowScallop, 0.0f, 1.0f, 0.005f, "Arms raggedness",
          "How uneven the arms' outer edge is.\n0 = smooth straight lines. Higher = lobes and bulges, bigger the further back.",
          nullptr },
        { "wakeBowSpeedFull", &OceanRenderConfig::wakeBowSpeedFull, 4.0f, 30.0f, 0.05f, "Arms full speed (m/s)",
          "The boat speed at which the arms reach full strength (they start at 3 m/s).\nLower = arms already at cruising speed. Higher = only a boat flat out throws them.",
          nullptr },
    } };
    return knobs;
}
} // namespace ocean
