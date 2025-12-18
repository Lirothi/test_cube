import {
  COLORS,
  WEAPON_RIDERS,
  RANGED_SHOT_CONFIG,
  LOOT_CONFIG,
  ENEMY_BEHAVIOR,
  BUFF_EFFECTS,
  PLAYER_CONFIG,
  BOSS_CONFIG,
  BOSS4_CONFIG,
  ENEMY_TYPES,
} from "./config.js";
import { TAU, rand, randi, clamp, hypot } from "./math.js";
import { sound } from "./audio.js";
import { applyArmorDamage } from "./player.js";
import { getEnemyTier } from "./spawn.js";
import { addTelegraph } from "./telegraph.js";
import { obstacleAvoidance, damageObstacle, resolveObstacles, OBSTACLE_TYPE } from "./obstacles.js";
import { addParticles } from "./particles.js";
import {
  clampEntityToWorld,
  player,
  buffs,
  enemies,
  enemyShots,
  voidZones,
  activeObstacles,
  obstacles,
  spawn,
  gems,
  bullets,
  dmgTexts,
} from "./state.js";
import {
  bulletPool,
  shotPool,
  voidPool,
  gemPool,
  dmgPool,
} from "./pools.js";
import { DPS_TRACKER } from "./upgrade.js";

function spawnShockwave(x, y, radius, color = COLORS.warn) {
  const exp = bulletPool.get();
  exp.alive = true;
  exp.x = x; exp.y = y;
  exp.vx = 0; exp.vy = 0;
  exp.r = radius;
  exp.life = 0.3;
  exp.maxLife = exp.life;
  exp.dmg = 0;
  exp.critChance = 0;
  exp.critMult = 1;
  exp.color = color;
  exp.isExplosion = true;
  bullets.push(exp);
}

function spawnDmgText(x, y, amount, color = COLORS.dmg, size = 14) {
  const d = dmgPool.get();
  d.alive = true;
  d.x = x; d.y = y;
  d.vx = rand(22, -22);
  d.vy = -rand(64, 38);
  d.maxLife = rand(0.62, 0.45);
  d.life = d.maxLife;
  d.text = String(Math.round(amount));
  d.color = color;
  d.size = size;
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

function damageEnemy(e, dmg, pushX, pushY, pushStrength, showText = true, crit = false, source = null, applyRiders = true) {
  if (dmg > 0) {
    const inflicted = Math.min(dmg, Math.max(0, e.hp));
    e.hp -= dmg;
    if (source) DPS_TRACKER[source] = (DPS_TRACKER[source] || 0) + inflicted;
    if (showText) {
      const color = crit ? COLORS.crit : COLORS.dmg;
      const size = crit ? 18 : 14;
      spawnDmgText(e.x, e.y - e.r - 6, dmg, color, size);
    }
  }
  if (pushStrength > 0) {
    const resist = e.knockResist || 0;
    const effPush = pushStrength * (1 - resist);
    e.kx += pushX * effPush;
    e.ky += pushY * effPush;
  }
  if (applyRiders && e.hp > 0 && source) {
    if (source === "magic") {
      const cfg = WEAPON_RIDERS.magic.slow;
      e.slowT = Math.max(e.slowT, cfg.duration);
      e.slowMul = cfg.mult;
    } else if (source === "rail") {
      const cfg = WEAPON_RIDERS.rail.burn;
      e.burnT = Math.max(e.burnT, cfg.duration);
      e.burnDps = Math.max(e.burnDps, dmg * cfg.dpsPct);
    } else if (source === "axe") {
      const cfg = WEAPON_RIDERS.axe.bleed;
      e.bleedT = Math.max(e.bleedT, cfg.duration);
      e.bleedDps = Math.max(e.bleedDps, dmg * cfg.dpsPct);
    }
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
    if (e.boss) {
      spawn.bossAlive = false;
      spawn.bossRef = null;
    }
    e.alive = false;
  }
}

function spawnEnemyShot(x, y, nx, ny, speed, dmg) {
  const s = shotPool.get();
  s.alive = true;
  s.x = x; s.y = y;
  s.vx = nx * speed;
  s.vy = ny * speed;
  s.r = RANGED_SHOT_CONFIG.radius;
  s.dmg = dmg;
  s.life = RANGED_SHOT_CONFIG.life;
  s.color = RANGED_SHOT_CONFIG.color; // red projectile
  enemyShots.push(s);
}

function spawnVoidZone(x, y, radius, duration, dps, color, type, tick = 0.25) {
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
  voidZones.push(z);
}

function updateEnemyShots(dt) {
  for (let i = enemyShots.length - 1; i >= 0; i--) {
    const s = enemyShots[i];
    if (!s.alive) { enemyShots[i] = enemyShots[enemyShots.length - 1]; enemyShots.pop(); shotPool.put(s); continue; }

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
    if (blocked) s.alive = false;

    const dx = player.x - s.x;
    const dy = player.y - s.y;
    const rr = player.r + s.r + RANGED_SHOT_CONFIG.hitPad;
    if (dx * dx + dy * dy <= rr * rr) {
      s.alive = false;
      if (buffs.shield <= 0 && player.iFrame <= 0) {
        const dmg = applyArmorDamage(s.dmg);
        player.hp -= dmg;
        player.iFrame = PLAYER_CONFIG.shotIFrame;
        spawnDmgText(player.x, player.y - player.r - 12, dmg, COLORS.warnHit);
        addParticles(player.x, player.y, COLORS.warnHitDim, 8, 360);
      } else {
        addParticles(s.x, s.y, COLORS.shieldBlock, 4, 260);
      }
    }

    if (s.life <= 0) s.alive = false;

    if (!s.alive) {
      enemyShots[i] = enemyShots[enemyShots.length - 1];
      enemyShots.pop();
      shotPool.put(s);
    }
  }
}

function updateVoidZones(dt, godMode) {
  for (let i = voidZones.length - 1; i >= 0; i--) {
    const z = voidZones[i];
    if (!z.alive) { voidZones[i] = voidZones[voidZones.length - 1]; voidZones.pop(); voidPool.put(z); continue; }

    z.life -= dt;
    if (z.life <= 0) z.alive = false;

    if (z.alive) {
      const dx = player.x - z.x;
      const dy = player.y - z.y;
      const rr = player.r + z.radius;
      if (dx * dx + dy * dy <= rr * rr && buffs.shield <= 0 && !godMode) {
        z.tickT -= dt;
        while (z.tickT <= 0) {
          z.tickT += z.tick;
          const dmg = z.dps * z.tick;
          player.hp -= dmg;
          spawnDmgText(player.x, player.y - player.r - 12, dmg, COLORS.warnHit, 14);
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
}

function updateEnemies(dt, godMode) {
  const slowMul = (buffs.slow > 0) ? BUFF_EFFECTS.slowMoveMult : 1.0;

  for (let i = 0; i < enemies.length; i++) {
    const e = enemies[i];
    if (!e.alive) continue;

    // rider DoTs and timers
    if (e.burnT > 0) {
      const dmg = e.burnDps * dt;
      e.burnT -= dt;
      if (dmg > 0) damageEnemy(e, dmg, 0, 0, 0, false, false, "rail", false);
      e.burnFx = (e.burnFx || 0) - dt;
      if (e.burnFx <= 0) {
        addParticles(e.x, e.y, COLORS.enemyF, 6, 160);
        e.burnFx = 0.12 + rand(0.12, 0.04);
      }
      if (!e.alive) continue;
    }
    if (e.bleedT > 0) {
      const dmg = e.bleedDps * dt;
      e.bleedT -= dt;
      if (dmg > 0) damageEnemy(e, dmg, 0, 0, 0, false, false, "axe", false);
      e.bleedFx = (e.bleedFx || 0) - dt;
      if (e.bleedFx <= 0) {
        addParticles(e.x, e.y, COLORS.warn, 6, 140);
        e.bleedFx = 0.14 + rand(0.14, 0.05);
      }
      if (!e.alive) continue;
    }
    if (e.slowT > 0) e.slowT -= dt;

    // knockback velocity
    e.x += e.kx * dt;
    e.y += e.ky * dt;
    const kbDecay = Math.pow(ENEMY_BEHAVIOR.knockbackDecayBase, dt);
    e.kx *= kbDecay;
    e.ky *= kbDecay;

    const dx = player.x - e.x;
    const dy = player.y - e.y;
    const d = hypot(dx, dy) || 1;
    let nx = dx / d, ny = dy / d;
    const avoid = obstacleAvoidance(e.x, e.y, e.r);
    nx += avoid.ax;
    ny += avoid.ay;
    const nlen = hypot(nx, ny) || 1;
    nx /= nlen; ny /= nlen;

    const statusSpeedMul = (e.slowT > 0) ? e.slowMul : 1.0;
    if (e.ranged) {
      const prefer = (e.spitter ? (e.spitRange || e.shotRange) : e.shotRange) || ENEMY_BEHAVIOR.rangedPreferredRange;
      const bossChase = (e.boss && d > prefer * ENEMY_BEHAVIOR.preferredFar)
        ? (1 + clamp((d - prefer * ENEMY_BEHAVIOR.preferredFar) / (prefer * 1.2), 0, 1.5))
        : 1;

      if (d < prefer * ENEMY_BEHAVIOR.preferredClose) {
        const flee = e.speed * ENEMY_BEHAVIOR.fleeMult;
        e.x += (-nx) * (flee * slowMul * statusSpeedMul) * dt;
        e.y += (-ny) * (flee * slowMul * statusSpeedMul) * dt;
      } else if (d > prefer * ENEMY_BEHAVIOR.preferredFar) {
        const creep = e.speed * ENEMY_BEHAVIOR.creepMult;
        e.x += (nx) * (creep * bossChase * slowMul * statusSpeedMul) * dt;
        e.y += (ny) * (creep * bossChase * slowMul * statusSpeedMul) * dt;
      } else {
        const strafe = e.speed * ENEMY_BEHAVIOR.strafeMult;
        const px = -ny, py = nx;
        const dir = (i & 1) ? 1 : -1;
        e.x += (px * dir) * (strafe * slowMul * statusSpeedMul) * dt;
        e.y += (py * dir) * (strafe * slowMul * statusSpeedMul) * dt;
      }

      if (e.spitter) {
        e.spitT -= dt;
        if (e.spitT <= 0 && d < (e.spitRange || ENEMY_BEHAVIOR.rangedPreferredRange)) {
          const slowFireMul = (buffs.slow > 0) ? BUFF_EFFECTS.slowFireMult : 1.0;
          e.spitT += (e.spitCd || RANGED_SHOT_CONFIG.defaultCd) * slowFireMul;
          const tx = player.x, ty = player.y;
          const marker = ++e.shotSeq;
          addTelegraph({
            x: tx, y: ty, radius: e.spitRadius, color: e.spitColor, time: e.spitTelegraph,
            fire: () => {
              if (e.alive && e.shotSeq === marker) spawnVoidZone(tx, ty, e.spitRadius, e.spitDuration, e.spitDps, e.spitColor, e.spitType, e.spitTick);
            }
          });
        }
      } else {
        e.shotT -= dt;
        if (e.shotT <= 0 && d < (e.shotRange || ENEMY_BEHAVIOR.rangedPreferredRange)) {
          const slowFireMul = (buffs.slow > 0) ? BUFF_EFFECTS.slowFireMult : 1.0;
          e.shotT += (e.shotCd || RANGED_SHOT_CONFIG.defaultCd) * slowFireMul;
          const marker = ++e.shotSeq;
          addTelegraph({
            x: e.x, y: e.y, dx: nx, dy: ny, radius: RANGED_SHOT_CONFIG.telegraphRadius, color: COLORS.warn, time: RANGED_SHOT_CONFIG.telegraphTime,
            follow: (tg) => {
              if (!e.alive) return;
              tg.x = e.x;
              tg.y = e.y;
            },
            fire: () => {
              if (e.alive && e.shotSeq === marker) spawnEnemyShot(e.x, e.y, nx, ny, e.shotSpeed || RANGED_SHOT_CONFIG.defaultSpeed, e.shotDmg || RANGED_SHOT_CONFIG.defaultDmg);
            }
          });
        }
      }
    } else {
      const bossChase = e.boss ? (1 + clamp((d - 320) / 900, 0, 1.5)) : 1;
      e.x += nx * (e.speed * slowMul * bossChase) * dt;
      e.y += ny * (e.speed * slowMul * bossChase) * dt;
    }

    if (e.boss) {
      // Boss X nova
      if (e.novaCd > 0) {
        e.novaT -= dt;
        if (e.novaT <= 0) {
          const slowFireMul = (buffs.slow > 0) ? BUFF_EFFECTS.slowFireMult : 1.0;
          e.novaT += (e.novaCd || 6) * slowFireMul;
          const marker = ++e.novaSeq;
          addTelegraph({
            x: e.x, y: e.y, radius: e.novaRadius, color: BOSS_CONFIG.telegraph.color, time: e.novaTelegraph,
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

      // Boss Y unique attacks
      if (e.voidCd > 0) {
        e.voidT -= dt;
        if (e.voidT <= 0) {
          const slowFireMul = (buffs.slow > 0) ? BUFF_EFFECTS.slowFireMult : 1.0;
          e.voidT += (e.voidCd || 5) * slowFireMul;
          const marker = ++e.voidSeq;
          const count = Math.max(1, e.voidCount || 1);
          for (let k = 0; k < count; k++) {
            const ang = rand(TAU, 0);
            const dist = rand(140, 60);
            const tx = player.x + Math.cos(ang) * dist;
            const ty = player.y + Math.sin(ang) * dist;
            addTelegraph({
              x: tx, y: ty, radius: e.voidRadius, color: e.voidColor, time: e.voidTelegraph,
              fire: () => {
                if (e.alive && e.voidSeq === marker) spawnVoidZone(tx, ty, e.voidRadius, e.voidDuration, e.voidDps, e.voidColor, "void", e.voidTick);
              }
            });
          }
        }
      }

      if (e.barrageCd > 0) {
        e.barrageT -= dt;
        if (e.barrageT <= 0) {
          const slowFireMul = (buffs.slow > 0) ? BUFF_EFFECTS.slowFireMult : 1.0;
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

      // Boss slam (melee strike)
      if (e.slamCd > 0) {
        e.slamT -= dt;
        if (e.slamT <= 0) {
          const slowFireMul = (buffs.slow > 0) ? BUFF_EFFECTS.slowFireMult : 1.0;
          e.slamT += (e.slamCd || 4.5) * slowFireMul;
          const marker = ++e.slamSeq;
          addTelegraph({
            x: e.x, y: e.y, radius: e.slamRadius || 120, color: e.slamColor || COLORS.warn, time: e.slamTelegraph || 0.9,
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
                }
              }
              addParticles(e.x, e.y, e.slamColor || COLORS.warn, 18, 360);
              spawnShockwave(e.x, e.y, r, e.slamColor || COLORS.warn);
            }
          });
        }
      }

      // Boss rockfall (Titan)
      if (e.rockCd > 0) {
        e.rockT -= dt;
        if (e.rockT <= 0) {
          const slowFireMul = (buffs.slow > 0) ? BUFF_EFFECTS.slowFireMult : 1.0;
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
                }
                addParticles(tx, ty, e.rockColor || COLORS.rock, 14, 420);
                spawnShockwave(tx, ty, r, e.rockColor || COLORS.rock);
              }
            });
          }
        }
      }

      // Boss prism burst (Archon)
      if (e.rayCd > 0) {
        e.rayT -= dt;
        if (e.rayT <= 0) {
          const slowFireMul = (buffs.slow > 0) ? BUFF_EFFECTS.slowFireMult : 1.0;
          e.rayT += (e.rayCd || 5.5) * slowFireMul;
          const marker = ++e.raySeq;
          const rays = Math.max(3, e.rayCount || 6);
          const baseAng = Math.atan2(player.y - e.y, player.x - e.x);
          addTelegraph({
            x: e.x, y: e.y, radius: 0, color: BOSS4_CONFIG.telegraph.color, time: e.rayTelegraph || 0.7,
            follow: (tg) => { if (e.alive) { tg.x = e.x; tg.y = e.y; } },
            fire: () => {
              if (!e.alive || e.raySeq !== marker) return;
              for (let k = 0; k < rays; k++) {
                const ang = baseAng + (TAU * k / rays);
                spawnEnemyShot(e.x, e.y, Math.cos(ang), Math.sin(ang), e.raySpeed || 600, e.rayDmg || 12);
              }
            }
          });
        }
      }

      // Boss minefield (Archon)
      if (e.mineCd > 0) {
        e.mineT -= dt;
        if (e.mineT <= 0) {
          const slowFireMul = (buffs.slow > 0) ? BUFF_EFFECTS.slowFireMult : 1.0;
          e.mineT += (e.mineCd || 7) * slowFireMul;
          const marker = ++e.mineSeq;
          const count = Math.max(1, e.mineCount || 4);
          for (let k = 0; k < count; k++) {
            const ang = rand(TAU, 0);
            const dist = rand(e.mineOffsetMax || 220, e.mineOffsetMin || 80);
            const tx = player.x + Math.cos(ang) * dist;
            const ty = player.y + Math.sin(ang) * dist;
            addTelegraph({
              x: tx, y: ty, radius: e.mineRadius || 85, color: e.mineColor || COLORS.voidFire, time: e.mineTelegraph || 1.0,
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
                }
                addParticles(tx, ty, e.mineColor || COLORS.voidFire, 16, 360);
              }
            });
          }
        }
      }
    }

    // contact damage
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

        player.x += cnx * PLAYER_CONFIG.hitPush;
        player.y += cny * PLAYER_CONFIG.hitPush;
      } else {
        e.kx -= cnx * PLAYER_CONFIG.shieldPushback * dt;
        e.ky -= cny * PLAYER_CONFIG.shieldPushback * dt;
      }
    }
  }
}

export {
  spawnShockwave,
  spawnDmgText,
  damageEnemy,
  updateEnemyShots,
  updateVoidZones,
  updateEnemies,
};
