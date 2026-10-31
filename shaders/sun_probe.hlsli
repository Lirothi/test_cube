#ifndef SUN_PROBE_HLSLI
#define SUN_PROBE_HLSLI

// THE SUN PROBE's place in the metering record (ExposureMetering.cpp kSunProbeBytes), behind the
// exposure record's 16 bytes. Written once a frame by bloom_conv_cs.hlsl stage 10 -- what the sun
// disc looks like in the frame, held while the disc is off it -- and read by the sun glare (stage 9)
// and the corona (tonemap_cs.hlsl SunRays) instead of each of their pixels tapping the disc.
static const uint kSunProbeHeld = 16u;      // last disc colour seen (rgb, 12 bytes), then the last gate
static const uint kSunProbeOut = 32u;       // this frame's colour x gate x off-frame fade (rgb), then the
                                            // visible share of the disc (0-1)
static const uint kSunProbeCentroid = 48u;  // centroid of what shows of the disc: tangent-plane offset
                                            // from its centre (xy, y up) -- where the corona's rays leave
// Time-of-day E1, behind the probe (ExposureMetering.cpp kCompensationRecordBytes), both written by
// exposure_solve_cs.hlsl: the exposure COMPENSATION in effect this frame (EV, the static trim plus the
// curve, positive brightens) -- night vision adds it to the camera's exposure to find what the eye is
// adapted to -- and at +4 the eye's adaptation before the curve, the state the solve adapts from.
static const uint kExposureCompensationOffset = 64u;
// ...and at +8 the frame's OPEN-SKY share, smoothed (the state the solve smooths from), at +12 the share of the OPEN
// night's camera the frame takes -- moved toward what the share asks for (TimeOfDaySettings::nightCameraOpenFrom /
// nightCameraOpenFull) at the adaptation's speeds, also a state.
static const uint kExposureOpenSkyOffset = kExposureCompensationOffset + 8u;
// THE PIXEL PROBE (ExposureMetering.cpp kPixelProbeBytes, bloom_conv_cs.hlsl stage 11), behind the
// compensation record: +0 the brightest pixel's rgb (pre-exposed) and +12 its luminance after the
// FFT input's local exposure; +16 the window's mean rgb and +28 the luminance of a 4x4 box at the
// brightest pixel. A debug read-out (editor MCP get_readouts), written only while it is asked for.
static const uint kPixelProbeOffset = 80u;

#endif // SUN_PROBE_HLSLI
