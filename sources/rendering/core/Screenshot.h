#pragma once

#include <string>

class Renderer;

// Reliable window capture for verification. GDI screen-blit and PrintWindow both read BLACK on
// this flip-model DXGI swapchain (the content presents via DWM and bypasses WM_PRINT / the GDI
// desktop bitmap). This reads the last-presented backbuffer back to the CPU and writes a PNG —
// occlusion-, foreground- and compositor-independent. Triggered by "--shot=<path>" (see App).
namespace Screenshot
{
    // Copy the last-presented backbuffer to a readback buffer (GPU-idle) and encode a PNG (WIC).
    // Returns false on any failure. Call at a safe point (between frames), not mid-record.
    bool SaveBackbufferPng(Renderer& renderer, const std::string& path);
}
