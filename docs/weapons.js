import { WEAPON_CONFIG, WEAPON_MASTERY, UPGRADE_CONFIG, CRIT_UPGRADES, AUGMENT_CONFIG } from "./config.js";
import { COLORS } from "./colors.js";
import { clamp, rand, TAU, hypot } from "./math.js";
import { sound } from "./audio.js";
import { triggerHealFx } from "./combat_fx.js";
import {
  player,
  enemies,
  activeObstacles,
  obstacles,
  clampPointToWorld,
  bullets,
  rails,
  axes,
  orbs,
  missiles,
  turrets,
  toxinPools,
  arcs,
  trinketBonuses,
} from "./state.js";
import {
  bulletPool,
  railPool,
  axePool,
  orbPool,
  missilePool,
  turretPool,
  toxinPool,
  arcPool,
} from "./pools.js";

export const weapons = {
  magic: { unlocked: true, level: 1, mastery: 0, t: 0, aug: null, augSeq: 0 },
  arc: { unlocked: false, level: 0, mastery: 0, t: 0, aug: null, augSeq: 0 },
  aura: { unlocked: false, level: 0, mastery: 0, tick: 0, aug: null, pulse: 0, pulseFx: 0, pulseFxMax: 0.25 },
  rail: { unlocked: false, level: 0, mastery: 0, t: 0, aug: null },
  axe: { unlocked: false, level: 0, mastery: 0, t: 0, aug: null },
  orb: { unlocked: false, level: 0, mastery: 0, t: 0, aug: null },
  missile: { unlocked: false, level: 0, mastery: 0, t: 0, aug: null },
  turret: { unlocked: false, level: 0, mastery: 0, slotTimers: [], aug: null, spawnSeq: 0 },
};

let ctxBuffs = null;
let ctxUpgradeState = null;
let runtime = { damageEnemy:null, damageObstacle:null, damageObstaclesInRadius:null, addParticles:null, spawnShockwave:null };

export function setWeaponContext({ buffs, upgradeState }) {
  ctxBuffs = buffs;
  ctxUpgradeState = upgradeState;
}

export function setWeaponRuntime({ damageEnemy, damageObstacle, damageObstaclesInRadius, addParticles, spawnShockwave }) {
  runtime = { damageEnemy, damageObstacle, damageObstaclesInRadius, addParticles, spawnShockwave };
}

function requireContext() {
  if (!ctxBuffs || !ctxUpgradeState) {
    throw new Error("Weapon context missing; call setWeaponContext({ buffs, upgradeState }) first.");
  }
  return { buffs: ctxBuffs, upgradeState: ctxUpgradeState };
}

function requireRuntime() {
  if (!runtime.damageEnemy || !runtime.damageObstacle || !runtime.damageObstaclesInRadius || !runtime.addParticles || !runtime.spawnShockwave) {
    throw new Error("Weapon runtime missing; call setWeaponRuntime({ damageEnemy, damageObstacle, damageObstaclesInRadius, addParticles, spawnShockwave }) first.");
  }
  return runtime;
}

export function spawnShockwave(x, y, radius, color = COLORS.warn) {
  const exp = bulletPool.get();
  exp.alive = true;
  exp.x = x; exp.y = y;
  exp.vx = 0; exp.vy = 0;
  exp.r = radius;
  exp.life = 0.2;
  exp.maxLife = exp.life;
  exp.dmg = 0;
  exp.critChance = 0;
  exp.critMult = 1;
  exp.color = color;
  exp.isExplosion = true;
  bullets.push(exp);
}

function calcCrit(dmg, chance, mult){
  const crit = Math.random() < chance;
  return { dmg: crit ? dmg * mult : dmg, crit };
}

function applySlow(e, mult, duration){
  e.slowT = Math.max(e.slowT || 0, duration);
  const next = Math.min(e.slowMul || 1, mult);
  e.slowMul = next;
}

function applyBurn(e, dmg, cfg, source){
  e.burnT = Math.max(e.burnT || 0, cfg.duration);
  e.burnDps = Math.max(e.burnDps || 0, dmg * cfg.dpsPct);
  if (source) e.burnSource = source;
  e.burnElement = "fire";
}

function applyBleed(e, dmg, cfg, source){
  e.bleedT = Math.max(e.bleedT || 0, cfg.duration);
  e.bleedDps = Math.max(e.bleedDps || 0, dmg * cfg.dpsPct);
  if (source) e.bleedSource = source;
}

function getTrinketMods() {
  return {
    dmg: trinketBonuses.dmgMult || 1,
    cd: trinketBonuses.cdMult || 1,
    critChance: trinketBonuses.critChance || 0,
    critMult: trinketBonuses.critMult || 0,
  };
}

export function resetWeapons() {
  weapons.magic.unlocked = true; weapons.magic.level = 1; weapons.magic.mastery = 0; weapons.magic.t = 0; weapons.magic.aug = null; weapons.magic.augSeq = 0;
  weapons.arc.unlocked = false; weapons.arc.level = 0; weapons.arc.mastery = 0; weapons.arc.t = 0; weapons.arc.aug = null; weapons.arc.augSeq = 0;
  weapons.aura.unlocked = false; weapons.aura.level = 0; weapons.aura.mastery = 0; weapons.aura.tick = 0; weapons.aura.aug = null; weapons.aura.pulse = 0; weapons.aura.pulseFx = 0; weapons.aura.pulseFxMax = 0.25;
  weapons.rail.unlocked = false; weapons.rail.level = 0; weapons.rail.mastery = 0; weapons.rail.t = 0; weapons.rail.aug = null;
  weapons.axe.unlocked = false; weapons.axe.level = 0; weapons.axe.mastery = 0; weapons.axe.t = 0; weapons.axe.aug = null;
  weapons.orb.unlocked = false; weapons.orb.level = 0; weapons.orb.mastery = 0; weapons.orb.t = 0; weapons.orb.aug = null;
  weapons.missile.unlocked = false; weapons.missile.level = 0; weapons.missile.mastery = 0; weapons.missile.t = 0; weapons.missile.aug = null;
  weapons.turret.unlocked = false; weapons.turret.level = 0; weapons.turret.mastery = 0; weapons.turret.slotTimers.length = 0; weapons.turret.aug = null; weapons.turret.spawnSeq = 0;
}

export function weaponCount() {
  let c = 0;
  if (weapons.magic.unlocked) c++;
  if (weapons.arc.unlocked) c++;
  if (weapons.aura.unlocked) c++;
  if (weapons.rail.unlocked) c++;
  if (weapons.axe.unlocked) c++;
  if (weapons.orb.unlocked) c++;
  if (weapons.missile.unlocked) c++;
  if (weapons.turret.unlocked) c++;
  return c;
}

export function magicStats() {
  const { buffs, upgradeState } = requireContext();
  const trinket = getTrinketMods();
  const cfg = WEAPON_CONFIG.magic;
  const lv = weapons.magic.level;
  const mastery = weapons.magic.mastery || 0;
  const masteryDmgMult = 1 + mastery * WEAPON_MASTERY.dmgMult;
  const masteryCrit = mastery * WEAPON_MASTERY.critChance;
  const masteryCritMult = mastery * WEAPON_MASTERY.critMult;
  const powerMul = (buffs.power > 0) ? cfg.powerDmgMult : 1.0;
  const cdMul = (buffs.power > 0) ? cfg.powerCdMult : 1.0;
  const dmg = (cfg.dmgBase + lv * cfg.dmgPerLevel) * powerMul * masteryDmgMult * trinket.dmg;
  const cdRaw = Math.max(cfg.cdMin, cfg.cdBase - lv * cfg.cdPerLevel) * cdMul;
  const cdReduce = Math.max(0.1, 1 - upgradeState.cdLv * UPGRADE_CONFIG.cdReduction);
  const cd = Math.max(cfg.cdMin, cdRaw * cdReduce * trinket.cd);
  const speed = cfg.speedBase + lv * cfg.speedPerLevel;
  const count = 1 + Math.floor((lv - 1) / cfg.countInterval);
  const range = cfg.range;
  const knock = (cfg.knockBase + lv * cfg.knockPerLevel) * powerMul;
  const critChance = Math.min(1, cfg.crit.base + lv * cfg.crit.perLevel + masteryCrit);
  const critMult = cfg.crit.multBase + lv * cfg.crit.multPerLevel + masteryCritMult + upgradeState.critMultLv * CRIT_UPGRADES.multPerLevel + trinket.critMult;
  const critChanceTotal = clamp(critChance + upgradeState.critChanceLv * CRIT_UPGRADES.chancePerLevel + trinket.critChance, 0, 1);
  return { dmg, cd, speed, count, range, knock, critChance: critChanceTotal, critMult };
}

export function arcStats() {
  const { buffs, upgradeState } = requireContext();
  const trinket = getTrinketMods();
  const cfg = WEAPON_CONFIG.arc;
  const lv = weapons.arc.level;
  const mastery = weapons.arc.mastery || 0;
  const masteryDmgMult = 1 + mastery * WEAPON_MASTERY.dmgMult;
  const masteryCrit = mastery * WEAPON_MASTERY.critChance;
  const masteryCritMult = mastery * WEAPON_MASTERY.critMult;
  const powerMul = (buffs.power > 0) ? cfg.powerDmgMult : 1.0;
  const cdMul = (buffs.power > 0) ? cfg.powerCdMult : 1.0;
  const dmg = (cfg.dmgBase + lv * cfg.dmgPerLevel) * powerMul * masteryDmgMult * trinket.dmg;
  const cdRaw = Math.max(cfg.cdMin, cfg.cdBase - lv * cfg.cdPerLevel) * cdMul;
  const cdReduce = Math.max(0.1, 1 - upgradeState.cdLv * UPGRADE_CONFIG.cdReduction);
  const cd = Math.max(cfg.cdMin, cdRaw * cdReduce * trinket.cd);
  const range = cfg.rangeBase + lv * cfg.rangePerLevel;
  const chains = cfg.chainBase + Math.floor((lv - 1) / cfg.chainInterval);
  const chainRange = cfg.chainRangeBase + lv * cfg.chainRangePerLevel;
  const falloff = cfg.falloff;
  const critChance = Math.min(1, cfg.crit.base + lv * cfg.crit.perLevel + masteryCrit);
  const critMult = cfg.crit.multBase + lv * cfg.crit.multPerLevel + masteryCritMult + upgradeState.critMultLv * CRIT_UPGRADES.multPerLevel + trinket.critMult;
  const critChanceTotal = clamp(critChance + upgradeState.critChanceLv * CRIT_UPGRADES.chancePerLevel + trinket.critChance, 0, 1);
  return { dmg, cd, range, chains, chainRange, falloff, critChance: critChanceTotal, critMult };
}

export function auraStats() {
  const { buffs, upgradeState } = requireContext();
  const trinket = getTrinketMods();
  const cfg = WEAPON_CONFIG.aura;
  const lv = weapons.aura.level;
  const mastery = weapons.aura.mastery || 0;
  const masteryDmgMult = 1 + mastery * WEAPON_MASTERY.dmgMult;
  const masteryCrit = mastery * WEAPON_MASTERY.critChance;
  const masteryCritMult = mastery * WEAPON_MASTERY.critMult;
  const powerMul = (buffs.power > 0) ? cfg.powerDmgMult : 1.0;
  const radius = cfg.radiusBase + lv * cfg.radiusPerLevel;
  const tickBase = cfg.tick;
  const tickStep = cfg.tickPerLevel || 0;
  const cdr = Math.max(0, upgradeState.cdLv * UPGRADE_CONFIG.cdReduction);
  const cdrTickScale = cfg.cdrTickScale || 0;
  const tickCdrMult = Math.max(0.5, 1 - cdr * cdrTickScale);
  const tick = Math.max(cfg.tickMin || 0, (tickBase + (lv - 1) * tickStep) * tickCdrMult);
  const dmg = (cfg.dmgBase + lv * cfg.dmgPerLevel) * powerMul * masteryDmgMult * trinket.dmg;
  const knock = (cfg.knockBase + lv * cfg.knockPerLevel) * powerMul;
  const critChance = Math.min(1, cfg.crit.base + lv * cfg.crit.perLevel + masteryCrit);
  const critMult = cfg.crit.multBase + lv * cfg.crit.multPerLevel + masteryCritMult + upgradeState.critMultLv * CRIT_UPGRADES.multPerLevel + trinket.critMult;
  const critChanceTotal = clamp(critChance + upgradeState.critChanceLv * CRIT_UPGRADES.chancePerLevel + trinket.critChance, 0, 1);
  return { radius, tick, dmg, knock, critChance: critChanceTotal, critMult };
}

export function axeStats() {
  const { buffs, upgradeState } = requireContext();
  const trinket = getTrinketMods();
  const cfg = WEAPON_CONFIG.axe;
  const lv = weapons.axe.level;
  const mastery = weapons.axe.mastery || 0;
  const masteryDmgMult = 1 + mastery * WEAPON_MASTERY.dmgMult;
  const masteryCrit = mastery * WEAPON_MASTERY.critChance;
  const masteryCritMult = mastery * WEAPON_MASTERY.critMult;
  const powerMul = (buffs.power > 0) ? cfg.powerDmgMult : 1.0;
  const cdMul = (buffs.power > 0) ? cfg.powerCdMult : 1.0;
  const cdRaw = Math.max(cfg.cdMin, cfg.cdBase - lv * cfg.cdPerLevel) * cdMul;
  const cdReduce = Math.max(0.1, 1 - upgradeState.cdLv * UPGRADE_CONFIG.cdReduction);
  let cd = Math.max(cfg.cdMin, cdRaw * cdReduce * trinket.cd);
  let dmg = (cfg.dmgBase + lv * cfg.dmgPerLevel) * powerMul * masteryDmgMult * trinket.dmg;
  const speed = cfg.speedBase + lv * cfg.speedPerLevel;
  const count = 1 + Math.floor((lv - 1) / cfg.countInterval);
  const gravity = cfg.gravity;
  const knock = (cfg.knockBase + lv * cfg.knockPerLevel) * powerMul;
  const critChance = Math.min(1, cfg.crit.base + lv * cfg.crit.perLevel + masteryCrit);
  const critMult = cfg.crit.multBase + lv * cfg.crit.multPerLevel + masteryCritMult + upgradeState.critMultLv * CRIT_UPGRADES.multPerLevel + trinket.critMult;
  const critChanceTotal = clamp(critChance + upgradeState.critChanceLv * CRIT_UPGRADES.chancePerLevel + trinket.critChance, 0, 1);
  return { cd, dmg, speed, count, gravity, knock, critChance: critChanceTotal, critMult };
}

export function railStats() {
  const { buffs, upgradeState } = requireContext();
  const trinket = getTrinketMods();
  const cfg = WEAPON_CONFIG.rail;
  const lv = weapons.rail.level;
  const mastery = weapons.rail.mastery || 0;
  const masteryDmgMult = 1 + mastery * WEAPON_MASTERY.dmgMult;
  const masteryCrit = mastery * WEAPON_MASTERY.critChance;
  const masteryCritMult = mastery * WEAPON_MASTERY.critMult;
  const powerMul = (buffs.power > 0) ? cfg.powerDmgMult : 1.0;
  const cdMul = (buffs.power > 0) ? cfg.powerCdMult : 1.0;
  const cdRaw = Math.max(cfg.cdMin, cfg.cdBase - lv * cfg.cdPerLevel) * cdMul;
  const cdReduce = Math.max(0.1, 1 - upgradeState.cdLv * UPGRADE_CONFIG.cdReduction);
  let cd = Math.max(cfg.cdMin, cdRaw * cdReduce * trinket.cd);
  let dmg = (cfg.dmgBase + lv * cfg.dmgPerLevel) * powerMul * masteryDmgMult * trinket.dmg;
  const speed = cfg.speedBase + lv * cfg.speedPerLevel;
  let pierce = cfg.pierceBase + Math.floor((lv + 1) / cfg.pierceLevelDivisor) + (buffs.power > 0 ? cfg.powerPierceBonus : 0);
  const range = cfg.rangeBase + lv * cfg.rangePerLevel;
  const knock = (cfg.knockBase + lv * cfg.knockPerLevel) * powerMul;
  const critChance = Math.min(1, cfg.crit.base + lv * cfg.crit.perLevel + masteryCrit);
  const critMult = cfg.crit.multBase + lv * cfg.crit.multPerLevel + masteryCritMult + upgradeState.critMultLv * CRIT_UPGRADES.multPerLevel + trinket.critMult;
  const critChanceTotal = clamp(critChance + upgradeState.critChanceLv * CRIT_UPGRADES.chancePerLevel + trinket.critChance, 0, 1);
  if (weapons.rail.aug === "rail_overpen") {
    dmg *= AUGMENT_CONFIG.rail.overpen.dmgMult;
    cd *= AUGMENT_CONFIG.rail.overpen.cdMult;
    pierce += AUGMENT_CONFIG.rail.overpen.pierce;
  }
  return { cd, dmg, speed, pierce, range, knock, critChance: critChanceTotal, critMult };
}

export function orbStats() {
  const { buffs, upgradeState } = requireContext();
  const trinket = getTrinketMods();
  const cfg = WEAPON_CONFIG.orb;
  const lv = weapons.orb.level;
  const mastery = weapons.orb.mastery || 0;
  const masteryDmgMult = 1 + mastery * WEAPON_MASTERY.dmgMult;
  const masteryCrit = mastery * WEAPON_MASTERY.critChance;
  const masteryCritMult = mastery * WEAPON_MASTERY.critMult;
  const powerMul = (buffs.power > 0) ? cfg.powerDmgMult : 1.0;
  const cdMul = (buffs.power > 0) ? cfg.powerCdMult : 1.0;
  const baseDmg = (cfg.dmgBase + lv * cfg.dmgPerLevel) * powerMul * masteryDmgMult * trinket.dmg;
  let tickDmg = baseDmg;
  const cdRaw = Math.max(cfg.cdMin, cfg.cdBase - lv * cfg.cdPerLevel) * cdMul;
  const cdReduce = Math.max(0.1, 1 - upgradeState.cdLv * UPGRADE_CONFIG.cdReduction);
  const cd = Math.max(cfg.cdMin, cdRaw * cdReduce * trinket.cd);
  const speed = cfg.speedBase + lv * cfg.speedPerLevel;
  const radius = cfg.pullRadiusBase + lv * cfg.pullRadiusPerLevel;
  let pull = cfg.pullBase + lv * cfg.pullPerLevel;
  const critChance = Math.min(1, cfg.crit.base + lv * cfg.crit.perLevel + masteryCrit);
  const critMult = cfg.crit.multBase + lv * cfg.crit.multPerLevel + masteryCritMult + upgradeState.critMultLv * CRIT_UPGRADES.multPerLevel + trinket.critMult;
  const critChanceTotal = clamp(critChance + upgradeState.critChanceLv * CRIT_UPGRADES.chancePerLevel + trinket.critChance, 0, 1);
  let explosion = baseDmg * cfg.explosionMult;
  let explosionRadius = radius;
  let park = cfg.parkTimeBase + lv * cfg.parkTimePerLevel;
  if (weapons.orb.aug === "orb_event_horizon") {
    park += AUGMENT_CONFIG.orb.eventHorizon.park;
    pull *= AUGMENT_CONFIG.orb.eventHorizon.pullMult;
    tickDmg *= AUGMENT_CONFIG.orb.eventHorizon.tickDmgMult;
  } else if (weapons.orb.aug === "orb_dark_burst") {
    park += AUGMENT_CONFIG.orb.darkBurst.park;
    pull *= AUGMENT_CONFIG.orb.darkBurst.pullMult;
    explosion *= AUGMENT_CONFIG.orb.darkBurst.dmgMult;
    explosionRadius = radius * AUGMENT_CONFIG.orb.darkBurst.radiusMult;
  }
  park = Math.max(0.4, park);
  return { dmg: tickDmg, cd, speed, radius, pull, range: cfg.range, tick: cfg.tick, park, explosion, explosionRadius, critChance: critChanceTotal, critMult };
}

export function missileStats() {
  const { buffs, upgradeState } = requireContext();
  const trinket = getTrinketMods();
  const cfg = WEAPON_CONFIG.missile;
  const lv = weapons.missile.level;
  const mastery = weapons.missile.mastery || 0;
  const masteryDmgMult = 1 + mastery * WEAPON_MASTERY.dmgMult;
  const masteryCrit = mastery * WEAPON_MASTERY.critChance;
  const masteryCritMult = mastery * WEAPON_MASTERY.critMult;
  const powerMul = (buffs.power > 0) ? cfg.powerDmgMult || 1 : 1;
  let dmg = (cfg.dmgBase + lv * cfg.dmgPerLevel) * powerMul * masteryDmgMult * trinket.dmg;
  const cdRaw = Math.max(cfg.cdMin, cfg.cdBase - lv * cfg.cdPerLevel);
  const cdReduce = Math.max(0.1, 1 - upgradeState.cdLv * UPGRADE_CONFIG.cdReduction);
  const cd = Math.max(cfg.cdMin, cdRaw * cdReduce * trinket.cd);
  const speed = cfg.speedBase + lv * cfg.speedPerLevel;
  const maxSpeed = speed * cfg.maxSpeedMult;
  const accel = cfg.accel;
  const turnRate = (cfg.turnRateDeg || 180) * (Math.PI / 180) * (1 + 0.04 * lv); // improved homing per level
  let count = 1 + Math.floor((lv - 1) / cfg.countInterval);
  const explosion = cfg.explosionRadiusBase + lv * cfg.explosionRadiusPerLevel;
  const life = cfg.life;
  const range = cfg.rangeBase + lv * (cfg.rangePerLevel || 0);
  const critChance = Math.min(1, cfg.crit.base + lv * cfg.crit.perLevel + masteryCrit);
  const critMult = cfg.crit.multBase + lv * cfg.crit.multPerLevel + masteryCritMult + upgradeState.critMultLv * CRIT_UPGRADES.multPerLevel + trinket.critMult;
  const critChanceTotal = clamp(critChance + upgradeState.critChanceLv * CRIT_UPGRADES.chancePerLevel + trinket.critChance, 0, 1);
  if (weapons.missile.aug === "missile_swarm") {
    count += AUGMENT_CONFIG.missile.swarm.extra;
    dmg *= AUGMENT_CONFIG.missile.swarm.dmgMult;
  }
  return { dmg, cd, speed, maxSpeed, accel, turnRate, count, explosion, life, range, critChance: critChanceTotal, critMult, radius: cfg.projectile.radius };
}

export function turretStats() {
  const { buffs, upgradeState } = requireContext();
  const trinket = getTrinketMods();
  const cfg = WEAPON_CONFIG.turret;
  const lv = weapons.turret.level;
  const mastery = weapons.turret.mastery || 0;
  const masteryDmgMult = 1 + mastery * WEAPON_MASTERY.dmgMult;
  const masteryCrit = mastery * WEAPON_MASTERY.critChance;
  const masteryCritMult = mastery * WEAPON_MASTERY.critMult;
  const powerMul = (buffs.power > 0) ? cfg.powerDmgMult : 1.0;
  const cdMul = (buffs.power > 0) ? cfg.powerCdMult : 1.0;
  const dmg = (cfg.dmgBase + lv * cfg.dmgPerLevel) * powerMul * masteryDmgMult * trinket.dmg;
  const cdRaw = Math.max(cfg.cdMin, cfg.cdBase - lv * cfg.cdPerLevel) * cdMul;
  const cdReduce = Math.max(0.1, 1 - upgradeState.cdLv * UPGRADE_CONFIG.cdReduction);
  const cd = Math.max(cfg.cdMin, cdRaw * cdReduce * trinket.cd);
  const tick = Math.max(cfg.tickMin, cfg.tickBase + (lv - 1) * cfg.tickPerLevel);
  const life = cfg.lifeBase + lv * cfg.lifePerLevel;
  const range = cfg.rangeBase + lv * cfg.rangePerLevel;
  const width = cfg.widthBase + lv * cfg.widthPerLevel;
  const turnRate = (cfg.turnRateBaseDeg + lv * cfg.turnRatePerLevelDeg) * (Math.PI / 180);
  let count = 0;
  for (let i = 0; i < cfg.countLevels.length; i++) {
    if (lv >= cfg.countLevels[i]) count++;
  }
  count = Math.min(cfg.maxActive, count);
  const critChance = Math.min(1, cfg.crit.base + lv * cfg.crit.perLevel + masteryCrit);
  const critMult = cfg.crit.multBase + lv * cfg.crit.multPerLevel + masteryCritMult + upgradeState.critMultLv * CRIT_UPGRADES.multPerLevel + trinket.critMult;
  const critChanceTotal = clamp(critChance + upgradeState.critChanceLv * CRIT_UPGRADES.chancePerLevel + trinket.critChance, 0, 1);
  return { dmg, cd, tick, life, range, width, turnRate, count, critChance: critChanceTotal, critMult };
}

export function findNearestEnemy(px, py, maxDist) {
  let best = null;
  let bestD = maxDist * maxDist;
  for (let i=0;i<enemies.length;i++){
    const e = enemies[i];
    if (!e.alive) continue;
    const dx = e.x - px, dy = e.y - py;
    const d2 = dx*dx + dy*dy;
    if (d2 < bestD){ bestD = d2; best = e; }
  }
  return best;
}

function findNearestEnemyExcluding(px, py, maxDist, exclude) {
  let best = null;
  let bestD = maxDist * maxDist;
  for (let i=0;i<enemies.length;i++){
    const e = enemies[i];
    if (!e.alive || exclude.has(e)) continue;
    const dx = e.x - px, dy = e.y - py;
    const d2 = dx*dx + dy*dy;
    if (d2 < bestD){ bestD = d2; best = e; }
  }
  return best;
}

function buildArcPoints(path, jitter = 12, segments = 2) {
  const points = [];
  for (let i = 0; i < path.length - 1; i++) {
    const a = path[i];
    const b = path[i + 1];
    if (i === 0) points.push({ x: a.x, y: a.y });
    const dx = b.x - a.x;
    const dy = b.y - a.y;
    const len = hypot(dx, dy) || 1;
    const nx = -dy / len;
    const ny = dx / len;
    for (let s = 1; s <= segments; s++) {
      const t = s / (segments + 1);
      const off = rand(jitter, -jitter);
      points.push({ x: a.x + dx * t + nx * off, y: a.y + dy * t + ny * off });
    }
    points.push({ x: b.x, y: b.y });
  }
  return points;
}

function spawnArcFx(path, intensity = 1) {
  if (path.length < 2) return;
  const arc = arcPool.get();
  arc.alive = true;
  arc.points = buildArcPoints(path, 12, 2);
  arc.life = 0.18;
  arc.maxLife = arc.life;
  arc.color = COLORS.arc;
  arc.intensity = intensity;
  arcs.push(arc);
}

function fireMagicBullet(){
  const { damageEnemy } = requireRuntime();
  const s = magicStats();
  const target = findNearestEnemy(player.x, player.y, s.range);
  if (!target) return;
  sound.play("shoot");

  const dx = target.x - player.x;
  const dy = target.y - player.y;
  const baseAng = Math.atan2(dy, dx);
  const count = s.count;
  const spread = WEAPON_CONFIG.magic.projectile.spread;
  const prism = weapons.magic.aug === "magic_prism";

  const spawnBullet = (ang, dmgMult = 1) => {
    const b = bulletPool.get();
    b.alive = true;
    b.x = player.x; b.y = player.y;
    b.r = WEAPON_CONFIG.magic.projectile.radius;
    b.dmg = s.dmg * dmgMult;
    b.life = WEAPON_CONFIG.magic.projectile.life;
    b.maxLife = b.life;
    b.critChance = s.critChance;
    b.critMult = s.critMult;
    b.isExplosion = false;
    b.color = null;
    b.vx = Math.cos(ang) * s.speed;
    b.vy = Math.sin(ang) * s.speed;
    bullets.push(b);
  };

  for (let i=0;i<count;i++){
    const t = (count === 1) ? 0 : (i/(count-1) - 0.5);
    const ang = baseAng + t * spread;
    spawnBullet(ang, 1);
  }

  if (prism) {
    weapons.magic.augSeq = (weapons.magic.augSeq || 0) + 1;
    if (weapons.magic.augSeq % AUGMENT_CONFIG.magic.prism.every === 0) {
      const extraAng = AUGMENT_CONFIG.magic.prism.angle;
      spawnBullet(baseAng + extraAng, AUGMENT_CONFIG.magic.prism.dmgMult);
      spawnBullet(baseAng - extraAng, AUGMENT_CONFIG.magic.prism.dmgMult);
    }
  }
}

function fireArcLance(){
  const { damageEnemy, damageObstaclesInRadius, addParticles, spawnShockwave } = requireRuntime();
  const s = arcStats();
  const first = findNearestEnemy(player.x, player.y, s.range);
  if (!first) return;
  sound.play("shoot");

  let chainCount = Math.max(1, s.chains);
  let falloff = s.falloff;
  let intensity = 1;
  if (weapons.arc.aug === "arc_capacitor") {
    weapons.arc.augSeq = (weapons.arc.augSeq || 0) + 1;
    if (weapons.arc.augSeq % AUGMENT_CONFIG.arc.capacitor.every === 0) {
      chainCount += AUGMENT_CONFIG.arc.capacitor.extraChains;
      falloff = 1;
      intensity = 1.25;
    }
  }

  const hit = new Set();
  const path = [{ x: player.x, y: player.y }];
  let current = first;
  let dmg = s.dmg;
  let last = null;
  const slowCfg = WEAPON_CONFIG.arc.slow;

  for (let i = 0; i < chainCount && current; i++) {
    const origin = path[path.length - 1];
    const hitDx = current.x - origin.x;
    const hitDy = current.y - origin.y;
    const hitLength = Math.hypot(hitDx, hitDy) || 1;
    hit.add(current);
    path.push({ x: current.x, y: current.y });
    const crit = calcCrit(dmg, s.critChance, s.critMult);
    damageEnemy(current, crit.dmg, hitDx / hitLength, hitDy / hitLength, 0, true, crit.crit, "arc");
    if (slowCfg) applySlow(current, slowCfg.mult, slowCfg.duration);
    if (addParticles) addParticles(current.x, current.y, COLORS.arc, 6, 220);
    last = current;
    dmg *= falloff;
    current = findNearestEnemyExcluding(current.x, current.y, s.chainRange, hit);
  }

  spawnArcFx(path, intensity);

  if (weapons.arc.aug === "arc_grounded" && last) {
    const radius = AUGMENT_CONFIG.arc.grounded.radius;
    if (radius > 0) {
      const shockDmg = s.dmg * AUGMENT_CONFIG.arc.grounded.dmgMult;
      const r2 = radius * radius;
      for (let i = 0; i < enemies.length; i++) {
        const e = enemies[i];
        if (!e.alive || hit.has(e)) continue;
        const dx = e.x - last.x;
        const dy = e.y - last.y;
        if (dx * dx + dy * dy <= r2) {
          const crit = calcCrit(shockDmg, s.critChance, s.critMult);
          const length = Math.hypot(dx, dy) || 1;
          damageEnemy(e, crit.dmg, dx / length, dy / length, 0, true, crit.crit, "arc");
        }
      }
      damageObstaclesInRadius(last.x, last.y, radius, shockDmg, true);
      if (spawnShockwave) spawnShockwave(last.x, last.y, radius, COLORS.arc);
      if (addParticles) addParticles(last.x, last.y, COLORS.arc, 16, 420);
    }
  }
}

function fireRailShot(){
  const { addParticles } = requireRuntime();
  const s = railStats();
  const target = findNearestEnemy(player.x, player.y, s.range);
  if (!target) return;
  sound.play("rail");

  const dx = target.x - player.x;
  const dy = target.y - player.y;
  const ang = Math.atan2(dy, dx);

  const r = railPool.get();
  r.alive = true;
  r.x = player.x;
  r.y = player.y;
  r.r = WEAPON_CONFIG.rail.projectile.radius;
  r.dmg = s.dmg;
  r.critChance = s.critChance;
  r.critMult = s.critMult;
  r.pierce = s.pierce;
  r.life = s.range / s.speed;
  r.vx = Math.cos(ang) * s.speed;
  r.vy = Math.sin(ang) * s.speed;
  r.trail.length = 0;
  r.trail.push({ x:r.x, y:r.y, life:WEAPON_CONFIG.rail.projectile.trailLife });
  rails.push(r);

  addParticles(player.x, player.y, COLORS.rail, 10, 520);
}

function throwAxe(){
  const s = axeStats();
  const throwCfg = WEAPON_CONFIG.axe.throw;
  const target = findNearestEnemy(player.x, player.y, throwCfg.range);
  if (!target) return;
  sound.play("axe");

  const count = s.count;
  for (let i=0;i<count;i++){
    const a = axePool.get();
    a.alive = true;
    a.x = player.x;
    a.y = player.y;
    a.r = throwCfg.radius;
    a.dmg = s.dmg;
    a.critChance = s.critChance;
    a.critMult = s.critMult;
    a.life = throwCfg.life;
    a.returning = false;
    a.rot = rand(TAU, 0);
    a.spin = rand(throwCfg.spinMax, throwCfg.spinMin) * (Math.random() < throwCfg.spinInvertChance ? -1 : 1);

    const dx = target.x - player.x;
    const dy = target.y - player.y;
    const ang = Math.atan2(dy, dx) + rand(throwCfg.angleJitter, -throwCfg.angleJitter);

    const sp = s.speed * rand(throwCfg.speedJitterMax, throwCfg.speedJitterMin);
    a.vx = Math.cos(ang) * sp;
    a.vy = Math.sin(ang) * sp - rand(throwCfg.launchVyMax, throwCfg.launchVyMin);
    axes.push(a);
  }
}

function fireOrb(){
  const s = orbStats();
  const target = findNearestEnemy(player.x, player.y, s.range);
  if (!target) return;
  sound.play("shoot");

  const dx = target.x - player.x;
  const dy = target.y - player.y;
  const ang = Math.atan2(dy, dx);
  const o = orbPool.get();
  o.alive = true;
  o.state = "fly";
  o.x = player.x;
  o.y = player.y;
  o.r = 10;
  o.vx = Math.cos(ang) * s.speed;
  o.vy = Math.sin(ang) * s.speed;
  o.life = s.range / s.speed;
  o.park = s.park;
  o.tick = 0; // pulse immediately on park
  o.pull = s.pull;
  o.radius = s.radius;
  o.dmg = s.dmg;
  o.explosion = s.explosion;
  o.explosionRadius = s.explosionRadius || s.radius;
  o.critChance = s.critChance;
  o.critMult = s.critMult;
  orbs.push(o);
}

function fireMissile(){
  const s = missileStats();
  const spread = 0.18;
  sound.play("shoot");
  for (let i=0;i<s.count;i++){
    const baseAng = rand(TAU, 0); // launch randomly; homing picks nearest in flight
    const t = (s.count === 1) ? 0 : (i/(s.count-1) - 0.5);
    const ang = baseAng + t * spread;
    const m = missilePool.get();
    m.alive = true;
    m.x = player.x; m.y = player.y;
    m.r = WEAPON_CONFIG.missile.projectile.radius;
    m.dmg = s.dmg;
    m.life = s.life;
    m.speed = s.speed * 0.6;
    m.maxSpeed = s.maxSpeed;
    m.accel = s.accel;
    m.turnRate = s.turnRate;
    m.explosion = s.explosion;
    m.critChance = s.critChance;
    m.critMult = s.critMult;
    m.vx = Math.cos(ang) * m.speed;
    m.vy = Math.sin(ang) * m.speed;
    missiles.push(m);
  }
}

function angleDelta(target, current) {
  return ((target - current + Math.PI * 3) % TAU) - Math.PI;
}

function turretSpawnPoint(radius, bodyRadius, sequence) {
  const goldenAngle = 2.399963229728653;
  for (let attempt = 0; attempt < 10; attempt++) {
    const angle = sequence * goldenAngle + attempt * TAU / 10;
    const distance = radius + (attempt % 3) * 12;
    const point = clampPointToWorld(
      player.x + Math.cos(angle) * distance,
      player.y + Math.sin(angle) * distance,
      bodyRadius + 4
    );
    let clear = true;
    for (let i = 0; i < activeObstacles.length; i++) {
      const obstacle = obstacles[activeObstacles[i]];
      if (!obstacle) continue;
      const dx = point.x - obstacle.x;
      const dy = point.y - obstacle.y;
      const reach = bodyRadius + obstacle.r + 5;
      if (dx * dx + dy * dy < reach * reach) {
        clear = false;
        break;
      }
    }
    if (!clear) continue;
    for (let i = 0; i < turrets.length; i++) {
      const other = turrets[i];
      const dx = point.x - other.x;
      const dy = point.y - other.y;
      const reach = bodyRadius + other.r + 12;
      if (dx * dx + dy * dy < reach * reach) {
        clear = false;
        break;
      }
    }
    if (clear) return point;
  }
  return clampPointToWorld(player.x, player.y, bodyRadius + 4);
}

function turretTargetClaimed(enemy, self) {
  for (let i = 0; i < turrets.length; i++) {
    const turret = turrets[i];
    if (turret !== self && turret.alive && turret.target === enemy) return true;
  }
  return false;
}

function findTurretTarget(turret) {
  let best = null;
  let bestD2 = turret.range * turret.range;
  let fallback = null;
  let fallbackD2 = bestD2;
  for (let i = 0; i < enemies.length; i++) {
    const enemy = enemies[i];
    if (!enemy.alive) continue;
    const dx = enemy.x - turret.x;
    const dy = enemy.y - turret.y;
    const d2 = dx * dx + dy * dy;
    if (d2 >= fallbackD2) continue;
    fallback = enemy;
    fallbackD2 = d2;
    if (!turretTargetClaimed(enemy, turret) && d2 < bestD2) {
      best = enemy;
      bestD2 = d2;
    }
  }
  return best || fallback;
}

function turretElement() {
  if (weapons.turret.aug === "turret_thermite") return "fire";
  if (weapons.turret.aug === "turret_plague") return "poison";
  return "";
}

function removeTurretForSlot(slotIndex) {
  for (let i = turrets.length - 1; i >= 0; i--) {
    const old = turrets[i];
    if (old.slotIndex !== slotIndex) continue;
    turrets[i] = turrets[turrets.length - 1];
    turrets.pop();
    old.alive = false;
    old.target = null;
    turretPool.put(old);
    return;
  }
}

function hasActiveTurretForSlot(slotIndex) {
  for (let i = 0; i < turrets.length; i++) {
    if (turrets[i].alive && turrets[i].slotIndex === slotIndex) return true;
  }
  return false;
}

function deployTurret(slotIndex, s) {
  const cfg = WEAPON_CONFIG.turret;
  const sequence = ++weapons.turret.spawnSeq;
  const point = turretSpawnPoint(cfg.spawnRadius, cfg.bodyRadius, sequence);
  const turret = turretPool.get();
  turret.alive = true;
  turret.id = sequence;
  turret.slotIndex = slotIndex;
  turret.x = point.x;
  turret.y = point.y;
  turret.r = cfg.bodyRadius;
  turret.life = s.life;
  turret.maxLife = s.life;
  turret.range = s.range;
  turret.width = s.width;
  turret.streamLength = 0;
  turret.blockedObstacle = -1;
  turret.firing = false;
  turret.tick = 0;
  turret.tickRate = s.tick;
  turret.retarget = 0;
  turret.target = null;
  turret.dmg = s.dmg;
  turret.critChance = s.critChance;
  turret.critMult = s.critMult;
  turret.turnRate = s.turnRate;
  turret.element = turretElement();
  turret.phase = rand(TAU, 0);
  turret.poolT = 0;
  turret.angle = sequence * 2.399963229728653;
  turret.target = findTurretTarget(turret);
  if (turret.target) turret.angle = Math.atan2(turret.target.y - turret.y, turret.target.x - turret.x);
  turrets.push(turret);
  sound.play("shoot");
}

function updateTurretDeployments(dt) {
  const s = turretStats();
  const timers = weapons.turret.slotTimers;

  while (timers.length < s.count) timers.push(0);
  if (timers.length > s.count) {
    for (let slotIndex = s.count; slotIndex < timers.length; slotIndex++) {
      removeTurretForSlot(slotIndex);
    }
    timers.length = s.count;
  }

  for (let slotIndex = 0; slotIndex < timers.length; slotIndex++) {
    timers[slotIndex] = Math.max(0, timers[slotIndex] - dt);
    if (timers[slotIndex] <= 0 && !hasActiveTurretForSlot(slotIndex)) {
      timers[slotIndex] = s.cd;
      deployTurret(slotIndex, s);
    }
  }
}

function updateTurretStreamBlock(turret) {
  const ux = Math.cos(turret.angle);
  const uy = Math.sin(turret.angle);
  let length = turret.range;
  let blockedObstacle = -1;
  for (let i = 0; i < activeObstacles.length; i++) {
    const obstacleIndex = activeObstacles[i];
    const obstacle = obstacles[obstacleIndex];
    if (!obstacle || obstacle.type === "lake") continue;
    const dx = obstacle.x - turret.x;
    const dy = obstacle.y - turret.y;
    const along = dx * ux + dy * uy;
    if (along <= 0 || along >= length + obstacle.r) continue;
    const side = Math.abs(dx * uy - dy * ux);
    const streamHalfWidth = turret.width * (0.48 - 0.20 * clamp(along / turret.range, 0, 1));
    if (side > obstacle.r + streamHalfWidth) continue;
    const hitDistance = Math.max(turret.r + 3, along - obstacle.r);
    if (hitDistance < length) {
      length = hitDistance;
      blockedObstacle = obstacleIndex;
    }
  }
  turret.streamLength = length;
  turret.blockedObstacle = blockedObstacle;
}

function enemyInsideTurretStream(enemy, turret, ux, uy) {
  const dx = enemy.x - turret.x;
  const dy = enemy.y - turret.y;
  const along = dx * ux + dy * uy;
  if (along < -enemy.r || along > turret.streamLength + enemy.r) return false;
  const side = Math.abs(dx * uy - dy * ux);
  const t = clamp(along / Math.max(1, turret.streamLength), 0, 1);
  const halfWidth = turret.width * (0.48 - 0.20 * t);
  return side <= halfWidth + enemy.r;
}

function damageTurretStream(turret) {
  const { damageEnemy, damageObstacle } = requireRuntime();
  const ux = Math.cos(turret.angle);
  const uy = Math.sin(turret.angle);
  const aug = weapons.turret.aug;
  for (let i = 0; i < enemies.length; i++) {
    const enemy = enemies[i];
    if (!enemy.alive || !enemyInsideTurretStream(enemy, turret, ux, uy)) continue;
    const hit = calcCrit(turret.dmg, turret.critChance, turret.critMult);
    damageEnemy(enemy, hit.dmg, ux, uy, 0, hit.crit, hit.crit, "turret", true, "direct", turret.element);
    if (aug === "turret_thermite") {
      applyBurn(
        enemy,
        turret.dmg / Math.max(0.01, turret.tickRate),
        AUGMENT_CONFIG.turret.thermite.burn,
        "turret"
      );
    } else if (aug === "turret_plague") {
      applySlow(enemy, AUGMENT_CONFIG.turret.plague.slowMult, AUGMENT_CONFIG.turret.plague.slowDuration);
    }
  }
  if (turret.blockedObstacle >= 0) {
    const obstacle = obstacles[turret.blockedObstacle];
    if (obstacle?.type === "forest") damageObstacle(turret.blockedObstacle, turret.dmg, false);
  }
}

function countToxinPools(ownerId) {
  let count = 0;
  for (let i = 0; i < toxinPools.length; i++) {
    if (toxinPools[i].alive && toxinPools[i].ownerId === ownerId) count++;
  }
  return count;
}

function spawnToxinPool(turret) {
  const cfg = AUGMENT_CONFIG.turret.plague.pool;
  if (toxinPools.length >= WEAPON_CONFIG.turret.maxActive * cfg.maxPerTurret) return;
  if (countToxinPools(turret.id) >= cfg.maxPerTurret) return;
  const ux = Math.cos(turret.angle);
  const uy = Math.sin(turret.angle);
  let poolDistance = turret.streamLength;
  if (turret.target?.alive) {
    const targetAlong = (turret.target.x - turret.x) * ux + (turret.target.y - turret.y) * uy;
    poolDistance = clamp(targetAlong, turret.r * 1.5, turret.streamLength);
  }
  const pool = toxinPool.get();
  pool.alive = true;
  pool.ownerId = turret.id;
  pool.x = turret.x + ux * poolDistance;
  pool.y = turret.y + uy * poolDistance;
  pool.radius = cfg.radius;
  pool.life = cfg.life;
  pool.maxLife = cfg.life;
  pool.tick = 0;
  pool.tickRate = cfg.tick;
  pool.dmg = (turret.dmg / Math.max(0.01, turret.tickRate)) * cfg.dpsMult * cfg.tick;
  pool.critChance = turret.critChance;
  pool.critMult = turret.critMult;
  pool.phase = turret.phase;
  toxinPools.push(pool);
}

function updateTurrets(dt) {
  const cfg = WEAPON_CONFIG.turret;
  for (let i = turrets.length - 1; i >= 0; i--) {
    const turret = turrets[i];
    turret.life -= dt;
    turret.phase += dt * 5.2;
    turret.retarget -= dt;
    if (
      turret.retarget <= 0
      || !turret.target
      || !turret.target.alive
      || (turret.target.x - turret.x) ** 2 + (turret.target.y - turret.y) ** 2 > turret.range * turret.range
    ) {
      turret.target = findTurretTarget(turret);
      turret.retarget = cfg.retargetInterval;
    }

    if (turret.target) {
      const desired = Math.atan2(turret.target.y - turret.y, turret.target.x - turret.x);
      const delta = angleDelta(desired, turret.angle);
      const maxTurn = turret.turnRate * dt;
      turret.angle += clamp(delta, -maxTurn, maxTurn);
      turret.firing = Math.abs(delta) <= cfg.alignmentAngle;
    } else {
      turret.firing = false;
    }

    if (turret.firing) {
      updateTurretStreamBlock(turret);
      turret.tick -= dt;
      if (turret.tick <= 0) {
        turret.tick += turret.tickRate;
        damageTurretStream(turret);
      }
      if (weapons.turret.aug === "turret_plague") {
        turret.poolT -= dt;
        if (turret.poolT <= 0) {
          turret.poolT += AUGMENT_CONFIG.turret.plague.pool.interval;
          spawnToxinPool(turret);
        }
      }
    } else {
      turret.streamLength = Math.max(0, turret.streamLength - turret.range * dt * 7);
      turret.blockedObstacle = -1;
      turret.tick = 0;
      turret.poolT = Math.max(0, turret.poolT);
    }

    if (turret.life <= 0) turret.alive = false;
    if (!turret.alive) {
      turrets[i] = turrets[turrets.length - 1];
      turrets.pop();
      turret.target = null;
      turretPool.put(turret);
    }
  }
}

function updateToxinPools(dt) {
  const { damageEnemy } = requireRuntime();
  const slowCfg = AUGMENT_CONFIG.turret.plague;
  for (let i = toxinPools.length - 1; i >= 0; i--) {
    const pool = toxinPools[i];
    pool.life -= dt;
    pool.phase += dt * 1.7;
    pool.tick -= dt;
    if (pool.tick <= 0) {
      pool.tick += pool.tickRate;
      const r2 = pool.radius * pool.radius;
      for (let j = 0; j < enemies.length; j++) {
        const enemy = enemies[j];
        if (!enemy.alive) continue;
        const dx = enemy.x - pool.x;
        const dy = enemy.y - pool.y;
        const reach = pool.radius + enemy.r;
        if (dx * dx + dy * dy > Math.max(r2, reach * reach)) continue;
        const hit = calcCrit(pool.dmg, pool.critChance, pool.critMult);
        damageEnemy(enemy, hit.dmg, 0, 0, 0, hit.crit, hit.crit, "turret", true, "poison", "poison");
        applySlow(enemy, slowCfg.slowMult, slowCfg.slowDuration);
      }
    }
    if (pool.life <= 0) pool.alive = false;
    if (!pool.alive) {
      toxinPools[i] = toxinPools[toxinPools.length - 1];
      toxinPools.pop();
      toxinPool.put(pool);
    }
  }
}

export function updateWeapons(dt){
  const { damageEnemy, damageObstacle } = requireRuntime();

  if (weapons.magic.unlocked){
    weapons.magic.t -= dt;
    if (weapons.magic.t <= 0){
      weapons.magic.t += magicStats().cd;
      fireMagicBullet();
    }
  }

  if (weapons.arc.unlocked){
    weapons.arc.t -= dt;
    if (weapons.arc.t <= 0){
      weapons.arc.t += arcStats().cd;
      fireArcLance();
    }
  }

  if (weapons.aura.unlocked){
    const s = auraStats();
    const aug = weapons.aura.aug;
    if (weapons.aura.pulseFx > 0) {
      weapons.aura.pulseFx = Math.max(0, weapons.aura.pulseFx - dt);
    }
    weapons.aura.tick -= dt;
    if (weapons.aura.tick <= 0){
      weapons.aura.tick += s.tick;
      const r = s.radius, r2 = r*r;
      let hitCount = 0;
      let obstacleHit = false;
      for (let i=0;i<enemies.length;i++){
        const e = enemies[i];
        if (!e.alive) continue;
        const dx = e.x - player.x;
        const dy = e.y - player.y;
        const d2 = dx*dx + dy*dy;
        if (d2 <= (r2 + e.r*e.r)){
          const d = Math.sqrt(d2) || 1;
          const nx = dx / d, ny = dy / d;
          const hit = calcCrit(s.dmg, s.critChance, s.critMult);
          damageEnemy(e, hit.dmg, nx, ny, s.knock, true, hit.crit, "aura");
          hitCount++;
        }
      }
      for (let i=0;i<activeObstacles.length;i++){
        const idx = activeObstacles[i];
        const o = obstacles[idx];
        if (!o) continue;
        const dx = o.x - player.x;
        const dy = o.y - player.y;
        const reach = r + o.r;
        if (dx*dx + dy*dy <= reach*reach){
          damageObstacle(idx, s.dmg, false);
          obstacleHit = true;
        }
      }
      
      if (aug === "aura_leech" && hitCount > 0) {
        const heal = player.maxHp * AUGMENT_CONFIG.aura.leechPct;
        const hpBefore = player.hp;
        player.hp = Math.min(player.maxHp, player.hp + heal);
        triggerHealFx(player.hp - hpBefore, 0.45);
      }
    }

    if (aug === "aura_pulse") {
      weapons.aura.pulse -= dt;
      if (weapons.aura.pulse <= 0) {
        weapons.aura.pulse = AUGMENT_CONFIG.aura.pulse.cd;
        weapons.aura.pulseFx = weapons.aura.pulseFxMax || 0.25;
        const r = s.radius;
        const r2 = r * r;
        const pulseDmg = s.dmg * AUGMENT_CONFIG.aura.pulse.dmgMult;
        const pulseKnock = s.knock * AUGMENT_CONFIG.aura.pulse.knockMult;
        for (let i=0;i<enemies.length;i++){
          const e = enemies[i];
          if (!e.alive) continue;
          const dx = e.x - player.x;
          const dy = e.y - player.y;
          const d2 = dx*dx + dy*dy;
          if (d2 <= (r2 + e.r*e.r)){
            const d = Math.sqrt(d2) || 1;
            const nx = dx / d, ny = dy / d;
            const hit = calcCrit(pulseDmg, s.critChance, s.critMult);
            damageEnemy(e, hit.dmg, nx, ny, pulseKnock, true, hit.crit, "aura");
          }
        }
      }
    } else {
      weapons.aura.pulse = 0;
    }
  }

  if (weapons.rail.unlocked){
    weapons.rail.t -= dt;
    if (weapons.rail.t <= 0){
      weapons.rail.t += railStats().cd;
      fireRailShot();
    }
  }

  if (weapons.axe.unlocked){
    weapons.axe.t -= dt;
    if (weapons.axe.t <= 0){
      weapons.axe.t += axeStats().cd;
      throwAxe();
    }
  }

  if (weapons.orb.unlocked){
    weapons.orb.t -= dt;
    if (weapons.orb.t <= 0){
      weapons.orb.t += orbStats().cd;
      fireOrb();
    }
  }

  if (weapons.missile.unlocked){
    weapons.missile.t -= dt;
    if (weapons.missile.t <= 0){
      weapons.missile.t += missileStats().cd;
      fireMissile();
    }
  }

  if (weapons.turret.unlocked){
    updateTurretDeployments(dt);
  }

  updateTurrets(dt);
  updateToxinPools(dt);
  updateBullets(dt);
  updateMissiles(dt);
  updateRailShots(dt);
  updateAxes(dt);
  updateOrbs(dt);
  updateArcs(dt);
}

export function updateBullets(dt){
  const { damageEnemy, damageObstacle } = requireRuntime();
  const knock = magicStats().knock;
  const magicAug = weapons.magic.aug;
  for (let i=bullets.length-1;i>=0;i--){
    const b = bullets[i];
    if (!b.alive){ bullets[i] = bullets[bullets.length-1]; bullets.pop(); bulletPool.put(b); continue; }

    b.life -= dt;
    b.x += b.vx * dt;
    b.y += b.vy * dt;

    if (b.isExplosion){
      b.maxLife = b.maxLife || b.life;
      if (b.life <= 0){
        b.alive = false;
      }
      if (!b.alive){
        bullets[i] = bullets[bullets.length-1];
        bullets.pop();
        bulletPool.put(b);
      }
      continue;
    }

    // obstacle collision
    let blocked = false;
    for (let j=0;j<activeObstacles.length;j++){
      const idx = activeObstacles[j];
      const o = obstacles[idx];
      if (!o) continue;
      if (o.type === "lake") continue;
      const dx = o.x - b.x;
      const dy = o.y - b.y;
      const rr = o.r + b.r;
      if (dx*dx + dy*dy <= rr*rr){
        if (o.type === "forest"){
          damageObstacle(idx, b.dmg, false);
        }
        blocked = true;
        break;
      }
    }
    if (blocked){
      b.alive = false;
    }

    for (let j=0;j<enemies.length;j++){
      const e = enemies[j];
      if (!e.alive) continue;
      const dx = e.x - b.x;
      const dy = e.y - b.y;
      const rr = e.r + b.r;
      if (dx*dx + dy*dy <= rr*rr){
        const d = hypot(b.vx, b.vy) || 1;
        const px = b.vx / d, py = b.vy / d;
        const hit = calcCrit(b.dmg, b.critChance, b.critMult);
        damageEnemy(e, hit.dmg, px, py, knock, true, hit.crit, "magic");
        if (magicAug === "magic_cryo") {
          applySlow(e, AUGMENT_CONFIG.magic.slow.mult, AUGMENT_CONFIG.magic.slow.duration);
        }
        b.alive = false;
        break;
      }
    }

    if (b.life <= 0) b.alive = false;

    if (!b.alive){
      bullets[i] = bullets[bullets.length-1];
      bullets.pop();
      bulletPool.put(b);
    }
  }
}

export function updateMissiles(dt){
  const { damageEnemy, damageObstacle, damageObstaclesInRadius, addParticles, spawnShockwave } = requireRuntime();
  const s = missileStats();
  const missileAug = weapons.missile.aug;
  for (let i=missiles.length-1;i>=0;i--){
    const m = missiles[i];
    if (!m.alive){ missiles[i] = missiles[missiles.length-1]; missiles.pop(); missilePool.put(m); continue; }

    m.life -= dt;

    // steering
    const target = findNearestEnemy(m.x, m.y, s.range);
    let vx = m.vx, vy = m.vy;
    let speed = Math.max(1e-3, Math.hypot(vx, vy));
    let ang = Math.atan2(vy, vx);
    if (target){
      const dx = target.x - m.x;
      const dy = target.y - m.y;
      const desired = Math.atan2(dy, dx);
      let delta = ((desired - ang + TAU*1.5) % TAU) - Math.PI;
      const maxTurn = m.turnRate * dt;
      if (delta > maxTurn) delta = maxTurn;
      else if (delta < -maxTurn) delta = -maxTurn;
      ang += delta;
      speed = Math.min(m.maxSpeed, speed + m.accel * dt);
      m.vx = Math.cos(ang) * speed;
      m.vy = Math.sin(ang) * speed;
    } else {
      speed = Math.min(m.maxSpeed, speed + m.accel * dt * 0.3);
      m.vx = Math.cos(ang) * speed;
      m.vy = Math.sin(ang) * speed;
    }

    m.x += m.vx * dt;
    m.y += m.vy * dt;

    let hit = false;
    for (let j=0;j<activeObstacles.length;j++){
      const idx = activeObstacles[j];
      const o = obstacles[idx];
      if (!o) continue;
      if (o.type === "lake") continue;
      const dx = o.x - m.x;
      const dy = o.y - m.y;
      const rr = o.r + m.r;
      if (dx*dx + dy*dy <= rr*rr){
        if (o.type === "forest"){
          damageObstacle(idx, m.dmg, false);
        }
        hit = true;
        break;
      }
    }
    for (let j=0;j<enemies.length;j++){
      const e = enemies[j];
      if (!e.alive) continue;
      const dx = e.x - m.x;
      const dy = e.y - m.y;
      const rr = e.r + m.r;
      if (dx*dx + dy*dy <= rr*rr){
        hit = true;
        break;
      }
    }

    if (hit || m.life <= 0){
      const crit = calcCrit(m.dmg, m.critChance, m.critMult);
      const r = m.explosion;
      const r2 = r * r;
      damageObstaclesInRadius(m.x, m.y, r, crit.dmg, true);
      for (let j=0;j<enemies.length;j++){
        const e = enemies[j];
        if (!e.alive) continue;
        const dx = e.x - m.x;
        const dy = e.y - m.y;
        const d2 = dx*dx + dy*dy;
        if (d2 <= r2){
          const d = Math.sqrt(d2) || 1;
          damageEnemy(e, crit.dmg, dx / d, dy / d, 0, true, crit.crit, "missile");
          if (missileAug === "missile_concussive") {
            applySlow(e, AUGMENT_CONFIG.missile.concussive.slowMult, AUGMENT_CONFIG.missile.concussive.slowDuration);
            const nx = dx / d, ny = dy / d;
            const knock = AUGMENT_CONFIG.missile.concussive.knock;
            e.kx += nx * knock;
            e.ky += ny * knock;
            applyBurn(e, crit.dmg, AUGMENT_CONFIG.missile.concussive.burn, "missile");
          }
        }
      }
      addParticles(m.x, m.y, COLORS.missile, 14, 480);
      spawnShockwave(m.x, m.y, r, COLORS.missile);
      m.alive = false;
    }

    if (!m.alive || m.life <= 0){
      missiles[i] = missiles[missiles.length-1];
      missiles.pop();
      missilePool.put(m);
    }
  }
}

export function updateRailShots(dt){
  const { damageEnemy, damageObstacle } = requireRuntime();
  const s = railStats();
  const railAug = weapons.rail.aug;
  const trailLife = WEAPON_CONFIG.rail.projectile.trailLife;
  const trailMax = WEAPON_CONFIG.rail.projectile.trailMax;
  for (let i=rails.length-1;i>=0;i--){
    const r = rails[i];
    if (!r.alive){ rails[i] = rails[rails.length-1]; rails.pop(); railPool.put(r); continue; }

    r.life -= dt;
    r.x += r.vx * dt;
    r.y += r.vy * dt;

    // obstacles stop rails
    let blocked = false;
    for (let j=0;j<activeObstacles.length;j++){
      const idx = activeObstacles[j];
      const o = obstacles[idx];
      if (!o) continue;
      if (o.type === "lake") continue;
      const dx = o.x - r.x;
      const dy = o.y - r.y;
      const rr = o.r + r.r;
      if (dx*dx + dy*dy <= rr*rr){
        if (o.type === "forest"){
          damageObstacle(idx, r.dmg, false);
        }
        blocked = true;
        break;
      }
    }
    if (blocked){ r.alive = false; continue; }

    // fade existing trail nodes
    for (let t=r.trail.length-1;t>=0;t--){
      r.trail[t].life -= dt;
      if (r.trail[t].life <= 0) r.trail.splice(t,1);
    }
    // add a fresh node to keep the streak behind the projectile
    r.trail.unshift({ x:r.x, y:r.y, life:trailLife });
    if (r.trail.length > trailMax) r.trail.pop();

    for (let j=0;j<enemies.length;j++){
      const e = enemies[j];
      if (!e.alive) continue;
      const dx = e.x - r.x;
      const dy = e.y - r.y;
      const rr = e.r + r.r;
      if (dx*dx + dy*dy <= rr*rr){
        const d = hypot(r.vx, r.vy) || 1;
        const px = r.vx / d, py = r.vy / d;
        const hit = calcCrit(r.dmg, r.critChance, r.critMult);
        damageEnemy(e, hit.dmg, px, py, s.knock, true, hit.crit, "rail");
        if (railAug === "rail_fire") {
          applyBurn(e, hit.dmg, AUGMENT_CONFIG.rail.burn, "rail");
        }
        r.pierce--;
        if (r.pierce <= 0){ r.alive = false; break; }
      }
    }

    if (r.life <= 0) r.alive = false;

    if (!r.alive){
      rails[i] = rails[rails.length-1];
      rails.pop();
      railPool.put(r);
    }
  }
}

export function updateAxes(dt){
  const { damageEnemy, damageObstacle } = requireRuntime();
  const s = axeStats();
  const axeAug = weapons.axe.aug;
  for (let i=axes.length-1;i>=0;i--){
    const a = axes[i];
    if (!a.alive){ axes[i] = axes[axes.length-1]; axes.pop(); axePool.put(a); continue; }

    a.life -= dt;
    if (a.returning) {
      const dx = player.x - a.x;
      const dy = player.y - a.y;
      const d = hypot(dx, dy) || 1;
      const speed = s.speed;
      a.vx = (dx / d) * speed;
      a.vy = (dy / d) * speed;
    } else {
      a.vy += s.gravity * dt;
    }
    a.x += a.vx * dt;
    a.y += a.vy * dt;
    a.rot += a.spin * dt;

    // obstacle collision
    let axeBlocked = false;
    for (let j=0;j<activeObstacles.length;j++){
      const idx = activeObstacles[j];
      const o = obstacles[idx];
      if (!o) continue;
      if (o.type === "lake") continue;
      const dx = o.x - a.x;
      const dy = o.y - a.y;
      const rr = o.r + a.r;
      if (dx*dx + dy*dy <= rr*rr){
        if (o.type === "forest"){
          damageObstacle(idx, s.dmg, false);
        }
        axeBlocked = true;
        break;
      }
    }
    if (axeBlocked){ axes[i] = axes[axes.length-1]; axes.pop(); axePool.put(a); continue; }

    for (let j=0;j<enemies.length;j++){
      const e = enemies[j];
      if (!e.alive) continue;
      const dx = e.x - a.x;
      const dy = e.y - a.y;
      const rr = e.r + a.r;
      if (dx*dx + dy*dy <= rr*rr){
        const d = hypot(a.vx, a.vy) || 1;
        const px = a.vx / d, py = a.vy / d;
        const hit = calcCrit(a.dmg, a.critChance, a.critMult);
        damageEnemy(e, hit.dmg, px, py, s.knock, true, hit.crit, "axe");
        if (axeAug === "axe_bleed") {
          applyBleed(e, hit.dmg, AUGMENT_CONFIG.axe.bleed, "axe");
        }
        if (axeAug === "axe_boomerang" && !a.returning) {
          a.returning = true;
          a.dmg *= AUGMENT_CONFIG.axe.boomerang.dmgMult;
          const returnLife = WEAPON_CONFIG.axe.throw.life * AUGMENT_CONFIG.axe.boomerang.returnLifeMult;
          a.life = Math.max(a.life, returnLife);
        } else {
          a.life -= WEAPON_CONFIG.axe.throw.hitLifeLoss;
        }
        if (a.life <= 0){ a.alive = false; break; }
      }
    }

    if (a.life <= 0) a.alive = false;

    if (!a.alive){
      axes[i] = axes[axes.length-1];
      axes.pop();
      axePool.put(a);
    }
  }
}

export function updateOrbs(dt){
  const { damageEnemy, damageObstaclesInRadius, addParticles } = requireRuntime();
  for (let i=orbs.length-1;i>=0;i--){
    const o = orbs[i];
    if (!o.alive){ orbs[i] = orbs[orbs.length-1]; orbs.pop(); orbPool.put(o); continue; }

    if (o.state === "fly"){
      o.life -= dt;
      o.x += o.vx * dt;
      o.y += o.vy * dt;
      // collide with rocks/trees (ignore lakes)
      let blocked = false;
      for (let j=0;j<activeObstacles.length;j++){
        const ob = obstacles[activeObstacles[j]];
        if (!ob) continue;
        if (ob.type === "lake") continue;
        const dx = o.x - ob.x;
        const dy = o.y - ob.y;
        const rr = o.r + ob.r;
        if (dx*dx + dy*dy <= rr*rr){
          const dist = Math.sqrt(dx*dx + dy*dy) || 1;
          o.x = ob.x + (dx / dist) * (rr + 1);
          o.y = ob.y + (dy / dist) * (rr + 1);
          o.state = "park";
          o.tick = 0;
          blocked = true;
          break;
        }
      }
      if (blocked) continue;
      // pull while flying
      const r2 = o.radius * o.radius;
      for (let j=0;j<enemies.length;j++){
        const e = enemies[j];
        if (!e.alive) continue;
        const dx = e.x - o.x;
        const dy = e.y - o.y;
        const d2 = dx*dx + dy*dy;
        if (d2 <= r2){
          const d = Math.sqrt(d2) || 1;
          const nx = dx / d, ny = dy / d;
          const pull = o.pull * dt;
          e.kx -= nx * pull;
          e.ky -= ny * pull;
        }
      }
      if (o.life <= 0){
        o.state = "park";
        o.tick = 0;
      }
    } else if (o.state === "park"){
      o.park -= dt;
      o.tick -= dt;
      if (o.tick <= 0){
        o.tick += orbStats().tick;
        const r2 = o.radius * o.radius;
        for (let j=0;j<enemies.length;j++){
          const e = enemies[j];
          if (!e.alive) continue;
          const dx = e.x - o.x;
          const dy = e.y - o.y;
          const d2 = dx*dx + dy*dy;
          if (d2 <= r2){
            const d = Math.sqrt(d2) || 1;
            const nx = dx / d, ny = dy / d;
            const pull = o.pull * dt;
            e.kx -= nx * pull;
            e.ky -= ny * pull;
            const hit = calcCrit(o.dmg, o.critChance, o.critMult);
            damageEnemy(e, hit.dmg, nx, ny, 0, true, hit.crit, "orb");
          }
        }
        damageObstaclesInRadius(o.x, o.y, o.radius, o.dmg, false);
      }

      if (o.park <= 0){
        const expRadius = o.explosionRadius || o.radius;
        const r2 = expRadius * expRadius;
        for (let j=0;j<enemies.length;j++){
          const e = enemies[j];
          if (!e.alive) continue;
          const dx = e.x - o.x;
          const dy = e.y - o.y;
          const d2 = dx*dx + dy*dy;
          if (d2 <= r2){
            const hit = calcCrit(o.explosion, o.critChance, o.critMult);
            const d = Math.sqrt(d2) || 1;
            damageEnemy(e, hit.dmg, dx / d, dy / d, 0, true, hit.crit, "orb");
          }
        }
        damageObstaclesInRadius(o.x, o.y, expRadius, o.explosion, true);
        addParticles(o.x, o.y, COLORS.bullet, 48, 720);
        spawnShockwave(o.x, o.y, expRadius, COLORS.bullet);
        o.alive = false;
      }
    }

    if (!o.alive){
      orbs[i] = orbs[orbs.length-1];
      orbs.pop();
      orbPool.put(o);
    }
  }
}

export function updateArcs(dt){
  for (let i = arcs.length - 1; i >= 0; i--) {
    const a = arcs[i];
    if (!a.alive) { a.points.length = 0; arcs[i] = arcs[arcs.length - 1]; arcs.pop(); arcPool.put(a); continue; }
    a.life -= dt;
    if (a.life <= 0) a.alive = false;
    if (!a.alive) {
      a.points.length = 0;
      arcs[i] = arcs[arcs.length - 1];
      arcs.pop();
      arcPool.put(a);
    }
  }
}
