import { player, buffs, BASE_STATS, obstacles, input, clampEntityToWorld, trinketBonuses } from "./state.js";
import { XP_CONFIG, UPGRADE_CONFIG, BUFF_EFFECTS } from "./config.js";
import { clamp, hypot } from "./math.js";
import { resolveObstacles } from "./obstacles.js";
import { upgradeState } from "./upgrade.js";

let runtime = { openLevelUp: null };

export function xpNeedForLevel(lvl) {
  const delta = lvl - 1;
  const lateDelta = Math.max(0, delta - XP_CONFIG.lateStart + 1);
  return Math.floor(
    XP_CONFIG.baseNeed +
    delta * XP_CONFIG.perLevel +
    Math.pow(delta, XP_CONFIG.curvePower) * XP_CONFIG.curveScale +
    Math.pow(lateDelta, XP_CONFIG.latePower) * XP_CONFIG.lateScale
  );
}

export function resetPlayer() {
  // place player at center but nudge out of obstacles if necessary
  player.x = 0; player.y = 0;
  for (let i = 0; i < obstacles.length; i++) {
    const o = obstacles[i];
    const reach = o.r + player.r + 2;
    const dx = player.x - o.x;
    const dy = player.y - o.y;
    if (dx * dx + dy * dy <= reach * reach) {
      const dist = Math.sqrt(dx * dx + dy * dy) || 1;
      player.x = o.x + (dx / dist) * (reach + 8);
      player.y = o.y + (dy / dist) * (reach + 8);
    }
  }
  player.hp = BASE_STATS.hp; player.maxHp = BASE_STATS.hp;
  player.speed = BASE_STATS.speed;
  player.pickup = BASE_STATS.pickup;
  player.armor = 0;
  player.resists.all = 0;
  player.resists.fire = 0;
  player.resists.poison = 0;
  player.resists.void = 0;
  player.iFrame = 0;
  player.level = 1;
  player.xp = 0;
  player.xpNeed = xpNeedForLevel(1);
  player.kills = 0;
  player.time = 0;
  buffs.magnet = 0;
  buffs.shield = 0;
  buffs.slow = 0;
  buffs.power = 0;
  buffs.haste = 0;
  buffs.xp = 0;
}

export function applyArmorDamage(dmg) {
  const armor = player.armor || 0;
  if (armor <= 0) return dmg;
  return Math.max(1, dmg - armor);
}

export function applyElementalDamage(dmg, type) {
  if (!type) return dmg;
  const resists = player.resists || {};
  const total = clamp((resists.all || 0) + (resists[type] || 0), 0, 1);
  return Math.max(0, dmg * (1 - total));
}

export function updatePlayer(dt) {
  let mx = 0, my = 0;
  if (input.up) my -= 1;
  if (input.down) my += 1;
  if (input.left) mx -= 1;
  if (input.right) mx += 1;

  const len = hypot(mx, my);
  if (len > 0) { mx /= len; my /= len; }

  const moveSpeed = player.speed * (buffs.haste > 0 ? BUFF_EFFECTS.hasteMoveMult : 1.0);
  player.x += mx * moveSpeed * dt;
  player.y += my * moveSpeed * dt;
  resolveObstacles(player, player.r);
  clampEntityToWorld(player, player.r);

  if (player.iFrame > 0) player.iFrame -= dt;
}

export function setPlayerRuntime({ openLevelUp }) {
  runtime.openLevelUp = openLevelUp;
}

function requireRuntime() {
  if (!runtime.openLevelUp) throw new Error("Player runtime missing; call setPlayerRuntime({ openLevelUp }) first.");
  return runtime;
}

export function addXP(amount) {
  const { openLevelUp } = requireRuntime();
  const buffMul = buffs.xp > 0 ? XP_CONFIG.buffMultiplier : 1.0;
  const upgradeMul = 1 + upgradeState.xpLv * UPGRADE_CONFIG.xpMultGain;
  const trinketMul = trinketBonuses.xpMult || 1;
  player.xp += amount * buffMul * upgradeMul * trinketMul;
  while (player.xp >= player.xpNeed){
    player.xp -= player.xpNeed;
    player.level++;
    player.xpNeed = xpNeedForLevel(player.level);
    openLevelUp();
    break;
  }
}
