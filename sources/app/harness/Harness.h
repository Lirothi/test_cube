#pragma once

class LevelManager;
class Renderer;
class Scene;
struct SceneRenderSettings;

namespace harness
{
// What a harness sees once a frame, at App::Run's safe point after the frame was presented.
struct FrameContext
{
    Renderer& renderer;
    Scene& scene;
    LevelManager& levelManager;
    SceneRenderSettings& settings;
    bool& running; // false ends the run; a harness reads it too (a dump due when the run ends)
};

// A run that is a measurement or a gate (architecture plan R3.3): what the command line asked for, done at the frame's
// safe point, with a verdict for main's exit code. Wanted says whether this run asked for it; OnBoot runs once the
// first level is in, OnFrame every frame while it is wanted, Verdict after the loop.
class Harness
{
public:
    virtual ~Harness() = default;
    virtual const char* Name() const = 0;
    virtual bool Wanted() const = 0;
    virtual void OnBoot(FrameContext&) {}
    // Right after Renderer::BeginFrame, the render thread idle (--scene-stress: requests that arrive mid-frame).
    virtual void OnBeginFrame(FrameContext&) {}
    // A frame threw (App::Run rethrows after): a harness with a verdict to write does it here (--scene-stress).
    virtual void OnFrameThrew(const char* /*what*/) {}
    virtual void OnFrame(FrameContext& ctx) = 0;
    virtual int Verdict() const { return 0; } // main's exit code when not 0
};
}
