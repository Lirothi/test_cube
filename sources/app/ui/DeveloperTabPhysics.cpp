// R10.4: Developer Controls > Physics (DeveloperWindow.h). A shipped build compiles none of this.
#if !TC_SHIPPING
#include "app/ui/DeveloperWindow.h"

#include <string>

#include "app/game/PlayWorld.h" // R6.2a
#include "app/scene/Scene.h"
#include "imgui.h"

void DeveloperWindow::DrawPhysicsTab(Scene& scene)
{
    // The physics world (docs/physics_plan.md). Game-side: this tab does not stop the render half.
    physics::Settings& settings = physics::GameSettings();
    ImGui::Checkbox("World", &settings.enabled);
    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Off: no bodies, no shapes, no cost; every object back where the level has it. The A/B."); }
    bool showShapes = settings.debug != 0;
    if (ImGui::Checkbox("Show collision", &showShapes)) { settings.debug = showShapes ? 1 : 0; }
    ImGui::DragFloat("Show radius (m)", &settings.debugRadius, 1.0f, 1.0f, 1000.0f, "%.0f");
    ImGui::DragFloat("Gravity (m/s2)", &settings.gravity, 0.05f, -30.0f, 30.0f, "%.2f");
    ImGui::DragFloat("Step rate (Hz)", &settings.stepHz, 1.0f, 10.0f, 480.0f, "%.0f");
    ImGui::DragInt("Threads", &settings.threads, 0.1f, 0, 16);
    ImGui::Checkbox("Step beside the frame", &settings.async);
    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("On: the step on its own thread, the poses taken at the hand-over; a step not done by then leaves the pose for a frame and the frame does not wait.\nOff: the step in Scene::Tick, on the game thread. --set=physics.async:0|1"); }
    const physics::Stats ps = scene.Physics().GetStats();
    ImGui::SeparatorText("The world");
    ImGui::Text("%u objects: %u static, %u kinematic, %u dynamic (%u in the water)", ps.objects, ps.staticBodies, ps.kinematicBodies, ps.dynamicBodies, ps.floatingBodies);
    ImGui::Text("%u awake, %u steps last frame, a step %.3f ms (avg %.3f)", ps.activeBodies, ps.stepsLastFrame, ps.stepMs, ps.stepMsEma);
    ImGui::Text("%llu hand-overs found the step running, %llu calls waited for one", static_cast<unsigned long long>(ps.handOversHeld), static_cast<unsigned long long>(ps.stepWaits));
    if (ps.stepErrors != 0) { ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "%llu steps ran out of room: bodies are falling through the world", static_cast<unsigned long long>(ps.stepErrors)); }
    ImGui::Text("%u meshes of %llu triangles, %u hulls, %u primitive; the last batch built in %.1f ms", ps.meshShapes, static_cast<unsigned long long>(ps.shapeTriangles),
        ps.hullShapes, ps.primitiveShapes, ps.lastBuildMs);
    ImGui::Text("%.1f MB held", static_cast<double>(ps.bytes) / (1024.0 * 1024.0));
    ImGui::SeparatorText("The player");
    const physics::PlayerState player = scene.Player().State();
    if (player.exists)
    {
        ImGui::Text("%s%s, %.2f m/s; the feet at %.2f %.2f %.2f", physics::PlayerModeName(player.mode), player.crouched ? ", crouched" : "", player.speed, player.feet.x, player.feet.y, player.feet.z);
    }
    else
    {
        ImGui::TextDisabled(ps.simulating ? "No body: the camera is free. F8 takes one." : "The world stands while the level is edited. Play (Alt+P) runs it.");
    }
    for (const game::PlayerKey& key : game::PlayerKeys())
    {
        ImGui::DragFloat(key.label, &(settings.player.*(key.member)), key.speed, key.lo, key.hi, key.format);
    }
    ImGui::TextDisabled("Height, crouched height and radius are read when the body is made. A level's Player Start says its own at Play, and what is set here during Play ends with it.");

    // A boat nobody steers stands on a hard mooring (physics::HoldMooring).
    ImGui::SeparatorText("Boats nobody steers");
    ImGui::DragFloat("Mooring stiffness (rad/s)", &settings.mooring, 0.05f, 0.0f, 10.0f, "%.2f");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("A boat with nobody at its helm and no bot, its own anchor up, holds its place along the water once it has\n"
                          "slowed down -- the waves still heave, pitch and roll it. How firmly: the hold's own frequency (2.5 = a wave's\n"
                          "push of a tenth of g moves it some 16 cm). 0 = no hold: it drifts.\n--set physics.mooring (0 .. 10)");
    }
    ImGui::DragFloat("Mooring turn (rad/s)", &settings.mooringTurn, 0.05f, 0.0f, 10.0f, "%.2f");
    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("And its heading, the same way. 0 = it swings freely.\n--set physics.mooringTurn (0 .. 10)"); }
    ImGui::DragFloat("Moors below (m/s)", &settings.mooringSettle, 0.05f, 0.0f, 20.0f, "%.2f");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("A boat let go (you left its helm, a bot finished) coasts on and moors where it has slowed to this.\n--set physics.mooringSettle (0 .. 20)");
    }

    // The race's bots (game::RaceBot): a race makes its own boats of the kinds that carry "raceBot"; B at a helm is the same bot.
    ImGui::SeparatorText("Race bots");
    game::BotSettings& bot = game::BotSettingsRef();
    for (const game::BotKey& key : game::BotKeys())
    {
        const bool flag = key.hi == 1.0f && key.lo == 0.0f && key.speed == 1.0f;
        if (flag)
        {
            bool on = bot.*(key.member) > 0.5f;
            if (ImGui::Checkbox(key.label, &on)) { bot.*(key.member) = on ? 1.0f : 0.0f; }
        }
        else { ImGui::DragFloat(key.label, &(bot.*(key.member)), key.speed, key.lo, key.hi, key.format); }
        if (ImGui::IsItemHovered()) { ImGui::SetTooltip("%s\n--set bot.%s (%g .. %g)", key.hint, key.key, key.lo, key.hi); }
    }
    game::RaceSettings& race = game::RaceSettingsRef();
    for (const game::RaceKey& key : game::RaceKeys())
    {
        ImGui::DragFloat(key.label, &(race.*(key.member)), key.speed, key.lo, key.hi, key.format);
        if (ImGui::IsItemHovered()) { ImGui::SetTooltip("%s\n--set race.%s (%g .. %g)", key.hint, key.key, key.lo, key.hi); }
    }
    const game::PlayWorld* play = game::ActivePlayWorld(); // R6.2a: the session's race
    const std::wstring raceLine = play ? play->race.HudLine() : std::wstring();
    if (!raceLine.empty()) { ImGui::Text("%ls", raceLine.c_str()); }
    else { ImGui::TextDisabled(play && play->race.HasTrack() ? "The course is there; no boat runs it." : "No course in Play: mark buoys with a Race gate number (Inspector > Race)."); }
}

#endif // !TC_SHIPPING
