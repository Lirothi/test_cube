import { COLORS } from "./colors.js";

export const BUILD = "Neon Survivors v0.99";
export { COLORS };

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
  curvePower: 1.17,
  curveScale: 1.0,
  lateStart: 45,
  latePower: 1.3,
  lateScale: 1.1,
  buffMultiplier: 1.7,
  cardChoices: 3,
};

export const DOT_CONFIG = {
  burnTick: 0.2,
  bleedTick: 0.2,
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
  armorGain: 2,
  resAllGain: 0.03,
  resFireGain: 0.05,
  resPoisonGain: 0.05,
  resVoidGain: 0.05,
  cdReduction: 0.06,
  xpMultGain: 0.06,
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
  timeScale: 0.009,
  maxEnemies: 165,
  squadInterval: 8,
  squadStart: 20,
  squadIntervalDrop: 0.01,
  squadIntervalMin: 6.5,
  squadIntervalMax: 14,
  scaling: { hp: 0.0005, speed: 0.0005, dmg: 0.0005 },
  ranged: { capBase: 10, capScaleTime: 140, chance: 0.05 },
  voids: { capBase: 6, capScaleTime: 130, chance: 0.06, fireBias: 0.55, voidBias: 0.2 },
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

export const ENEMY_TIERS = [
  { time: 0,   label: "Tier 1",  hpMult: 1.0,  dmgMult: 1.0,  speedMult: 1.0,  spawnRateMult: 1.0,  maxEnemies: 140, rangedChanceMult: 1.0,  voidChanceMult: 1.0,  xpMult: 1.0 },
  { time: 120, label: "Tier 2",  hpMult: 1.1,  dmgMult: 1.06, speedMult: 1.02, spawnRateMult: 1.08, maxEnemies: 150, rangedChanceMult: 1.04, voidChanceMult: 1.04, xpMult: 1.05 },
  { time: 240, label: "Tier 3",  hpMult: 1.2,  dmgMult: 1.12, speedMult: 1.04, spawnRateMult: 1.16, maxEnemies: 160, rangedChanceMult: 1.08, voidChanceMult: 1.08, xpMult: 1.1 },
  { time: 360, label: "Tier 4",  hpMult: 1.32, dmgMult: 1.2,  speedMult: 1.06, spawnRateMult: 1.25, maxEnemies: 170, rangedChanceMult: 1.12, voidChanceMult: 1.12, xpMult: 1.15 },
  { time: 480, label: "Tier 5",  hpMult: 1.45, dmgMult: 1.28, speedMult: 1.08, spawnRateMult: 1.34, maxEnemies: 180, rangedChanceMult: 1.16, voidChanceMult: 1.16, xpMult: 1.2 },
  { time: 600, label: "Tier 6",  hpMult: 1.6,  dmgMult: 1.36, speedMult: 1.1,  spawnRateMult: 1.44, maxEnemies: 190, rangedChanceMult: 1.2,  voidChanceMult: 1.2,  xpMult: 1.25 },
  { time: 720, label: "Tier 7",  hpMult: 1.76, dmgMult: 1.45, speedMult: 1.12, spawnRateMult: 1.55, maxEnemies: 200, rangedChanceMult: 1.24, voidChanceMult: 1.24, xpMult: 1.3 },
  { time: 840, label: "Tier 8",  hpMult: 1.94, dmgMult: 1.55, speedMult: 1.14, spawnRateMult: 1.66, maxEnemies: 210, rangedChanceMult: 1.28, voidChanceMult: 1.28, xpMult: 1.35 },
  { time: 960, label: "Tier 9",  hpMult: 2.14, dmgMult: 1.66, speedMult: 1.16, spawnRateMult: 1.78, maxEnemies: 220, rangedChanceMult: 1.32, voidChanceMult: 1.32, xpMult: 1.4 },
  { time: 1080, label: "Tier 10", hpMult: 2.36, dmgMult: 1.78, speedMult: 1.18, spawnRateMult: 1.9,  maxEnemies: 230, rangedChanceMult: 1.36, voidChanceMult: 1.36, xpMult: 1.45 },
  { time: 1200, label: "Tier 11", hpMult: 2.6,  dmgMult: 1.9,  speedMult: 1.2,  spawnRateMult: 2.02, maxEnemies: 240, rangedChanceMult: 1.4,  voidChanceMult: 1.4,  xpMult: 1.5 },
  { time: 1320, label: "Tier 12", hpMult: 2.86, dmgMult: 2.03, speedMult: 1.22, spawnRateMult: 2.15, maxEnemies: 250, rangedChanceMult: 1.44, voidChanceMult: 1.44, xpMult: 1.56 },
  { time: 1440, label: "Tier 13", hpMult: 3.14, dmgMult: 2.16, speedMult: 1.24, spawnRateMult: 2.28, maxEnemies: 260, rangedChanceMult: 1.48, voidChanceMult: 1.48, xpMult: 1.62 },
  { time: 1560, label: "Tier 14", hpMult: 3.45, dmgMult: 2.3,  speedMult: 1.26, spawnRateMult: 2.42, maxEnemies: 270, rangedChanceMult: 1.52, voidChanceMult: 1.52, xpMult: 1.68 },
  { time: 1680, label: "Tier 15", hpMult: 3.8,  dmgMult: 2.45, speedMult: 1.28, spawnRateMult: 2.56, maxEnemies: 280, rangedChanceMult: 1.56, voidChanceMult: 1.56, xpMult: 1.75 },
];

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
  //bomb: { radius: 340, dmgBase: 45, dmgPerLevel: 4, telegraphTime: 0.65, particles: 58, particleSpread: 600 },
  shockwave: { baseKnock: 920, dmgBase: 45, dmgPerLevel: 4, damageRadius: 340, knockPush: 360 },
  bonuses: { healPct: 0.45, magnet: 15, shield: 10, freeze: 10, xp: 12, power: 8, haste: 10 },
};

export const TRINKET_CONFIG = {
  slots: 5,
  choices: 2,
  chest: { timerStart: 80, timerMin: 130, timerMax: 200, activeMax: 1 },
};

export const AUGMENT_CONFIG = {
  choices: 2,
  chest: { timerStart: 60, timerMin: 105, timerMax: 145, activeMax: 1 },
  elitePack: { countMin: 5, countMax: 7, radiusMin: 80, radiusMax: 180 },
  magic: {
    slow: { mult: 0.65, duration: 1.3 },
    prism: { every: 4, dmgMult: 0.6, angle: 0.32 },
  },
  rail: {
    burn: { dpsPct: 0.50, duration: 2.2 },
    overpen: { pierce: 2, dmgMult: 1.18, cdMult: 0.95 },
  },
  axe: {
    bleed: { dpsPct: 0.35, duration: 3.0 },
    boomerang: { dmgMult: 0.6, returnLifeMult: 0.7 },
  },
  aura: {
    pulse: { cd: 2.5, dmgMult: 1.8, knockMult: 2.2 },
    leechPct: 0.005,
  },
  orb: {
    eventHorizon: { park: 0.7, pullMult: 1.3, tickDmgMult: 1.3 },
    darkBurst: { dmgMult: 1.2, radiusMult: 1.3, park: -0.4, pullMult: 0.6 },
  },
  missile: {
    swarm: { extra: 1, dmgMult: 0.8 },
    concussive: { slowMult: 0.55, slowDuration: 0.7, knock: 260, burn: { dpsPct: 0.30, duration: 2.0 } },
  },
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
    dmgBase: 6,
    dmgPerLevel: 4,
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
    cdBase: 4.0,
    cdPerLevel: 0.6,
    cdMin: 1.5,
    dmgBase: 30,
    dmgPerLevel: 10,
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
    dmgBase: 8,
    dmgPerLevel: 6,
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
    explosionMult: 1.5,
    powerDmgMult: 1.25,
    powerCdMult: 0.9,
    crit: { base: 0.1, perLevel: 0.015, multBase: 1.6, multPerLevel: 0.05 },
    maxLevel: 6,
  },
  missile: {
    dmgBase: 15,
    dmgPerLevel: 6,
    cdBase: 2.6,
    cdPerLevel: 0.18,
    cdMin: 0.9,
    speedBase: 320,
    speedPerLevel: 10,
    accel: 380,
    maxSpeedMult: 1.4,
    turnRateDeg: 160, // base turn; improves with level in code
    life: 2.8,
    countInterval: 2,
    explosionRadiusBase: 46,
    explosionRadiusPerLevel: 4,
    rangeBase: 900,
    rangePerLevel: 20,
    crit: { base: 0.10, perLevel: 0.015, multBase: 1.6, multPerLevel: 0.05 },
    projectile: { radius: 6 },
    maxLevel: 8,
  },
};

export const WEAPON_MASTERY = {
  dmgMult: 0.05,       // +5% damage per mastery level
  critChance: 0.01,    // +1% crit chance per mastery level
  critMult: 0.05,      // +0.05 crit multiplier per mastery level
};

export const MAX_WEAPONS = 4;

export const DPS_LABELS = {
  magic: "Magic Bullet",
  aura: "Holy Aura",
  rail: "Railgun",
  axe: "Axe Throw",
  orb: "Singularity Orb",
  missile: "Homing Missiles",
};

export const WEAPON_RIDERS = {};

export const ENEMY_BEHAVIOR = {
  knockbackDecayBase: 0.02,
  rangedPreferredRange: 520,
  preferredClose: 0.62,
  preferredFar: 1.18,
  fleeMult: 0.95,
  creepMult: 0.45,
  strafeMult: 0.35,
  spitAimOffsetMin: 0.25,
  spitAimOffsetMax: 0.45,
};

export const RANGED_SHOT_CONFIG = {
  startDelayMax: 1.2,
  startDelayMin: 0.35,
  defaultCd: 1.4,
  telegraphRadius: 26,
  telegraphTime: 0.32,
  defaultSpeed: 360,
  defaultDmg: 8,
  radius: 4,
  life: 2.25,
  hitPad: 3,
  color: "rgba(255,59,102,.95)",
};

export const OBSTACLE_CONFIG = {
  lakes: { count: 540, sizeMin: 60, sizeMax: 140, blobs: 4 },
  forests: { count: 2420, sizeMin: 20, sizeMax: 34, hp: 50 },
  rocks: { count: 1560, sizeMin: 34, sizeMax: 62, hp: 420 },
  //lakes: { count: 0, sizeMin: 60, sizeMax: 140, blobs: 4 },
  //forests: { count: 0, sizeMin: 20, sizeMax: 34, hp: 50 },
  //rocks: { count: 0, sizeMin: 34, sizeMax: 62, hp: 420 },
  spawnRadius: 26000,
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
  interval: 10,
  hpMult: 10,
  speedMult: 1.15,
  dmgMult: 2,
  knockResist: 0.45,
  extraGems: 4,
  telegraphColor: "#ffd94a",
  telegraphRadius: 45,
  telegraphTime: 0.7,
  markerColor: "#ffd94a",
};

export const BOSS_CONFIG = {
  spawnTime: 180,
  telegraph: { radius: 120, time: 1.4, color: COLORS.bossTelegraph },
  nova: { cd: 4, shots: 18, shotSpeed: 520, shotDmg: 12, radius: 140, telegraph: 1.05 },
  lootGems: 16,
};

export const BOSS2_CONFIG = {
  spawnTime: 360,
  //spawnTime: 2,
  telegraph: { radius: 140, time: 1.5, color: "#63c7ff" },
  voidAttack: { cd: 5, count: 3, radius: 120, duration: 3, dps: 16, tick: 0.35, color: COLORS.voidPoison, telegraph: 0.85 },
  barrage: { cd: 3.8, shots: 12, waves: 2, waveDelay: 0.18, speed: 540, dmg: 12 },
  lootGems: 18,
};

export const BOSS3_CONFIG = {
  spawnTime: 540,
  //spawnTime: 5,
  telegraph: { radius: 150, time: 1.5, color: "#ff7b3b" },
  slam: { cd: 4.5, radius: 150, dmg: 20, telegraph: 1.2, color: "#ff9a3c" },
  rockfall: { cd: 6, count: 4, radius: 90, dmg: 15, telegraph: 1.2, offsetMin: 60, offsetMax: 200, color: "#ffb46b" },
  lootGems: 20,
};

export const BOSS4_CONFIG = {
  spawnTime: 720,
  telegraph: { radius: 170, time: 1.6, color: "#7cf6ff" },
  homingMissiles: { cd: 4.5, count: 4, speed: 320, dmg: 21, life: 3.2, turnRateDeg: 100, telegraphRadius: 80, telegraphTime: 0.8 },
  minefield: { cd: 4, count: 5, radius: 85, dmg: 22, telegraph: 1.0, offsetMin: 80, offsetMax: 220, color: "#7cf6ff" },
  lootGems: 24,
};

export const BOSS5_CONFIG = {
  spawnTime: 900,
  //spawnTime: 2,
  telegraph: { radius: 190, time: 1.7, color: "#b67bff" },
  blink: { cd: 4.6, radius: 140, dmg: 26, telegraph: 0.9, rangeMin: 120, rangeMax: 260, color: "#b67bff" },
  rift: { cd: 5.4, count: 4, radius: 110, duration: 3.6, dps: 15, tick: 0.35, pull: 260, telegraph: 0.85, offsetMin: 80, offsetMax: 210, color: COLORS.voidDark },
  split: { cd: 2.4, speed: 260, dmg: 18, life: 2.4, splitAfter: 0.9, splitCount: 5, splitSpread: 0.32, telegraph: 0.7, color: "#b67bff" },
  lootGems: 28,
};

// Base stats (HP scaling is handled in spawnController)
export const ENEMY_TYPES = {
  A: { name:"Basic",  r:10, hp:22, speed:70,  dmg:6.0,  color:COLORS.enemyA, xp:1, gem:1 },
  B: { name:"Fast",   r: 8, hp:16, speed:105, dmg:4.6,  color:COLORS.enemyB, xp:1, gem:1 },
  C: { name:"Tank",   r:16, hp:74, speed:50,  dmg:8.0,  color:COLORS.enemyC, xp:3, gem:2 },
  P: { // poison spitter
    name:"Toxic", r:10, hp:28, speed:62, dmg:6.5, color:COLORS.enemyP, xp:2, gem:2,
    ranged:true,
    spit:{ cd:3.6, range:520, radius:68, duration:4.8, dps:9.0, tick:0.35, telegraph:0.75, color:COLORS.voidPoison, type:"poison" }
  },
  F: { // fire spitter
    name:"Scorcher", r:11, hp:32, speed:60, dmg:7.5, color:COLORS.enemyF, xp:2, gem:2,
    ranged:true,
    spit:{ cd:4.2, range:540, radius:74, duration:4.6, dps:11.5, tick:0.3, telegraph:0.75, color:COLORS.voidFire, type:"fire" }
  },
  V: { // void spitter
    name:"Voidcaller", r:10, hp:30, speed:58, dmg:6.8, color:COLORS.enemyV, xp:2, gem:2,
    ranged:true,
    spit:{ cd:4.1, range:560, radius:72, duration:4.6, dps:10.5, tick:0.35, telegraph:0.75, color:COLORS.voidDark, type:"void" }
  },
  X: { // mini-boss
    name:"Overseer", r:20, hp:2000, speed:55, dmg:12, color:COLORS.enemyBoss, xp:8, gem:10,
    boss:true, knockResist:0.55,
    ranged:true, shotCd:1.6, shotDmg:9, shotSpeed:420, shotRange:720,
    nova: BOSS_CONFIG.nova,
    lootGems: BOSS_CONFIG.lootGems,
  },
  Y: { // late void boss
    name:"Eclipse", r:22, hp:3800, speed:58, dmg:14, color:"#9df2ff", xp:10, gem:14,
    boss:true, knockResist:0.85,
    ranged:true, shotCd:1.35, shotDmg:10, shotSpeed:480, shotRange:720,
    telegraph: BOSS2_CONFIG.telegraph,
    voidAttack: BOSS2_CONFIG.voidAttack,
    barrage: BOSS2_CONFIG.barrage,
    lootGems: BOSS2_CONFIG.lootGems,
  },
  Z: { // melee juggernaut
    name:"Titan", r:24, hp:7200, speed:62, dmg:18, color:"#ff7b3b", xp:12, gem:16,
    boss:true, knockResist:0.7,
    ranged:false, shotCd:0, shotDmg:0, shotSpeed:0, shotRange:0,
    slam: BOSS3_CONFIG.slam,
    rockfall: BOSS3_CONFIG.rockfall,
    telegraph: BOSS3_CONFIG.telegraph,
    lootGems: BOSS3_CONFIG.lootGems,
  },
  W: { // ranged sentinel
    name:"Archon", r:22, hp:12200, speed:75, dmg:16, color:"#7cf6ff", xp:14, gem:18,
    boss:true, knockResist:0.85,
    ranged:true, shotCd:1.25, shotDmg:24, shotSpeed:520, shotRange:720,
    telegraph: BOSS4_CONFIG.telegraph,
    homingMissiles: BOSS4_CONFIG.homingMissiles,
    minefield: BOSS4_CONFIG.minefield,
    lootGems: BOSS4_CONFIG.lootGems,
  },
  Q: { // phase boss
    name:"Parallax", r:24, hp:16500, speed:70, dmg:18, color:COLORS.enemyQ, xp:16, gem:20,
    boss:true, knockResist:0.9,
    telegraph: BOSS5_CONFIG.telegraph,
    blink: BOSS5_CONFIG.blink,
    rift: BOSS5_CONFIG.rift,
    split: BOSS5_CONFIG.split,
    lootGems: BOSS5_CONFIG.lootGems,
  },
  S: { name:"Bulwark", r:14, hp:96, speed:58, dmg:9.5, color:COLORS.enemyS, xp:3, gem:2, knockResist:0.35 },
  R: { // ranged kiter (tries to keep distance and shoot)
    name:"Ranged", r:9, hp:16, speed:60, dmg:3.2, color:COLORS.enemyR, xp:2, gem:1,
    ranged:true, shotCd:2.35, shotDmg:6.0, shotSpeed:340, shotRange:520
  },
};
