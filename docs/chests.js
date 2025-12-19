import { CHEST_CONFIG, TRINKET_CONFIG, AUGMENT_CONFIG, COLORS } from "./config.js";
import { rand, randi, hypot, TAU } from "./math.js";
import { addTelegraph } from "./telegraph.js";
import { isBlockedByObstacle, spawnElitePackAt } from "./spawn.js";
import { addParticles } from "./particles.js";
import { damageEnemy } from "./enemies.js";
import { trinketSlotsFull } from "./trinkets.js";
import { popFloatText } from "./float_text.js";
import {
  player,
  buffs,
  chests,
  enemies,
  clampPointToWorld,
} from "./state.js";
import { chestPool } from "./pools.js";

let runtime = { addXP: null, openTrinket: null, openAug: null };

export function setChestRuntime({ addXP, openTrinket, openAug }) {
  runtime.addXP = addXP;
  runtime.openTrinket = openTrinket;
  runtime.openAug = openAug;
}

function requireRuntime() {
  if (!runtime.addXP) throw new Error("Chest runtime missing; call setChestRuntime({ addXP }) first.");
  return runtime;
}

const chestSpawn = {
  t: CHEST_CONFIG.timerStart,
  min: CHEST_CONFIG.timerMin,
  max: CHEST_CONFIG.timerMax,
  activeMax: CHEST_CONFIG.activeMax,
};

const trinketSpawn = {
  t: TRINKET_CONFIG.chest.timerStart,
  min: TRINKET_CONFIG.chest.timerMin,
  max: TRINKET_CONFIG.chest.timerMax,
  activeMax: TRINKET_CONFIG.chest.activeMax,
};

const augSpawn = {
  t: AUGMENT_CONFIG.chest.timerStart,
  min: AUGMENT_CONFIG.chest.timerMin,
  max: AUGMENT_CONFIG.chest.timerMax,
  activeMax: AUGMENT_CONFIG.chest.activeMax,
};

const FLOAT_LIFE = 0.9;

function queueChestBomb(x, y) {
  requireRuntime(); // ensure runtime present
  const radius = CHEST_CONFIG.bomb.radius;
  const dmg = CHEST_CONFIG.bomb.dmgBase + player.level * CHEST_CONFIG.bomb.dmgPerLevel;
  addTelegraph({
    x, y,
    radius,
    color: COLORS.warn,
    time: CHEST_CONFIG.bomb.telegraphTime,
    fire: () => {
      for (let i = 0; i < enemies.length; i++) {
        const e = enemies[i];
        if (!e.alive) continue;
        const dx = e.x - x, dy = e.y - y;
        const r2 = radius * radius;
        if (dx * dx + dy * dy <= r2) {
          damageEnemy(e, dmg, 0, 0, 0, false);
        }
      }
      addParticles(x, y, COLORS.warn, CHEST_CONFIG.bomb.particles, CHEST_CONFIG.bomb.particleSpread);
    }
  });
}

const CHEST_BONUSES = [
  {
    id: "heal",
    label: "HEAL",
    color: COLORS.heal,
    apply: () => {
      requireRuntime();
      const amt = player.maxHp * CHEST_CONFIG.bonuses.healPct;
      player.hp = Math.min(player.maxHp, player.hp + amt);
      popFloatText(player.x, player.y - 14, `+${Math.ceil(amt)} HP`, COLORS.heal, 16, FLOAT_LIFE);
    }
  },
  {
    id: "level",
    label: "LEVEL UP",
    color: COLORS.gold,
    apply: () => {
      const { addXP } = requireRuntime();
      addXP(player.xpNeed);
      popFloatText(player.x, player.y - 14, "LEVEL UP!", COLORS.gold, 16, FLOAT_LIFE);
    }
  },
  {
    id: "magnet",
    label: "MAGNET",
    color: COLORS.gem,
    apply: () => {
      requireRuntime();
      buffs.magnet = Math.max(buffs.magnet, CHEST_CONFIG.bonuses.magnet);
      popFloatText(player.x, player.y - 14, "MAGNET!", COLORS.gem, 16, FLOAT_LIFE);
    }
  },
  {
    id: "shockwave",
    label: "SHOCKWAVE",
    color: COLORS.player,
    apply: () => {
      requireRuntime();
      const baseKnock = CHEST_CONFIG.shockwave.baseKnock;
      const dmg = CHEST_CONFIG.shockwave.dmgBase + player.level * CHEST_CONFIG.shockwave.dmgPerLevel;
      for (let i = 0; i < enemies.length; i++) {
        const e = enemies[i];
        if (!e.alive) continue;
        const dx = e.x - player.x, dy = e.y - player.y;
        const d = hypot(dx, dy) || 1;
        const nx = dx / d, ny = dy / d;
        e.kx += nx * baseKnock;
        e.ky += ny * baseKnock;
        if (d < CHEST_CONFIG.shockwave.damageRadius) damageEnemy(e, dmg, nx, ny, CHEST_CONFIG.shockwave.knockPush, false);
      }
      addParticles(player.x, player.y, COLORS.player, 42, 520);
      popFloatText(player.x, player.y - 14, "SHOCKWAVE!", COLORS.player, 16, FLOAT_LIFE);
    }
  },
  {
    id: "shield",
    label: "SHIELD",
    color: "#7fe7ff",
    apply: () => {
      requireRuntime();
      buffs.shield = Math.max(buffs.shield, CHEST_CONFIG.bonuses.shield);
      popFloatText(player.x, player.y - 14, "SHIELD!", "#7fe7ff", 16, FLOAT_LIFE);
    }
  },
  {
    id: "freeze",
    label: "FREEZE",
    color: "#b160ff",
    apply: () => {
      requireRuntime();
      buffs.slow = Math.max(buffs.slow, CHEST_CONFIG.bonuses.freeze);
      popFloatText(player.x, player.y - 14, "FREEZE!", "#b160ff", 16, FLOAT_LIFE);
    }
  },
  {
    id: "xpboost",
    label: "XP BOOST",
    color: COLORS.gold,
    apply: () => {
      requireRuntime();
      buffs.xp = Math.max(buffs.xp, CHEST_CONFIG.bonuses.xp);
      popFloatText(player.x, player.y - 14, "XP BOOST!", COLORS.gold, 16, FLOAT_LIFE);
    }
  },
  {
    id: "overcharge",
    label: "OVERCHARGE",
    color: "#ff9dfc",
    apply: () => {
      requireRuntime();
      buffs.power = Math.max(buffs.power, CHEST_CONFIG.bonuses.power);
      popFloatText(player.x, player.y - 14, "OVERCHARGE!", "#ff9dfc", 16, FLOAT_LIFE);
    }
  },
  {
    id: "sprint",
    label: "HYPER SPRINT",
    color: COLORS.player,
    apply: () => {
      requireRuntime();
      buffs.haste = Math.max(buffs.haste, CHEST_CONFIG.bonuses.haste);
      popFloatText(player.x, player.y - 14, "SPEED UP!", COLORS.player, 16, FLOAT_LIFE);
    }
  },
  {
    id: "bomb",
    label: "BOMB",
    color: COLORS.warn,
    apply: () => {
      requireRuntime();
      queueChestBomb(player.x, player.y);
      popFloatText(player.x, player.y - 14, "BOMB!", COLORS.warn, 16, FLOAT_LIFE);
    }
  },
];

function countChests(kind) {
  let count = 0;
  for (let i = 0; i < chests.length; i++) {
    if (chests[i].alive && chests[i].kind === kind) count++;
  }
  return count;
}

function spawnChest(camX, camY, W, H, kind, activeMax) {
  if (countChests(kind) >= activeMax) return null;

  const c = chestPool.get();
  c.alive = true;
  c.pulse = rand(TAU, 0);
  c.kind = kind;

  for (let tries = 0; tries < CHEST_CONFIG.spawnTries; tries++) {
    const x = player.x + rand(W * CHEST_CONFIG.spawnOffset, -W * CHEST_CONFIG.spawnOffset);
    const y = player.y + rand(H * CHEST_CONFIG.spawnOffset, -H * CHEST_CONFIG.spawnOffset);
    const dx = x - player.x, dy = y - player.y;
    if (dx * dx + dy * dy > CHEST_CONFIG.spawnMinDist * CHEST_CONFIG.spawnMinDist && !isBlockedByObstacle(x, y, c.r || 0, CHEST_CONFIG.radiusPadding || 8)) {
      c.x = x;
      c.y = y;
      break;
    }
  }
  const limited = clampPointToWorld(c.x || player.x, c.y || player.y, c.r || 0);
  c.x = limited.x;
  c.y = limited.y;
  chests.push(c);
  return c;
}

export function updateChests(dt, camX, camY, W, H) {
  chestSpawn.t -= dt;
  if (chestSpawn.t <= 0) {
    chestSpawn.t = rand(chestSpawn.max, chestSpawn.min);
    spawnChest(camX, camY, W, H, "bonus", chestSpawn.activeMax);
  }

  if (!trinketSlotsFull()) {
    trinketSpawn.t -= dt;
    if (trinketSpawn.t <= 0) {
      trinketSpawn.t = rand(trinketSpawn.max, trinketSpawn.min);
      spawnChest(camX, camY, W, H, "trinket", trinketSpawn.activeMax);
    }
  }

  augSpawn.t -= dt;
  if (augSpawn.t <= 0) {
    augSpawn.t = rand(augSpawn.max, augSpawn.min);
    const chest = spawnChest(camX, camY, W, H, "aug", augSpawn.activeMax);
    if (chest) {
      const packCount = randi(AUGMENT_CONFIG.elitePack.countMax + 1, AUGMENT_CONFIG.elitePack.countMin);
      spawnElitePackAt(chest.x, chest.y, packCount, AUGMENT_CONFIG.elitePack.radiusMin, AUGMENT_CONFIG.elitePack.radiusMax);
    }
  }

  for (let i = chests.length - 1; i >= 0; i--) {
    const c = chests[i];
    if (!c.alive) { chests[i] = chests[chests.length - 1]; chests.pop(); chestPool.put(c); continue; }
    c.pulse += dt * CHEST_CONFIG.pulseSpeed;

    const dx = c.x - player.x, dy = c.y - player.y;
    const rr = c.r + player.r + CHEST_CONFIG.radiusPadding;
    if (dx * dx + dy * dy <= rr * rr) {
      c.alive = false;
      if (c.kind === "trinket") {
        if (trinketSlotsFull()) {
          const { addXP } = requireRuntime();
          const lvl = Math.max(1, player.level || 1);
          const mult = Math.min(1.6, 0.6 + lvl * 0.02);
          const bonus = Math.max(1, Math.round(player.xpNeed * mult));
          addXP(bonus);
          addParticles(c.x, c.y, COLORS.gold, CHEST_CONFIG.openParticles.count, CHEST_CONFIG.openParticles.spread);
          popFloatText(c.x, c.y - 10, `+${bonus} XP`, COLORS.gold, 18, FLOAT_LIFE);
        } else {
          if (runtime.openTrinket) runtime.openTrinket();
          addParticles(c.x, c.y, COLORS.trinket, CHEST_CONFIG.openParticles.count, CHEST_CONFIG.openParticles.spread);
          popFloatText(c.x, c.y - 10, "TRINKET", COLORS.trinket, 18, FLOAT_LIFE);
        }
      } else if (c.kind === "aug") {
        const opened = runtime.openAug ? runtime.openAug() : false;
        if (opened) {
          addParticles(c.x, c.y, COLORS.aug, CHEST_CONFIG.openParticles.count, CHEST_CONFIG.openParticles.spread);
          popFloatText(c.x, c.y - 10, "AUGMENT", COLORS.aug, 18, FLOAT_LIFE);
        } else {
          let bonus = CHEST_BONUSES[randi(CHEST_BONUSES.length)];
          if (player.hp / player.maxHp < CHEST_CONFIG.healBias.hpPct && Math.random() < CHEST_CONFIG.healBias.chance) bonus = CHEST_BONUSES[0];
          addParticles(c.x, c.y, COLORS.chest, CHEST_CONFIG.openParticles.count, CHEST_CONFIG.openParticles.spread);
          popFloatText(c.x, c.y - 10, bonus.label, bonus.color, 18, FLOAT_LIFE);
          bonus.apply();
        }
      } else {
        let bonus = CHEST_BONUSES[randi(CHEST_BONUSES.length)];
        if (player.hp / player.maxHp < CHEST_CONFIG.healBias.hpPct && Math.random() < CHEST_CONFIG.healBias.chance) bonus = CHEST_BONUSES[0];
        addParticles(c.x, c.y, COLORS.chest, CHEST_CONFIG.openParticles.count, CHEST_CONFIG.openParticles.spread);
        popFloatText(c.x, c.y - 10, bonus.label, bonus.color, 18, FLOAT_LIFE);
        bonus.apply();
      }
    }

    if (!c.alive) {
      chests[i] = chests[chests.length - 1];
      chests.pop();
      chestPool.put(c);
    }
  }
}

export function resetChests() {
  chestSpawn.t = CHEST_CONFIG.timerStart;
  trinketSpawn.t = TRINKET_CONFIG.chest.timerStart;
  augSpawn.t = AUGMENT_CONFIG.chest.timerStart;
}
