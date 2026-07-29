const STORAGE_KEY = "neon-survivors-visual-settings-v1";
const GLOW_MODES = ["full", "reduced", "off"];
const GLOW_LAYER_MODES = ["lowres", "inline"];
const MOTION_MODES = ["auto", "reduced", "full"];
const QUALITY_MODES = ["high", "efficient"];

const motionQuery = typeof matchMedia === "function"
  ? matchMedia("(prefers-reduced-motion: reduce)")
  : null;
const listeners = new Set();

function readStoredSettings() {
  try {
    const parsed = JSON.parse(localStorage.getItem(STORAGE_KEY) || "{}");
    return parsed && typeof parsed === "object" ? parsed : {};
  } catch {
    return {};
  }
}

function validMode(value, allowed, fallback) {
  return allowed.includes(value) ? value : fallback;
}

const stored = readStoredSettings();

export const visualSettings = {
  glowMode: validMode(stored.glowMode, GLOW_MODES, "full"),
  glowLayerMode: validMode(stored.glowLayerMode, GLOW_LAYER_MODES, "lowres"),
  motionMode: validMode(stored.motionMode, MOTION_MODES, "auto"),
  qualityMode: validMode(stored.qualityMode, QUALITY_MODES, "high"),
  glowScale: 1,
  particleScale: 1,
  reducedMotion: false,
  screenShake: false,
  revision: 0,
};

function persistSettings() {
  try {
    localStorage.setItem(STORAGE_KEY, JSON.stringify({
      glowMode: visualSettings.glowMode,
      glowLayerMode: visualSettings.glowLayerMode,
      motionMode: visualSettings.motionMode,
      qualityMode: visualSettings.qualityMode,
    }));
  } catch {
    // Storage can be unavailable in private/file contexts; runtime settings still work.
  }
}

function applySettings({ persist = true } = {}) {
  visualSettings.glowScale = visualSettings.glowMode === "full"
    ? 1
    : visualSettings.glowMode === "reduced" ? 0.45 : 0;
  visualSettings.particleScale = visualSettings.qualityMode === "high" ? 1 : 0.58;
  visualSettings.reducedMotion = visualSettings.motionMode === "reduced"
    || (visualSettings.motionMode === "auto" && motionQuery?.matches === true);
  visualSettings.revision++;

  if (document.body) {
    document.body.dataset.glow = visualSettings.glowMode;
    document.body.dataset.glowLayer = visualSettings.glowLayerMode;
    document.body.dataset.effectsQuality = visualSettings.qualityMode;
    document.body.dataset.motion = visualSettings.motionMode;
    document.body.classList.toggle("reduced-motion", visualSettings.reducedMotion);
  }

  if (persist) persistSettings();
  for (const listener of listeners) listener(visualSettings);
}

function cycle(current, allowed) {
  return allowed[(allowed.indexOf(current) + 1) % allowed.length];
}

export function cycleGlowMode() {
  visualSettings.glowMode = cycle(visualSettings.glowMode, GLOW_MODES);
  applySettings();
}

export function cycleGlowLayerMode() {
  visualSettings.glowLayerMode = cycle(visualSettings.glowLayerMode, GLOW_LAYER_MODES);
  applySettings();
}

export function cycleMotionMode() {
  visualSettings.motionMode = cycle(visualSettings.motionMode, MOTION_MODES);
  applySettings();
}

export function cycleQualityMode() {
  visualSettings.qualityMode = cycle(visualSettings.qualityMode, QUALITY_MODES);
  applySettings();
}

export function subscribeVisualSettings(listener) {
  listeners.add(listener);
  listener(visualSettings);
  return () => listeners.delete(listener);
}

export function getVisualSettingsLabels() {
  const glow = visualSettings.glowMode === "full"
    ? "Full"
    : visualSettings.glowMode === "reduced" ? "Reduced" : "Off";
  const glowLayer = visualSettings.glowLayerMode === "lowres" ? "Low-res" : "Inline";
  const motion = visualSettings.motionMode === "auto"
    ? `System${motionQuery?.matches ? " (Reduced)" : ""}`
    : visualSettings.motionMode === "reduced" ? "Reduced" : "Full";
  const quality = visualSettings.qualityMode === "high" ? "High" : "Efficient";
  return { glow, glowLayer, motion, quality };
}

motionQuery?.addEventListener?.("change", () => {
  if (visualSettings.motionMode === "auto") applySettings({ persist: false });
});

applySettings({ persist: false });
