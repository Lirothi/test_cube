#pragma once
#if WITH_EDITOR

#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "editor/intent/EditorActionRegistry.h" // EditorActionContext, EditorIntent, EditorSceneDocument
#include "editor/intent/EditorSceneQuery.h"    // editorquery::ProbeGroundHeight

// R10.3: the builders behind EditorActionRegistry's table, one file a domain (editor/intent/EditorActions*.cpp):
//   EditorActionsCommon.cpp      the helpers every builder shares -- parameter access, counting, folding into one undo entry;
//   EditorActionsObjects.cpp     select-and-change actions on objects: delete, duplicate, show / hide, group, rename, isolate, replace;
//   EditorActionsAppearance.cpp  how an object looks: colour, surface, material, cast shadow, the live material params;
//   EditorActionsGame.cpp        what the game reads off an object: buoyancy, race gates and bots, vessels and boat cards, physics, the Player Start;
//   EditorActionsLights.cpp      lights: when a lamp comes on, a light's settings, adding lights to meshes;
//   EditorActionsZones.cpp       zones: tracing a waterline, creating a zone, thinning what is inside one;
//   EditorActionsTransform.cpp   moving things: move / rotate / scale, align / distribute / snap, randomizing, burying;
//   EditorActionsEnvironment.cpp the level's environment: a setting by its path, the time of day and the date;
//   EditorActionsSpawn.cpp       making things: spawn (scatter), place, and the scatter's anchor;
// Each was cut VERBATIM out of EditorActionRegistry.cpp's anonymous namespace into `namespace editoractions`. This
// header holds what several files share and declares what one file calls in another (the table calls them all);
// a builder only its own file calls is not declared here. Internal to the action registry: nothing else includes it.
namespace editoractions
{
    // Names continue the LEVEL's numbering: "<stem> NNN" takes the next NNN after the highest the
    // document already has for that stem. Restarting at 001 on every spawn/place minted names the
    // level already had -- and then a phrase excluding "old_boat 001" could not tell the boat pulled
    // up on the beach from the one just placed in the lagoon, and left out both.
    class NameCounter
    {
    public:
        explicit NameCounter(const EditorSceneDocument& document) : document_(document) {}

        std::string Next(const std::string& stem)
        {
            auto it = next_.find(stem);
            if (it == next_.end()) { it = next_.emplace(stem, editornames::HighestNameOrdinal(document_, stem)).first; }
            char buffer[192];
            std::snprintf(buffer, sizeof(buffer), "%s %03zu", stem.c_str(), ++it->second);
            return buffer;
        }

    private:
        const EditorSceneDocument& document_;
        std::map<std::string, std::size_t> next_;
    };

    // Defined in EditorActionsSpawn.cpp with the scatter it was written for. Declared here because
    // createZone wants the same answer to "where is the designer pointing": a zone made by
    // a phrase and a scatter made by a phrase must land in the same place.
    Math::float3 ResolveScatterAnchor(EditorContext& ctx,
        const EditorSpatialFilter& where,
        float radius);

    // ------------------------------------------------------------------- ground
    //
    // Shared by `bury` and by `spawn`'s placement: both answer "where is the surface
    // under this point", and both have to ignore the objects they are about to move or
    // create, or they measure against themselves.

    // The ground probe and its start height now live in editor/intent/EditorSceneQuery,
    // because the scatter is no longer the only caller: the model can ask how high the
    // ground is at a point, and the two answers have to be the same answer. They were a
    // copy here for exactly as long as there was one caller.
    using editorquery::kGroundProbeUp;
    using editorquery::ProbeGroundHeight;

    // Horizontal step for the two extra probes that estimate the slope.
    constexpr float kSlopeProbeStep = 0.75f;

    // EditorActionsCommon.cpp
    const EditorObject* FindEnvironmentObject(const EditorSceneDocument& document, EditorObjectId id);
    bool IsBulkObjectSupported(const EditorSceneDocument& document, EditorObjectId id);
    std::unique_ptr<EditorCommand> FoldIntoOneEntry(
        std::vector<std::unique_ptr<EditorCommand>>&& commands,
        const std::string& label);
    std::string CountedObjects(std::size_t count);
    float NumberOr(const nlohmann::json& params, const char* key, float fallback);
    bool BoolOr(const nlohmann::json& params, const char* key, bool fallback);
    std::string StringOr(const nlohmann::json& params, const char* key, const char* fallback);
    void RangeOr(const nlohmann::json& params, const char* key, float& lo, float& hi);
    Math::float3 Vec3Or(const nlohmann::json& params, const char* key, const Math::float3& fallback);
    std::uint32_t SeedFor(const nlohmann::json& params, const EditorSceneDocument& document);

    // EditorActionsObjects.cpp
    std::unique_ptr<EditorCommand> BuildDelete(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent&,
        std::string& outStatus);
    std::unique_ptr<EditorCommand> BuildDuplicate(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent&,
        std::string& outStatus);
    std::unique_ptr<EditorCommand> BuildSetEnabled(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outStatus);
    std::unique_ptr<EditorCommand> BuildGroup(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outStatus);
    void RefineGroupPreview(const EditorActionContext& actionCtx,
        const EditorIntent& intent,
        EditorIntentPreview& preview);
    void RefineRenamePreview(const EditorActionContext& actionCtx,
        const EditorIntent& intent,
        EditorIntentPreview& preview);
    std::unique_ptr<EditorCommand> BuildRename(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outStatus);
    std::unique_ptr<EditorCommand> BuildIsolate(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent&,
        std::string& outStatus);
    std::unique_ptr<EditorCommand> BuildReplace(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outStatus);

    // EditorActionsAppearance.cpp
    std::unique_ptr<EditorCommand> BuildSetCastShadow(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outStatus);
    std::unique_ptr<EditorCommand> BuildSetColor(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outStatus);
    std::unique_ptr<EditorCommand> BuildSetSurface(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outStatus);
    std::unique_ptr<EditorCommand> BuildSetMaterial(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outStatus);

    // EditorActionsGame.cpp
    std::unique_ptr<EditorCommand> BuildSetBuoyant(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outStatus);
    std::unique_ptr<EditorCommand> BuildSetRaceGate(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outStatus);
    std::unique_ptr<EditorCommand> BuildSetRaceBot(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outStatus);
    std::unique_ptr<EditorCommand> BuildSetVessel(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outStatus);
    std::unique_ptr<EditorCommand> BuildSetBoatCard(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outStatus);
    std::unique_ptr<EditorCommand> BuildSetPhysics(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outStatus);
    std::unique_ptr<EditorCommand> BuildKeepSimulation(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent&,
        std::string& outStatus);
    std::unique_ptr<EditorCommand> BuildPlacePlayerStart(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outStatus);

    // EditorActionsLights.cpp
    std::unique_ptr<EditorCommand> BuildSetOnAt(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outStatus);
    std::unique_ptr<EditorCommand> BuildSetLight(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outStatus);
    std::unique_ptr<EditorCommand> BuildAddLights(const EditorActionContext&,
        const std::vector<EditorObjectId>&,
        const EditorIntent& intent,
        std::string& outStatus);

    // EditorActionsZones.cpp
    std::unique_ptr<EditorCommand> BuildTraceZone(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outStatus);
    void RefineThinPreview(const EditorActionContext& actionCtx,
        const EditorIntent& intent,
        EditorIntentPreview& preview);
    std::unique_ptr<EditorCommand> BuildThin(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outStatus);
    std::unique_ptr<EditorCommand> BuildCreateZone(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outStatus);

    // EditorActionsTransform.cpp
    std::unique_ptr<EditorCommand> BuildBury(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outStatus);
    std::unique_ptr<EditorCommand> BuildMove(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets, const EditorIntent& intent, std::string& outStatus);
    std::unique_ptr<EditorCommand> BuildRotate(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets, const EditorIntent& intent, std::string& outStatus);
    std::unique_ptr<EditorCommand> BuildScale(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets, const EditorIntent& intent, std::string& outStatus);
    std::unique_ptr<EditorCommand> BuildAlign(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outStatus);
    std::unique_ptr<EditorCommand> BuildDistribute(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outStatus);
    std::unique_ptr<EditorCommand> BuildSnap(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outStatus);
    std::unique_ptr<EditorCommand> BuildRandomizeRotation(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outStatus);
    std::unique_ptr<EditorCommand> BuildRandomizeScale(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outStatus);

    // EditorActionsEnvironment.cpp
    std::unique_ptr<EditorCommand> BuildSetEnvironment(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>&,
        const EditorIntent& intent,
        std::string& outStatus);
    std::unique_ptr<EditorCommand> BuildSetTime(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>&, const EditorIntent& intent, std::string& outStatus);
    std::unique_ptr<EditorCommand> BuildSetDate(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>&, const EditorIntent& intent, std::string& outStatus);
    void RefineSetTime(const EditorActionContext& actionCtx, const EditorIntent& intent, EditorIntentPreview& preview);
    void RefineSetDate(const EditorActionContext& actionCtx, const EditorIntent& intent, EditorIntentPreview& preview);

    // EditorActionsSpawn.cpp
    std::unique_ptr<EditorCommand> BuildSpawn(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>&,
        const EditorIntent& intent,
        std::string& outStatus);
    std::unique_ptr<EditorCommand> BuildPlace(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>&,
        const EditorIntent& intent,
        std::string& outStatus);
}

#endif // WITH_EDITOR
