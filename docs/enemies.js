import {
  COLORS,
  RANGED_SHOT_CONFIG,
  LOOT_CONFIG,
  ENEMY_BEHAVIOR,
  BUFF_EFFECTS,
  PLAYER_CONFIG,
  BOSS_CONFIG,
  BOSS4_CONFIG,
  ENEMY_TYPES,
  DOT_CONFIG,
} from "./config.js";
import { TAU, rand, randi, clamp, hypot } from "./math.js";
import { sound } from "./audio.js";
import { applyArmorDamage, applyElementalDamage } from "./player.js";
import { getEnemyTier } from "./spawn.js";
import { addTelegraph, TELEGRAPH_KIND } from "./telegraph.js";
import { obstacleAvoidance, damageObstacle, resolveObstacles, OBSTACLE_TYPE } from "./obstacles.js";
import { addDirectionalParticles, addParticles } from "./particles.js";
import { spawnShockwave } from "./weapons.js";
import {
  triggerPlayerDamageFx,
  triggerShieldBlockFx,
} from "./combat_fx.js";
import {
  clampPointToWorld,
  clampEntityToWorld,
  WORLD,
  player,
  buffs,
  enemies,
  enemyShots,
  voidZones,
  activeObstacles,
  obstacles,
  spawn,
  gems,
  dmgTexts,
} from "./state.js";
import {
  enemyPool,
  shotPool,
  voidPool,
  gemPool,
  dmgPool,
} from "./pools.js";
import { DPS_TRACKER } from "./upgrade.js";

const OFFSCREEN_BOSS_CHASE_MULT = 1.5;
const BOSS_STUCK_DIST2 = 9;
const BOSS_STUCK_DECAY = 2.0;
const BOSS_STUCK_AVOID_SCALE = 1.6;
const BOSS_STUCK_AVOID_MAX = 2.0;
let runtime = { openAug: null, onEnemyKilled: null, onEnemyDamaged: null };
const MAGE_ORB_ELEMENTS = [
  { type: "fire", color: COLORS.aoeFire },
  { type: "poison", color: COLORS.aoePoison },
  { type: "void", color: COLORS.aoeVoid },
];

export function setEnemyRuntime({ openAug, onEnemyKilled, onEnemyDamaged }) {
  runtime.openAug = openAug;
  runtime.onEnemyKilled = onEnemyKilled;
  runtime.onEnemyDamaged = onEnemyDamaged;
}

function spawnDmgText(x, y, amount, color = COLORS.dmg, size = 14, crit = false) {
  const d = dmgPool.get();
  d.alive = true;
  d.x = x; d.y = y;
  d.vx = crit ? rand(16, -16) : rand(22, -22);
  d.vy = crit ? -rand(86, 62) : -rand(64, 38);
  d.maxLife = crit ? rand(0.82, 0.68) : rand(0.62, 0.45);
  d.life = d.maxLife;
  d.text = String(Math.round(amount));
  d.color = color;
  d.size = size;
  d.crit = !!crit;
  dmgTexts.push(d);
}

function dropGem(x, y, value = 1) {
  const g = gemPool.get();
  g.alive = true;
  g.x = x; g.y = y;
  const a = rand(TAU, 0);
  const s = rand(LOOT_CONFIG.dropSpeedMax, LOOT_CONFIG.dropSpeedMin);
  g.vx = Math.cos(a) * s;
  g.vy = Math.sin(a) * s;
  g.v = value;
  g.r = LOOT_CONFIG.gemRadiusBase + value * LOOT_CONFIG.gemRadiusScale;
  g.maxLife = 60;
  g.life = g.maxLife;
  gems.push(g);
}

function damageEnemy(e, dmg, pushX, pushY, pushStrength, showText = true, crit = false, source = null, applyRiders = true, damageKind = "direct", element = "") {
  let inflicted = 0;
  if (dmg > 0) {
    inflicted = Math.min(dmg, Math.max(0, e.hp));
    e.hp -= dmg;
    if (source) DPS_TRACKER[source] = (DPS_TRACKER[source] || 0) + inflicted;
    if (inflicted > 0) {
      e._lastHitSource = source || "";
      e._lastHitCrit = !!crit;
      e._lastHitKind = damageKind || "direct";
      e._lastHitElement = element || "";
      if (damageKind === "direct") {
        let fxDx = pushX || 0;
        let fxDy = pushY || 0;
        let fxLen = Math.hypot(fxDx, fxDy);
        if (fxLen <= 0.001) {
          fxDx = e.x - player.x;
          fxDy = e.y - player.y;
          fxLen = Math.hypot(fxDx, fxDy);
        }
        if (fxLen > 0.001) {
          fxDx /= fxLen;
          fxDy /= fxLen;
        } else {
          fxDx = 1;
          fxDy = 0;
        }
        e.hitFlashMax = crit ? 0.16 : 0.085;
        e.hitFlash = e.hitFlashMax;
        e.hitCrit = !!crit;
        e.hitFxDx = fxDx;
        e.hitFxDy = fxDy;
        addDirectionalParticles(
          e.x,
          e.y,
          crit ? COLORS.crit : COLORS.dmg,
          crit ? 7 : 3,
          crit ? 420 : 260,
          fxDx,
          fxDy,
          crit
        );
      }
    }
    if (showText) {
      const color = crit ? COLORS.crit : COLORS.dmg;
      const size = crit ? 18 : 14;
      spawnDmgText(e.x, e.y - e.r - 6, dmg, color, size, crit);
    }
  }
  if (pushStrength > 0) {
    const resist = e.knockResist || 0;
    const effPush = pushStrength * (1 - resist);
    e.kx += pushX * effPush;
    e.ky += pushY * effPush;
  }
  if (runtime.onEnemyDamaged && (inflicted > 0 || pushStrength > 0)) {
    runtime.onEnemyDamaged(e, {
      amount: inflicted,
      crit: !!crit,
      source: source || "",
      damageKind: damageKind || "direct",
      element: element || "",
      knock: pushStrength > 0,
      pushStrength: Math.max(0, pushStrength || 0),
    });
  }
  if (applyRiders && e.hp > 0 && source) {
    // aug riders are applied in weapon logic
  }
  if (e.hp <= 0) {
    player.kills++;
    addParticles(e.x, e.y, e.color, 12 + randi(8), 300);

    const info = ENEMY_TYPES[e.type];
    const tier = getEnemyTier(player.time);
    const xpMult = tier.xpMult || 1;
    const xpValue = Math.max(1, Math.round(info.xp * xpMult));
    const extra = e.gemBonus || 0;
    for (let i = 0; i < info.gem + extra; i++) {
      dropGem(e.x + rand(LOOT_CONFIG.dropJitter, -LOOT_CONFIG.dropJitter), e.y + rand(LOOT_CONFIG.dropJitter, -LOOT_CONFIG.dropJitter), xpValue);
    }
    if (runtime.onEnemyKilled) runtime.onEnemyKilled(e, { source: e._lastHitSource || "", crit: !!e._lastHitCrit, damageKind: e._lastHitKind || "direct", element: e._lastHitElement || "" });
    if (e.boss) {
      spawn.bossAlive = false;
      spawn.bossRef = null;
      if (e.type === "Q") {
        spawn.parallaxDefeated = true;
        spawn.bossPairT = spawn.bossPairInterval || 180;
      }
      if (runtime.openAug) runtime.openAug();
    }
    e.alive = false;
  }
}

function spawnEnemyShot(x, y, nx, ny, speed, dmg, color = RANGED_SHOT_CONFIG.color) {
  const s = shotPool.get();
  s.alive = true;
  s.x = x; s.y = y;
  s.vx = nx * speed;
  s.vy = ny * speed;
  s.speed = speed;
  s.turnRate = 0;
  s.homing = false;
  s.r = RANGED_SHOT_CONFIG.radius;
  s.dmg = dmg;
  s.life = RANGED_SHOT_CONFIG.life;
  s.color = color;
  s.elementType = "";
  s.explodes = false;
  s.explosionRadius = 0;
  s.splitT = 0;
  s.splitCount = 0;
  s.splitSpread = 0;
  s.splitSpeed = 0;
  s.splitDmg = 0;
  enemyShots.push(s);
}

function spawnHomingShot(x, y, tx, ty, speed, dmg, turnRate, life, color, angleOffset = 0) {
  const s = shotPool.get();
  const ang = Math.atan2(ty - y, tx - x) + angleOffset;
  s.alive = true;
  s.x = x; s.y = y;
  s.vx = Math.cos(ang) * speed;
  s.vy = Math.sin(ang) * speed;
  s.speed = speed;
  s.turnRate = turnRate;
  s.homing = true;
  s.r = RANGED_SHOT_CONFIG.radius;
  s.dmg = dmg;
  s.life = life;
  s.color = color || RANGED_SHOT_CONFIG.color;
  s.elementType = "";
  s.explodes = false;
  s.explosionRadius = 0;
  s.splitT = 0;
  s.splitCount = 0;
  s.splitSpread = 0;
  s.splitSpeed = 0;
  s.splitDmg = 0;
  enemyShots.push(s);
}

function spawnSplitShot(x, y, nx, ny, speed, dmg, life, splitAfter, splitCount, splitSpread, splitSpeed, splitDmg, color) {
  const s = shotPool.get();
  s.alive = true;
  s.x = x; s.y = y;
  s.vx = nx * speed;
  s.vy = ny * speed;
  s.speed = speed;
  s.turnRate = 0;
  s.homing = false;
  s.r = RANGED_SHOT_CONFIG.radius;
  s.dmg = dmg;
  s.life = life;
  s.color = color || RANGED_SHOT_CONFIG.color;
  s.elementType = "";
  s.explodes = false;
  s.explosionRadius = 0;
  s.splitT = splitAfter || 0;
  s.splitCount = splitCount || 0;
  s.splitSpread = splitSpread || 0;
  s.splitSpeed = splitSpeed || speed;
  s.splitDmg = splitDmg || dmg;
  enemyShots.push(s);
}

function spawnMageOrb(x, y, nx, ny, speed, dmg, life, radius, explosionRadius) {
  const element = MAGE_ORB_ELEMENTS[randi(MAGE_ORB_ELEMENTS.length)];
  const s = shotPool.get();
  s.alive = true;
  s.x = x; s.y = y;
  s.vx = nx * speed;
  s.vy = ny * speed;
  s.speed = speed;
  s.turnRate = 0;
  s.homing = false;
  s.r = radius;
  s.dmg = dmg;
  s.life = life;
  s.color = element.color;
  s.elementType = element.type;
  s.explodes = true;
  s.explosionRadius = explosionRadius;
  s.splitT = 0;
  s.splitCount = 0;
  s.splitSpread = 0;
  s.splitSpeed = 0;
  s.splitDmg = 0;
  enemyShots.push(s);
}

function spawnVoidZone(x, y, radius, duration, dps, color, type, tick = 0.25, pull = 0) {
  const z = voidPool.get();
  z.alive = true;
  z.x = x; z.y = y;
  z.radius = radius;
  z.maxLife = duration;
  z.life = duration;
  z.dps = dps;
  z.tick = tick;
  z.tickT = tick;
  z.color = color;
  z.type = type || "poison";
  z.pull = pull || 0;
  voidZones.push(z);
}

function updateEnemyShots(dt) {
  for (let i = enemyShots.length - 1; i >= 0; i--) {
    const s = enemyShots[i];
    if (!s.alive) { enemyShots[i] = enemyShots[enemyShots.length - 1]; enemyShots.pop(); shotPool.put(s); continue; }

    if (s.homing) {
      const tx = player.x - s.x;
      const ty = player.y - s.y;
      const targetAng = Math.atan2(ty, tx);
      const curAng = Math.atan2(s.vy, s.vx);
      let delta = targetAng - curAng;
      if (delta > Math.PI) delta -= TAU;
      else if (delta < -Math.PI) delta += TAU;
      const maxTurn = (s.turnRate || 0) * dt;
      if (maxTurn > 0) {
        const turn = clamp(delta, -maxTurn, maxTurn);
        const speed = s.speed || Math.hypot(s.vx, s.vy);
        s.vx = Math.cos(curAng + turn) * speed;
        s.vy = Math.sin(curAng + turn) * speed;
        s.speed = speed;
      }
    }

    if (s.splitT > 0) {
      s.splitT -= dt;
      if (s.splitT <= 0) {
        const count = s.splitCount || 0;
        if (count > 0) {
          const baseAng = Math.atan2(s.vy, s.vx);
          const step = (count > 1) ? (s.splitSpread || 0) : 0;
          const start = -step * (count - 1) * 0.5;
          const speed = s.splitSpeed || s.speed || Math.hypot(s.vx, s.vy);
          const dmg = s.splitDmg || s.dmg;
          for (let k = 0; k < count; k++) {
            const ang = baseAng + start + step * k;
            spawnEnemyShot(s.x, s.y, Math.cos(ang), Math.sin(ang), speed, dmg, s.color);
          }
        }
        s.alive = false;
      }
    }
    if (!s.alive) {
      enemyShots[i] = enemyShots[enemyShots.length - 1];
      enemyShots.pop();
      shotPool.put(s);
      continue;
    }

    s.life -= dt;
    s.x += s.vx * dt;
    s.y += s.vy * dt;

    // obstacle collision (ignore lakes)
    let blocked = false;
    for (let j = 0; j < activeObstacles.length; j++) {
      const idx = activeObstacles[j];
      const o = obstacles[idx];
      if (!o) continue;
      if (o.type === OBSTACLE_TYPE.LAKE) continue;
      const dx = o.x - s.x;
      const dy = o.y - s.y;
      const rr = o.r + s.r;
      if (dx * dx + dy * dy <= rr * rr) {
        if (o.type === OBSTACLE_TYPE.FOREST) {
          damageObstacle(idx, s.dmg, false);
        }
        blocked = true;
        break;
      }
    }
    if (blocked) {
      if (s.explodes) explodeEnemyShot(s);
      s.alive = false;
    }
    if (!s.alive) {
      enemyShots[i] = enemyShots[enemyShots.length - 1];
      enemyShots.pop();
      shotPool.put(s);
      continue;
    }

    const dx = player.x - s.x;
    const dy = player.y - s.y;
    const rr = player.r + s.r + RANGED_SHOT_CONFIG.hitPad;
    if (dx * dx + dy * dy <= rr * rr) {
      if (s.explodes) {
        explodeEnemyShot(s);
      } else {
        applyShotDamage(s.dmg, s.elementType);
      }
      s.alive = false;
    }

    if (!s.alive) {
      enemyShots[i] = enemyShots[enemyShots.length - 1];
      enemyShots.pop();
      shotPool.put(s);
      continue;
    }

    if (s.life <= 0) {
      if (s.explodes) explodeEnemyShot(s);
      s.alive = false;
    }

    if (!s.alive) {
      enemyShots[i] = enemyShots[enemyShots.length - 1];
      enemyShots.pop();
      shotPool.put(s);
    }
  }
}

function applyShotDamage(dmg, elementType = "") {
  if (buffs.shield <= 0 && player.iFrame <= 0) {
    const final = elementType ? applyElementalDamage(dmg, elementType) : applyArmorDamage(dmg);
    player.hp -= final;
    player.iFrame = PLAYER_CONFIG.shotIFrame;
    spawnDmgText(player.x, player.y - player.r - 12, final, COLORS.warnHit);
    addParticles(player.x, player.y, COLORS.warnHitDim, 8, 360);
    triggerPlayerDamageFx(final);
  } else {
    addParticles(player.x, player.y, COLORS.shieldBlock, 4, 260);
    if (buffs.shield > 0) triggerShieldBlockFx();
  }
}

function explodeEnemyShot(s) {
  const r = s.explosionRadius || 0;
  if (r > 0) {
    spawnShockwave(s.x, s.y, r, s.color);
  }
  const dx = player.x - s.x;
  const dy = player.y - s.y;
  const rr = r + player.r;
  if (dx * dx + dy * dy <= rr * rr) {
    applyShotDamage(s.dmg, s.elementType);
  }
}

function updateVoidZones(dt, godMode) {
  let pulled = false;
  for (let i = voidZones.length - 1; i >= 0; i--) {
    const z = voidZones[i];
    if (!z.alive) { voidZones[i] = voidZones[voidZones.length - 1]; voidZones.pop(); voidPool.put(z); continue; }

    z.life -= dt;
    if (z.life <= 0) z.alive = false;

    if (z.alive) {
      const dx = player.x - z.x;
      const dy = player.y - z.y;
      const rr = player.r + z.radius;
      const inside = dx * dx + dy * dy <= rr * rr;
      if (inside && z.pull > 0 && !godMode) {
        const d = Math.sqrt(dx * dx + dy * dy) || 1;
        const nx = -dx / d;
        const ny = -dy / d;
        player.x += nx * z.pull * dt;
        player.y += ny * z.pull * dt;
        pulled = true;
      }
      if (inside && buffs.shield <= 0 && !godMode) {
        z.tickT -= dt;
        while (z.tickT <= 0) {
          z.tickT += z.tick;
          const dmg = applyElementalDamage(z.dps * z.tick, z.type);
          player.hp -= dmg;
          spawnDmgText(player.x, player.y - player.r - 12, dmg, COLORS.warnHit, 14);
          triggerPlayerDamageFx(dmg);
        }
      } else {
        z.tickT = z.tick;
      }
    }

    if (!z.alive) {
      voidZones[i] = voidZones[voidZones.length - 1];
      voidZones.pop();
      voidPool.put(z);
    }
  }
  if (pulled) {
    resolveObstacles(player, player.r);
    clampEntityToWorld(player, player.r);
  }
}

function getSlowFireMul() {
  return (buffs.slow > 0) ? BUFF_EFFECTS.slowFireMult : 1.0;
}

function updateRiderTimers(e, dt, burnSource = null, bleedSource = null) {
  if (e.burnT > 0) {
    e.burnT -= dt;
    if (e.burnTickT == null) e.burnTickT = DOT_CONFIG.burnTick;
    e.burnTickT -= dt;
    while (e.burnTickT <= 0 && e.burnT > 0) {
      e.burnTickT += DOT_CONFIG.burnTick;
      const dmg = e.burnDps * DOT_CONFIG.burnTick;
      if (dmg > 0) damageEnemy(e, dmg, 0, 0, 0, true, false, burnSource, false, "burn", e.burnElement || "fire");
      addParticles(e.x, e.y, COLORS.enemyF, 6, 160);
    }
    if (e.burnT <= 0) e.burnTickT = DOT_CONFIG.burnTick;
      
    if (!e.alive) return false;
  }
  if (e.bleedT > 0) {
    e.bleedT -= dt;
    if (e.bleedTickT == null) e.bleedTickT = DOT_CONFIG.bleedTick;
    e.bleedTickT -= dt;
    while (e.bleedTickT <= 0 && e.bleedT > 0) {
      e.bleedTickT += DOT_CONFIG.bleedTick;
      const dmg = e.bleedDps * DOT_CONFIG.bleedTick;
      if (dmg > 0) damageEnemy(e, dmg, 0, 0, 0, true, false, bleedSource, false, "bleed");
      addParticles(e.x, e.y, COLORS.warn, 6, 140);
    }
    if (e.bleedT <= 0) e.bleedTickT = DOT_CONFIG.bleedTick;
    if (!e.alive) return false;
  }
  if (e.slowT > 0) e.slowT -= dt;
  return true;
}

function applyKnockback(e, dt) {
  e.x += e.kx * dt;
  e.y += e.ky * dt;
  const kbDecay = Math.pow(ENEMY_BEHAVIOR.knockbackDecayBase, dt);
  e.kx *= kbDecay;
  e.ky *= kbDecay;
}

function getBossAvoidMult(e) {
  if (!e.boss) return 1.0;
  const t = e.stuckT || 0;
  return 1 + clamp(t * BOSS_STUCK_AVOID_SCALE, 0, BOSS_STUCK_AVOID_MAX);
}

function updateBossStuck(e, dt, startX, startY) {
  if (!e.boss) return;
  const dx = e.x - startX;
  const dy = e.y - startY;
  const moved2 = dx * dx + dy * dy;
  if (moved2 < BOSS_STUCK_DIST2) {
    e.stuckT = Math.min(2, (e.stuckT || 0) + dt);
  } else {
    e.stuckT = Math.max(0, (e.stuckT || 0) - dt * BOSS_STUCK_DECAY);
  }
}

function getEnemyTargeting(e, avoidMult = 1) {
  const dx = player.x - e.x;
  const dy = player.y - e.y;
  const d = hypot(dx, dy) || 1;
  let nx = dx / d, ny = dy / d;
  const avoid = obstacleAvoidance(e.x, e.y, e.r);
  const ax = avoid.ax * avoidMult;
  const ay = avoid.ay * avoidMult;
  nx += ax;
  ny += ay;
  const nlen = hypot(nx, ny) || 1;
  nx /= nlen; ny /= nlen;
  return { d, nx, ny, ax, ay };
}

function getBossMoveDir(baseX, baseY, ax, ay) {
  let fx = baseX + ax;
  let fy = baseY + ay;
  let f2 = fx * fx + fy * fy;
  if (f2 < 0.01 && (ax * ax + ay * ay) > 0.0001) {
    let tx = -ay;
    let ty = ax;
    if (tx * baseX + ty * baseY < 0) { tx = -tx; ty = -ty; }
    fx = tx;
    fy = ty;
    f2 = fx * fx + fy * fy;
  }
  const flen = Math.sqrt(f2) || 1;
  return { x: fx / flen, y: fy / flen };
}

function getRangedPreferredDistance(e) {
  return (e.spitter ? (e.spitRange || e.shotRange) : e.shotRange) || ENEMY_BEHAVIOR.rangedPreferredRange;
}

function isBossOffscreen(e, viewW, viewH) {
  if (!e.boss || !(viewW > 0 && viewH > 0)) return false;
  const camX = player.x - viewW * 0.5;
  const camY = player.y - viewH * 0.5;
  const pad = 20;
  return (e.x < camX - pad || e.x > camX + viewW + pad || e.y < camY - pad || e.y > camY + viewH + pad);
}

function updateRangedMovement(e, i, dt, slowMul, statusSpeedMul, d, nx, ny, ax, ay, prefer, offscreen) {
  const bossChase = (e.boss && d > prefer * ENEMY_BEHAVIOR.preferredFar)
    ? (1 + clamp((d - prefer * ENEMY_BEHAVIOR.preferredFar) / (prefer * 1.2), 0, 1.5))
    : 1;
  const chaseBoost = (e.boss && offscreen) ? OFFSCREEN_BOSS_CHASE_MULT : 1;

  if (d < prefer * ENEMY_BEHAVIOR.preferredClose) {
    const flee = e.speed * ENEMY_BEHAVIOR.fleeMult;
    if (e.boss) {
      const bnx = (player.x - e.x) / d;
      const bny = (player.y - e.y) / d;
      const fleeDir = getBossMoveDir(-bnx, -bny, ax, ay);
      const fnx = fleeDir.x;
      const fny = fleeDir.y;
      e.x += fnx * (flee * slowMul * statusSpeedMul) * dt;
      e.y += fny * (flee * slowMul * statusSpeedMul) * dt;
    } else {
      e.x += (-nx) * (flee * slowMul * statusSpeedMul) * dt;
      e.y += (-ny) * (flee * slowMul * statusSpeedMul) * dt;
    }
  } else if (d > prefer * ENEMY_BEHAVIOR.preferredFar) {
    const creep = e.speed * ENEMY_BEHAVIOR.creepMult;
    if (e.boss) {
      const bnx = (player.x - e.x) / d;
      const bny = (player.y - e.y) / d;
      const chaseDir = getBossMoveDir(bnx, bny, ax, ay);
      const cnx = chaseDir.x;
      const cny = chaseDir.y;
      e.x += cnx * (creep * bossChase * chaseBoost * slowMul * statusSpeedMul) * dt;
      e.y += cny * (creep * bossChase * chaseBoost * slowMul * statusSpeedMul) * dt;
    } else {
      e.x += (nx) * (creep * bossChase * chaseBoost * slowMul * statusSpeedMul) * dt;
      e.y += (ny) * (creep * bossChase * chaseBoost * slowMul * statusSpeedMul) * dt;
    }
  } else {
    const strafe = e.speed * ENEMY_BEHAVIOR.strafeMult;
    const px = -ny, py = nx;
    const dir = (i & 1) ? 1 : -1;
    e.x += (px * dir) * (strafe * slowMul * statusSpeedMul) * dt;
    e.y += (py * dir) * (strafe * slowMul * statusSpeedMul) * dt;
  }
}

function updateRangedAttacks(e, dt, d, nx, ny) {
  if (e.spitter) {
    e.spitT -= dt;
    if (e.spitT <= 0 && d < (e.spitRange || ENEMY_BEHAVIOR.rangedPreferredRange) * ENEMY_BEHAVIOR.preferredFar) {
      const slowFireMul = getSlowFireMul();
      e.spitT += (e.spitCd || RANGED_SHOT_CONFIG.defaultCd) * slowFireMul;
      const ang = rand(TAU, 0);
      const off = (e.spitRadius || 0) * rand(ENEMY_BEHAVIOR.spitAimOffsetMax, ENEMY_BEHAVIOR.spitAimOffsetMin);
      const tx = player.x + Math.cos(ang) * off;
      const ty = player.y + Math.sin(ang) * off;
      const marker = ++e.shotSeq;
      addTelegraph({
        x: tx, y: ty, radius: e.spitRadius, color: e.spitColor, time: e.spitTelegraph,
        kind: TELEGRAPH_KIND.AOE,
        element: e.spitType,
        fire: () => {
          spawnVoidZone(tx, ty, e.spitRadius, e.spitDuration, e.spitDps, e.spitColor, e.spitType, e.spitTick);
        }
      });
    }
  } else {
    e.shotT -= dt;
    if (e.shotT <= 0 && d < (e.shotRange || ENEMY_BEHAVIOR.rangedPreferredRange) * ENEMY_BEHAVIOR.preferredFar) {
      const slowFireMul = getSlowFireMul();
      e.shotT += (e.shotCd || RANGED_SHOT_CONFIG.defaultCd) * slowFireMul;
      const marker = ++e.shotSeq;
      addTelegraph({
        x: e.x, y: e.y, dx: nx, dy: ny, radius: RANGED_SHOT_CONFIG.telegraphRadius, color: COLORS.warn, time: RANGED_SHOT_CONFIG.telegraphTime,
        kind: TELEGRAPH_KIND.PROJECTILE,
        length: d,
        dangerWidth: ((e.shotRadius || RANGED_SHOT_CONFIG.radius) + player.r) * 2,
        follow: (tg) => {
          if (!e.alive) return;
          tg.x = e.x;
          tg.y = e.y;
        },
        fire: () => {
          if (!e.alive || e.shotSeq !== marker) return;
          if (e.shotType === "mage_orb") {
            spawnMageOrb(
              e.x,
              e.y,
              nx,
              ny,
              e.shotSpeed || RANGED_SHOT_CONFIG.defaultSpeed,
              e.shotDmg || RANGED_SHOT_CONFIG.defaultDmg,
              e.shotLife || RANGED_SHOT_CONFIG.life,
              e.shotRadius || RANGED_SHOT_CONFIG.radius,
              e.shotExplosionRadius || RANGED_SHOT_CONFIG.radius * 6
            );
          } else {
            spawnEnemyShot(e.x, e.y, nx, ny, e.shotSpeed || RANGED_SHOT_CONFIG.defaultSpeed, e.shotDmg || RANGED_SHOT_CONFIG.defaultDmg);
          }
        }
      });
    }
  }
}

function updateRangedEnemy(e, i, dt, slowMul, d, nx, ny, ax, ay, offscreen) {
  const statusSpeedMul = (e.slowT > 0) ? e.slowMul : 1.0;
  const prefer = getRangedPreferredDistance(e);
  updateRangedMovement(e, i, dt, slowMul, statusSpeedMul, d, nx, ny, ax, ay, prefer, offscreen);
  updateRangedAttacks(e, dt, d, nx, ny);
}

function updateMeleeEnemy(e, dt, slowMul, d, nx, ny, ax, ay, offscreen) {
  const bossChase = e.boss ? (1 + clamp((d - 320) / 900, 0, 1.5)) : 1;
  const chaseBoost = (e.boss && offscreen) ? OFFSCREEN_BOSS_CHASE_MULT : 1;
  if (e.boss) {
    const bnx = (player.x - e.x) / d;
    const bny = (player.y - e.y) / d;
    const chaseDir = getBossMoveDir(bnx, bny, ax, ay);
    const cnx = chaseDir.x;
    const cny = chaseDir.y;
    e.x += cnx * (e.speed * slowMul * bossChase * chaseBoost) * dt;
    e.y += cny * (e.speed * slowMul * bossChase * chaseBoost) * dt;
  } else {
    e.x += nx * (e.speed * slowMul * bossChase * chaseBoost) * dt;
    e.y += ny * (e.speed * slowMul * bossChase * chaseBoost) * dt;
  }
}

function updateBossNova(e, dt) {
  if (!(e.novaCd > 0)) return;
  e.novaT -= dt;
  if (e.novaT <= 0) {
    const slowFireMul = getSlowFireMul();
    e.novaT += (e.novaCd || 6) * slowFireMul;
    const marker = ++e.novaSeq;
    addTelegraph({
      x: e.x, y: e.y, radius: e.novaRadius, color: BOSS_CONFIG.telegraph.color, time: e.novaTelegraph,
      kind: TELEGRAPH_KIND.NOVA,
      follow: (tg) => {
        if (!e.alive) return;
        tg.x = e.x;
        tg.y = e.y;
      },
      fire: () => {
        if (!e.alive || e.novaSeq !== marker) return;
        for (let k = 0; k < e.novaShots; k++) {
          const ang = (TAU * k) / e.novaShots;
          spawnEnemyShot(e.x, e.y, Math.cos(ang), Math.sin(ang), e.novaShotSpeed || RANGED_SHOT_CONFIG.defaultSpeed, e.novaShotDmg || RANGED_SHOT_CONFIG.defaultDmg);
        }
      }
    });
  }
}

function updateBossAoe(e, dt) {
  if (!(e.aoeCd > 0)) return;
  e.aoeT -= dt;
  if (e.aoeT <= 0) {
    const slowFireMul = getSlowFireMul();
    e.aoeT += (e.aoeCd || 5) * slowFireMul;
    const marker = ++e.aoeSeq;
    const count = Math.max(1, e.aoeCount || 1);
    for (let k = 0; k < count; k++) {
      const ang = rand(TAU, 0);
      const dist = rand(140, 60);
      const tx = player.x + Math.cos(ang) * dist;
      const ty = player.y + Math.sin(ang) * dist;
      addTelegraph({
        x: tx, y: ty, radius: e.aoeRadius, color: e.aoeColor, time: e.aoeTelegraph,
        kind: TELEGRAPH_KIND.AOE,
        element: e.aoeType,
        fire: () => {
          if (e.alive && e.aoeSeq === marker) {
            spawnVoidZone(tx, ty, e.aoeRadius, e.aoeDuration, e.aoeDps, e.aoeColor, e.aoeType, e.aoeTick);
          }
        }
      });
    }
  }
}

function updateBossBarrage(e, dt) {
  if (!(e.barrageCd > 0)) return;
  e.barrageT -= dt;
  if (e.barrageT <= 0) {
    const slowFireMul = getSlowFireMul();
    e.barrageT += (e.barrageCd || 4) * slowFireMul;
    const baseAng = rand(TAU, 0);
    const waves = Math.max(1, e.barrageWaves || 1);
    const shots = Math.max(4, e.barrageShots || 8);
    const waveDelay = e.barrageWaveDelay || 0;
    for (let w = 0; w < waves; w++) {
      const fireWave = () => {
        if (!e.alive) return;
        for (let k = 0; k < shots; k++) {
          const ang = baseAng + (TAU * k / shots) + w * 0.15;
          spawnEnemyShot(e.x, e.y, Math.cos(ang), Math.sin(ang), e.barrageShotSpeed || RANGED_SHOT_CONFIG.defaultSpeed, e.barrageShotDmg || RANGED_SHOT_CONFIG.defaultDmg);
        }
      };
      if (waveDelay > 0 && w > 0) {
        setTimeout(fireWave, waveDelay * 1000 * w);
      } else {
        fireWave();
      }
    }
  }
}

function updateBossSlam(e, dt, godMode) {
  if (!(e.slamCd > 0)) return;
  e.slamT -= dt;
  if (e.slamT <= 0) {
    const slowFireMul = getSlowFireMul();
    e.slamT += (e.slamCd || 4.5) * slowFireMul;
    const marker = ++e.slamSeq;
    addTelegraph({
      x: e.x, y: e.y, radius: e.slamRadius || 120, color: e.slamColor || COLORS.warn, time: e.slamTelegraph || 0.9,
      kind: TELEGRAPH_KIND.IMPACT,
      follow: (tg) => {
        if (!e.alive) return;
        tg.x = e.x; tg.y = e.y;
      },
      fire: () => {
        if (!e.alive || e.slamSeq !== marker) return;
        const r = e.slamRadius || 120;
        const dx = player.x - e.x;
        const dy = player.y - e.y;
        if (dx * dx + dy * dy <= (player.r + r) * (player.r + r)) {
          if (!godMode && buffs.shield <= 0) {
            const dmg = applyArmorDamage(e.slamDmg || 20);
            player.hp -= dmg;
            player.iFrame = PLAYER_CONFIG.meleeIFrame;
            spawnDmgText(player.x, player.y - player.r - 12, dmg, COLORS.warnHit);
            triggerPlayerDamageFx(dmg);
          } else if (!godMode && buffs.shield > 0) {
            triggerShieldBlockFx();
          }
        }
        addParticles(e.x, e.y, e.slamColor || COLORS.warn, 18, 360);
        spawnShockwave(e.x, e.y, r, e.slamColor || COLORS.warn);
      }
    });
  }
}

function updateBossRock(e, dt, godMode) {
  if (!(e.rockCd > 0)) return;
  e.rockT -= dt;
  if (e.rockT <= 0) {
    const slowFireMul = getSlowFireMul();
    e.rockT += (e.rockCd || 6) * slowFireMul;
    const marker = ++e.rockSeq;
    const count = Math.max(1, e.rockCount || 1);
    for (let k = 0; k < count; k++) {
      const ang = rand(TAU, 0);
      const dist = rand(e.rockOffsetMax || 200, e.rockOffsetMin || 60);
      const tx = player.x + Math.cos(ang) * dist;
      const ty = player.y + Math.sin(ang) * dist;
      addTelegraph({
        x: tx, y: ty, radius: e.rockRadius || 90, color: e.rockColor || COLORS.rock, time: e.rockTelegraph || 0.9,
        kind: TELEGRAPH_KIND.IMPACT,
        fire: () => {
          if (!e.alive || e.rockSeq !== marker) return;
          const r = e.rockRadius || 90;
          const dx = player.x - tx;
          const dy = player.y - ty;
          if (dx * dx + dy * dy <= (player.r + r) * (player.r + r) && !godMode && buffs.shield <= 0) {
            const dmg = applyArmorDamage(e.rockDmg || 30);
            player.hp -= dmg;
            player.iFrame = PLAYER_CONFIG.meleeIFrame;
            spawnDmgText(player.x, player.y - player.r - 12, dmg, COLORS.warnHit);
            triggerPlayerDamageFx(dmg);
          } else if (dx * dx + dy * dy <= (player.r + r) * (player.r + r) && !godMode && buffs.shield > 0) {
            triggerShieldBlockFx();
          }
          addParticles(tx, ty, e.rockColor || COLORS.rock, 14, 420);
          spawnShockwave(tx, ty, r, e.rockColor || COLORS.rock);
        }
      });
    }
  }
}

function updateBossHomingMissiles(e, dt) {
  if (!(e.homingCd > 0)) return;
  e.homingT -= dt;
  if (e.homingT <= 0) {
    const slowFireMul = getSlowFireMul();
    e.homingT += (e.homingCd || 5.5) * slowFireMul;
    const marker = ++e.homingSeq;
    const count = Math.max(1, e.homingCount || 1);
    addTelegraph({
      x: e.x, y: e.y, radius: e.homingTelegraphRadius || RANGED_SHOT_CONFIG.telegraphRadius, color: BOSS4_CONFIG.telegraph.color, time: e.homingTelegraphTime || RANGED_SHOT_CONFIG.telegraphTime,
      kind: TELEGRAPH_KIND.PROJECTILE,
      count,
      dangerWidth: (RANGED_SHOT_CONFIG.radius + player.r) * 2,
      follow: (tg) => { if (e.alive) { tg.x = e.x; tg.y = e.y; } },
      fire: () => {
        if (!e.alive || e.homingSeq !== marker) return;
        for (let k = 0; k < count; k++) {
          const offset = rand(1.3, -1.3);
          spawnHomingShot(
            e.x,
            e.y,
            player.x,
            player.y,
            e.homingSpeed || RANGED_SHOT_CONFIG.defaultSpeed,
            e.homingDmg || RANGED_SHOT_CONFIG.defaultDmg,
            e.homingTurnRate || (Math.PI * 0.5),
            e.homingLife || RANGED_SHOT_CONFIG.life,
            BOSS4_CONFIG.telegraph.color,
            offset
          );
        }
      }
    });
  }
}

function updateBossMine(e, dt, godMode) {
  if (!(e.mineCd > 0)) return;
  e.mineT -= dt;
  if (e.mineT <= 0) {
    const slowFireMul = getSlowFireMul();
    e.mineT += (e.mineCd || 7) * slowFireMul;
    const marker = ++e.mineSeq;
    const count = Math.max(1, e.mineCount || 4);
    for (let k = 0; k < count; k++) {
      const ang = rand(TAU, 0);
      const dist = rand(e.mineOffsetMax || 220, e.mineOffsetMin || 80);
      const tx = player.x + Math.cos(ang) * dist;
      const ty = player.y + Math.sin(ang) * dist;
      addTelegraph({
        x: tx, y: ty, radius: e.mineRadius || 85, color: e.mineColor || COLORS.aoeFire, time: e.mineTelegraph || 1.0,
        kind: TELEGRAPH_KIND.MINE,
        fire: () => {
          if (!e.alive || e.mineSeq !== marker) return;
          const r = e.mineRadius || 85;
          const dx = player.x - tx;
          const dy = player.y - ty;
          if (dx * dx + dy * dy <= (player.r + r) * (player.r + r) && !godMode && buffs.shield <= 0) {
            const dmg = applyArmorDamage(e.mineDmg || 20);
            player.hp -= dmg;
            player.iFrame = PLAYER_CONFIG.shotIFrame;
            spawnDmgText(player.x, player.y - player.r - 12, dmg, COLORS.warnHit);
            triggerPlayerDamageFx(dmg);
          } else if (dx * dx + dy * dy <= (player.r + r) * (player.r + r) && !godMode && buffs.shield > 0) {
            triggerShieldBlockFx();
          }
          addParticles(tx, ty, e.mineColor || COLORS.aoeFire, 16, 360);
          spawnShockwave(tx, ty, r, e.mineColor || COLORS.aoeFire);
        }
      });
    }
  }
}

function updateBossBlink(e, dt, godMode) {
  if (!(e.blinkCd > 0)) return;
  e.blinkT -= dt;
  if (e.blinkT <= 0) {
    const slowFireMul = getSlowFireMul();
    e.blinkT += (e.blinkCd || 6) * slowFireMul;
    const marker = ++e.blinkSeq;
    const ang = rand(TAU, 0);
    const minDist = e.blinkRangeMin || 120;
    const maxDist = e.blinkRangeMax || 260;
    const dist = rand(maxDist, minDist);
    const target = clampPointToWorld(
      player.x + Math.cos(ang) * dist,
      player.y + Math.sin(ang) * dist,
      e.r
    );
    addTelegraph({
      x: target.x, y: target.y, radius: e.blinkRadius || 120, color: e.blinkColor || COLORS.warn, time: e.blinkTelegraph || 0.9,
      kind: TELEGRAPH_KIND.BLINK,
      fire: () => {
        if (!e.alive || e.blinkSeq !== marker) return;
        e.x = target.x;
        e.y = target.y;
        resolveObstacles(e, e.r);
        const r = e.blinkRadius || 120;
        const dx = player.x - target.x;
        const dy = player.y - target.y;
        if (dx * dx + dy * dy <= (player.r + r) * (player.r + r)) {
          if (!godMode && buffs.shield <= 0) {
            const dmg = applyArmorDamage(e.blinkDmg || 20);
            player.hp -= dmg;
            player.iFrame = PLAYER_CONFIG.meleeIFrame;
            spawnDmgText(player.x, player.y - player.r - 12, dmg, COLORS.warnHit);
            triggerPlayerDamageFx(dmg);
          } else if (!godMode && buffs.shield > 0) {
            triggerShieldBlockFx();
          }
        }
        addParticles(target.x, target.y, e.blinkColor || COLORS.warn, 16, 360);
        spawnShockwave(target.x, target.y, r, e.blinkColor || COLORS.warn);
      }
    });
  }
}

function updateBossRift(e, dt) {
  if (!(e.riftCd > 0)) return;
  e.riftT -= dt;
  if (e.riftT <= 0) {
    const slowFireMul = getSlowFireMul();
    e.riftT += (e.riftCd || 6) * slowFireMul;
    const marker = ++e.riftSeq;
    const count = Math.max(1, e.riftCount || 1);
    for (let k = 0; k < count; k++) {
      const ang = rand(TAU, 0);
      const dist = rand(e.riftOffsetMax || 200, e.riftOffsetMin || 80);
      const tx = player.x + Math.cos(ang) * dist;
      const ty = player.y + Math.sin(ang) * dist;
      addTelegraph({
        x: tx, y: ty, radius: e.riftRadius || 100, color: e.riftColor || COLORS.aoeVoid, time: e.riftTelegraph || 0.8,
        kind: TELEGRAPH_KIND.RIFT,
        element: "void",
        fire: () => {
          if (!e.alive || e.riftSeq !== marker) return;
          spawnVoidZone(
            tx,
            ty,
            e.riftRadius || 100,
            e.riftDuration || 3,
            e.riftDps || 0,
            e.riftColor || COLORS.aoeVoid,
            "void",
            e.riftTick || 0.35,
            e.riftPull || 0
          );
        }
      });
    }
  }
}

function updateBossSplit(e, dt) {
  if (!(e.splitCd > 0)) return;
  e.splitT -= dt;
  if (e.splitT <= 0) {
    const slowFireMul = getSlowFireMul();
    e.splitT += (e.splitCd || 5) * slowFireMul;
    const marker = ++e.splitSeq;
    addTelegraph({
      x: e.x, y: e.y, radius: RANGED_SHOT_CONFIG.telegraphRadius, color: e.splitColor || COLORS.warn, time: e.splitTelegraph || RANGED_SHOT_CONFIG.telegraphTime,
      kind: TELEGRAPH_KIND.PROJECTILE,
      dangerWidth: (RANGED_SHOT_CONFIG.radius + player.r) * 2,
      count: e.splitCount || 3,
      spread: e.splitSpread || 0.3,
      follow: (tg) => {
        if (!e.alive) return;
        tg.x = e.x;
        tg.y = e.y;
        const tx = player.x - e.x;
        const ty = player.y - e.y;
        const distance = Math.hypot(tx, ty) || 1;
        tg.dx = tx / distance;
        tg.dy = ty / distance;
        tg.length = distance;
      },
      fire: () => {
        if (!e.alive || e.splitSeq !== marker) return;
        const dx = player.x - e.x;
        const dy = player.y - e.y;
        const d = hypot(dx, dy) || 1;
        const nx = dx / d, ny = dy / d;
        const speed = e.splitSpeed || RANGED_SHOT_CONFIG.defaultSpeed;
        const dmg = e.splitDmg || RANGED_SHOT_CONFIG.defaultDmg;
        spawnSplitShot(
          e.x,
          e.y,
          nx,
          ny,
          speed,
          dmg,
          e.splitLife || RANGED_SHOT_CONFIG.life,
          e.splitAfter || 0.8,
          e.splitCount || 3,
          e.splitSpread || 0.3,
          speed,
          dmg,
          e.splitColor || RANGED_SHOT_CONFIG.color
        );
      }
    });
  }
}

function updateBossAttacks(e, dt, godMode) {
  updateBossNova(e, dt);
  updateBossAoe(e, dt);
  updateBossBarrage(e, dt);
  updateBossSlam(e, dt, godMode);
  updateBossRock(e, dt, godMode);
  updateBossHomingMissiles(e, dt);
  updateBossMine(e, dt, godMode);
  updateBossBlink(e, dt, godMode);
  updateBossRift(e, dt);
  updateBossSplit(e, dt);
}

function handleContactDamage(e, dt, godMode) {
  resolveObstacles(e, e.r);
  if (!e.boss) clampEntityToWorld(e, e.r);

  const cdx = player.x - e.x;
  const cdy = player.y - e.y;
  const cd = hypot(cdx, cdy) || 1;
  const cnx = cdx / cd;
  const cny = cdy / cd;

  const rr = player.r + e.r;
  if (cd < rr) {
    if (godMode) {
      // ignore melee damage
    } else if (buffs.shield <= 0 && player.iFrame <= 0) {
      const hurt = applyArmorDamage(e.dmg);
      player.hp -= hurt;
      player.iFrame = PLAYER_CONFIG.meleeIFrame;
      sound.play("hurt");

      // floating damage numbers for melee hits too
      spawnDmgText(player.x, player.y - player.r - 12, hurt, COLORS.warnHit);
      addParticles(player.x, player.y, COLORS.warnHitDim, 6, 340);
      triggerPlayerDamageFx(hurt);

      player.x += cnx * PLAYER_CONFIG.hitPush;
      player.y += cny * PLAYER_CONFIG.hitPush;
    } else {
      e.kx -= cnx * PLAYER_CONFIG.shieldPushback * dt;
      e.ky -= cny * PLAYER_CONFIG.shieldPushback * dt;
      if (buffs.shield > 0) triggerShieldBlockFx();
    }
  }
}

function updateEnemies(dt, godMode, viewW, viewH) {
  const slowMul = (buffs.slow > 0) ? BUFF_EFFECTS.slowMoveMult : 1.0;

  for (let i = 0; i < enemies.length; i++) {
    const e = enemies[i];
    if (!e.alive) continue;
    if (e.hitFlash > 0) e.hitFlash = Math.max(0, e.hitFlash - dt);
    const startX = e.x;
    const startY = e.y;

    if (!updateRiderTimers(e, dt, e.burnSource, e.bleedSource)) continue;

    applyKnockback(e, dt);

    const avoidMult = getBossAvoidMult(e);
    const { d, nx, ny, ax, ay } = getEnemyTargeting(e, avoidMult);
    const offscreen = isBossOffscreen(e, viewW, viewH);
    if (e.ranged) {
      updateRangedEnemy(e, i, dt, slowMul, d, nx, ny, ax, ay, offscreen);
    } else {
      updateMeleeEnemy(e, dt, slowMul, d, nx, ny, ax, ay, offscreen);
    }

    if (e.boss) updateBossAttacks(e, dt, godMode);

    handleContactDamage(e, dt, godMode);
    updateBossStuck(e, dt, startX, startY);
  }
}

function cleanupDeadEnemies(camX, camY, viewW, viewH) {
  const minX = camX - WORLD.despawnPad;
  const maxX = camX + viewW + WORLD.despawnPad;
  const minY = camY - WORLD.despawnPad;
  const maxY = camY + viewH + WORLD.despawnPad;
  const worldLim = WORLD.halfSize + WORLD.despawnPad;

  for (let i = enemies.length - 1; i >= 0; i--) {
    const e = enemies[i];
    const outWorld = (e.x < -worldLim || e.x > worldLim || e.y < -worldLim || e.y > worldLim);
    if (!e.alive || e.x < minX || e.x > maxX || e.y < minY || e.y > maxY || outWorld) {
      if (e.boss) continue; // never despawn the boss offscreen
      enemies[i] = enemies[enemies.length - 1];
      enemies.pop();
      if (e.alive) e.alive = false;
      if (e.boss) spawn.bossAlive = false;
      enemyPool.put(e);
    }
  }
}

export {
  spawnDmgText,
  damageEnemy,
  updateEnemyShots,
  updateVoidZones,
  updateEnemies,
  cleanupDeadEnemies,
};
