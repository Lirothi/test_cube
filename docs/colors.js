const WORLD = Object.freeze({
  background: "#05060a",
  grid: "rgba(40, 240, 255, 0.045)",
  gridMinor: "rgba(40, 240, 255, 0.025)",
  gridMajor: "rgba(40, 240, 255, 0.065)",
  accent: "rgba(177, 96, 255, 0.055)",
  boundary: "rgba(127, 231, 255, 0.82)",
});

const FRIENDLY = Object.freeze({
  player: "#25f0ff",
  bullet: "#b160ff",
  rail: "#9ac2ff",
  arc: "#7cf6ff",
  missile: "#ff9a3c",
  missileStroke: "rgba(0,0,0,0.3)",
  turret: "#58f7ff",
  turretFire: "#ff9a3c",
  turretPoison: "#a6ff48",
  auraFill: "rgba(70, 255, 143, 0.24)",
  auraStroke: "rgba(70, 255, 143, 0.60)",
  playerGlow: "rgba(255,255,255,.8)",
  playerCore: "rgba(255,255,255,.16)",
});

const ENEMIES = Object.freeze({
  basic: "#ff3b66",
  fast: "#ff6eff",
  tank: "#7161ff",
  boss: "#ff9df2",
  toxic: "#8fd14f",
  scorcher: "#ff9347",
  voidcaller: "#7b5cff",
  parallax: "#b67bff",
  bulwark: "#5142e0",
  ranged: "#ff5cc8",
  mage: "#6f8cff",
});

const DANGER = Object.freeze({
  warning: "#ff3b66",
  bossTelegraph: "#ff3b66",
  hit: "rgba(255,59,102,.95)",
  hitDim: "rgba(255,59,102,.75)",
  poisonZone: "rgba(166, 255, 72, 0.49)",
  fireZone: "rgba(252, 118, 23, 0.53)",
  voidZone: "rgba(123, 92, 255, 0.45)",
});

const REWARDS = Object.freeze({
  gem: "#2d78d3",
  gold: "#ffd94a",
  critical: "#ffeb3b",
  chest: "#46ff8f",
  trinket: "#7cffd9",
  augment: "#8d7bff",
  quest: "#f2a024",
});

const DEFENSE = Object.freeze({
  heal: "#46ff8f",
  shield: "rgba(127,231,255,.85)",
  shieldRing: "rgba(127,231,255,.75)",
  magnetRing: "rgba(255,217,74,.28)",
});

const ENVIRONMENT = Object.freeze({
  lake: "#0b1b3d",
  lakeEdge: "#040b18",
  lakeOutline: "#1d638c",
  lakeDeep: "#071329",
  lakeShallow: "#10315a",
  lakeRipple: "rgba(99, 199, 255, 0.20)",
  lakeHighlight: "rgba(154, 245, 255, 0.18)",
  forest: "#2c7b34",
  forestGround: "#102816",
  forestTrunk: "#102014",
  forestShade: "rgba(4, 12, 7, 0.46)",
  rock: "#8a8d94",
  rockShadow: "rgba(0, 0, 0, 0.34)",
  rockFacet: "rgba(220, 226, 236, 0.18)",
  obstacleDamage: "rgba(5, 6, 10, 0.78)",
});

const UI = Object.freeze({
  text: "#d7f6ff",
  damageText: "rgba(215,246,255,.95)",
  overlayDim: "rgba(0,0,0,.25)",
});

const COMPANIONS = Object.freeze({
  igorek: "#7fe7ff",
  lumen: "#7cffd9",
  volt: "#ffd94a",
  byte: "#b160ff",
  aegis: "#46ff8f",
  cage: "#a1acc2",
});

export const COLOR_ROLES = Object.freeze({
  world: WORLD,
  friendly: FRIENDLY,
  enemies: ENEMIES,
  danger: DANGER,
  rewards: REWARDS,
  defense: DEFENSE,
  environment: ENVIRONMENT,
  ui: UI,
  companions: COMPANIONS,
});

// Flat aliases preserve the existing public API while COLOR_ROLES remains
// the single semantic source of truth for Canvas, gameplay modules, and UI.
export const COLORS = Object.freeze({
  bg: WORLD.background,
  grid: WORLD.grid,
  gridMinor: WORLD.gridMinor,
  gridMajor: WORLD.gridMajor,
  worldAccent: WORLD.accent,
  worldBoundary: WORLD.boundary,

  player: FRIENDLY.player,
  bullet: FRIENDLY.bullet,
  rail: FRIENDLY.rail,
  arc: FRIENDLY.arc,
  missile: FRIENDLY.missile,
  missileStroke: FRIENDLY.missileStroke,
  turret: FRIENDLY.turret,
  turretFire: FRIENDLY.turretFire,
  turretPoison: FRIENDLY.turretPoison,
  aura: FRIENDLY.auraFill,
  auraStroke: FRIENDLY.auraStroke,
  playerAura: FRIENDLY.auraFill,
  playerAuraStroke: FRIENDLY.auraStroke,
  playerGlow: FRIENDLY.playerGlow,
  playerCore: FRIENDLY.playerCore,

  enemyA: ENEMIES.basic,
  enemyB: ENEMIES.fast,
  enemyC: ENEMIES.tank,
  enemyBoss: ENEMIES.boss,
  enemyP: ENEMIES.toxic,
  enemyF: ENEMIES.scorcher,
  enemyV: ENEMIES.voidcaller,
  enemyQ: ENEMIES.parallax,
  enemyS: ENEMIES.bulwark,
  enemyR: ENEMIES.ranged,
  enemyM: ENEMIES.mage,

  bossTelegraph: DANGER.bossTelegraph,
  warn: DANGER.warning,
  warnHit: DANGER.hit,
  warnHitDim: DANGER.hitDim,
  aoePoison: DANGER.poisonZone,
  aoeFire: DANGER.fireZone,
  aoeVoid: DANGER.voidZone,

  gem: REWARDS.gem,
  gold: REWARDS.gold,
  crit: REWARDS.critical,
  chest: REWARDS.chest,
  trinket: REWARDS.trinket,
  aug: REWARDS.augment,
  quest: REWARDS.quest,

  heal: DEFENSE.heal,
  warnShield: DEFENSE.shield,
  shieldBlock: DEFENSE.shield,
  auraRingShield: DEFENSE.shieldRing,
  magnetRing: DEFENSE.magnetRing,

  lake: ENVIRONMENT.lake,
  lakeEdge: ENVIRONMENT.lakeEdge,
  lakeOutline: ENVIRONMENT.lakeOutline,
  lakeDeep: ENVIRONMENT.lakeDeep,
  lakeShallow: ENVIRONMENT.lakeShallow,
  lakeRipple: ENVIRONMENT.lakeRipple,
  lakeHighlight: ENVIRONMENT.lakeHighlight,
  forest: ENVIRONMENT.forest,
  forestGround: ENVIRONMENT.forestGround,
  forestTrunk: ENVIRONMENT.forestTrunk,
  forestShade: ENVIRONMENT.forestShade,
  rock: ENVIRONMENT.rock,
  rockShadow: ENVIRONMENT.rockShadow,
  rockFacet: ENVIRONMENT.rockFacet,
  obstacleDamage: ENVIRONMENT.obstacleDamage,

  text: UI.text,
  dmg: UI.damageText,
  overlayDim: UI.overlayDim,

  companionIgorek: COMPANIONS.igorek,
  companionLumen: COMPANIONS.lumen,
  companionVolt: COMPANIONS.volt,
  companionByte: COMPANIONS.byte,
  companionAegis: COMPANIONS.aegis,
  companionCage: COMPANIONS.cage,
});
