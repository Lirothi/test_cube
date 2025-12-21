import { PLAYER_CONFIG, XP_CONFIG, SPAWN_CONFIG, ELITE_CONFIG } from "./config.js";
import { clamp } from "./math.js";

export const WORLD = { halfSize: 20000, spawnPad: 80, despawnPad: 240 };

export const BASE_STATS = { hp: 120, speed: 230, pickup: 130 };

export const player = {
  x: 0,
  y: 0,
  r: PLAYER_CONFIG.radius,
  hp: BASE_STATS.hp,
  maxHp: BASE_STATS.hp,
  speed: BASE_STATS.speed,
  pickup: BASE_STATS.pickup,
  armor: 0,
  resists: { all: 0, fire: 0, poison: 0, void: 0 },
  iFrame: 0,
  level: 1,
  xp: 0,
  xpNeed: XP_CONFIG.baseNeed,
  kills: 0,
  time: 0,
};

export const buffs = {
  magnet: 0,
  shield: 0,
  slow: 0,
  power: 0,
  haste: 0,
  xp: 0,
};

export const trinkets = [];
export const trinketBonuses = {
  dmgMult: 1,
  cdMult: 1,
  xpMult: 1,
  critChance: 0,
  critMult: 0,
  reviveCharges: 0,
  secondChanceOffered: false,
};

export const quest = {
  active: false,
  completed: false,
  type: "",
  target: 0,
  progress: 0,
  timer: 0,
  duration: 0,
  dropChance: 0,
  giverX: 0,
  giverY: 0,
  giverR: 16,
  giverActive: false,
  spawnT: 0,
  touchCd: 0,
  cooldown: 0,
  lastHp: BASE_STATS.hp,
};

export const questItems = [];

export function updateBuffs(dt) {
  buffs.magnet = Math.max(0, buffs.magnet - dt);
  buffs.shield = Math.max(0, buffs.shield - dt);
  buffs.slow = Math.max(0, buffs.slow - dt);
  buffs.power = Math.max(0, buffs.power - dt);
  buffs.haste = Math.max(0, buffs.haste - dt);
  buffs.xp = Math.max(0, buffs.xp - dt);
}

export const input = { up: false, down: false, left: false, right: false };

export const enemies = [];
export const bullets = [];
export const missiles = [];
export const rails = [];
export const axes = [];
export const orbs = [];
export const enemyShots = [];
export const telegraphs = [];
export const voidZones = [];
export const gems = [];
export const particles = [];
export const chests = [];
export const dmgTexts = [];
export const floatTexts = [];
export const obstacles = [];
export const activeObstacles = [];

export const spawn = {
  acc: 0,
  baseRate: SPAWN_CONFIG.baseRate,
  timeScale: SPAWN_CONFIG.timeScale,
  maxEnemies: SPAWN_CONFIG.maxEnemies,
  squadT: SPAWN_CONFIG.squadInterval,
  eliteT: ELITE_CONFIG.interval,
  bossSpawned: false,
  boss2Spawned: false,
  boss3Spawned: false,
  boss4Spawned: false,
  boss5Spawned: false,
  parallaxDefeated: false,
  bossPairT: 0,
  bossPairInterval: 180,
  bossAlive: false,
  bossRef: null,
};

export function clampPointToWorld(x, y, margin = 0) {
  const lim = Math.max(0, WORLD.halfSize - margin);
  return { x: clamp(x, -lim, lim), y: clamp(y, -lim, lim) };
}

export function clampEntityToWorld(entity, margin = 0) {
  const p = clampPointToWorld(entity.x, entity.y, margin);
  entity.x = p.x;
  entity.y = p.y;
}
