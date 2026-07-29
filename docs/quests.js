import { LOOT_CONFIG, COLORS, CRIT_UPGRADES } from "./config.js";
import { rand, randi, TAU, clamp, hypot } from "./math.js";
import { isBlockedByObstacle } from "./spawn.js";
import { weapons } from "./weapons.js";
import { upgradeState } from "./upgrade.js";
import { quest, questItems, player, clampPointToWorld, gems, enemies, voidZones, chests, spawn } from "./state.js";
import { gemPool } from "./pools.js";
import { popFloatText } from "./float_text.js";

const QUEST_TOUCH_CD = 0.8;
const QUEST_SPAWN_DELAY = 12;
const QUEST_RESPAWN_DELAY = 18;
const QUEST_RESPAWN_JITTER = 0.25;
const QUEST_GIVER_MIN_DIST = 1200;
const QUEST_GIVER_MAX_DIST = 2200;
const QUEST_ITEM_MIN_DIST = 900;
const QUEST_ITEM_MAX_DIST = 2400;
const QUEST_NOTICE_LIFE = 3.0;

const QUEST_HISTORY_MAX = 8;
const QUEST_NOHIT_WINDOW = 4;
const QUEST_NOHIT_CAP = 1;
const QUEST_PICK_ATTEMPTS = 40;
const QUEST_GATING_CRIT_MIN = 0.12;
const QUEST_LEVEL_SCALE = {
  glassPenalty: { base: 0.72, perLevel: 0.01, min: 0.45, max: 0.72 },
  phase: { earlyMaxExclusive: 12, midMaxExclusive: 24 },
  gatingLevel: {
    orbitalDefense: 6,
    magnetSprint: 5,
    crowdControl: 5,
    bossPrep: 10,
    glassCannon: 10,
  },
  reward: {
    countBase: 8,
    countPerLevel: 0.65,
    countMin: 6,
    valueBase: 2,
    valuePerLevel: 0.35,
    valueMin: 1,
  },
  kill: { rollMax: 34, rollMin: 22, baseMult: 1.0, perLevel: 0.14, durationBase: 75, durationPerLevel: 0.8 },
  scavenge: { rollMax: 6, rollMin: 3, perLevel: 0.07, durationBase: 90, durationPerLevel: 1.0 },
  drop: { rollMax: 9, rollMin: 4, perLevel: 0.16, durationBase: 90, durationPerLevel: 1.0 },
  nohit: { durationMax: 21, durationMin: 12, perLevel: 0.26 },
  closeQuarters: { base: 10, perLevel: 0.42, rangeMultiplier: 7.5, durationBase: 55, durationPerLevel: 0.6 },
  longShot: { base: 9, perLevel: 0.38, minRangeBase: 240, minRangePerLevel: 7, durationBase: 60, durationPerLevel: 0.7 },
  executionChain: { base: 6, perLevel: 0.16, windowBase: 3.0, windowPerLevel: 0.02, windowMin: 2.1, durationBase: 15, durationPerLevel: 0.22 },
  elementPurge: { base: 8, perLevel: 0.28, durationBase: 65, durationPerLevel: 0.8 },
  eliteHunt: { twoTargetLevel: 18, oneTarget: 1, twoTarget: 2, durationBase: 30, durationPerLevel: 0.25 },
  overkill: { base: 220, perLevel: 36, durationBase: 14, durationPerLevel: 0.18 },
  auraDiscipline: { base: 6, perLevel: 0.34, durationBase: 10, durationPerLevel: 0.2 },
  critFestival: { base: 8, perLevel: 0.24, durationBase: 18, durationPerLevel: 0.2 },
  dotTrial: { base: 90, perLevel: 22, durationBase: 18, durationPerLevel: 0.25 },
  orbitalDefense: { base: 7, perLevel: 0.22, durationBase: 16, durationPerLevel: 0.2, minSpeedMul: 0.6, requiredMoveRatio: 0.78 },
  magnetSprint: { base: 16, perLevel: 0.5, durationBase: 13, durationPerLevel: 0.15 },
  treasureRoute: { durationBase: 28, durationPerLevel: 0.12 },
  hazardSurvivor: { base: 4, perLevel: 0.08, durationBase: 20, durationPerLevel: 0.2 },
  pacifistPulse: { durationBase: 14, durationPerLevel: 0.14 },
  crowdControl: { base: 18, perLevel: 0.85, durationBase: 18, durationPerLevel: 0.2, durationMultiplier: 1.5 },
  bossPrep: { base: 180, perLevel: 30 },
  glassCannon: { base: 10, perLevel: 0.32, durationBase: 20, durationPerLevel: 0.2 },
  perfectSweep: { durationBase: 12, durationPerLevel: 0.18, radiusBase: 165, radiusPerLevel: 3, radiusMin: 165, radiusMax: 300 },
};

const QUEST_TYPES_BY_FAMILY = {
  core: ["kill", "scavenge", "drop", "nohit"],
  combat: ["close_quarters", "long_shot", "execution_chain", "element_purge", "elite_hunt", "overkill", "crit_festival", "boss_prep", "glass_cannon", "perfect_sweep"],
  utility: ["magnet_sprint", "treasure_route", "hazard_survivor", "crowd_control", "orbital_defense"],
  weapon: ["aura_discipline", "pacifist_pulse", "dot_trial"],
};

const QUEST_META = {
  kill: { family: "core", tier: 0, weight: 1.0 },
  scavenge: { family: "core", tier: 0, weight: 1.0 },
  drop: { family: "core", tier: 0, weight: 0.95 },
  nohit: { family: "core", tier: 1, weight: 0.75 },
  close_quarters: { family: "combat", tier: 1, weight: 0.95 },
  long_shot: { family: "combat", tier: 1, weight: 0.95 },
  execution_chain: { family: "combat", tier: 2, weight: 0.85 },
  element_purge: { family: "combat", tier: 1, weight: 0.85 },
  elite_hunt: { family: "combat", tier: 2, weight: 0.75 },
  overkill: { family: "combat", tier: 2, weight: 0.9 },
  crit_festival: { family: "combat", tier: 1, weight: 0.85 },
  boss_prep: { family: "combat", tier: 2, weight: 0.75 },
  glass_cannon: { family: "combat", tier: 2, weight: 0.7 },
  perfect_sweep: { family: "combat", tier: 2, weight: 0.75 },
  magnet_sprint: { family: "utility", tier: 1, weight: 1.0 },
  treasure_route: { family: "utility", tier: 0, weight: 0.85 },
  hazard_survivor: { family: "utility", tier: 1, weight: 0.85 },
  crowd_control: { family: "utility", tier: 1, weight: 0.95 },
  orbital_defense: { family: "utility", tier: 2, weight: 0.8 },
  aura_discipline: { family: "weapon", tier: 2, weight: 0.8 },
  pacifist_pulse: { family: "weapon", tier: 2, weight: 0.7 },
  dot_trial: { family: "weapon", tier: 2, weight: 0.8 },
};

let runtime = { addXP: null, openAug: null, openTrinket: null, addParticles: null };
let questTypeHistory = [];
let questFamilyHistory = [];
let completionStreak = 0;
let failureStreak = 0;
let questSerial = 1;

export function setQuestRuntime({ addXP, openAug, openTrinket, addParticles }) {
  runtime.addXP = addXP;
  runtime.openAug = openAug;
  runtime.openTrinket = openTrinket;
  runtime.addParticles = addParticles;
}

function requireRuntime() {
  if (!runtime.addXP) throw new Error("Quest runtime missing; call setQuestRuntime({ addXP, openAug, openTrinket, addParticles }) first.");
  return runtime;
}

function hasAnyChestActive() {
  for (let i = 0; i < chests.length; i++) {
    if (chests[i].alive) return true;
  }
  return false;
}

function hasDotSourceAvailable() {
  return (weapons.rail.unlocked && weapons.rail.aug === "rail_fire")
    || (weapons.axe.unlocked && weapons.axe.aug === "axe_bleed")
    || (weapons.missile.unlocked && weapons.missile.aug === "missile_concussive");
}

function aliveEnemyCount(includeBoss = false) {
  let count = 0;
  for (let i = 0; i < enemies.length; i++) {
    const e = enemies[i];
    if (!e.alive) continue;
    if (!includeBoss && e.boss) continue;
    count++;
  }
  return count;
}

function aliveEliteCount() {
  let count = 0;
  for (let i = 0; i < enemies.length; i++) {
    const e = enemies[i];
    if (e.alive && e.elite) count++;
  }
  return count;
}

function countEnemiesInZone(x, y, r) {
  let count = 0;
  const r2 = r * r;
  for (let i = 0; i < enemies.length; i++) {
    const e = enemies[i];
    if (!e.alive || e.boss) continue;
    const dx = e.x - x;
    const dy = e.y - y;
    if (dx * dx + dy * dy <= r2) count++;
  }
  return count;
}

function pickQuestGiverPos(minDist = QUEST_GIVER_MIN_DIST, maxDist = QUEST_GIVER_MAX_DIST) {
  let pos = clampPointToWorld(player.x, player.y, quest.giverR);
  for (let tries = 0; tries < 16; tries++) {
    const ang = rand(TAU, 0);
    const dist = rand(maxDist, minDist);
    const x = player.x + Math.cos(ang) * dist;
    const y = player.y + Math.sin(ang) * dist;
    const candidate = clampPointToWorld(x, y, quest.giverR);
    if (!isBlockedByObstacle(candidate.x, candidate.y, quest.giverR, 6)) {
      pos = candidate;
      break;
    }
  }
  quest.giverX = pos.x;
  quest.giverY = pos.y;
}

function scheduleQuestGiver(delay) {
  quest.giverActive = false;
  quest.spawnT = Math.max(0, delay);
}

function getRespawnDelay() {
  return QUEST_RESPAWN_DELAY * rand(1 + QUEST_RESPAWN_JITTER, 1 - QUEST_RESPAWN_JITTER);
}

function activateQuestGiver() {
  quest.giverActive = true;
  pickQuestGiverPos();
}

function spawnQuestItem(x, y, type = "scavenge") {
  const item = { alive: true, x, y, r: 12, type };
  questItems.push(item);
}

function spawnQuestItems(count, minDist = QUEST_ITEM_MIN_DIST, maxDist = QUEST_ITEM_MAX_DIST, type = "scavenge") {
  for (let i = 0; i < count; i++) {
    let pos = clampPointToWorld(player.x, player.y, 9);
    for (let tries = 0; tries < 14; tries++) {
      const ang = rand(TAU, 0);
      const dist = rand(maxDist, minDist);
      const x = player.x + Math.cos(ang) * dist;
      const y = player.y + Math.sin(ang) * dist;
      const candidate = clampPointToWorld(x, y, 9);
      if (!isBlockedByObstacle(candidate.x, candidate.y, 9, 6)) {
        pos = candidate;
        break;
      }
    }
    spawnQuestItem(pos.x, pos.y, type);
  }
}

function clearQuestItems() {
  questItems.length = 0;
}

function enemyElement(e) {
  if (!e) return "";
  if (e.type === "F" || e.type === "Z") return "fire";
  if (e.type === "P") return "poison";
  if (e.type === "V" || e.type === "Y" || e.type === "W" || e.type === "Q") return "void";
  return "";
}

function getAliveElementTargetCounts() {
  const counts = { fire: 0, poison: 0, void: 0 };
  for (let i = 0; i < enemies.length; i++) {
    const e = enemies[i];
    if (!e.alive || e.boss) continue;
    const element = enemyElement(e);
    if (element) counts[element]++;
  }
  return counts;
}

function hasAliveElementTargets() {
  const counts = getAliveElementTargetCounts();
  return counts.fire > 0 || counts.poison > 0 || counts.void > 0;
}

function elementLabel(el) {
  if (el === "fire") return "Fire";
  if (el === "poison") return "Poison";
  if (el === "void") return "Void";
  return "Element";
}

function clampTarget(v) {
  return Math.max(1, Math.floor(v));
}

function setCommonQuest(type) {
  const meta = QUEST_META[type];
  quest.active = true;
  quest.completed = false;
  quest.type = type;
  quest.family = meta.family;
  quest.tier = meta.tier;
  quest.rewardTier = meta.tier;
  quest.progress = 0;
  quest.target = 0;
  quest.timer = 0;
  quest.duration = 0;
  quest.dropChance = 0;
  quest.combo = 0;
  quest.comboT = 0;
  quest.comboWindow = 0;
  quest.element = "";
  quest.source = "";
  quest.forbidSource = "";
  quest.minSpeed = 0;
  quest.zoneX = 0;
  quest.zoneY = 0;
  quest.zoneR = 0;
  quest.zoneNeed = 0;
  quest.zoneRemain = 0;
  quest.moveTimer = 0;
  quest.moveX = player.x;
  quest.moveY = player.y;
  quest.lastHp = player.hp;
  quest.instance = questSerial++;
}

function restoreQuestPenalty() {
  if ((quest.type !== "glass_cannon" && !quest.penaltyActive) || quest.armorMul >= 1) return;
  player.armor = quest.prevArmor || 0;
  player.resists.all = quest.prevResAll || 0;
  player.resists.fire = quest.prevResFire || 0;
  player.resists.poison = quest.prevResPoison || 0;
  player.resists.void = quest.prevResVoid || 0;
  quest.armorMul = 1;
  quest.resistMul = 1;
  quest.penaltyActive = false;
}

function applyGlassPenalty(lv) {
  const s = QUEST_LEVEL_SCALE.glassPenalty;
  const strength = clamp(s.base - lv * s.perLevel, s.min, s.max);
  quest.armorMul = strength;
  quest.resistMul = strength;
  quest.prevArmor = player.armor;
  quest.prevResAll = player.resists.all || 0;
  quest.prevResFire = player.resists.fire || 0;
  quest.prevResPoison = player.resists.poison || 0;
  quest.prevResVoid = player.resists.void || 0;
  player.armor = (player.armor || 0) * quest.armorMul;
  player.resists.all = (player.resists.all || 0) * quest.resistMul;
  player.resists.fire = (player.resists.fire || 0) * quest.resistMul;
  player.resists.poison = (player.resists.poison || 0) * quest.resistMul;
  player.resists.void = (player.resists.void || 0) * quest.resistMul;
  quest.penaltyActive = true;
}

function pickSweepCenter(radius) {
  let best = { x: player.x, y: player.y, count: 0 };
  for (let i = 0; i < enemies.length; i++) {
    const e = enemies[i];
    if (!e.alive || e.boss) continue;
    const c = countEnemiesInZone(e.x, e.y, radius);
    if (c > best.count) best = { x: e.x, y: e.y, count: c };
  }
  const clamped = clampPointToWorld(best.x, best.y, radius + 4);
  best.x = clamped.x;
  best.y = clamped.y;
  return best;
}

function assignQuestByType(type) {
  setCommonQuest(type);
  const lv = Math.max(1, player.level || 1);

  if (type === "kill") {
    const s = QUEST_LEVEL_SCALE.kill;
    quest.target = clampTarget(randi(s.rollMax, s.rollMin) * (s.baseMult + lv * s.perLevel));
    quest.duration = clampTarget(s.durationBase + lv * s.durationPerLevel);
    return;
  }
  if (type === "scavenge") {
    const s = QUEST_LEVEL_SCALE.scavenge;
    quest.target = clampTarget(randi(s.rollMax, s.rollMin) + lv * s.perLevel);
    quest.duration = clampTarget(s.durationBase + lv * s.durationPerLevel);
    spawnQuestItems(quest.target, QUEST_ITEM_MIN_DIST, QUEST_ITEM_MAX_DIST, "scavenge");
    return;
  }
  if (type === "drop") {
    const s = QUEST_LEVEL_SCALE.drop;
    quest.target = clampTarget(randi(s.rollMax, s.rollMin) + lv * s.perLevel);
    quest.duration = clampTarget(s.durationBase + lv * s.durationPerLevel);
    quest.dropChance = rand(0.16, 0.09);
    return;
  }
  if (type === "nohit") {
    const s = QUEST_LEVEL_SCALE.nohit;
    quest.duration = clampTarget(randi(s.durationMax, s.durationMin) + lv * s.perLevel);
    quest.target = quest.duration;
    return;
  }
  if (type === "close_quarters") {
    const s = QUEST_LEVEL_SCALE.closeQuarters;
    quest.target = clampTarget(s.base + lv * s.perLevel);
    quest.duration = clampTarget(s.durationBase + lv * s.durationPerLevel);
    return;
  }
  if (type === "long_shot") {
    const s = QUEST_LEVEL_SCALE.longShot;
    quest.target = clampTarget(s.base + lv * s.perLevel);
    quest.duration = clampTarget(s.durationBase + lv * s.durationPerLevel);
    quest.zoneR = s.minRangeBase + lv * s.minRangePerLevel;
    return;
  }
  if (type === "execution_chain") {
    const s = QUEST_LEVEL_SCALE.executionChain;
    quest.target = clampTarget(s.base + lv * s.perLevel);
    quest.comboWindow = clamp(s.windowBase - lv * s.windowPerLevel, s.windowMin, s.windowBase);
    quest.duration = clampTarget(s.durationBase + lv * s.durationPerLevel);
    return;
  }
  if (type === "element_purge") {
    const s = QUEST_LEVEL_SCALE.elementPurge;
    quest.target = clampTarget(s.base + lv * s.perLevel);
    quest.duration = clampTarget(s.durationBase + lv * s.durationPerLevel);
    const counts = getAliveElementTargetCounts();
    const options = ["fire", "poison", "void"];
    let best = "";
    let bestCount = -1;
    for (let i = 0; i < options.length; i++) {
      const el = options[i];
      const count = counts[el];
      if (count > bestCount) {
        bestCount = count;
        best = el;
      }
    }
    quest.element = best;
    return;
  }
  if (type === "elite_hunt") {
    const s = QUEST_LEVEL_SCALE.eliteHunt;
    quest.target = lv >= s.twoTargetLevel ? s.twoTarget : s.oneTarget;
    quest.duration = clampTarget(s.durationBase + lv * s.durationPerLevel);
    return;
  }
  if (type === "overkill") {
    const s = QUEST_LEVEL_SCALE.overkill;
    quest.target = clampTarget(s.base + lv * s.perLevel);
    quest.duration = clampTarget(s.durationBase + lv * s.durationPerLevel);
    return;
  }
  if (type === "aura_discipline") {
    const s = QUEST_LEVEL_SCALE.auraDiscipline;
    quest.target = clampTarget(s.base + lv * s.perLevel);
    quest.duration = clampTarget(s.durationBase + lv * s.durationPerLevel);
    quest.source = "aura";
    quest.forbidSource = "magic";
    return;
  }
  if (type === "crit_festival") {
    const s = QUEST_LEVEL_SCALE.critFestival;
    quest.target = clampTarget(s.base + lv * s.perLevel);
    quest.duration = clampTarget(s.durationBase + lv * s.durationPerLevel);
    return;
  }
  if (type === "dot_trial") {
    const s = QUEST_LEVEL_SCALE.dotTrial;
    quest.target = clampTarget(s.base + lv * s.perLevel);
    quest.duration = clampTarget(s.durationBase + lv * s.durationPerLevel);
    return;
  }
  if (type === "orbital_defense") {
    const s = QUEST_LEVEL_SCALE.orbitalDefense;
    quest.target = clampTarget(s.base + lv * s.perLevel);
    quest.duration = clampTarget(s.durationBase + lv * s.durationPerLevel);
    quest.minSpeed = player.speed * s.minSpeedMul;
    return;
  }
  if (type === "magnet_sprint") {
    const s = QUEST_LEVEL_SCALE.magnetSprint;
    quest.target = clampTarget(s.base + lv * s.perLevel);
    quest.duration = clampTarget(s.durationBase + lv * s.durationPerLevel);
    return;
  }
  if (type === "treasure_route") {
    const s = QUEST_LEVEL_SCALE.treasureRoute;
    quest.target = 1;
    quest.duration = clampTarget(s.durationBase + lv * s.durationPerLevel);
    return;
  }
  if (type === "hazard_survivor") {
    const s = QUEST_LEVEL_SCALE.hazardSurvivor;
    quest.target = clampTarget(s.base + lv * s.perLevel);
    quest.duration = clampTarget(s.durationBase + lv * s.durationPerLevel);
    return;
  }
  if (type === "pacifist_pulse") {
    const s = QUEST_LEVEL_SCALE.pacifistPulse;
    quest.duration = clampTarget(s.durationBase + lv * s.durationPerLevel);
    quest.target = quest.duration;
    quest.source = "aura";
    return;
  }
  if (type === "crowd_control") {
    const s = QUEST_LEVEL_SCALE.crowdControl;
    quest.target = clampTarget(s.base + lv * s.perLevel);
    const baseDuration = clampTarget(s.durationBase + lv * s.durationPerLevel);
    quest.duration = Math.max(1, Math.round(baseDuration * s.durationMultiplier));
    return;
  }
  if (type === "boss_prep") {
    const s = QUEST_LEVEL_SCALE.bossPrep;
    quest.target = clampTarget(s.base + lv * s.perLevel);
    const eliteWindow = Math.max(8, Math.min(36, spawn.eliteT || 16));
    quest.duration = eliteWindow;
    return;
  }
  if (type === "glass_cannon") {
    const s = QUEST_LEVEL_SCALE.glassCannon;
    quest.target = clampTarget(s.base + lv * s.perLevel);
    quest.duration = clampTarget(s.durationBase + lv * s.durationPerLevel);
    applyGlassPenalty(lv);
    return;
  }
  if (type === "perfect_sweep") {
    const s = QUEST_LEVEL_SCALE.perfectSweep;
    quest.duration = clampTarget(s.durationBase + lv * s.durationPerLevel);
    quest.zoneR = clamp(s.radiusBase + lv * s.radiusPerLevel, s.radiusMin, s.radiusMax);
    const center = pickSweepCenter(quest.zoneR);
    quest.zoneX = center.x;
    quest.zoneY = center.y;
    quest.target = Math.max(1, center.count);
    quest.zoneNeed = quest.target;
    quest.zoneRemain = center.count;
  }
}

function phaseFamilyWeights() {
  const lv = Math.max(1, player.level || 1);
  const phase = QUEST_LEVEL_SCALE.phase;
  let w;
  if (lv < phase.earlyMaxExclusive) {
    w = { core: 0.75, combat: 0.20, utility: 0.05, weapon: 0.0 };
  } else if (lv < phase.midMaxExclusive) {
    w = { core: 0.50, combat: 0.30, utility: 0.15, weapon: 0.05 };
  } else {
    w = { core: 0.35, combat: 0.35, utility: 0.20, weapon: 0.10 };
  }
  if (failureStreak > 0) {
    w = { core: 0.62, combat: 0.10, utility: 0.28, weapon: 0.0 };
  } else if (completionStreak >= 2) {
    w = { core: 0.28, combat: 0.42, utility: 0.20, weapon: 0.10 };
  }
  return w;
}

function canOfferQuestType(type) {
  const gate = QUEST_LEVEL_SCALE.gatingLevel;
  if (type === "element_purge") return player.time >= 38 && hasAliveElementTargets();
  if (type === "elite_hunt") return player.time >= 35 || aliveEliteCount() > 0;
  if (type === "aura_discipline") return weapons.aura.unlocked;
  if (type === "pacifist_pulse") return weapons.aura.unlocked;
  if (type === "dot_trial") return hasDotSourceAvailable();
  if (type === "crit_festival") {
    const bonusCrit = upgradeState.critChanceLv * CRIT_UPGRADES.chancePerLevel;
    return bonusCrit + (weapons.magic.unlocked ? 0.1 : 0) >= QUEST_GATING_CRIT_MIN;
  }
  if (type === "orbital_defense") return player.level >= gate.orbitalDefense;
  if (type === "magnet_sprint") return gems.length > 4 || player.level >= gate.magnetSprint;
  if (type === "treasure_route") return hasAnyChestActive();
  if (type === "hazard_survivor") return player.time >= 40;
  if (type === "crowd_control") return player.level >= gate.crowdControl;
  if (type === "boss_prep") return player.level >= gate.bossPrep && spawn.eliteT > 1.2;
  if (type === "glass_cannon") return player.level >= gate.glassCannon;
  if (type === "perfect_sweep") return aliveEnemyCount(false) >= 6;
  return true;
}

function countRecentType(type, window) {
  let count = 0;
  for (let i = Math.max(0, questTypeHistory.length - window); i < questTypeHistory.length; i++) {
    if (questTypeHistory[i] === type) count++;
  }
  return count;
}

function inRecentFamilies(family, window) {
  for (let i = Math.max(0, questFamilyHistory.length - window); i < questFamilyHistory.length; i++) {
    if (questFamilyHistory[i] === family) return true;
  }
  return false;
}

function pushQuestHistory(type, family) {
  questTypeHistory.push(type);
  questFamilyHistory.push(family);
  while (questTypeHistory.length > QUEST_HISTORY_MAX) questTypeHistory.shift();
  while (questFamilyHistory.length > QUEST_HISTORY_MAX) questFamilyHistory.shift();
}

function weightedPick(candidates, weights) {
  let total = 0;
  for (let i = 0; i < candidates.length; i++) total += Math.max(0, weights[i] || 0);
  if (total <= 0) return null;
  let roll = Math.random() * total;
  for (let i = 0; i < candidates.length; i++) {
    roll -= Math.max(0, weights[i] || 0);
    if (roll <= 0) return candidates[i];
  }
  return candidates[candidates.length - 1];
}

function chooseQuestType() {
  const familyWeights = phaseFamilyWeights();
  const familyKeys = Object.keys(QUEST_TYPES_BY_FAMILY);
  let fallback = "kill";

  for (let attempt = 0; attempt < QUEST_PICK_ATTEMPTS; attempt++) {
    const familyCandidates = [];
    const familyScores = [];
    for (let i = 0; i < familyKeys.length; i++) {
      const family = familyKeys[i];
      let available = 0;
      const types = QUEST_TYPES_BY_FAMILY[family];
      for (let t = 0; t < types.length; t++) {
        if (canOfferQuestType(types[t])) available++;
      }
      if (available <= 0) continue;
      familyCandidates.push(family);
      familyScores.push(familyWeights[family] || 0);
    }

    const pickedFamily = weightedPick(familyCandidates, familyScores);
    if (!pickedFamily) break;

    const types = QUEST_TYPES_BY_FAMILY[pickedFamily];
    const choices = [];
    const scores = [];
    for (let i = 0; i < types.length; i++) {
      const type = types[i];
      if (!canOfferQuestType(type)) continue;
      const meta = QUEST_META[type];
      let score = Math.max(0.01, meta.weight);

      const lastType = questTypeHistory[questTypeHistory.length - 1] || "";
      if (type === lastType) score *= 0.05;
      if (inRecentFamilies(meta.family, 2)) score *= 0.25;
      if (type === "nohit" && countRecentType("nohit", QUEST_NOHIT_WINDOW) >= QUEST_NOHIT_CAP) score *= 0.01;
      if (failureStreak > 0 && meta.tier >= 2) score *= 0.55;
      if (completionStreak >= 2 && meta.tier >= 2) score *= 1.25;

      choices.push(type);
      scores.push(score);
      if (type === "kill") fallback = type;
    }
    const picked = weightedPick(choices, scores);
    if (picked) return picked;
  }

  return fallback;
}

function formatTimeLeft(justGiven = false) {
  if (quest.duration <= 0) return "";
  const left = justGiven ? Math.ceil(quest.duration) : Math.max(0, Math.ceil(quest.duration - quest.timer));
  return ` | T-${left}s`;
}

function formatQuestObjective(justGiven = false) {
  const p = Math.max(0, Math.floor(quest.progress));
  const t = Math.max(1, Math.floor(quest.target || 1));
  let text = "-";

  if (quest.type === "kill") text = justGiven ? `Kill ${t} mobs` : `Kill ${p}/${t} mobs`;
  else if (quest.type === "scavenge") text = justGiven ? `Loot ${t} relics` : `Loot ${p}/${t} relics`;
  if (quest.type === "drop") {
    const pct = Math.round(clamp(quest.dropChance, 0, 1) * 100);
    text = justGiven ? `Collect ${t} trophies (${pct}% drop)` : `Collect ${p}/${t} trophies (${pct}% drop)`;
  } else if (quest.type === "nohit") text = justGiven ? "Take no damage" : `No hit ${p}/${t}s`;
  else if (quest.type === "close_quarters") text = justGiven ? `Close-range kills: ${t}` : `Close-range kills ${p}/${t}`;
  else if (quest.type === "long_shot") text = justGiven ? `Long-range kills: ${t}` : `Long-range kills ${p}/${t}`;
  else if (quest.type === "execution_chain") text = justGiven ? `Kill chain x${t} in ${quest.comboWindow.toFixed(1)}s` : `Chain ${quest.combo}/${t}`;
  else if (quest.type === "element_purge") text = justGiven ? `Kill ${t} ${elementLabel(quest.element)} enemies` : `${elementLabel(quest.element)} enemies ${p}/${t}`;
  else if (quest.type === "elite_hunt") text = justGiven ? `Hunt ${t} elite${t > 1 ? "s" : ""}` : `Elite kills ${p}/${t}`;
  else if (quest.type === "overkill") text = justGiven ? `Deal ${t} total damage` : `Damage ${p}/${t}`;
  else if (quest.type === "aura_discipline") text = justGiven ? `Aura kills ${t} (no Magic)` : `Aura kills ${p}/${t}`;
  else if (quest.type === "crit_festival") text = justGiven ? `Land ${t} critical hits` : `Critical hits ${p}/${t}`;
  else if (quest.type === "dot_trial") text = justGiven ? `Deal ${t} DoT damage` : `DoT damage ${p}/${t}`;
  else if (quest.type === "orbital_defense") {
    const s = QUEST_LEVEL_SCALE.orbitalDefense;
    const needMove = Math.floor(quest.duration * (s.requiredMoveRatio || 1));
    const moveT = Math.floor(quest.moveTimer || 0);
    text = justGiven ? `Stay fast ${needMove}s and kill ${t}` : `Move ${moveT}/${needMove}s | Kills ${p}/${t}`;
  } else if (quest.type === "magnet_sprint") text = justGiven ? `Collect ${t} gems fast` : `Gems ${p}/${t}`;
  else if (quest.type === "treasure_route") text = justGiven ? "Open a chest" : `Chest ${p}/${t}`;
  else if (quest.type === "hazard_survivor") text = justGiven ? `Stay in hazards ${t}s total` : `Hazard time ${p}/${t}s`;
  else if (quest.type === "pacifist_pulse") text = justGiven ? "Aura-only survival" : `Aura-only survival ${p}/${t}s`;
  else if (quest.type === "crowd_control") text = justGiven ? `Knock back ${t} different enemies` : `Different enemies knocked back ${p}/${t}`;
  else if (quest.type === "boss_prep") text = justGiven ? `Deal ${t} damage before next elite` : `Prep damage ${p}/${t}`;
  else if (quest.type === "glass_cannon") text = justGiven ? `Kill ${t} while defenses are reduced` : `Glass kills ${p}/${t}`;
  else if (quest.type === "perfect_sweep") text = justGiven ? `Clear marked zone (${t} enemies)` : `Sweep ${p}/${t}`;

  return `${text}${formatTimeLeft(justGiven)}`;
}
function pushQuestNotice(text, color = COLORS.quest, size = 18) {
  popFloatText(player.x, player.y - 28, text, color, size, QUEST_NOTICE_LIFE, 16, 50, 80);
}

function completeQuest() {
  if (quest.completed) return;
  restoreQuestPenalty();
  completionStreak++;
  failureStreak = 0;
  quest.completed = true;
  pushQuestNotice("Objective Complete!", COLORS.gold, 20);
}

function resetQuestData() {
  restoreQuestPenalty();
  quest.active = false;
  quest.completed = false;
  quest.type = "";
  quest.family = "";
  quest.tier = 0;
  quest.rewardTier = 0;
  quest.progress = 0;
  quest.target = 0;
  quest.timer = 0;
  quest.duration = 0;
  quest.dropChance = 0;
  quest.combo = 0;
  quest.comboT = 0;
  quest.comboWindow = 0;
  quest.element = "";
  quest.source = "";
  quest.forbidSource = "";
  quest.minSpeed = 0;
  quest.zoneX = 0;
  quest.zoneY = 0;
  quest.zoneR = 0;
  quest.zoneNeed = 0;
  quest.zoneRemain = 0;
  quest.instance = 0;
  quest.penaltyActive = false;
  quest.armorMul = 1;
  quest.resistMul = 1;
  quest.moveTimer = 0;
  quest.moveX = player.x;
  quest.moveY = player.y;
  quest.lastHp = player.hp;
  clearQuestItems();
}

function abortQuest() {
  failureStreak++;
  completionStreak = 0;
  resetQuestData();
  scheduleQuestGiver(getRespawnDelay());
  pushQuestNotice("Objective Failed!", COLORS.warn, 20);
}

function assignQuest() {
  const type = chooseQuestType();
  assignQuestByType(type);
  pushQuestHistory(type, QUEST_META[type].family);
  quest.giverActive = true;
  pushQuestNotice(`Quest: ${formatQuestObjective(true)}`);
}

function spawnRewardGems(x, y, count = 8, value = 2) {
  for (let i = 0; i < count; i++) {
    const g = gemPool.get();
    g.alive = true;
    g.x = x + rand(LOOT_CONFIG.dropJitter, -LOOT_CONFIG.dropJitter);
    g.y = y + rand(LOOT_CONFIG.dropJitter, -LOOT_CONFIG.dropJitter);
    const ang = rand(TAU, 0);
    const sp = rand(LOOT_CONFIG.dropSpeedMax, LOOT_CONFIG.dropSpeedMin);
    g.vx = Math.cos(ang) * sp;
    g.vy = Math.sin(ang) * sp;
    g.v = value;
    g.r = LOOT_CONFIG.gemRadiusBase + value * LOOT_CONFIG.gemRadiusScale;
    g.maxLife = 35;
    g.life = g.maxLife;
    gems.push(g);
  }
}

function grantExpReward(mult = 1) {
  const lvl = Math.max(1, player.level || 1);
  const s = QUEST_LEVEL_SCALE.reward;
  const count = Math.max(s.countMin, Math.floor((s.countBase + lvl * s.countPerLevel) * mult));
  const value = Math.max(s.valueMin, Math.floor((s.valueBase + lvl * s.valuePerLevel) * mult));
  spawnRewardGems(quest.giverX, quest.giverY, count * 2, value);
  if (runtime.addParticles) runtime.addParticles(quest.giverX, quest.giverY, COLORS.gold, 18 + Math.floor(6 * mult), 420 + 100 * mult);
}

function rewardQuest() {
  const { openAug, openTrinket } = requireRuntime();
  const tier = quest.rewardTier || 0;
  const augChance = tier >= 2 ? 0.45 : (tier === 1 ? 0.35 : 0.20);
  const trinketChance = 0.25;
  const roll = Math.random();

  if (roll < augChance) {
    if (openAug && openAug()) return;
    grantExpReward(1.25 + tier * 0.1);
    return;
  }
  if (roll < augChance + trinketChance) {
    if (openTrinket && openTrinket()) return;
    grantExpReward(1.20 + tier * 0.1);
    return;
  }
  grantExpReward(1.0 + tier * 0.15);
}

function updateQuestItems(dt) {
  let completedNow = false;
  for (let i = questItems.length - 1; i >= 0; i--) {
    const it = questItems[i];
    if (!it.alive) {
      questItems[i] = questItems[questItems.length - 1];
      questItems.pop();
      continue;
    }
    const dx = it.x - player.x;
    const dy = it.y - player.y;
    const rr = it.r + player.r;
    if (dx * dx + dy * dy <= rr * rr) {
      it.alive = false;
      quest.progress = Math.min(quest.target, quest.progress + 1);
      if (quest.progress >= quest.target) {
        completeQuest();
        completedNow = true;
      }
    }
    if (!it.alive) {
      questItems[i] = questItems[questItems.length - 1];
      questItems.pop();
    }
  }
  if (completedNow) clearQuestItems();
}

function updateNoHit(dt) {
  if (player.hp < quest.lastHp - 0.01) {
    abortQuest();
    return false;
  }
  quest.lastHp = player.hp;
  quest.timer += dt;
  quest.progress = Math.min(quest.target, Math.floor(quest.timer));
  if (quest.timer >= quest.duration) completeQuest();
  return true;
}

function updateTimedBasic(dt) {
  quest.timer += dt;
  if (quest.progress >= quest.target) {
    completeQuest();
    return true;
  }
  if (quest.duration > 0 && quest.timer >= quest.duration) {
    abortQuest();
    return false;
  }
  return true;
}

function updateActiveQuest(dt) {
  if (quest.type === "scavenge" || quest.type === "drop") {
    updateQuestItems(dt);
    if (!quest.completed) updateTimedBasic(dt);
    return;
  }
  if (quest.type === "nohit") {
    updateNoHit(dt);
    return;
  }
  if (quest.type === "execution_chain") {
    quest.timer += dt;
    quest.comboT = Math.max(0, quest.comboT - dt);
    if (quest.comboT <= 0) quest.combo = 0;
    quest.progress = quest.combo;
    if (quest.progress >= quest.target) {
      completeQuest();
      return;
    }
    if (quest.timer >= quest.duration) abortQuest();
    return;
  }
  if (quest.type === "orbital_defense") {
    const s = QUEST_LEVEL_SCALE.orbitalDefense;
    const requiredMove = quest.duration * (s.requiredMoveRatio || 1);
    const dx = player.x - quest.moveX;
    const dy = player.y - quest.moveY;
    const speed = hypot(dx, dy) / Math.max(1e-4, dt);
    quest.moveX = player.x;
    quest.moveY = player.y;
    if (speed >= quest.minSpeed) quest.moveTimer += dt;
    quest.timer += dt;
    if (quest.moveTimer >= requiredMove && quest.progress >= quest.target) {
      completeQuest();
      return;
    }
    if (quest.timer >= quest.duration) abortQuest();
    return;
  }
  if (quest.type === "hazard_survivor") {
    let inside = false;
    for (let i = 0; i < voidZones.length; i++) {
      const z = voidZones[i];
      if (!z.alive) continue;
      const dx = z.x - player.x;
      const dy = z.y - player.y;
      const rr = z.r + player.r;
      if (dx * dx + dy * dy <= rr * rr) {
        inside = true;
        break;
      }
    }
    if (inside) quest.progress += dt;
    quest.timer += dt;
    if (quest.progress >= quest.target) {
      completeQuest();
      return;
    }
    if (quest.timer >= quest.duration) abortQuest();
    return;
  }
  if (quest.type === "pacifist_pulse") {
    quest.timer += dt;
    quest.progress = Math.floor(quest.timer);
    if (quest.timer >= quest.duration) completeQuest();
    return;
  }
  if (quest.type === "perfect_sweep") {
    quest.timer += dt;
    const remain = countEnemiesInZone(quest.zoneX, quest.zoneY, quest.zoneR);
    quest.zoneRemain = remain;
    quest.progress = Math.max(0, quest.target - remain);
    if (remain <= 0 && quest.target > 0) {
      completeQuest();
      return;
    }
    if (quest.timer >= quest.duration) abortQuest();
    return;
  }

  updateTimedBasic(dt);
}

export function resetQuests() {
  questTypeHistory = [];
  questFamilyHistory = [];
  completionStreak = 0;
  failureStreak = 0;
  resetQuestData();
  quest.touchCd = 0;
  quest.cooldown = 0;
  quest.giverX = player.x;
  quest.giverY = player.y;
  scheduleQuestGiver(QUEST_SPAWN_DELAY);
}
export function onEnemyDamaged(e, evt = {}) {
  if (!quest.active || quest.completed) return;
  const amount = Math.max(0, evt.amount || 0);
  const crit = !!evt.crit;
  const source = evt.source || "";
  const damageKind = evt.damageKind || "direct";
  const knock = !!evt.knock;

  if (quest.type === "overkill" || quest.type === "boss_prep") {
    if (amount > 0) quest.progress = Math.min(quest.target, quest.progress + amount);
    return;
  }
  if (quest.type === "crit_festival") {
    if (crit && amount > 0) quest.progress = Math.min(quest.target, quest.progress + 1);
    return;
  }
  if (quest.type === "dot_trial") {
    if (amount > 0 && (damageKind === "burn" || damageKind === "bleed")) {
      quest.progress = Math.min(quest.target, quest.progress + amount);
    }
    return;
  }
  if (quest.type === "crowd_control") {
    if (knock && e && e._questCcId !== quest.instance) {
      e._questCcId = quest.instance;
      quest.progress = Math.min(quest.target, quest.progress + 1);
    }
    return;
  }
  if (quest.type === "aura_discipline" && quest.forbidSource && source === quest.forbidSource) {
    abortQuest();
  }
}

export function onEnemyKilled(e, meta = {}) {
  if (!quest.active || quest.completed) return;
  const source = meta.source || "";

  if (quest.type === "kill") {
    if (!e.boss) quest.progress = Math.min(quest.target, quest.progress + 1);
    if (quest.progress >= quest.target) completeQuest();
    return;
  }
  if (quest.type === "drop") {
    if (!e.boss && Math.random() < quest.dropChance) spawnQuestItem(e.x, e.y, "drop");
    return;
  }
  if (quest.type === "close_quarters") {
    if (e.boss) return;
    const dx = e.x - player.x;
    const dy = e.y - player.y;
    const closeR = (player.r + e.r) * QUEST_LEVEL_SCALE.closeQuarters.rangeMultiplier;
    if (dx * dx + dy * dy <= closeR * closeR) quest.progress = Math.min(quest.target, quest.progress + 1);
    return;
  }
  if (quest.type === "long_shot") {
    if (e.boss) return;
    const dx = e.x - player.x;
    const dy = e.y - player.y;
    if (dx * dx + dy * dy >= quest.zoneR * quest.zoneR) quest.progress = Math.min(quest.target, quest.progress + 1);
    return;
  }
  if (quest.type === "execution_chain") {
    if (e.boss) return;
    quest.combo = Math.max(0, quest.combo) + 1;
    quest.comboT = quest.comboWindow;
    quest.progress = quest.combo;
    return;
  }
  if (quest.type === "element_purge") {
    if (e.boss) return;
    if (enemyElement(e) === quest.element) quest.progress = Math.min(quest.target, quest.progress + 1);
    return;
  }
  if (quest.type === "elite_hunt") {
    if (e.elite) quest.progress = Math.min(quest.target, quest.progress + 1);
    return;
  }
  if (quest.type === "aura_discipline") {
    if (source === "aura") quest.progress = Math.min(quest.target, quest.progress + 1);
    return;
  }
  if (quest.type === "orbital_defense") {
    if (!e.boss) quest.progress = Math.min(quest.target, quest.progress + 1);
    return;
  }
  if (quest.type === "pacifist_pulse") {
    if (source && source !== quest.source) {
      abortQuest();
      return;
    }
    return;
  }
  if (quest.type === "glass_cannon") {
    if (!e.boss) quest.progress = Math.min(quest.target, quest.progress + 1);
    return;
  }
}

export function onGemCollected() {
  if (!quest.active || quest.completed) return;
  if (quest.type !== "magnet_sprint") return;
  quest.progress = Math.min(quest.target, quest.progress + 1);
}

export function onChestOpened() {
  if (!quest.active || quest.completed) return;
  if (quest.type !== "treasure_route") return;
  quest.progress = Math.min(quest.target, quest.progress + 1);
}

export function updateQuests(dt) {
  if (quest.touchCd > 0) quest.touchCd = Math.max(0, quest.touchCd - dt);
  if (quest.cooldown > 0) quest.cooldown = Math.max(0, quest.cooldown - dt);

  if (!quest.giverActive) {
    quest.spawnT = Math.max(0, quest.spawnT - dt);
    if (quest.spawnT <= 0) activateQuestGiver();
  }

  const dx = quest.giverX - player.x;
  const dy = quest.giverY - player.y;
  const rr = quest.giverR + player.r;
  const touching = quest.giverActive && (dx * dx + dy * dy <= rr * rr);

  if (touching && quest.touchCd <= 0) {
    quest.touchCd = QUEST_TOUCH_CD;
    if (quest.completed) {
      rewardQuest();
      resetQuestData();
      quest.cooldown = 2.0;
      scheduleQuestGiver(getRespawnDelay());
    } else if (!quest.active && quest.cooldown <= 0) {
      assignQuest();
    }
  }

  if (!quest.active || quest.completed) return;
  updateActiveQuest(dt);
}

export function getQuestHudText() {
  if (!quest.active && !quest.completed) return "-";
  if (quest.completed) return "Complete! Return to giver.";
  return formatQuestObjective(false);
}
