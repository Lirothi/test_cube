import { WEAPON_CONFIG, WEAPON_MASTERY, UPGRADE_CONFIG, CRIT_UPGRADES } from "./config.js";
import { COLORS } from "./colors.js";
import { clamp, rand, TAU, hypot } from "./math.js";
import { sound } from "./audio.js";
import {
  player,
  enemies,
  activeObstacles,
  obstacles,
  bullets,
  rails,
  axes,
  orbs,
  missiles,
} from "./state.js";
import {
  bulletPool,
  railPool,
  axePool,
  orbPool,
  missilePool,
} from "./pools.js";

export const weapons = {
  magic: { unlocked: true, level: 1, mastery: 0, t: 0 },
  aura: { unlocked: false, level: 0, mastery: 0, tick: 0 },
  rail: { unlocked: false, level: 0, mastery: 0, t: 0 },
  axe: { unlocked: false, level: 0, mastery: 0, t: 0 },
  orb: { unlocked: false, level: 0, mastery: 0, t: 0 },
  missile: { unlocked: false, level: 0, mastery: 0, t: 0 },
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

function calcCrit(dmg, chance, mult){
  const crit = Math.random() < chance;
  return { dmg: crit ? dmg * mult : dmg, crit };
}

export function resetWeapons() {
  weapons.magic.unlocked = true; weapons.magic.level = 1; weapons.magic.mastery = 0; weapons.magic.t = 0;
  weapons.aura.unlocked = false; weapons.aura.level = 0; weapons.aura.mastery = 0; weapons.aura.tick = 0;
  weapons.rail.unlocked = false; weapons.rail.level = 0; weapons.rail.mastery = 0; weapons.rail.t = 0;
  weapons.axe.unlocked = false; weapons.axe.level = 0; weapons.axe.mastery = 0; weapons.axe.t = 0;
  weapons.orb.unlocked = false; weapons.orb.level = 0; weapons.orb.mastery = 0; weapons.orb.t = 0;
  weapons.missile.unlocked = false; weapons.missile.level = 0; weapons.missile.mastery = 0; weapons.missile.t = 0;
}

export function weaponCount() {
  let c = 0;
  if (weapons.magic.unlocked) c++;
  if (weapons.aura.unlocked) c++;
  if (weapons.rail.unlocked) c++;
  if (weapons.axe.unlocked) c++;
  if (weapons.orb.unlocked) c++;
  if (weapons.missile.unlocked) c++;
  return c;
}

export function magicStats() {
  const { buffs, upgradeState } = requireContext();
  const cfg = WEAPON_CONFIG.magic;
  const lv = weapons.magic.level;
  const mastery = weapons.magic.mastery || 0;
  const masteryDmgMult = 1 + mastery * WEAPON_MASTERY.dmgMult;
  const masteryCrit = mastery * WEAPON_MASTERY.critChance;
  const masteryCritMult = mastery * WEAPON_MASTERY.critMult;
  const powerMul = (buffs.power > 0) ? cfg.powerDmgMult : 1.0;
  const cdMul = (buffs.power > 0) ? cfg.powerCdMult : 1.0;
  const dmg = (cfg.dmgBase + lv * cfg.dmgPerLevel) * powerMul * masteryDmgMult;
  const cdRaw = Math.max(cfg.cdMin, cfg.cdBase - lv * cfg.cdPerLevel) * cdMul;
  const cdReduce = Math.max(0.1, 1 - upgradeState.cdLv * UPGRADE_CONFIG.cdReduction);
  const cd = Math.max(cfg.cdMin, cdRaw * cdReduce);
  const speed = cfg.speedBase + lv * cfg.speedPerLevel;
  const count = 1 + Math.floor((lv - 1) / cfg.countInterval);
  const range = cfg.range;
  const knock = (cfg.knockBase + lv * cfg.knockPerLevel) * powerMul;
  const critChance = Math.min(1, cfg.crit.base + lv * cfg.crit.perLevel + masteryCrit);
  const critMult = cfg.crit.multBase + lv * cfg.crit.multPerLevel + masteryCritMult + upgradeState.critMultLv * CRIT_UPGRADES.multPerLevel;
  const critChanceTotal = clamp(critChance + upgradeState.critChanceLv * CRIT_UPGRADES.chancePerLevel, 0, 1);
  return { dmg, cd, speed, count, range, knock, critChance: critChanceTotal, critMult };
}

export function auraStats() {
  const { buffs, upgradeState } = requireContext();
  const cfg = WEAPON_CONFIG.aura;
  const lv = weapons.aura.level;
  const mastery = weapons.aura.mastery || 0;
  const masteryDmgMult = 1 + mastery * WEAPON_MASTERY.dmgMult;
  const masteryCrit = mastery * WEAPON_MASTERY.critChance;
  const masteryCritMult = mastery * WEAPON_MASTERY.critMult;
  const powerMul = (buffs.power > 0) ? cfg.powerDmgMult : 1.0;
  const radius = cfg.radiusBase + lv * cfg.radiusPerLevel;
  const tick = cfg.tick;
  const dmg = (cfg.dmgBase + lv * cfg.dmgPerLevel) * powerMul * masteryDmgMult;
  const knock = (cfg.knockBase + lv * cfg.knockPerLevel) * powerMul;
  const critChance = Math.min(1, cfg.crit.base + lv * cfg.crit.perLevel + masteryCrit);
  const critMult = cfg.crit.multBase + lv * cfg.crit.multPerLevel + masteryCritMult + upgradeState.critMultLv * CRIT_UPGRADES.multPerLevel;
  const critChanceTotal = clamp(critChance + upgradeState.critChanceLv * CRIT_UPGRADES.chancePerLevel, 0, 1);
  return { radius, tick, dmg, knock, critChance: critChanceTotal, critMult };
}

export function axeStats() {
  const { buffs, upgradeState } = requireContext();
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
  const cd = Math.max(cfg.cdMin, cdRaw * cdReduce);
  const dmg = (cfg.dmgBase + lv * cfg.dmgPerLevel) * powerMul * masteryDmgMult;
  const speed = cfg.speedBase + lv * cfg.speedPerLevel;
  const count = 1 + Math.floor((lv - 1) / cfg.countInterval);
  const gravity = cfg.gravity;
  const knock = (cfg.knockBase + lv * cfg.knockPerLevel) * powerMul;
  const critChance = Math.min(1, cfg.crit.base + lv * cfg.crit.perLevel + masteryCrit);
  const critMult = cfg.crit.multBase + lv * cfg.crit.multPerLevel + masteryCritMult + upgradeState.critMultLv * CRIT_UPGRADES.multPerLevel;
  const critChanceTotal = clamp(critChance + upgradeState.critChanceLv * CRIT_UPGRADES.chancePerLevel, 0, 1);
  return { cd, dmg, speed, count, gravity, knock, critChance: critChanceTotal, critMult };
}

export function railStats() {
  const { buffs, upgradeState } = requireContext();
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
  const cd = Math.max(cfg.cdMin, cdRaw * cdReduce);
  const dmg = (cfg.dmgBase + lv * cfg.dmgPerLevel) * powerMul * masteryDmgMult;
  const speed = cfg.speedBase + lv * cfg.speedPerLevel;
  const pierce = cfg.pierceBase + Math.floor((lv + 1) / cfg.pierceLevelDivisor) + (buffs.power > 0 ? cfg.powerPierceBonus : 0);
  const range = cfg.rangeBase + lv * cfg.rangePerLevel;
  const knock = (cfg.knockBase + lv * cfg.knockPerLevel) * powerMul;
  const critChance = Math.min(1, cfg.crit.base + lv * cfg.crit.perLevel + masteryCrit);
  const critMult = cfg.crit.multBase + lv * cfg.crit.multPerLevel + masteryCritMult + upgradeState.critMultLv * CRIT_UPGRADES.multPerLevel;
  const critChanceTotal = clamp(critChance + upgradeState.critChanceLv * CRIT_UPGRADES.chancePerLevel, 0, 1);
  return { cd, dmg, speed, pierce, range, knock, critChance: critChanceTotal, critMult };
}

export function orbStats() {
  const { buffs, upgradeState } = requireContext();
  const cfg = WEAPON_CONFIG.orb;
  const lv = weapons.orb.level;
  const mastery = weapons.orb.mastery || 0;
  const masteryDmgMult = 1 + mastery * WEAPON_MASTERY.dmgMult;
  const masteryCrit = mastery * WEAPON_MASTERY.critChance;
  const masteryCritMult = mastery * WEAPON_MASTERY.critMult;
  const powerMul = (buffs.power > 0) ? cfg.powerDmgMult : 1.0;
  const cdMul = (buffs.power > 0) ? cfg.powerCdMult : 1.0;
  const dmg = (cfg.dmgBase + lv * cfg.dmgPerLevel) * powerMul * masteryDmgMult;
  const cdRaw = Math.max(cfg.cdMin, cfg.cdBase - lv * cfg.cdPerLevel) * cdMul;
  const cdReduce = Math.max(0.1, 1 - upgradeState.cdLv * UPGRADE_CONFIG.cdReduction);
  const cd = Math.max(cfg.cdMin, cdRaw * cdReduce);
  const speed = cfg.speedBase + lv * cfg.speedPerLevel;
  const radius = cfg.pullRadiusBase + lv * cfg.pullRadiusPerLevel;
  const pull = cfg.pullBase + lv * cfg.pullPerLevel;
  const critChance = Math.min(1, cfg.crit.base + lv * cfg.crit.perLevel + masteryCrit);
  const critMult = cfg.crit.multBase + lv * cfg.crit.multPerLevel + masteryCritMult + upgradeState.critMultLv * CRIT_UPGRADES.multPerLevel;
  const critChanceTotal = clamp(critChance + upgradeState.critChanceLv * CRIT_UPGRADES.chancePerLevel, 0, 1);
  const explosion = dmg * cfg.explosionMult;
  const park = cfg.parkTimeBase + lv * cfg.parkTimePerLevel;
  return { dmg, cd, speed, radius, pull, range: cfg.range, tick: cfg.tick, park, explosion, critChance: critChanceTotal, critMult };
}

export function missileStats() {
  const { buffs, upgradeState } = requireContext();
  const cfg = WEAPON_CONFIG.missile;
  const lv = weapons.missile.level;
  const mastery = weapons.missile.mastery || 0;
  const masteryDmgMult = 1 + mastery * WEAPON_MASTERY.dmgMult;
  const masteryCrit = mastery * WEAPON_MASTERY.critChance;
  const masteryCritMult = mastery * WEAPON_MASTERY.critMult;
  const powerMul = (buffs.power > 0) ? cfg.powerDmgMult || 1 : 1;
  const dmg = (cfg.dmgBase + lv * cfg.dmgPerLevel) * powerMul * masteryDmgMult;
  const cdRaw = Math.max(cfg.cdMin, cfg.cdBase - lv * cfg.cdPerLevel);
  const cdReduce = Math.max(0.1, 1 - upgradeState.cdLv * UPGRADE_CONFIG.cdReduction);
  const cd = Math.max(cfg.cdMin, cdRaw * cdReduce);
  const speed = cfg.speedBase + lv * cfg.speedPerLevel;
  const maxSpeed = speed * cfg.maxSpeedMult;
  const accel = cfg.accel;
  const turnRate = (cfg.turnRateDeg || 180) * (Math.PI / 180) * (1 + 0.04 * lv); // improved homing per level
  const count = 1 + Math.floor((lv - 1) / cfg.countInterval);
  const explosion = cfg.explosionRadiusBase + lv * cfg.explosionRadiusPerLevel;
  const life = cfg.life;
  const range = cfg.rangeBase + lv * (cfg.rangePerLevel || 0);
  const critChance = Math.min(1, cfg.crit.base + lv * cfg.crit.perLevel + masteryCrit);
  const critMult = cfg.crit.multBase + lv * cfg.crit.multPerLevel + masteryCritMult + upgradeState.critMultLv * CRIT_UPGRADES.multPerLevel;
  const critChanceTotal = clamp(critChance + upgradeState.critChanceLv * CRIT_UPGRADES.chancePerLevel, 0, 1);
  return { dmg, cd, speed, maxSpeed, accel, turnRate, count, explosion, life, range, critChance: critChanceTotal, critMult, radius: cfg.projectile.radius };
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

  for (let i=0;i<count;i++){
    const b = bulletPool.get();
    b.alive = true;
    b.x = player.x; b.y = player.y;
    b.r = WEAPON_CONFIG.magic.projectile.radius;
    b.dmg = s.dmg;
    b.life = WEAPON_CONFIG.magic.projectile.life;
    b.maxLife = b.life;
    b.critChance = s.critChance;
    b.critMult = s.critMult;
    b.isExplosion = false;
    b.color = null;

    const t = (count === 1) ? 0 : (i/(count-1) - 0.5);
    const ang = baseAng + t * spread;

    b.vx = Math.cos(ang) * s.speed;
    b.vy = Math.sin(ang) * s.speed;
    bullets.push(b);
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

export function updateWeapons(dt){
  const { damageEnemy, damageObstacle } = requireRuntime();

  if (weapons.magic.unlocked){
    weapons.magic.t -= dt;
    if (weapons.magic.t <= 0){
      weapons.magic.t += magicStats().cd;
      fireMagicBullet();
    }
  }

  if (weapons.aura.unlocked){
    const s = auraStats();
    weapons.aura.tick -= dt;
    if (weapons.aura.tick <= 0){
      weapons.aura.tick += s.tick;
      const r = s.radius, r2 = r*r;
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
        }
      }
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

  updateBullets(dt);
  updateMissiles(dt);
  updateRailShots(dt);
  updateAxes(dt);
  updateOrbs(dt);
}

export function updateBullets(dt){
  const { damageEnemy, damageObstacle } = requireRuntime();
  const knock = magicStats().knock;
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
          damageEnemy(e, crit.dmg, 0, 0, 0, true, crit.crit, "missile");
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
  for (let i=axes.length-1;i>=0;i--){
    const a = axes[i];
    if (!a.alive){ axes[i] = axes[axes.length-1]; axes.pop(); axePool.put(a); continue; }

    a.life -= dt;
    a.vy += s.gravity * dt;
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
        a.life -= WEAPON_CONFIG.axe.throw.hitLifeLoss;
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
            damageEnemy(e, hit.dmg, 0, 0, 0, true, hit.crit, "orb");
          }
        }
        damageObstaclesInRadius(o.x, o.y, o.radius, o.dmg, false);
      }

      if (o.park <= 0){
        const r2 = o.radius * o.radius;
        for (let j=0;j<enemies.length;j++){
          const e = enemies[j];
          if (!e.alive) continue;
          const dx = e.x - o.x;
          const dy = e.y - o.y;
          if (dx*dx + dy*dy <= r2){
            const hit = calcCrit(o.explosion, o.critChance, o.critMult);
            damageEnemy(e, hit.dmg, 0, 0, 0, true, hit.crit, "orb");
          }
        }
        damageObstaclesInRadius(o.x, o.y, o.radius, o.explosion, true);
        addParticles(o.x, o.y, COLORS.bullet, 48, 720);
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
