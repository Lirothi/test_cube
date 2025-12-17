export const BUILD = "Neon Survivors v1.3 (melee fix + ranged nerf + stronger magnet)";

export const COLORS = {
  bg: "#05060a",
  grid: "rgba(40, 240, 255, 0.06)",
  player: "#25f0ff",
  bullet: "#b160ff",
  rail: "#9af5ff",
  aura: "rgba(70, 255, 143, 0.24)",
  auraStroke: "rgba(70, 255, 143, 0.60)",
  gem: "#ffd94a",
  gold: "#ffd94a",
  crit: "#ffeb3b",
  chest: "#46ff8f",
  enemyA: "#ff3b66",
  enemyB: "#ff6eff",
  enemyC: "#7a6bff",
  enemyBoss: "#ff9df2",
  bossTelegraph: "#ffe26a",
  enemyP: "#40a357ff",
  enemyF: "#ff9347",
  enemyS: "#3a2acfff",
  enemyR: "#25f0ff",
  voidPoison: "rgba(115,255,148,0.38)",
  voidFire: "rgba(255,147,71,0.38)",
  dmg: "rgba(215,246,255,.95)",
  heal: "#46ff8f",
  warn: "#ff3b66",
  text: "#d7f6ff",
};

export const PLAYER_CONFIG = {
  radius: 12,
  hasteMoveMult: 1.25,
  hitPush: 12,
  meleeIFrame: 0.25,
  shotIFrame: 0.22,
  shieldPushback: 120,
};

export const XP_CONFIG = {
  baseNeed: 7,
  perLevel: 6,
  curvePower: 1.15,
  curveScale: 1.0,
  buffMultiplier: 1.7,
  cardChoices: 3,
};

export const BUFF_EFFECTS = {
  slowMoveMult: 0.58,
  slowFireMult: 1.25,
  magnetRadiusMult: 5.0,
  magnetPullBase: 1500,
  magnetPullPowered: 4000,
  hasteMoveMult: 1.25,
};

export const UPGRADE_CONFIG = {
  speedMult: 1.10,
  hpBaseGain: 22,
  hpPerLevelGain: 5,
  hpHealPct: 0.60,
  pickupGain: 30,
  weightNewWeapon: 2.8,
  weightPickup: 1.35,
  passiveMaxLevel: 5,
};

export const CRIT_UPGRADES = {
  chancePerLevel: 0.06,
  multPerLevel: 0.14,
  maxLevels: 4,
};

export const SPAWN_CONFIG = {
  baseRate: 0.55,
  timeScale: 0.018,
  maxEnemies: 165,
  squadInterval: 8,
  squadStart: 20,
  squadIntervalDrop: 0.01,
  squadIntervalMin: 6.5,
  squadIntervalMax: 14,
  scaling: { hp: 0.004, speed: 0.001, dmg: 0.003 },
  ranged: { capBase: 10, capScaleTime: 140, chance: 0.05 },
  voids: { capBase: 6, capScaleTime: 130, chance: 0.06, fireBias: 0.55 },
  rolls: {
    fast: 0.18,
    tank: 0.86,
    brute: 0.60,       // chance to upgrade late tanks into Bulwarks
    void: 0.70,        // convert some rolls into void spitters after threshold
    lateMix: 0.76,
    lateTank: 0.50,
    lateRanged: 0.62,
    lateBrute: 0.80,
    lateVoid: 0.78,
  },
  thresholds: { fast: 25, ranged: 40, tank: 55, brute: 70, void: 65, lateMix: 110 },
  mixedPoolTimes: { extraFast: 25, ranged: 35, tank: 50, brute: 70, void: 65 },
  mixedCount: { base: 3, scaleTime: 90, max: 6 },
  mixedStatMult: { hp: 1.08, speed: 1.02, dmg: 1.06 },
  squadReserve: 4,
};

export const TELEGRAPH_CONFIG = {
  radius: 40,
  time: 0.4,
  enemyRadius: 26,
  enemyTime: 0.32,
};

export const CHEST_CONFIG = {
  timerStart: 9,
  timerMin: 14,
  timerMax: 22,
  activeMax: 2,
  spawnTries: 12,
  spawnOffset: 0.42,
  spawnMinDist: 140,
  radiusPadding: 8,
  indicatorMargin: 34,
  indicatorSize: 13,
  pulseSpeed: 2.8,
  healBias: { hpPct: 0.35, chance: 0.55 },
  openParticles: { count: 22, spread: 420 },
  bomb: { radius: 340, dmgBase: 35, dmgPerLevel: 3, telegraphTime: 0.65, particles: 58, particleSpread: 600 },
  shockwave: { baseKnock: 820, dmgBase: 15, dmgPerLevel: 2, damageRadius: 240, knockPush: 260 },
  bonuses: { healPct: 0.45, magnet: 15, shield: 10, freeze: 10, xp: 12, power: 8, haste: 10 },
};

export const WEAPON_CONFIG = {
  magic: {
    dmgBase: 9,
    dmgPerLevel: 3.6,
    cdBase: 0.72,
    cdPerLevel: 0.08,
    cdMin: 0.26,
    speedBase: 580,
    speedPerLevel: 18,
    countInterval: 3,
    range: 780,
    knockBase: 120,
    knockPerLevel: 18,
    powerDmgMult: 1.25,
    powerCdMult: 0.82,
    crit: { base: 0.12, perLevel: 0.01, multBase: 1.7, multPerLevel: 0.05 },
    projectile: { radius: 3.3, life: 1.25, spread: 0.12 },
    maxLevel: 8,
  },
  aura: {
    radiusBase: 56,
    radiusPerLevel: 14,
    tick: 0.22,
    dmgBase: 5,
    dmgPerLevel: 3.4,
    knockBase: 70,
    knockPerLevel: 10,
    powerDmgMult: 1.22,
    crit: { base: 0.08, perLevel: 0.012, multBase: 1.55, multPerLevel: 0.04 },
    maxLevel: 6,
  },
  axe: {
    cdBase: 1.40,
    cdPerLevel: 0.14,
    cdMin: 0.50,
    dmgBase: 18,
    dmgPerLevel: 8,
    speedBase: 440,
    speedPerLevel: 20,
    countInterval: 2,
    gravity: 920,
    knockBase: 160,
    knockPerLevel: 22,
    powerDmgMult: 1.22,
    powerCdMult: 0.84,
    crit: { base: 0.14, perLevel: 0.015, multBase: 1.9, multPerLevel: 0.06 },
    maxLevel: 6,
    throw: {
      range: 920,
      radius: 7.0,
      life: 1.35,
      spinMin: 8,
      spinMax: 18,
      angleJitter: 0.14,
      speedJitterMin: 0.92,
      speedJitterMax: 1.06,
      launchVyMin: 220,
      launchVyMax: 320,
      hitLifeLoss: 0.32,
      spinInvertChance: 0.5,
    },
  },
  rail: {
    cdBase: 4.4,
    cdPerLevel: 0.6,
    cdMin: 1.5,
    dmgBase: 30,
    dmgPerLevel: 18,
    speedBase: 1160,
    speedPerLevel: 60,
    pierceBase: 3,
    pierceLevelDivisor: 1,
    powerPierceBonus: 2,
    rangeBase: 1480,
    rangePerLevel: 90,
    knockBase: 240,
    knockPerLevel: 28,
    powerDmgMult: 1.25,
    powerCdMult: 0.82,
    crit: { base: 0.18, perLevel: 0.02, multBase: 2, multPerLevel: 0.08 },
    projectile: { radius: 5.0, trailLife: 0.34, trailMax: 12 },
    maxLevel: 6,
  },
  orb: {
    dmgBase: 16,
    dmgPerLevel: 8,
    cdBase: 3.6,
    cdPerLevel: 0.26,
    cdMin: 1.7,
    speedBase: 230,
    speedPerLevel: -15,
    range: 520,
    pullRadiusBase: 110,
    pullRadiusPerLevel: 20,
    pullBase: 500,
    pullPerLevel: 30,
    tick: 0.45,
    parkTimeBase: 2.2,
    parkTimePerLevel: 0.25,
    explosionMult: 2.0,
    powerDmgMult: 1.25,
    powerCdMult: 0.9,
    crit: { base: 0.1, perLevel: 0.015, multBase: 1.6, multPerLevel: 0.05 },
    maxLevel: 6,
  },
};

export const MAX_WEAPONS = 4;

export const DPS_LABELS = { magic:"Magic", aura:"Aura", rail:"Railgun", axe:"Axe", orb:"Orb" };

export const WEAPON_RIDERS = {
  magic: { slow: { mult: 0.65, duration: 1.3 } },             // slows enemies on hit
  rail:  { burn: { dpsPct: 0.50, duration: 2.2 } },            // burns enemies for % of hit dmg per second
  axe:   { bleed:{ dpsPct: 0.35, duration: 3.0 } },            // bleeds enemies for % of hit dmg per second
};

export const ENEMY_BEHAVIOR = {
  knockbackDecayBase: 0.02,
  rangedPreferredRange: 520,
  preferredClose: 0.62,
  preferredFar: 1.18,
  fleeMult: 0.95,
  creepMult: 0.45,
  strafeMult: 0.35,
};

export const RANGED_SHOT_CONFIG = {
  startDelayMax: 1.2,
  startDelayMin: 0.35,
  defaultCd: 1.4,
  telegraphRadius: 26,
  telegraphTime: 0.32,
  defaultSpeed: 360,
  defaultDmg: 8,
  radius: 3.7,
  life: 2.25,
  hitPad: 3,
  color: "rgba(255,59,102,.95)",
};

export const LOOT_CONFIG = {
  dropSpeedMin: 36,
  dropSpeedMax: 110,
  gemRadiusBase: 4.8,
  gemRadiusScale: 1.2,
  pickupPadding: 4,
  frictionBase: 0.001,
  dropJitter: 10,
};

export const LOOP_CONFIG = {
  maxDt: 0.033,
};

export const ELITE_CONFIG = {
  interval: 15,
  hpMult: 10,
  dmgMult: 2,
  knockResist: 0.45,
  extraGems: 4,
  telegraphColor: "#ffd94a",
  telegraphRadius: 38,
  telegraphTime: 0.7,
  markerColor: "#ffd94a",
};

export const BOSS_CONFIG = {
  spawnTime: 180,
  telegraph: { radius: 120, time: 1.4, color: COLORS.bossTelegraph },
  nova: { cd: 4, shots: 20, shotSpeed: 520, shotDmg: 12, radius: 140, telegraph: 1.05 },
  lootGems: 16,
};

// Base stats (HP scaling is handled in spawnController)
export const ENEMY_TYPES = {
  A: { name:"Basic",  r:10, hp:22, speed:70,  dmg:6.0,  color:COLORS.enemyA, xp:1, gem:1 },
  B: { name:"Fast",   r: 8, hp:16, speed:105, dmg:4.6,  color:COLORS.enemyB, xp:1, gem:1 },
  C: { name:"Tank",   r:16, hp:74, speed:50,  dmg:8.0,  color:COLORS.enemyC, xp:3, gem:2 },
  P: { // poison spitter
    name:"Toxic", r:10, hp:28, speed:62, dmg:6.5, color:COLORS.enemyP, xp:2, gem:1,
    ranged:true,
    spit:{ cd:3.6, range:520, radius:68, duration:4.8, dps:9.0, telegraph:0.5, color:COLORS.voidPoison, type:"poison" }
  },
  F: { // fire spitter
    name:"Scorcher", r:11, hp:32, speed:60, dmg:7.5, color:COLORS.enemyF, xp:2, gem:1,
    ranged:true,
    spit:{ cd:4.2, range:540, radius:74, duration:4.6, dps:11.5, telegraph:0.55, color:COLORS.voidFire, type:"fire" }
  },
  X: { // mini-boss
    name:"Overseer", r:20, hp:2000, speed:75, dmg:12, color:COLORS.enemyBoss, xp:8, gem:10,
    boss:true, knockResist:0.55,
    ranged:true, shotCd:1.6, shotDmg:9, shotSpeed:420, shotRange:720,
    nova: BOSS_CONFIG.nova
  },
  S: { name:"Bulwark", r:14, hp:96, speed:58, dmg:9.5, color:COLORS.enemyS, xp:3, gem:2, knockResist:0.35 },
  R: { // ranged kiter (tries to keep distance and shoot)
    name:"Ranged", r:9, hp:16, speed:60, dmg:3.2, color:COLORS.enemyR, xp:2, gem:1,
    ranged:true, shotCd:2.35, shotDmg:6.0, shotSpeed:340, shotRange:520
  },
};
