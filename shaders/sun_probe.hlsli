#ifndef SUN_PROBE_HLSLI
#define SUN_PROBE_HLSLI

// THE SUN PROBE's place in the metering record (ExposureMetering.cpp kSunProbeBytes), behind the
// exposure record's 16 bytes. Written once a frame by bloom_conv_cs.hlsl stage 10 -- what the sun
// disc looks like in the frame, held while the disc is off it -- and read by the sun glare (stage 9)
// and the corona (tonemap_cs.hlsl SunRays) instead of each of their pixels tapping the disc.
static const uint kSunProbeHeld = 16u;  // last disc colour seen (rgb, 12 bytes), then the last gate
static const uint kSunProbeOut = 32u;   // this frame's colour x gate x off-frame fade (rgb)

#endif // SUN_PROBE_HLSLI
