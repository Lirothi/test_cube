#include "app/scene/PlayerGates.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

#include "app/scene/PlayerSystem.h"
#include "app/scene/VesselGates.h"
#include "core/task/TaskSystem.h"
#include "core/threading/RenderThread.h"
#include "core/threading/ThreadRole.h"
#include "physics/PhysicsWorld.h"
#include "physics/WaterSurface.h"
#include "rendering/renderables/RenderableObject.h"

// R14.6: the player through the world's own frame -- physics::World::Tick with the hand-over around it, at 60 and at 300
// frames a second, the road the game takes: a spawn, the keys set before the Tick, the eye read after it. The physics' gate
// "player" moves the capsule by hand and sees nothing of the world's schedule; a player moved on the fixed step would pass
// it. Here the run-up, the stop and the jump must come out the same at both rates, the eye must move in every frame of a
// run and in the very frame of a press, and it must be the eye of a capsule moved by hand with the frame's own time, frame
// for frame (what World::TickPlayer did before R14.6b; a player stepped on the fixed step, or shown between two steps as the
// bodies are, is not).
namespace
{
    using namespace physics;

    // A floor: a static box 200 m across, its top at y 0.
    class FloorObject final : public RenderableObject
    {
    public:
        FloorObject() : RenderableObject("", L"")
        {
            CollisionSettings shape;
            shape.shape = ShapeKind::Custom;
            CollisionPrimitive box;
            box.kind = PrimitiveKind::Box;
            box.center = Math::float3(0.0f, -1.0f, 0.0f);
            box.halfExtents = Math::float3(100.0f, 1.0f, 100.0f);
            shape.primitives.push_back(box);
            SetCollisionSettings(shape);
            SetPhysicsMotion(Motion::Static);
        }
        const std::string& GetGeometryPath() const override { return path_; }
        bool IsSimpleRender() const override { return false; }

    private:
        std::string path_ = "gate-floor"; // a custom shape reads no file; the world only wants a name
    };

    struct FlatWater final : WaterSurface
    {
        float level = 0.0f;
        bool Present() const override { return true; }
        bool HasWaves() const override { return false; }
        float Level() const override { return level; }
        float SampleHeight(float, float, float) const override { return level; }
    };

    // One frame as the game runs it: the Tick (the player in it), the step kicked where the render thread's frame ends,
    // the hand-over.
    void Frame(physics::World& world, float dt)
    {
        world.Tick(dt);
        world.KickStep();
        world.FinishStep();
        world.SyncPoses();
    }

    struct RateRun
    {
        float runUp = 0.0f; // s to 95 % of the walk, the press's frame counted
        float stop = 0.0f;  // m from letting go to standing
        float jump = 0.0f;  // m the feet rise
        int still = 0;      // frames of a 1 s run whose eye did not move
        int frames = 0;
        bool onFloor = false;
        bool pressMoved = false; // the eye moved in the frame the walk was pressed
        bool jumpMoved = false;  // and in the frame of the jump's press
        float gap = 0.0f;        // m, the farthest the eye stood from the capsule moved by hand
        int compared = 0;        // frames it was compared in
        std::uint64_t digest = 1469598103934665603ull; // FNV-1a over every frame's eye
    };

    RateRun RunAt(int fps)
    {
        RateRun out;
        const float dt = 1.0f / static_cast<float>(fps);
        const PlayerSettings tuning = GameSettings().player;
        const auto floor = std::make_unique<FloorObject>();
        physics::World world;
        PlayerSystem walker(world);
        Frame(world, dt); // the floor's body is this world's: the objects' queue goes to the world that drains it first
        // The capsule moved by hand with the frame's own time (World::MoveCharacter, as TickPlayer did), in a world of its own.
        const auto directFloor = std::make_unique<FloorObject>();
        physics::World direct;
        Frame(direct, dt);
        CharacterKey reference = kNoCharacter;
        bool pendingJump = false; // a press the player holds until its Tick takes it
        Math::float3 eye(0.0f, 0.0f, 0.0f);
        bool seen = false;
        const auto frame = [&](const PlayerInput& input)
        {
            walker.SetInput(input);
            Frame(world, dt);
            seen = walker.Eye(eye);
            const float values[4] = { seen ? 1.0f : 0.0f, eye.x, eye.y, eye.z };
            const unsigned char* bytes = reinterpret_cast<const unsigned char*>(values);
            for (std::size_t i = 0; i < sizeof(values); ++i) { out.digest = (out.digest ^ bytes[i]) * 1099511628211ull; }
            ++out.frames;
            PlayerInput taken = input;
            taken.jump = taken.jump || pendingJump;
            pendingJump = false;
            Frame(direct, dt);
            direct.MoveCharacter(reference, std::clamp(dt, 1.0e-4f, 1.0f / 30.0f), taken, tuning, kNoWater);
            CharacterState by;
            if (seen && direct.GetCharacter(reference, tuning, by))
            {
                out.gap = std::max(out.gap, (eye - by.eye).Length());
                ++out.compared;
            }
        };
        const auto frames = [fps](float seconds) { return static_cast<int>(seconds * static_cast<float>(fps) + 0.5f); };
        const PlayerInput none;
        PlayerInput forward;
        forward.move = Math::float3(1.0f, 0.0f, 0.0f);

        walker.Spawn(Math::float3(0.0f, 0.0f, 0.0f));
        reference = direct.AddCharacter(Math::float3(0.0f, 0.0f, 0.0f), tuning);
        for (int i = 0; i < frames(0.3f); ++i) { frame(none); }
        out.onFloor = walker.State().onGround;

        // The press moves the eye in its own frame: the keys are set before the Tick, the eye read after it.
        Math::float3 before = eye;
        frame(forward);
        out.pressMoved = seen && eye.x > before.x;
        float t = dt;
        while (walker.State().speed < 0.95f * tuning.walkSpeed && t < 2.0f)
        {
            frame(forward);
            t += dt;
        }
        out.runUp = t;

        // A run: the eye moves in every frame.
        for (int i = 0; i < frames(1.0f); ++i)
        {
            before = eye;
            frame(forward);
            if (eye.x == before.x && eye.y == before.y && eye.z == before.z) { ++out.still; }
        }

        // The stop.
        const float from = walker.State().feet.x;
        for (int i = 0; i < frames(1.0f); ++i) { frame(none); }
        out.stop = walker.State().feet.x - from;

        // The jump, pressed and let go before the frame's Tick: the press stays until a Tick takes it.
        for (int i = 0; i < frames(0.3f); ++i) { frame(none); }
        const float ground = walker.State().feet.y;
        before = eye;
        PlayerInput press;
        press.jump = true;
        walker.SetInput(press);
        pendingJump = true;
        frame(none);
        out.jumpMoved = seen && eye.y > before.y;
        float top = walker.State().feet.y;
        for (int i = 0; i < frames(1.2f); ++i)
        {
            frame(none);
            top = std::max(top, walker.State().feet.y);
        }
        out.jump = top - ground;
        return out;
    }

    std::string Num(float value)
    {
        char text[32];
        std::snprintf(text, sizeof(text), "%.3f", value);
        return text;
    }

    std::string Hex(std::uint64_t value)
    {
        char text[32];
        std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
        return text;
    }

    int GatePlayerFrame(std::string& detail)
    {
        const Settings saved = GameSettings();
        const bool savedRenderThread = threading::g_renderThreadEnabled;
        threading::g_renderThreadEnabled = true;
        threading::RoleScope role(threading::ThreadRole::Game);
        TaskSystem::Get().Start();
        const auto finish = [&](int verdict)
        {
            GameSettings() = saved;
            threading::g_renderThreadEnabled = savedRenderThread;
            return verdict;
        };
        Settings& settings = GameSettings();
        settings = Settings{};
        settings.async = true; // the step on its own thread, the poses at the hand-over: the game's own schedule
        settings.threads = 2;
        settings.simulate = true;
        std::string wrong;
        const auto check = [&wrong](bool ok, const std::string& what)
        {
            if (!ok) { wrong += " " + what + ";"; }
        };

        // ---- At 60 and at 300 frames a second.
        const PlayerSettings tuning;
        const RateRun at60 = RunAt(60);
        const RateRun at300 = RunAt(300);
        const float runUp = 0.95f * tuning.walkSpeed / tuning.acceleration;
        const float jump = tuning.jumpSpeed * tuning.jumpSpeed / (2.0f * -settings.gravity);
        detail = "the world's frame, 60 / 300 fps: run-up " + Num(at60.runUp) + " / " + Num(at300.runUp) + " s (" + Num(runUp) + "), stops in " + Num(at60.stop) + " / " +
            Num(at300.stop) + " m (0.20), jumps " + Num(at60.jump) + " / " + Num(at300.jump) + " m (" + Num(jump) + "), frames of a run the eye stood " +
            std::to_string(at60.still) + " / " + std::to_string(at300.still) + ", the eye moved in the press's frame " + std::to_string(at60.pressMoved) + std::to_string(at60.jumpMoved) +
            " / " + std::to_string(at300.pressMoved) + std::to_string(at300.jumpMoved) + "; off a capsule moved by hand " + Num(at60.gap * 1000.0f) + " / " + Num(at300.gap * 1000.0f) + " mm in " +
            std::to_string(at60.compared) + " / " + std::to_string(at300.compared) + " frames; the eye's road " + Hex(at60.digest) + " / " + Hex(at300.digest) + " (" +
            std::to_string(at60.frames) + " / " + std::to_string(at300.frames) + " frames)";
        for (const RateRun* run : { &at60, &at300 })
        {
            const std::string fps = run == &at60 ? "60" : "300";
            check(run->onFloor, fps + " fps: not on the floor");
            check(std::fabs(run->runUp - runUp) <= 0.05f, fps + " fps: the run-up");
            check(std::fabs(run->stop - 0.20f) <= 0.06f, fps + " fps: the stop");
            check(std::fabs(run->jump - jump) <= 0.07f, fps + " fps: the jump");
            check(run->still == 0, fps + " fps: the eye stood in frames of a run");
            check(run->pressMoved, fps + " fps: the walk's press did not move the eye in its frame");
            check(run->jumpMoved, fps + " fps: the jump's press did not move the eye in its frame");
            check(run->compared == run->frames && run->gap <= 0.001f, fps + " fps: the eye is not the capsule's moved by the frame's own time");
        }
        // The same at both rates, to what a 60 Hz frame's integration allows. Both rates err the same way and 300 the less, so
        // the gap is at most 60's own error: the run-up counts whole frames (1/60 + 1/300 s); the stop is short by at most one
        // frame's travel at the walk (4 m/s / 60: 6.7 cm); the jump's top, by the semi-implicit step, by half a frame's rise
        // at the jump's speed (4.2 m/s / 120: 3.5 cm).
        check(std::fabs(at60.runUp - at300.runUp) <= 1.0f / 60.0f + 1.0f / 300.0f, "the run-up differs between the rates");
        check(std::fabs(at60.stop - at300.stop) <= tuning.walkSpeed / 60.0f, "the stop differs between the rates");
        check(std::fabs(at60.jump - at300.jump) <= tuning.jumpSpeed / 120.0f, "the jump differs between the rates");

        // ---- The road, at 60.
        {
            const auto floor = std::make_unique<FloorObject>();
            physics::World world;
            PlayerSystem walker(world);
            FlatWater water;
            water.level = 2.0f;
            bool wet = false;
            const auto tick = [&](int count, const PlayerInput& input)
            {
                for (int i = 0; i < count; ++i)
                {
                    world.SetWater(wet ? &water : nullptr);
                    walker.SetInput(input);
                    Frame(world, 1.0f / 60.0f);
                }
            };
            const PlayerInput none;
            PlayerInput forward;
            forward.move = Math::float3(1.0f, 0.0f, 0.0f);
            Math::float3 eye;
            const auto standsAt = [&walker](float x) { const PlayerState state = walker.State(); return state.exists && std::fabs(state.feet.x - x) < 0.01f && std::fabs(state.feet.y) < 0.05f; };

            check(!walker.State().exists && !walker.Eye(eye), "a body before any spawn");
            walker.Spawn(Math::float3(0.0f, 0.0f, 0.0f));
            tick(20, none);
            const bool spawned = standsAt(0.0f) && walker.Eye(eye) && walker.State().possessed;
            check(spawned, "spawned, it should stand at the start with an eye");
            // F8: the body stays and takes no keys, the eye says nothing; back, it says again.
            walker.SetPossessed(false);
            const bool freed = !walker.Eye(eye) && walker.State().exists && !walker.State().possessed;
            tick(30, forward);
            const bool idle = standsAt(0.0f);
            walker.SetPossessed(true);
            tick(1, none);
            const bool back = walker.Eye(eye);
            check(freed && idle && back, "F8: freed " + std::to_string(freed) + ", no keys " + std::to_string(idle) + ", back " + std::to_string(back));
            // The level that left: the capsule goes, the next frame brings it back at the start (the session says where).
            walker.Spawn(Math::float3(3.0f, 0.0f, 0.0f));
            tick(20, none);
            tick(30, forward);
            const float walked = walker.State().feet.x;
            walker.LevelLeft();
            const bool left = !walker.State().exists && !walker.Eye(eye);
            tick(1, none);
            const bool returned = standsAt(3.0f) && walker.Eye(eye);
            check(walked > 4.0f && left && returned, "the level that left: walked to " + Num(walked) + ", gone " + std::to_string(left) + ", back " + std::to_string(returned));
            // A world gone (physics.enabled:0) takes the capsule; its return brings it back at the start.
            tick(30, forward);
            settings.enabled = false;
            tick(1, none);
            const bool gone = !walker.State().exists && !walker.Eye(eye);
            settings.enabled = true;
            tick(2, none);
            const bool rebuilt = standsAt(3.0f) && walker.Eye(eye);
            check(gone && rebuilt, "the world gone: no body " + std::to_string(gone) + ", back with it " + std::to_string(rebuilt));
            // Water over the swim depth, under its feet each frame: it floats, the eye just over the surface.
            wet = true;
            tick(240, none);
            wet = false;
            const bool eyed = walker.Eye(eye);
            const bool swims = eyed && walker.State().mode == PlayerMode::Swimming && eye.y >= water.level && eye.y <= water.level + 0.3f;
            check(swims, "in 2 m of water it should swim, the eye at " + Num(eye.y));
            // Under physics.killY it comes back at the start.
            settings.killY = -5.0f;
            walker.Spawn(Math::float3(150.0f, 0.0f, 0.0f)); // past the floor's edge
            float lowest = 0.0f;
            bool respawned = false;
            float lastY = 0.0f;
            for (int i = 0; i < 120; ++i)
            {
                tick(1, none);
                const float y = walker.State().feet.y;
                lowest = std::min(lowest, y);
                respawned = respawned || (lastY < -4.0f && y > -1.0f);
                lastY = y;
            }
            check(respawned, "under physics.killY it should come back, lowest " + Num(lowest));
            // Gone: no body, no eye, and the frames do not bring it back.
            walker.Remove();
            tick(2, none);
            check(!walker.State().exists && !walker.Eye(eye), "removed, it came back");
            detail += "; the road: spawn " + std::to_string(spawned) + ", F8 " + std::to_string(freed && idle && back) + ", the level left " + std::to_string(left && returned) +
                ", the world gone " + std::to_string(gone && rebuilt) + ", swims with the eye at " + Num(eye.y) + ", under killY back " + std::to_string(respawned) + " (lowest " + Num(lowest) + ")";
        }
        if (!wrong.empty())
        {
            detail += "; WRONG:" + wrong;
            return finish(1);
        }
        return finish(0);
    }
} // namespace

std::vector<physics::ExtraGate> PlayerGates()
{
    return { { "player", &GatePlayerFrame } };
}

std::vector<physics::ExtraGate> SceneGates()
{
    std::vector<physics::ExtraGate> gates = VesselGates();
    for (const physics::ExtraGate& gate : PlayerGates()) { gates.push_back(gate); }
    return gates;
}
