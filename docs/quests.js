import { LOOT_CONFIG, COLORS } from "./config.js";
import { rand, randi, TAU, clamp } from "./math.js";
import { isBlockedByObstacle } from "./spawn.js";
import { quest, questItems, player, clampPointToWorld, gems } from "./state.js";
import { gemPool } from "./pools.js";
import { popFloatText } from "./float_text.js";

const QUEST_TYPES = ["kill", "scavenge", "drop", "nohit"];
const QUEST_TOUCH_CD = 0.8;
const QUEST_SPAWN_DELAY = 12;
const QUEST_RESPAWN_DELAY = 18;
const QUEST_RESPAWN_JITTER = 0.25;
const QUEST_GIVER_MIN_DIST = 1200;
const QUEST_GIVER_MAX_DIST = 2200;
const QUEST_ITEM_MIN_DIST = 900;
const QUEST_ITEM_MAX_DIST = 2400;
const QUEST_NOTICE_LIFE = 3.0;
const QUEST_KILL_LVL_SCALE = 0.15;
const QUEST_SCAVENGE_LVL_SCALE = 0.06;
const QUEST_DROP_LVL_SCALE = 0.15;

let runtime = { addXP: null, openAug: null, openTrinket: null, addParticles: null };

export function setQuestRuntime({ addXP, openAug, openTrinket, addParticles }) {
  runtime.addXP = addXP;
  runtime.openAug = openAug;
  runtime.openTrinket = openTrinket;
  runtime.addParticles = addParticles;
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

function formatQuestObjective(justGiven=false) {
  if (quest.type === "kill") {
    return justGiven ? `Kill ${quest.target} mobs` : `Kill ${quest.progress}/${quest.target} mobs`;
  }
  if (quest.type === "scavenge") {
    return justGiven ? `Loot ${quest.target} relics`: `Loot ${quest.progress}/${quest.target} relics`;
  }
  if (quest.type === "drop") {
    const pct = Math.round(clamp(quest.dropChance, 0, 1) * 100);
    return justGiven ? `Collect ${quest.target} trophies (drop rate: ${pct}%)` : `Collect ${quest.progress}/${quest.target} trophies (drop rate: ${pct}%)`;
  }
  if (quest.type === "nohit") {
    return justGiven ? `No hit for ${quest.target}s`: `No hit for ${quest.progress}/${quest.target}s`;
  }
  return "-";
}

function pushQuestNotice(text, color = COLORS.quest, size = 18) {
  popFloatText(player.x, player.y - 28, text, color, size, QUEST_NOTICE_LIFE, 16, 50, 80);
}

function completeQuest() {
  if (quest.completed) return;
  quest.completed = true;
  pushQuestNotice("Objective Complete!", COLORS.gold, 20);
}

function abortQuest() {
  quest.active = false;
  quest.completed = false;
  quest.type = "";
  quest.progress = 0;
  quest.target = 0;
  quest.timer = 0;
  quest.duration = 0;
  quest.dropChance = 0;
  quest.lastHp = player.hp;
  clearQuestItems();
  scheduleQuestGiver(getRespawnDelay());
  pushQuestNotice("Objective Failed!", COLORS.warn, 20);
}

function assignQuest() {
  const type = QUEST_TYPES[randi(QUEST_TYPES.length)];
  quest.active = true;
  quest.completed = false;
  quest.type = type;
  quest.progress = 0;
  quest.timer = 0;
  quest.duration = 0;
  quest.dropChance = 0;
  quest.lastHp = player.hp;

  const lvl = Math.max(1, player.level || 1);
  if (type === "kill") {
    quest.target = Math.floor(randi(40, 25) * (1 + lvl * QUEST_KILL_LVL_SCALE));
  } else if (type === "scavenge") {
    quest.target = randi(5, 3) + Math.floor(lvl * QUEST_SCAVENGE_LVL_SCALE);
    spawnQuestItems(quest.target, QUEST_ITEM_MIN_DIST, QUEST_ITEM_MAX_DIST, "scavenge");
  } else if (type === "drop") {
    quest.target = randi(8, 4) + Math.floor(lvl * QUEST_DROP_LVL_SCALE);
    quest.dropChance = rand(0.15, 0.09);
  } else {
    quest.duration = randi(26, 14);
    quest.target = quest.duration;
  }

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

function grantExpReward() {
  const lvl = Math.max(1, player.level || 1);
  const count = 8 + Math.floor(lvl * 0.65);
  const value = 2 + Math.floor(lvl * 0.35);
  spawnRewardGems(quest.giverX, quest.giverY, count * 2, value);
  if (runtime.addParticles) runtime.addParticles(quest.giverX, quest.giverY, COLORS.gold, 18, 420);
}

function rewardQuest() {
  const roll = randi(3);
  if (roll === 0) {
    if (runtime.openAug) runtime.openAug();
    else grantExpReward();
    return;
  }
  if (roll === 1) {
    grantExpReward();
    return;
  }
  if (runtime.openTrinket && runtime.openTrinket()) return;
  grantExpReward();
}

function updateQuestItems(dt) {
  let completedNow = false;
  for (let i = questItems.length - 1; i >= 0; i--) {
    const it = questItems[i];
    if (!it.alive) { questItems[i] = questItems[questItems.length - 1]; questItems.pop(); continue; }
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
  if (quest.timer >= quest.duration) {
    completeQuest();
  }
  return true;
}

export function resetQuests() {
  quest.active = false;
  quest.completed = false;
  quest.type = "";
  quest.target = 0;
  quest.progress = 0;
  quest.timer = 0;
  quest.duration = 0;
  quest.dropChance = 0;
  quest.touchCd = 0;
  quest.cooldown = 0;
  quest.lastHp = player.hp;
  clearQuestItems();
  quest.giverX = player.x;
  quest.giverY = player.y;
  scheduleQuestGiver(QUEST_SPAWN_DELAY);
}

export function onEnemyKilled(e) {
  if (!quest.active || quest.completed) return;
  if (quest.type === "kill") {
    if (!e.boss) {
      quest.progress = Math.min(quest.target, quest.progress + 1);
      if (quest.progress >= quest.target) completeQuest();
    }
  } else if (quest.type === "drop") {
    if (!e.boss && Math.random() < quest.dropChance) {
      spawnQuestItem(e.x, e.y, "drop");
    }
  }
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
      quest.active = false;
      quest.completed = false;
      quest.type = "";
      quest.progress = 0;
      quest.target = 0;
      quest.timer = 0;
      quest.duration = 0;
      quest.dropChance = 0;
      quest.cooldown = 2.0;
      clearQuestItems();
      scheduleQuestGiver(getRespawnDelay());
    } else if (!quest.active && quest.cooldown <= 0) {
      assignQuest();
    }
  }

  if (!quest.active || quest.completed) return;

  if (quest.type === "scavenge" || quest.type === "drop") {
    updateQuestItems(dt);
  } else if (quest.type === "nohit") {
    if (!updateNoHit(dt)) return;
  }
}

export function getQuestHudText() {
  if (!quest.active && !quest.completed) return "-";
  if (quest.completed) return "Complete! Return to giver.";
  return formatQuestObjective();
}
