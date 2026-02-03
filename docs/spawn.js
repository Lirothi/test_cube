import {
  ENEMY_TIERS,
  ENEMY_TYPES,
  SPAWN_CONFIG,
  ELITE_CONFIG,
  RANGED_SHOT_CONFIG,
  TELEGRAPH_CONFIG,
  BOSS_CONFIG,
  BOSS2_CONFIG,
  BOSS3_CONFIG,
  BOSS4_CONFIG,
  BOSS5_CONFIG,
} from "./config.js";
import { COLORS } from "./colors.js";
import { clamp, rand, randi } from "./math.js";
import { sound } from "./audio.js";
import { addTelegraph } from "./telegraph.js";
import {
  WORLD,
  player,
  enemies,
  spawn,
  activeObstacles,
  obstacles,
  clampPointToWorld,
  clampEntityToWorld,
} from "./state.js";
import { enemyPool } from "./pools.js";

export function getEnemyTier(t) {
  let tier = ENEMY_TIERS[0];
  for (let i = 0; i < ENEMY_TIERS.length; i++) {
    if (t >= ENEMY_TIERS[i].time) tier = ENEMY_TIERS[i];
    else break;
  }
  return tier;
}

export function isBlockedByObstacle(x, y, r = 0, pad = 0) {
  const rad = r + pad;
  for (let i = 0; i < obstacles.length; i++) {
    const o = obstacles[i];
    if (!o) continue;
    const reach = o.r + rad;
    const dx = o.x - x, dy = o.y - y;
    if (dx * dx + dy * dy <= reach * reach) return true;
  }
  return false;
}

export function pickSpawnPos(camX, camY, W, H, opts = {}) {
  const padExtra = opts.allowOutside ? WORLD.spawnPad + 200 : 0;
  const pad = WORLD.spawnPad + padExtra;
  const minX = camX - pad, maxX = camX + W + pad;
  const minY = camY - pad, maxY = camY + H + pad;
  const tries = opts.tries || 14;
  const radius = opts.radius || 12;
  let last = { x: minX, y: minY };

  for (let t = 0; t < tries; t++) {
    const side = randi(4);
    let x, y;
    if (side === 0) { x = rand(maxX, minX); y = minY; }
    else if (side === 1) { x = maxX; y = rand(maxY, minY); }
    else if (side === 2) { x = rand(maxX, minX); y = maxY; }
    else { x = minX; y = rand(maxY, minY); }

    const candidate = opts.allowOutside ? { x, y } : clampPointToWorld(x, y, radius);
    last = candidate;
    if (!opts.avoidObstacles || !isBlockedByObstacle(candidate.x, candidate.y, radius, 6)) return candidate;
  }
  return last;
}

export function getActiveBoss(){
  if (spawn.bossRef && spawn.bossRef.alive) return spawn.bossRef;
  for (let i=0;i<enemies.length;i++){
    const e = enemies[i];
    if (e.alive && e.boss){
      spawn.bossRef = e;
      return e;
    }
  }
  spawn.bossRef = null;
  return null;
}

function getBossTelegraph(typeKey){
  if (typeKey === "X") return BOSS_CONFIG.telegraph;
  if (typeKey === "Y") return BOSS2_CONFIG.telegraph;
  if (typeKey === "Z") return BOSS3_CONFIG.telegraph;
  if (typeKey === "W") return BOSS4_CONFIG.telegraph;
  if (typeKey === "Q") return BOSS5_CONFIG.telegraph;
  return BOSS_CONFIG.telegraph;
}

function spawnBossWithTelegraph(typeKey, camX, camY, W, H, hpMult, spdMult, dmgMult) {
  const info = ENEMY_TYPES[typeKey];
  if (!info) return;
  const pos = pickSpawnPos(camX, camY, W, H, { allowOutside:true, radius: info.r, avoidObstacles:true });
  const tele = getBossTelegraph(typeKey);
  addTelegraph({
    x: pos.x, y: pos.y,
    radius: tele.radius,
    color: tele.color,
    time: tele.time,
    fire: () => {
      spawn.bossAlive = true;
      spawnEnemy(typeKey, camX, camY, W, H, hpMult, spdMult, dmgMult, false, pos);
      sound.play("boss");
    }
  });
}

function spawnBossPair(camX, camY, W, H, hpMult, spdMult, dmgMult){
  const pool = ["X", "Y", "Z", "W"];
  if (pool.length === 0) return;
  const first = pool[randi(pool.length)];
  let second = pool[randi(pool.length)];
  if (pool.length > 1) {
    while (second === first) second = pool[randi(pool.length)];
  }
  spawnBossWithTelegraph(first, camX, camY, W, H, hpMult, spdMult, dmgMult);
  spawnBossWithTelegraph(second, camX, camY, W, H, hpMult, spdMult, dmgMult);
}

function spawnMixedSquad(t, camX, camY, W, H, hpMult, spdMult, dmgMult){
  // Spawn a small mixed pack to keep composition varied.
  if (enemies.length >= spawn.maxEnemies - SPAWN_CONFIG.squadReserve) return;
  const pool = ["A", "B"];
  if (t > SPAWN_CONFIG.mixedPoolTimes.extraFast) pool.push("B");
  if (t > SPAWN_CONFIG.mixedPoolTimes.ranged) { pool.push("R"); pool.push("M"); }
  if (t > SPAWN_CONFIG.mixedPoolTimes.tank) pool.push("C");
  if (t > SPAWN_CONFIG.mixedPoolTimes.brute) pool.push("S");
  if (t > SPAWN_CONFIG.mixedPoolTimes.void){ pool.push("P"); pool.push("F"); pool.push("V"); }
  const count = clamp(
    SPAWN_CONFIG.mixedCount.base + Math.floor(t / SPAWN_CONFIG.mixedCount.scaleTime),
    SPAWN_CONFIG.mixedCount.base,
    SPAWN_CONFIG.mixedCount.max
  );
  for (let i=0;i<count;i++){
    const pick = pool[randi(pool.length)];
    spawnEnemy(
      pick,
      camX,
      camY,
      W,
      H,
      hpMult * SPAWN_CONFIG.mixedStatMult.hp,
      spdMult * SPAWN_CONFIG.mixedStatMult.speed,
      dmgMult * SPAWN_CONFIG.mixedStatMult.dmg
    );
  }
}

function pickEliteType(t) {
  const pool = ["A", "B"];
  if (t > SPAWN_CONFIG.thresholds.ranged) { pool.push("R"); pool.push("M"); }
  if (t > SPAWN_CONFIG.thresholds.tank) pool.push("C");
  if (t > SPAWN_CONFIG.thresholds.brute) pool.push("S");
  if (t > SPAWN_CONFIG.thresholds.void) { pool.push("P"); pool.push("F"); pool.push("V"); }
  return pool[randi(pool.length)];
}

function pickVoidType() {
  if (Math.random() < (SPAWN_CONFIG.voids.voidBias || 0)) return "V";
  return (Math.random() < SPAWN_CONFIG.voids.fireBias) ? "F" : "P";
}

export function spawnElitePackAt(x, y, count, radiusMin = 80, radiusMax = 180) {
  const t = player.time;
  const tier = getEnemyTier(t);
  const hpMult = (1 + t * SPAWN_CONFIG.scaling.hp) * tier.hpMult;
  const spdMult = (1 + t * SPAWN_CONFIG.scaling.speed) * tier.speedMult;
  const dmgMult = (1 + t * SPAWN_CONFIG.scaling.dmg) * tier.dmgMult;

  for (let i = 0; i < count; i++) {
    const typeKey = pickEliteType(t);
    let pos = clampPointToWorld(x, y, ENEMY_TYPES[typeKey].r);
    for (let tries = 0; tries < 8; tries++) {
      const ang = rand(Math.PI * 2, 0);
      const dist = rand(radiusMax, radiusMin);
      const px = x + Math.cos(ang) * dist;
      const py = y + Math.sin(ang) * dist;
      const candidate = clampPointToWorld(px, py, ENEMY_TYPES[typeKey].r);
      if (!isBlockedByObstacle(candidate.x, candidate.y, ENEMY_TYPES[typeKey].r, 6)) {
        pos = candidate;
        break;
      }
    }
    spawnEnemy(typeKey, 0, 0, 0, 0, hpMult, spdMult, dmgMult, true, pos);
  }
}

export function spawnController(dt, camX, camY, W, H){
  spawn.squadT -= dt;
  spawn.eliteT -= dt;

  const t = player.time;
  const tier = getEnemyTier(t);
  const rate = (spawn.baseRate + t * spawn.timeScale) * tier.spawnRateMult;
  spawn.acc += dt * rate;
  spawn.maxEnemies = tier.maxEnemies;
  const bossHpMult = (1 + t * SPAWN_CONFIG.scaling.hp) * tier.hpMult;
  const bossSpdMult = (1 + t * SPAWN_CONFIG.scaling.speed) * tier.speedMult;
  const bossDmgMult = (1 + t * SPAWN_CONFIG.scaling.dmg) * tier.dmgMult;

  if (!spawn.bossSpawned && t >= BOSS_CONFIG.spawnTime){
    spawn.bossSpawned = true;
    const pos = pickSpawnPos(camX, camY, W, H, { allowOutside:true, radius: ENEMY_TYPES.X.r, avoidObstacles:true });
    addTelegraph({
      x: pos.x, y: pos.y,
      radius: BOSS_CONFIG.telegraph.radius,
      color: BOSS_CONFIG.telegraph.color,
      time: BOSS_CONFIG.telegraph.time,
      fire: () => {
        spawn.bossAlive = true;
        spawnEnemy("X", camX, camY, W, H, bossHpMult, bossSpdMult, bossDmgMult, false, pos);
        sound.play("boss");
      }
    });
  }
  if (!spawn.boss2Spawned && t >= BOSS2_CONFIG.spawnTime){
    spawn.boss2Spawned = true;
    const pos = pickSpawnPos(camX, camY, W, H, { allowOutside:true, radius: ENEMY_TYPES.Y.r, avoidObstacles:true });
    addTelegraph({
      x: pos.x, y: pos.y,
      radius: BOSS2_CONFIG.telegraph.radius,
      color: BOSS2_CONFIG.telegraph.color,
      time: BOSS2_CONFIG.telegraph.time,
      fire: () => {
        spawn.bossAlive = true;
        spawnEnemy("Y", camX, camY, W, H, bossHpMult, bossSpdMult, bossDmgMult, false, pos);
        sound.play("boss");
      }
    });
  }
  if (!spawn.boss3Spawned && t >= BOSS3_CONFIG.spawnTime){
    spawn.boss3Spawned = true;
    const pos = pickSpawnPos(camX, camY, W, H, { allowOutside:true, radius: ENEMY_TYPES.Z.r, avoidObstacles:true });
    addTelegraph({
      x: pos.x, y: pos.y,
      radius: BOSS3_CONFIG.telegraph.radius,
      color: BOSS3_CONFIG.telegraph.color,
      time: BOSS3_CONFIG.telegraph.time,
      fire: () => {
        spawn.bossAlive = true;
        spawnEnemy("Z", camX, camY, W, H, bossHpMult, bossSpdMult, bossDmgMult, false, pos);
        sound.play("boss");
      }
    });
  }
  if (!spawn.boss4Spawned && t >= BOSS4_CONFIG.spawnTime){
    spawn.boss4Spawned = true;
    const pos = pickSpawnPos(camX, camY, W, H, { allowOutside:true, radius: ENEMY_TYPES.W.r, avoidObstacles:true });
    addTelegraph({
      x: pos.x, y: pos.y,
      radius: BOSS4_CONFIG.telegraph.radius,
      color: BOSS4_CONFIG.telegraph.color,
      time: BOSS4_CONFIG.telegraph.time,
      fire: () => {
        spawn.bossAlive = true;
        spawnEnemy("W", camX, camY, W, H, bossHpMult, bossSpdMult, bossDmgMult, false, pos);
        sound.play("boss");
      }
    });
  }
  if (!spawn.boss5Spawned && t >= BOSS5_CONFIG.spawnTime){
    spawn.boss5Spawned = true;
    const pos = pickSpawnPos(camX, camY, W, H, { allowOutside:true, radius: ENEMY_TYPES.Q.r, avoidObstacles:true });
    addTelegraph({
      x: pos.x, y: pos.y,
      radius: BOSS5_CONFIG.telegraph.radius,
      color: BOSS5_CONFIG.telegraph.color,
      time: BOSS5_CONFIG.telegraph.time,
      fire: () => {
        spawn.bossAlive = true;
        spawnEnemy("Q", camX, camY, W, H, bossHpMult, bossSpdMult, bossDmgMult, false, pos);
        sound.play("boss");
      }
    });
  }
  if (spawn.parallaxDefeated) {
    spawn.bossPairT -= dt;
    if (spawn.bossPairT <= 0) {
      spawn.bossPairT += (spawn.bossPairInterval || 180);
      spawnBossPair(camX, camY, W, H, bossHpMult, bossSpdMult, bossDmgMult);
    }
  }

  if (enemies.length >= spawn.maxEnemies) return;

  // Requested change:
  //  - less HP scaling
  //  - more damage scaling
  const hpMult  = (1 + t * SPAWN_CONFIG.scaling.hp) * tier.hpMult;
  const spdMult = (1 + t * SPAWN_CONFIG.scaling.speed) * tier.speedMult;
  const dmgMult = (1 + t * SPAWN_CONFIG.scaling.dmg) * tier.dmgMult;

  if (t > SPAWN_CONFIG.squadStart && spawn.squadT <= 0){
    spawn.squadT = clamp(
      SPAWN_CONFIG.squadIntervalMax - t * SPAWN_CONFIG.squadIntervalDrop,
      SPAWN_CONFIG.squadIntervalMin,
      SPAWN_CONFIG.squadIntervalMax
    );
    spawnMixedSquad(t, camX, camY, W, H, hpMult, spdMult, dmgMult);
  }

  if (spawn.eliteT <= 0 && enemies.length < spawn.maxEnemies){
    spawn.eliteT = ELITE_CONFIG.interval;
    const pos = pickSpawnPos(camX, camY, W, H, { allowOutside:false, radius: 14, avoidObstacles:true });
    addTelegraph({
      x: pos.x, y: pos.y,
      radius: ELITE_CONFIG.telegraphRadius,
      color: ELITE_CONFIG.telegraphColor,
      time: ELITE_CONFIG.telegraphTime,
      fire: () => {
        const eliteType = pickEliteType(t);
        spawnEnemy(eliteType, camX, camY, W, H, hpMult, spdMult, dmgMult, true, pos);
      }
    });
  }

  // Ranged spawns are intentionally rare (and capped) to avoid projectile spam
  let rangedCount = 0;
  let voidCount = 0;
  for (let i=0;i<enemies.length;i++){
    const e = enemies[i];
    if (!e.alive) continue;
    if (e.type === "R" || e.type === "M") rangedCount++;
    else if (e.type === "P" || e.type === "F" || e.type === "V") voidCount++;
  }
  const rangedCap = SPAWN_CONFIG.ranged.capBase + Math.floor(t / SPAWN_CONFIG.ranged.capScaleTime); // slowly grows over time
  const rangedChance = SPAWN_CONFIG.ranged.chance * tier.rangedChanceMult; // chance per spawn (after fast roll)
  const voidCap = SPAWN_CONFIG.voids.capBase + Math.floor(t / SPAWN_CONFIG.voids.capScaleTime);
  const voidChance = SPAWN_CONFIG.voids.chance * tier.voidChanceMult;


  while (spawn.acc >= 1 && enemies.length < spawn.maxEnemies){
    spawn.acc -= 1;

    const roll = Math.random();
    let typeKey = "A";

    if (t > SPAWN_CONFIG.thresholds.fast && roll < SPAWN_CONFIG.rolls.fast) typeKey = "B";

    // small chance to spawn a ranged kiter (capped)
    if (t > SPAWN_CONFIG.thresholds.ranged && roll >= SPAWN_CONFIG.rolls.fast && roll < (SPAWN_CONFIG.rolls.fast + rangedChance) && rangedCount < rangedCap){
      typeKey = (Math.random() < SPAWN_CONFIG.ranged.mageChance) ? "M" : "R";
      rangedCount++;
    }

    if (t > SPAWN_CONFIG.thresholds.tank && roll > SPAWN_CONFIG.rolls.tank) typeKey = "C";

    if (t > SPAWN_CONFIG.thresholds.brute && typeKey === "C" && Math.random() < SPAWN_CONFIG.rolls.brute) typeKey = "S";

    if (t > SPAWN_CONFIG.thresholds.void && roll > SPAWN_CONFIG.rolls.void && voidCount < voidCap && Math.random() < voidChance){
      typeKey = pickVoidType();
      voidCount++;
    }

    if (t > SPAWN_CONFIG.thresholds.lateMix && roll > SPAWN_CONFIG.rolls.lateMix && typeKey !== "P" && typeKey !== "F" && typeKey !== "V"){
      const r2 = Math.random();
      if (r2 < SPAWN_CONFIG.rolls.lateTank){
        typeKey = "C";
      } else if (r2 < SPAWN_CONFIG.rolls.lateRanged && rangedCount < rangedCap){
        typeKey = (Math.random() < SPAWN_CONFIG.ranged.mageChance) ? "M" : "R";
        rangedCount++;
      } else if (r2 < SPAWN_CONFIG.rolls.lateVoid && voidCount < voidCap){
        typeKey = pickVoidType();
        voidCount++;
      } else if (r2 < SPAWN_CONFIG.rolls.lateBrute){
        typeKey = "S";
      } else {
        typeKey = "B";
      }
    }

    spawnEnemy(typeKey, camX, camY, W, H, hpMult, spdMult, dmgMult);
  }
}
export function spawnEnemy(typeKey, camX, camY, W, H, hpMult, spdMult, dmgMult, elite = false, pos = null) {
  const info = ENEMY_TYPES[typeKey];
  const e = enemyPool.get();
  e.alive = true;
  e.type = typeKey;
  e.r = info.r;

  e.maxHp = info.hp * hpMult * (elite ? ELITE_CONFIG.hpMult : 1);
  e.hp = e.maxHp;

  e.speed = info.speed * spdMult * (elite ? ELITE_CONFIG.speedMult : 1);

  // contact dmg scales with time
  e.dmg = info.dmg * dmgMult * (elite ? ELITE_CONFIG.dmgMult : 1);

  e.color = info.color;
  e.kx = 0; e.ky = 0;

  // ranged setup
  e.ranged = !!info.ranged;
  e.shotCd = info.shotCd || 0;
  e.shotDmg = (info.shotDmg || 0) * dmgMult; // projectile dmg scales with time too
  e.shotSpeed = info.shotSpeed || 0;
  e.shotRange = info.shotRange || 0;
  e.shotType = info.shotType || "";
  e.shotRadius = info.shotRadius || RANGED_SHOT_CONFIG.radius;
  e.shotLife = info.shotLife || RANGED_SHOT_CONFIG.life;
  e.shotExplosionRadius = info.shotExplosionRadius || 0;
  e.shotT = e.ranged ? rand(e.shotCd * RANGED_SHOT_CONFIG.startDelayMax, e.shotCd * RANGED_SHOT_CONFIG.startDelayMin) : 0;
  e.shotSeq = 0;
  e.aoeSeq = 0;
  e.novaSeq = 0;
  e.rockSeq = 0;
  e.homingSeq = 0;
  e.mineSeq = 0;
  e.spitter = !!info.spit;
  e.spitCd = info.spit ? info.spit.cd : 0;
  e.spitRange = info.spit ? info.spit.range : e.shotRange;
  e.spitRadius = info.spit ? info.spit.radius : 0;
  e.spitDuration = info.spit ? info.spit.duration : 0;
  e.spitDps = (info.spit ? info.spit.dps : 0) * dmgMult;
  e.spitTick = info.spit ? (info.spit.tick || 0.25) : 0.25;
  e.spitColor = info.spit ? info.spit.color : COLORS.warn;
  e.spitTelegraph = info.spit ? (info.spit.telegraph || TELEGRAPH_CONFIG.enemyTime) : TELEGRAPH_CONFIG.enemyTime;
  e.spitType = info.spit ? (info.spit.type || "poison") : "";
  e.spitT = e.spitter ? rand((e.spitCd || RANGED_SHOT_CONFIG.defaultCd) * RANGED_SHOT_CONFIG.startDelayMax, (e.spitCd || RANGED_SHOT_CONFIG.defaultCd) * RANGED_SHOT_CONFIG.startDelayMin) : 0;
  e.boss = !!info.boss;
  e.novaCd = info.nova ? info.nova.cd : 0;
  e.novaShots = info.nova ? info.nova.shots : 0;
  e.novaShotSpeed = info.nova ? info.nova.shotSpeed : 0;
  e.novaShotDmg = info.nova ? info.nova.shotDmg * dmgMult : 0;
  e.novaRadius = info.nova ? info.nova.radius : 0;
  e.novaTelegraph = info.nova ? info.nova.telegraph : TELEGRAPH_CONFIG.enemyTime;
  e.novaT = e.boss ? rand(e.novaCd * RANGED_SHOT_CONFIG.startDelayMax, e.novaCd * RANGED_SHOT_CONFIG.startDelayMin) : 0;
  e.aoeCd = info.aoeAttack ? info.aoeAttack.cd : 0;
  e.aoeT = info.aoeAttack ? rand(info.aoeAttack.cd * RANGED_SHOT_CONFIG.startDelayMax, info.aoeAttack.cd * RANGED_SHOT_CONFIG.startDelayMin) : 0;
  e.aoeCount = info.aoeAttack ? info.aoeAttack.count : 0;
  e.aoeRadius = info.aoeAttack ? info.aoeAttack.radius : 0;
  e.aoeDuration = info.aoeAttack ? info.aoeAttack.duration : 0;
  e.aoeDps = info.aoeAttack ? info.aoeAttack.dps * dmgMult : 0;
  e.aoeTick = info.aoeAttack ? (info.aoeAttack.tick || 0.35) : 0.35;
  e.aoeColor = info.aoeAttack ? (info.aoeAttack.color || COLORS.aoePoison) : COLORS.aoePoison;
  e.aoeTelegraph = info.aoeAttack ? (info.aoeAttack.telegraph || TELEGRAPH_CONFIG.enemyTime) : TELEGRAPH_CONFIG.enemyTime;
  e.aoeType = info.aoeAttack ? (info.aoeAttack.type || "void") : "void";
  e.barrageCd = info.barrage ? info.barrage.cd : 0;
  e.barrageT = info.barrage ? rand(info.barrage.cd * RANGED_SHOT_CONFIG.startDelayMax, info.barrage.cd * RANGED_SHOT_CONFIG.startDelayMin) : 0;
  e.barrageShots = info.barrage ? info.barrage.shots : 0;
  e.barrageWaves = info.barrage ? info.barrage.waves : 0;
  e.barrageWaveDelay = info.barrage ? info.barrage.waveDelay : 0;
  e.barrageShotSpeed = info.barrage ? info.barrage.speed : 0;
  e.barrageShotDmg = info.barrage ? info.barrage.dmg * dmgMult : 0;
  e.slamCd = info.slam ? info.slam.cd : 0;
  e.slamT = info.slam ? rand(info.slam.cd * RANGED_SHOT_CONFIG.startDelayMax, info.slam.cd * RANGED_SHOT_CONFIG.startDelayMin) : 0;
  e.slamRadius = info.slam ? info.slam.radius : 0;
  e.slamDmg = info.slam ? info.slam.dmg * dmgMult : 0;
  e.slamTelegraph = info.slam ? (info.slam.telegraph || 0.9) : 0;
  e.slamColor = info.slam ? (info.slam.color || COLORS.warn) : COLORS.warn;
  e.slamSeq = 0;
  e.rockCd = info.rockfall ? info.rockfall.cd : 0;
  e.rockT = info.rockfall ? rand(info.rockfall.cd * RANGED_SHOT_CONFIG.startDelayMax, info.rockfall.cd * RANGED_SHOT_CONFIG.startDelayMin) : 0;
  e.rockCount = info.rockfall ? info.rockfall.count : 0;
  e.rockRadius = info.rockfall ? info.rockfall.radius : 0;
  e.rockDmg = info.rockfall ? info.rockfall.dmg * dmgMult : 0;
  e.rockTelegraph = info.rockfall ? info.rockfall.telegraph : 0.8;
  e.rockOffsetMin = info.rockfall ? info.rockfall.offsetMin : 0;
  e.rockOffsetMax = info.rockfall ? info.rockfall.offsetMax : 0;
  e.rockColor = info.rockfall ? (info.rockfall.color || COLORS.rock) : COLORS.rock;
  e.homingCd = info.homingMissiles ? info.homingMissiles.cd : 0;
  e.homingT = info.homingMissiles ? rand(info.homingMissiles.cd * RANGED_SHOT_CONFIG.startDelayMax, info.homingMissiles.cd * RANGED_SHOT_CONFIG.startDelayMin) : 0;
  e.homingCount = info.homingMissiles ? info.homingMissiles.count : 0;
  e.homingSpeed = info.homingMissiles ? info.homingMissiles.speed : 0;
  e.homingDmg = info.homingMissiles ? info.homingMissiles.dmg * dmgMult : 0;
  e.homingLife = info.homingMissiles ? info.homingMissiles.life : 0;
  e.homingTurnRate = info.homingMissiles ? (info.homingMissiles.turnRateDeg || 0) * Math.PI / 180 : 0;
  e.homingTelegraphRadius = info.homingMissiles ? info.homingMissiles.telegraphRadius : 0;
  e.homingTelegraphTime = info.homingMissiles ? info.homingMissiles.telegraphTime : 0;
  e.mineCd = info.minefield ? info.minefield.cd : 0;
  e.mineT = info.minefield ? rand(info.minefield.cd * RANGED_SHOT_CONFIG.startDelayMax, info.minefield.cd * RANGED_SHOT_CONFIG.startDelayMin) : 0;
  e.mineCount = info.minefield ? info.minefield.count : 0;
  e.mineRadius = info.minefield ? info.minefield.radius : 0;
  e.mineDmg = info.minefield ? info.minefield.dmg * dmgMult : 0;
  e.mineTelegraph = info.minefield ? info.minefield.telegraph : 0.9;
  e.mineOffsetMin = info.minefield ? info.minefield.offsetMin : 0;
  e.mineOffsetMax = info.minefield ? info.minefield.offsetMax : 0;
  e.mineColor = info.minefield ? (info.minefield.color || COLORS.aoeFire) : COLORS.aoeFire;

  // blink setup
  e.blinkCd = info.blink ? info.blink.cd : 0;
  e.blinkRadius = info.blink ? info.blink.radius : 0;
  e.blinkDmg = info.blink ? info.blink.dmg : 0;
  e.blinkTelegraph = info.blink ? (info.blink.telegraph || TELEGRAPH_CONFIG.enemyTime) : TELEGRAPH_CONFIG.enemyTime;
  e.blinkRangeMin = info.blink ? (info.blink.rangeMin || 0) : 0;
  e.blinkRangeMax = info.blink ? (info.blink.rangeMax || 0) : 0;
  e.blinkColor = info.blink ? (info.blink.color || COLORS.warn) : COLORS.warn;
  e.blinkT = info.blink ? rand(info.blink.cd * RANGED_SHOT_CONFIG.startDelayMax, info.blink.cd * RANGED_SHOT_CONFIG.startDelayMin) : 0;
  e.blinkSeq = 0;

  // rift setup
  e.riftCd = info.rift ? info.rift.cd : 0;
  e.riftCount = info.rift ? info.rift.count : 0;
  e.riftRadius = info.rift ? info.rift.radius : 0;
  e.riftDuration = info.rift ? info.rift.duration : 0;
  e.riftDps = info.rift ? info.rift.dps * dmgMult : 0;
  e.riftTick = info.rift ? (info.rift.tick || 0.35) : 0.35;
  e.riftPull = info.rift ? (info.rift.pull || 0) : 0;
  e.riftColor = info.rift ? (info.rift.color || COLORS.aoePoison) : COLORS.aoePoison;
  e.riftTelegraph = info.rift ? (info.rift.telegraph || TELEGRAPH_CONFIG.enemyTime) : TELEGRAPH_CONFIG.enemyTime;
  e.riftOffsetMin = info.rift ? (info.rift.offsetMin || 0) : 0;
  e.riftOffsetMax = info.rift ? (info.rift.offsetMax || 0) : 0;
  e.riftT = info.rift ? rand(info.rift.cd * RANGED_SHOT_CONFIG.startDelayMax, info.rift.cd * RANGED_SHOT_CONFIG.startDelayMin) : 0;
  e.riftSeq = 0;

  // split setup
  e.splitCd = info.split ? info.split.cd : 0;
  e.splitSpeed = info.split ? info.split.speed : 0;
  e.splitDmg = info.split ? info.split.dmg * dmgMult : 0;
  e.splitLife = info.split ? info.split.life : 0;
  e.splitAfter = info.split ? info.split.splitAfter : 0;
  e.splitCount = info.split ? info.split.splitCount : 0;
  e.splitSpread = info.split ? info.split.splitSpread : 0;
  e.splitTelegraph = info.split ? (info.split.telegraph || TELEGRAPH_CONFIG.enemyTime) : TELEGRAPH_CONFIG.enemyTime;
  e.splitColor = info.split ? (info.split.color || COLORS.warn) : COLORS.warn;
  e.splitT = info.split ? rand(info.split.cd * RANGED_SHOT_CONFIG.startDelayMax, info.split.cd * RANGED_SHOT_CONFIG.startDelayMin) : 0;
  e.splitSeq = 0;
  e.slowT = 0; e.slowMul = 1;
  e.burnT = 0; e.burnDps = 0; e.burnSource = null;
  e.bleedT = 0; e.bleedDps = 0; e.bleedSource = null;
  e.stuckT = 0;
  e.elite = elite;
  e.knockResist = (info.knockResist || 0) + (elite ? ELITE_CONFIG.knockResist : 0);
  const bossLoot = info.lootGems != null ? info.lootGems : (e.boss ? BOSS_CONFIG.lootGems : 0);
  e.gemBonus = (elite ? ELITE_CONFIG.extraGems : 0) + (e.boss ? bossLoot : 0);
  if (e.boss) {
    spawn.bossAlive = true;
    spawn.bossRef = e;
  }

  const p = pos || pickSpawnPos(camX, camY, W, H, { allowOutside: e.boss, radius: e.r, avoidObstacles: true });
  e.x = p.x; e.y = p.y;
  if (!e.boss) clampEntityToWorld(e, e.r);

  enemies.push(e);
}

export function resetSpawnState(){
  spawn.acc = 0;
  spawn.baseRate = SPAWN_CONFIG.baseRate;
  spawn.timeScale = SPAWN_CONFIG.timeScale;
  spawn.maxEnemies = SPAWN_CONFIG.maxEnemies;
  spawn.squadT = SPAWN_CONFIG.squadInterval;
  spawn.eliteT = ELITE_CONFIG.interval;
  spawn.bossSpawned = false;
  spawn.boss2Spawned = false;
  spawn.boss3Spawned = false;
  spawn.boss4Spawned = false;
  spawn.boss5Spawned = false;
  spawn.parallaxDefeated = false;
  spawn.bossPairT = 0;
  spawn.bossPairInterval = 180;
  spawn.bossAlive = false;
  spawn.bossRef = null;
}
