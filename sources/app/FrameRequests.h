#pragma once

#include <cstdint>

// What a --set / --sweep / MCP set_settings asks of App::Run's frame: the settings rows write these
// (app/settings/FrameSettingRows.cpp), the loop takes them at its safe point. They lived in App.cpp beside the chain of
// names that wrote them until architecture plan R2.2 moved the chain into the settings registry.

// "--set=camera.fly:<m/s>" / MCP set_settings: fly along the camera's own view, negative = backwards, 0 stops --
// a person flying through a tunnel in a running editor (the camera's heading is its own; --cam-fly's is the world's).
inline float g_camFlyForward = 0.0f;
// `frame.recordDt` (MCP, for a film; 0 = off): the world advances by exactly this many seconds on a frame an MCP
// screenshot captures and stands on every other frame -- so a script sets the boat's helm and the camera, takes the
// frame, and the next one is 1/60 s of the world later whatever the PNG cost. A real-time capture drops and doubles
// frames; a `burst` without this lets the clocks drift apart (the wake fell behind the boat).
inline float g_recordDt = 0.0f;
// `frame.timeScale` (--set / MCP; 1 = real time): the world's clock runs this many times faster -- a three-lap race of
// five minutes in forty seconds at 8. Everything of the world goes by the frame's dt (the ocean's and the wind's clock,
// the physics' steps, the boats' water, the bots, the race's clock), so the dt is what is scaled, capped at 0.1 s as a
// frame's always is: a frame too slow for the scale slows the world, it does not jump it. What a scaled run is: the game
// at (frame rate / scale) frames a second -- at 240 fps and 4 the physics steps once a frame and the water's heights
// land 30 times a second of the world, as at 60 fps; past frame rate / 15 the physics drops steps (physics.maxSteps).
// The HUD says so while it is not 1.
inline float g_timeScale = 1.0f;
// "--set=dlss.mode:<sl::DLSSMode>" / MCP set_settings: switch the upscaler in a RUNNING process, at the frame's safe
// point (the dev window's combo path). 0 = off (native), 2 = Balanced, 6 = DLAA. -1 = nothing asked.
inline int g_requestDlssMode = -1;
// "--set=aa.renderScale:<0.25..1>": the render scale without DLSS (the resolution group's native road), at the same safe
// point as dlss.mode. < 0 = nothing asked.
inline float g_requestRenderScale = -1.0f;
// Occlusion plan S0, `scene.replicate:<K>`: the level's static meshes K*K-1 more times on a grid (App.cpp
// ReplicateLevelObjects). Recorded by the --set row, executed once the level is in. 0 = nothing asked.
inline std::uint32_t g_sceneReplicateRequest = 0;
