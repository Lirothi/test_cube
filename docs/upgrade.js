import {
  XP_CONFIG,
  UPGRADE_CONFIG,
  CRIT_UPGRADES,
  MAX_WEAPONS,
  DPS_LABELS,
  WEAPON_CONFIG,
} from "./config.js";
import { randi } from "./math.js";
import { weapons, weaponCount } from "./weapons.js";
import { player } from "./state.js";

export const DPS_TRACKER = { magic:0, aura:0, rail:0, axe:0, orb:0, missile:0 };

export const upgradeState = { speedLv:0, hpLv:0, pickupLv:0, armorLv:0, cdLv:0, xpLv:0, critChanceLv:0, critMultLv:0 };
export function resetUpgradeState(){
  upgradeState.speedLv=0;
  upgradeState.hpLv=0;
  upgradeState.pickupLv=0;
  upgradeState.armorLv=0;
  upgradeState.cdLv=0;
  upgradeState.xpLv=0;
  upgradeState.critChanceLv=0;
  upgradeState.critMultLv=0;
}
export function resetDps(){ DPS_TRACKER.magic=0; DPS_TRACKER.aura=0; DPS_TRACKER.rail=0; DPS_TRACKER.axe=0; DPS_TRACKER.orb=0; DPS_TRACKER.missile=0; }

function weaponTag(weapon, maxLv){
  if (!weapon.unlocked) return "Weapon - Unlock";
  const base = `Weapon - Lv ${weapon.level}/${maxLv}`;
  return weapon.mastery > 0 ? `${base} (M${weapon.mastery})` : base;
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
    tag: () => `Passive - Lv ${upgradeState.speedLv}/${UPGRADE_CONFIG.passiveMaxLevel}`,
    can: () => upgradeState.speedLv < UPGRADE_CONFIG.passiveMaxLevel,
    apply: () => { upgradeState.speedLv++; player.speed *= UPGRADE_CONFIG.speedMult; }
  },
  {
    id: "hp", title: "Max HP Up",
    desc: "Increase maximum HP and heal a bit immediately.",
    tag: () => `Passive - Lv ${upgradeState.hpLv}/${UPGRADE_CONFIG.passiveMaxLevel}`,
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
    tag: () => `Passive - Lv ${upgradeState.armorLv}/${UPGRADE_CONFIG.passiveMaxLevel}`,
    can: () => upgradeState.armorLv < UPGRADE_CONFIG.passiveMaxLevel,
    apply: () => { upgradeState.armorLv++; player.armor += UPGRADE_CONFIG.armorGain; }
  },
  {
    id: "pickup", title: "Pickup Range",
    desc: "Collect XP gems from farther away and pull them in faster.",
    tag: () => `Passive - Lv ${upgradeState.pickupLv}/${UPGRADE_CONFIG.passiveMaxLevel}`,
    can: () => upgradeState.pickupLv < UPGRADE_CONFIG.passiveMaxLevel,
    apply: () => { upgradeState.pickupLv++; player.pickup += UPGRADE_CONFIG.pickupGain; }
  },
  {
    id: "xp", title: "XP Gain",
    desc: "Increase XP gained from all sources.",
    tag: () => `Passive - Lv ${upgradeState.xpLv}/${UPGRADE_CONFIG.passiveMaxLevel}`,
    can: () => upgradeState.xpLv < UPGRADE_CONFIG.passiveMaxLevel,
    apply: () => { upgradeState.xpLv++; }
  },
  {
    id: "cdr", title: "Cooldown Reduction",
    desc: "Reduce weapon cooldowns for all attacks.",
    tag: () => `Passive - Lv ${upgradeState.cdLv}/${UPGRADE_CONFIG.passiveMaxLevel}`,
    can: () => upgradeState.cdLv < UPGRADE_CONFIG.passiveMaxLevel,
    apply: () => { upgradeState.cdLv++; }
  },
  {
    id: "critChance", title: "Critical Chance",
    desc: "Increase critical strike chance for all weapons.",
    tag: () => `Passive - Lv ${upgradeState.critChanceLv}/${CRIT_UPGRADES.maxLevels}`,
    can: () => upgradeState.critChanceLv < CRIT_UPGRADES.maxLevels,
    apply: () => { upgradeState.critChanceLv = Math.min(CRIT_UPGRADES.maxLevels, upgradeState.critChanceLv + 1); }
  },
  {
    id: "critMult", title: "Critical Damage",
    desc: "Increase critical damage multiplier for all weapons.",
    tag: () => `Passive - Lv ${upgradeState.critMultLv}/${CRIT_UPGRADES.maxLevels}`,
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
