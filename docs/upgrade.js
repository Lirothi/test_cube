import {
  XP_CONFIG,
  UPGRADE_CONFIG,
  CRIT_UPGRADES,
  MAX_WEAPONS,
  DPS_LABELS,
  WEAPON_CONFIG,
} from "./config.js";
import { fmtFloat, randi } from "./math.js";
import { weapons, weaponCount } from "./weapons.js";
import { BASE_STATS, player } from "./state.js";

export const DPS_TRACKER = { magic:0, aura:0, rail:0, axe:0, orb:0, missile:0 };

export const upgradeState = { speedLv:0, hpLv:0, pickupLv:0, armorLv:0, resAllLv:0, resFireLv:0, resPoisonLv:0, resVoidLv:0, cdLv:0, xpLv:0, critChanceLv:0, critMultLv:0 };
export function resetUpgradeState(){
  upgradeState.speedLv=0;
  upgradeState.hpLv=0;
  upgradeState.pickupLv=0;
  upgradeState.armorLv=0;
  upgradeState.resAllLv=0;
  upgradeState.resFireLv=0;
  upgradeState.resPoisonLv=0;
  upgradeState.resVoidLv=0;
  upgradeState.cdLv=0;
  upgradeState.xpLv=0;
  upgradeState.critChanceLv=0;
  upgradeState.critMultLv=0;
}
export function resetDps(){ DPS_TRACKER.magic=0; DPS_TRACKER.aura=0; DPS_TRACKER.rail=0; DPS_TRACKER.axe=0; DPS_TRACKER.orb=0; DPS_TRACKER.missile=0; }

function weaponTag(weapon, maxLv, stats, extraParts = []){
  if (!weapon.unlocked) return "Weapon - Unlock";
  const parts = [];
  const base = `Lv ${weapon.level}/${maxLv}${weapon.mastery > 0 ? ` (M${weapon.mastery})` : ""}`;
  parts.push(base);
  if (stats){
    for (const part of extraParts) parts.push(part);
    if (stats.dmg) parts.push(`DMG ${Math.round(stats.dmg)}`);
    if (stats.cd) parts.push(`CD ${fmtFloat(stats.cd, 2)}s`);
    if (stats.count) parts.push(`Count ${stats.count}`);
    if (stats.speed) parts.push(`SPD ${Math.round(stats.speed)}`);
    if (stats.range) parts.push(`Range ${Math.round(stats.range)}`);
    if (stats.radius) parts.push(`Radius ${Math.round(stats.radius)}`);
    if (stats.tick) parts.push(`Tick ${fmtFloat(stats.tick, 2)}s`);
    if (stats.pull) parts.push(`Pull ${Math.round(stats.pull)}`);
    if (stats.park) parts.push(`Park ${fmtFloat(stats.park, 2)}s`);
    if (stats.explosion) parts.push(`Explode ${Math.round(stats.explosion)}`);
    if (stats.explosionRadius) parts.push(`Blast ${Math.round(stats.explosionRadius)}`);
    if (stats.pierce) parts.push(`Pierce ${Math.round(stats.pierce)}`);
    if (stats.knock) parts.push(`Knock ${Math.round(stats.knock)}`);
    if (stats.gravity) parts.push(`Grav ${Math.round(stats.gravity)}`);
    if (stats.maxSpeed) parts.push(`MaxSPD ${Math.round(stats.maxSpeed)}`);
    if (stats.accel) parts.push(`Accel ${Math.round(stats.accel)}`);
    if (stats.turnRate) parts.push(`Turn ${Math.round(stats.turnRate * (180 / Math.PI))}deg/s`);
    if (stats.life) parts.push(`Life ${fmtFloat(stats.life, 2)}s`);
    if (stats.critChance != null && stats.critMult != null) {
      parts.push(`Crit ${Math.round(stats.critChance * 100)}% x${fmtFloat(stats.critMult, 2)}`);
    }
  }
  return parts.join(" | ");
}

const UPGRADES = [
  {
    id: "magic", title: "Magic Bullet",
    desc: "Shoots the nearest enemy automatically. Max level unlocks mastery ranks that boost damage and crits.",
    tag: () => weaponTag(weapons.magic, WEAPON_CONFIG.magic.maxLevel),
    can: () => true,
    apply: () => {
      weapons.magic.unlocked = true;
      if (weapons.magic.level < WEAPON_CONFIG.magic.maxLevel) weapons.magic.level++;
      else weapons.magic.mastery++;
    }
  },
  {
    id: "aura", title: "Holy Aura",
    desc: "A luminous field around you that damages and pushes enemies back. Extra ranks past max add damage and crit scaling.",
    tag: () => weaponTag(weapons.aura, WEAPON_CONFIG.aura.maxLevel),
    can: () => (weapons.aura.unlocked) || weaponCount() < MAX_WEAPONS,
    apply: () => {
      if (!weapons.aura.unlocked){ weapons.aura.unlocked=true; weapons.aura.level=1; return; }
      if (weapons.aura.level < WEAPON_CONFIG.aura.maxLevel) weapons.aura.level++;
      else weapons.aura.mastery++;
    }
  },
  {
    id: "rail", title: "Railgun",
    desc: "Charges a piercing rail shot that crosses the map with huge damage. Mastery after max level boosts damage/crit.",
    tag: () => weaponTag(weapons.rail, WEAPON_CONFIG.rail.maxLevel),
    can: () => (weapons.rail.unlocked) || weaponCount() < MAX_WEAPONS,
    apply: () => {
      if (!weapons.rail.unlocked){ weapons.rail.unlocked=true; weapons.rail.level=1; return; }
      if (weapons.rail.level < WEAPON_CONFIG.rail.maxLevel) weapons.rail.level++;
      else weapons.rail.mastery++;
    }
  },
  {
    id: "axe", title: "Axe Throw",
    desc: "Throws axes in a neon arc. Strong burst + heavy knockback. Mastery adds damage/crit scaling past max.",
    tag: () => weaponTag(weapons.axe, WEAPON_CONFIG.axe.maxLevel),
    can: () => (weapons.axe.unlocked) || weaponCount() < MAX_WEAPONS,
    apply: () => {
      if (!weapons.axe.unlocked){ weapons.axe.unlocked=true; weapons.axe.level=1; return; }
      if (weapons.axe.level < WEAPON_CONFIG.axe.maxLevel) weapons.axe.level++;
      else weapons.axe.mastery++;
    }
  },
  {
    id: "orb", title: "Singularity Orb",
    desc: "Launch an orb that parks, pulls enemies inward, pulses damage, then explodes. Mastery boosts damage/crit after max.",
    tag: () => weaponTag(weapons.orb, WEAPON_CONFIG.orb.maxLevel),
    can: () => (weapons.orb.unlocked) || weaponCount() < MAX_WEAPONS,
    apply: () => {
      if (!weapons.orb.unlocked){ weapons.orb.unlocked=true; weapons.orb.level=1; return; }
      if (weapons.orb.level < WEAPON_CONFIG.orb.maxLevel) weapons.orb.level++;
      else weapons.orb.mastery++;
    }
  },
  {
    id: "missile", title: "Homing Missiles",
    desc: "Fire guided missiles that arc toward enemies and explode for splash damage. Mastery adds damage/crit scaling past max.",
    tag: () => weaponTag(weapons.missile, WEAPON_CONFIG.missile.maxLevel),
    can: () => (weapons.missile.unlocked) || weaponCount() < MAX_WEAPONS,
    apply: () => {
      if (!weapons.missile.unlocked){ weapons.missile.unlocked=true; weapons.missile.level=1; return; }
      if (weapons.missile.level < WEAPON_CONFIG.missile.maxLevel) weapons.missile.level++;
      else weapons.missile.mastery++;
    }
  },
  {
    id: "speed", title: "Speed Up",
    desc: "Move faster to kite swarms and reach chests sooner.",
    tag: () => {
      const curSpeed = player.speed;
      const nextSpeed = curSpeed * UPGRADE_CONFIG.speedMult;
      const curPct = Math.round(((curSpeed / BASE_STATS.speed) - 1) * 100);
      const nextPct = Math.round(((nextSpeed / BASE_STATS.speed) - 1) * 100);
      return `Lv ${upgradeState.speedLv}/${UPGRADE_CONFIG.passiveMaxLevel} | +${Math.round((UPGRADE_CONFIG.speedMult - 1) * 100)}% per Lv | Speed ${Math.round(curSpeed)} (${curPct}%) -> ${Math.round(nextSpeed)} (${nextPct}%)`;
    },
    can: () => upgradeState.speedLv < UPGRADE_CONFIG.passiveMaxLevel,
    apply: () => { upgradeState.speedLv++; player.speed *= UPGRADE_CONFIG.speedMult; }
  },
  {
    id: "hp", title: "Max HP Up",
    desc: "Increase maximum HP and heal a bit immediately.",
    tag: () => {
      const nextLv = upgradeState.hpLv + 1;
      const add = UPGRADE_CONFIG.hpBaseGain + nextLv * UPGRADE_CONFIG.hpPerLevelGain;
      const heal = Math.floor(add * UPGRADE_CONFIG.hpHealPct);
      return `Lv ${upgradeState.hpLv}/${UPGRADE_CONFIG.passiveMaxLevel} | +${add} Max HP | Heal ${heal} | Max ${Math.round(player.maxHp)} -> ${Math.round(player.maxHp + add)}`;
    },
    can: () => upgradeState.hpLv < UPGRADE_CONFIG.passiveMaxLevel,
    apply: () => {
      upgradeState.hpLv++;
      const add = UPGRADE_CONFIG.hpBaseGain + upgradeState.hpLv * UPGRADE_CONFIG.hpPerLevelGain;
      player.maxHp += add;
      player.hp = Math.min(player.maxHp, player.hp + Math.floor(add * UPGRADE_CONFIG.hpHealPct));
    }
  },
  {
    id: "armor", title: "Armor Plating",
    desc: "Reduce incoming physical damage. Does not protect against void zones.",
    tag: () => `Lv ${upgradeState.armorLv}/${UPGRADE_CONFIG.passiveMaxLevel} | +${UPGRADE_CONFIG.armorGain} per Lv | Armor ${Math.round(player.armor)} -> ${Math.round(player.armor + UPGRADE_CONFIG.armorGain)}`,
    can: () => upgradeState.armorLv < UPGRADE_CONFIG.passiveMaxLevel,
    apply: () => { upgradeState.armorLv++; player.armor += UPGRADE_CONFIG.armorGain; }
  },
  {
    id: "resAll", title: "Elemental Ward",
    desc: "Reduce elemental damage of all types.",
    tag: () => {
      const cur = Math.round((player.resists.all || 0) * 100);
      const next = Math.round(Math.min(1, (player.resists.all || 0) + UPGRADE_CONFIG.resAllGain) * 100);
      return `Lv ${upgradeState.resAllLv}/${UPGRADE_CONFIG.passiveMaxLevel} | +${Math.round(UPGRADE_CONFIG.resAllGain * 100)}% per Lv | All Res ${cur}% -> ${next}%`;
    },
    can: () => upgradeState.resAllLv < UPGRADE_CONFIG.passiveMaxLevel,
    apply: () => { upgradeState.resAllLv++; player.resists.all += UPGRADE_CONFIG.resAllGain; }
  },
  {
    id: "resFire", title: "Fire Resistance",
    desc: "Reduce fire damage taken.",
    tag: () => {
      const cur = Math.round((player.resists.fire || 0) * 100);
      const next = Math.round(Math.min(1, (player.resists.fire || 0) + UPGRADE_CONFIG.resFireGain) * 100);
      return `Lv ${upgradeState.resFireLv}/${UPGRADE_CONFIG.passiveMaxLevel} | +${Math.round(UPGRADE_CONFIG.resFireGain * 100)}% per Lv | Fire Res ${cur}% -> ${next}%`;
    },
    can: () => upgradeState.resFireLv < UPGRADE_CONFIG.passiveMaxLevel,
    apply: () => { upgradeState.resFireLv++; player.resists.fire += UPGRADE_CONFIG.resFireGain; }
  },
  {
    id: "resPoison", title: "Poison Resistance",
    desc: "Reduce poison damage taken.",
    tag: () => {
      const cur = Math.round((player.resists.poison || 0) * 100);
      const next = Math.round(Math.min(1, (player.resists.poison || 0) + UPGRADE_CONFIG.resPoisonGain) * 100);
      return `Lv ${upgradeState.resPoisonLv}/${UPGRADE_CONFIG.passiveMaxLevel} | +${Math.round(UPGRADE_CONFIG.resPoisonGain * 100)}% per Lv | Poison Res ${cur}% -> ${next}%`;
    },
    can: () => upgradeState.resPoisonLv < UPGRADE_CONFIG.passiveMaxLevel,
    apply: () => { upgradeState.resPoisonLv++; player.resists.poison += UPGRADE_CONFIG.resPoisonGain; }
  },
  {
    id: "resVoid", title: "Void Resistance",
    desc: "Reduce void damage taken.",
    tag: () => {
      const cur = Math.round((player.resists.void || 0) * 100);
      const next = Math.round(Math.min(1, (player.resists.void || 0) + UPGRADE_CONFIG.resVoidGain) * 100);
      return `Lv ${upgradeState.resVoidLv}/${UPGRADE_CONFIG.passiveMaxLevel} | +${Math.round(UPGRADE_CONFIG.resVoidGain * 100)}% per Lv | Void Res ${cur}% -> ${next}%`;
    },
    can: () => upgradeState.resVoidLv < UPGRADE_CONFIG.passiveMaxLevel,
    apply: () => { upgradeState.resVoidLv++; player.resists.void += UPGRADE_CONFIG.resVoidGain; }
  },
  {
    id: "pickup", title: "Pickup Range",
    desc: "Collect XP gems from farther away and pull them in faster.",
    tag: () => `Lv ${upgradeState.pickupLv}/${UPGRADE_CONFIG.passiveMaxLevel} | +${UPGRADE_CONFIG.pickupGain} per Lv | Pickup ${Math.round(player.pickup)} -> ${Math.round(player.pickup + UPGRADE_CONFIG.pickupGain)}`,
    can: () => upgradeState.pickupLv < UPGRADE_CONFIG.passiveMaxLevel,
    apply: () => { upgradeState.pickupLv++; player.pickup += UPGRADE_CONFIG.pickupGain; }
  },
  {
    id: "xp", title: "XP Gain",
    desc: "Increase XP gained from all sources.",
    tag: () => {
      const cur = Math.round(upgradeState.xpLv * UPGRADE_CONFIG.xpMultGain * 100);
      const next = Math.round((upgradeState.xpLv + 1) * UPGRADE_CONFIG.xpMultGain * 100);
      return `Lv ${upgradeState.xpLv}/${UPGRADE_CONFIG.passiveMaxLevel} | +${Math.round(UPGRADE_CONFIG.xpMultGain * 100)}% per Lv | XP +${cur}% -> +${next}%`;
    },
    can: () => upgradeState.xpLv < UPGRADE_CONFIG.passiveMaxLevel,
    apply: () => { upgradeState.xpLv++; }
  },
  {
    id: "cdr", title: "Cooldown Reduction",
    desc: "Reduce weapon cooldowns for all attacks.",
    tag: () => {
      const cur = Math.round(upgradeState.cdLv * UPGRADE_CONFIG.cdReduction * 100);
      const next = Math.round((upgradeState.cdLv + 1) * UPGRADE_CONFIG.cdReduction * 100);
      return `Lv ${upgradeState.cdLv}/${UPGRADE_CONFIG.passiveMaxLevel} | -${Math.round(UPGRADE_CONFIG.cdReduction * 100)}% per Lv | CDR -${cur}% -> -${next}%`;
    },
    can: () => upgradeState.cdLv < UPGRADE_CONFIG.passiveMaxLevel,
    apply: () => { upgradeState.cdLv++; }
  },
  {
    id: "critChance", title: "Critical Chance",
    desc: "Increase critical strike chance for all weapons.",
    tag: () => {
      const cur = Math.round(upgradeState.critChanceLv * CRIT_UPGRADES.chancePerLevel * 100);
      const next = Math.round((upgradeState.critChanceLv + 1) * CRIT_UPGRADES.chancePerLevel * 100);
      return `Lv ${upgradeState.critChanceLv}/${CRIT_UPGRADES.maxLevels} | +${Math.round(CRIT_UPGRADES.chancePerLevel * 100)}% per Lv | Crit +${cur}% -> +${next}%`;
    },
    can: () => upgradeState.critChanceLv < CRIT_UPGRADES.maxLevels,
    apply: () => { upgradeState.critChanceLv = Math.min(CRIT_UPGRADES.maxLevels, upgradeState.critChanceLv + 1); }
  },
  {
    id: "critMult", title: "Critical Damage",
    desc: "Increase critical damage multiplier for all weapons.",
    tag: () => {
      const cur = fmtFloat(upgradeState.critMultLv * CRIT_UPGRADES.multPerLevel, 2);
      const next = fmtFloat((upgradeState.critMultLv + 1) * CRIT_UPGRADES.multPerLevel, 2);
      return `Lv ${upgradeState.critMultLv}/${CRIT_UPGRADES.maxLevels} | +${fmtFloat(CRIT_UPGRADES.multPerLevel, 2)}x per Lv | Crit Dmg +${cur}x -> +${next}x`;
    },
    can: () => upgradeState.critMultLv < CRIT_UPGRADES.maxLevels,
    apply: () => { upgradeState.critMultLv = Math.min(CRIT_UPGRADES.maxLevels, upgradeState.critMultLv + 1); }
  },
];

export function pickUpgrades(n=XP_CONFIG.cardChoices){
  const available = [];
  const capReached = weaponCount() >= MAX_WEAPONS;
  for (let i=0;i<UPGRADES.length;i++){
    const u = UPGRADES[i];
    const isWeapon = (u.id === "magic" || u.id === "aura" || u.id === "rail" || u.id === "axe" || u.id === "orb" || u.id === "missile");
    const isLocked = (u.id === "magic") ? false : (u.id === "aura" ? !weapons.aura.unlocked : u.id === "rail" ? !weapons.rail.unlocked : u.id === "axe" ? !weapons.axe.unlocked : u.id === "orb" ? !weapons.orb.unlocked : u.id === "missile" ? !weapons.missile.unlocked : false);
    if (capReached && isWeapon && isLocked) continue; // hard cap enforcement
    if (u.can()) available.push(u);
  }
  if (!available.length) return [];
  const picks = [];
  const used = new Set();
  for (let k=0;k<n;k++){
    let best = null, bestScore = -1;
    for (let i=0;i<available.length;i++){
      const u = available[i];
      if (used.has(u.id)) continue;
      let w = 1;
      if ((u.id==="aura" && !weapons.aura.unlocked) || (u.id==="axe" && !weapons.axe.unlocked) || (u.id==="rail" && !weapons.rail.unlocked) || (u.id==="orb" && !weapons.orb.unlocked) || (u.id==="missile" && !weapons.missile.unlocked)) w = UPGRADE_CONFIG.weightNewWeapon;
      if (u.id==="pickup") w *= UPGRADE_CONFIG.weightPickup;
      if (u.id==="resAll" || u.id==="resFire" || u.id==="resPoison" || u.id==="resVoid") w *= UPGRADE_CONFIG.weightRes;
      const s = Math.random() * w;
      if (s > bestScore){ bestScore = s; best = u; }
    }
    if (!best) break;
    used.add(best.id);
    picks.push(best);
  }
  while (picks.length < n) picks.push(available[randi(available.length)]);
  return picks;
}

export function listUpgradeSummary(){
  const parts = [];
  const mTag = (w) => w.mastery ? ` (M${w.mastery})` : "";
  parts.push(`Magic Bullet Lv ${weapons.magic.level}${mTag(weapons.magic)}`);
  if (weapons.aura.unlocked) parts.push(`Holy Aura Lv ${weapons.aura.level}${mTag(weapons.aura)}`);
  if (weapons.rail.unlocked) parts.push(`Railgun Lv ${weapons.rail.level}${mTag(weapons.rail)}`);
  if (weapons.axe.unlocked) parts.push(`Axe Throw Lv ${weapons.axe.level}${mTag(weapons.axe)}`);
  if (weapons.orb.unlocked) parts.push(`Singularity Orb Lv ${weapons.orb.level}${mTag(weapons.orb)}`);
  if (weapons.missile.unlocked) parts.push(`Homing Missiles Lv ${weapons.missile.level}${mTag(weapons.missile)}`);
  if (upgradeState.speedLv) parts.push(`Speed +${upgradeState.speedLv}`);
  if (upgradeState.hpLv) parts.push(`Max HP +${upgradeState.hpLv}`);
  if (upgradeState.armorLv) parts.push(`Armor +${upgradeState.armorLv}`);
  if (upgradeState.resAllLv) parts.push(`All Res +${Math.round(upgradeState.resAllLv * UPGRADE_CONFIG.resAllGain * 100)}%`);
  if (upgradeState.resFireLv) parts.push(`Fire Res +${Math.round(upgradeState.resFireLv * UPGRADE_CONFIG.resFireGain * 100)}%`);
  if (upgradeState.resPoisonLv) parts.push(`Poison Res +${Math.round(upgradeState.resPoisonLv * UPGRADE_CONFIG.resPoisonGain * 100)}%`);
  if (upgradeState.resVoidLv) parts.push(`Void Res +${Math.round(upgradeState.resVoidLv * UPGRADE_CONFIG.resVoidGain * 100)}%`);
  if (upgradeState.pickupLv) parts.push(`Pickup +${upgradeState.pickupLv}`);
  if (upgradeState.xpLv) parts.push(`XP +${Math.round(upgradeState.xpLv * UPGRADE_CONFIG.xpMultGain * 100)}%`);
  if (upgradeState.cdLv) parts.push(`Cooldown -${Math.round(upgradeState.cdLv * UPGRADE_CONFIG.cdReduction * 100)}%`);
  return parts.join(" | ");
}

export function formatDpsSummary(){
  const t = Math.max(player.time, 0.1);
  const entries = [];
  const append = (key, unlocked) => {
    if (!unlocked) return;
    const dmg = DPS_TRACKER[key] || 0;
    const dps = dmg / t;
    entries.push(`${DPS_LABELS[key]}: ${Math.round(dps)} DPS (${Math.round(dmg)} dmg)`);
  };
  append("magic", weapons.magic.unlocked);
  append("aura", weapons.aura.unlocked);
  append("rail", weapons.rail.unlocked);
  append("axe", weapons.axe.unlocked);
  append("orb", weapons.orb.unlocked);
  append("missile", weapons.missile.unlocked);
  return entries.length ? entries.join(" | ") : "No weapon damage";
}
