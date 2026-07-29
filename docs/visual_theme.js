import { COLORS } from "./colors.js";

const GLOW = Object.freeze({
  low: 10,
  medium: 18,
  high: 28,
});

const OPACITY = Object.freeze({
  dim: 0.25,
  soft: 0.45,
  medium: 0.6,
  strong: 0.85,
  solid: 0.95,
});

const CANVAS = Object.freeze({
  strokeDim: "rgba(255,255,255,.10)",
  enemyOutline: "rgba(255,255,255,.34)",
  enemyDetail: "rgba(255,255,255,.78)",
  textStroke: "rgba(0,0,0,.35)",
  chestFill: "rgba(70,255,143,0.18)",
  trinketFill: "rgba(124,255,217,0.18)",
  augFill: "rgba(141,123,255,0.18)",
  companionFill: "rgba(154,255,106,0.18)",
  questFill: "rgba(255,184,74,0.18)",
  auraFill: COLORS.playerAura,
  auraStroke: COLORS.playerAuraStroke,
  playerGlow: COLORS.playerGlow,
  playerCore: COLORS.playerCore,
  shieldRing: COLORS.auraRingShield,
  magnetRing: COLORS.magnetRing,
  overlayDim: COLORS.overlayDim,
  orbRing: "rgba(177,96,255,.35)",
  hpBarBg: "rgba(255,255,255,.12)",
  hpBarShadow: "rgba(37,240,255,.6)",
  hpBarFill: "rgba(255,37,37,.75)",
  hpBarPlayerFill: "rgba(255,37,37,.5)",
  axeShadow: "rgba(177,96,255,.9)",
  axeBody: "rgba(177,96,255,.95)",
  axeEdge: "rgba(37,240,255,.95)",
  railTrailBase: "rgba(154,245,255,", // alpha is appended by the renderer
  rangedAim: "rgba(255,132,150,.782)",
  rangedAimHot: "rgba(255,205,214,.943)",
});

export const VISUAL_TOKENS = Object.freeze({
  glow: GLOW,
  opacity: OPACITY,
  canvas: CANVAS,
});
