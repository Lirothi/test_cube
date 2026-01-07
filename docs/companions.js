import { COLORS, COMPANION_CONFIG } from "./config.js";
import { rand, TAU } from "./math.js";
import { player, companions, trinketBonuses, gems } from "./state.js";

const COMPANION_DATA = [
  {
    id: "igorek",
    name: "Igorek",
    color: COLORS.companionIgorek,
    r: 6,
    orbitRadius: 34,
    orbitSpeed: 1.35,
    pickup: 144,
    pull: 1150,
    speed: 0,
    returnSpeed: 0,
    seekRadius: 230,
    desc: "Small speed boost. Chases nearby XP gems.",
    tag: () => "+5% Speed | Gems 144",
    buffs: { speedMult: 1.05 },
  },
  {
    id: "lumen_moth",
    name: "Lumen Moth",
    color: COLORS.companionLumen,
    r: 5,
    orbitRadius: 46,
    orbitSpeed: 1.1,
    pickup: 135,
    pull: 1050,
    speed: 0,
    returnSpeed: 0,
    seekRadius: 210,
    desc: "All-resistance boost. Chases XP gems.",
    tag: () => "+2% All Res | Gems 135",
    buffs: { resAll: 0.02 },
  },
  {
    id: "volt_pup",
    name: "Volt Pup",
    color: COLORS.companionVolt,
    r: 6,
    orbitRadius: 40,
    orbitSpeed: 1.55,
    pickup: 130,
    pull: 1100,
    speed: 0,
    returnSpeed: 0,
    seekRadius: 200,
    desc: "Small crit chance boost. Chases gems.",
    tag: () => "+3% Crit | Gems 130",
    buffs: { critChance: 0.03 },
  },
  {
    id: "byte_sprite",
    name: "Byte Sprite",
    color: COLORS.companionByte,
    r: 5,
    orbitRadius: 54,
    orbitSpeed: 0.95,
    pickup: 140,
    pull: 1150,
    speed: 0,
    returnSpeed: 0,
    seekRadius: 220,
    desc: "Slight XP gain boost. Chases gems.",
    tag: () => "+5% XP | Gems 140",
    buffs: { xpMult: 1.05 },
  },
  {
    id: "aegis_wisp",
    name: "Aegis Wisp",
    color: COLORS.companionAegis,
    r: 6,
    orbitRadius: 62,
    orbitSpeed: 0.85,
    pickup: 130,
    pull: 1000,
    speed: 0,
    returnSpeed: 0,
    seekRadius: 190,
    desc: "Light armor + res boost. Chases gems.",
    tag: () => "+1 Armor, +1% Res | Gems 130",
    buffs: { armor: 1, resAll: 0.01 },
  },
];

export const COMPANIONS = [...COMPANION_DATA];

function applyCompanionBuff(b) {
  if (!b) return;
  if (b.speedMult) player.speed *= b.speedMult;
  if (b.armor) player.armor += b.armor;
  if (b.resAll) player.resists.all += b.resAll;
  if (b.resFire) player.resists.fire += b.resFire;
  if (b.resPoison) player.resists.poison += b.resPoison;
  if (b.resVoid) player.resists.void += b.resVoid;
  if (b.xpMult) trinketBonuses.xpMult *= b.xpMult;
  if (b.dmgMult) trinketBonuses.dmgMult *= b.dmgMult;
  if (b.cdMult) trinketBonuses.cdMult *= b.cdMult;
  if (b.critChance) trinketBonuses.critChance += b.critChance;
  if (b.critMult) trinketBonuses.critMult += b.critMult;
}

export function resetCompanions() {
  companions.length = 0;
}

export function companionSlotsFull() {
  return companions.length >= COMPANION_CONFIG.slots;
}

export function hasCompanion(id) {
  return companions.some((c) => c.id === id);
}

function makeCompanionInstance(c) {
  const angle = rand(TAU, 0);
  const x = player.x + Math.cos(angle) * c.orbitRadius;
  const y = player.y + Math.sin(angle) * c.orbitRadius;
  return {
    id: c.id,
    name: c.name,
    color: c.color,
    r: c.r,
    orbitRadius: c.orbitRadius,
    orbitSpeed: c.orbitSpeed,
    pickup: c.pickup,
    pull: c.pull,
    speed: c.speed,
    returnSpeed: c.returnSpeed,
    seekRadius: c.seekRadius,
    angle,
    x,
    y,
  };
}

export function addCompanion(id) {
  if (companionSlotsFull()) return false;
  if (hasCompanion(id)) return false;
  const data = COMPANION_DATA.find((c) => c.id === id);
  if (!data) return false;
  companions.push(makeCompanionInstance(data));
  applyCompanionBuff(data.buffs);
  return true;
}

export function updateCompanions(dt) {
  const claimedGems = new Set();
  const liveGems = gems.filter((g) => g.alive);
  for (let i = 0; i < companions.length; i++) {
    const c = companions[i];
    const toPlayer = Math.hypot(player.x - c.x, player.y - c.y);
    const leash = Math.max(c.seekRadius || 0, 180) + c.orbitRadius * 1.4;
    const forceReturn = toPlayer > leash;
    let target = null;
    let bestD2 = Infinity;
    const seek = c.seekRadius || 0;
    if (!forceReturn && seek > 0 && liveGems.length) {
      const seek2 = seek * seek;
      const current = c.targetGem;
      if (current && current.alive && !claimedGems.has(current)) {
        const dx = current.x - c.x;
        const dy = current.y - c.y;
        const d2 = dx * dx + dy * dy;
        if (d2 <= seek2) {
          bestD2 = d2;
          target = current;
        }
      }
      if (!target) {
        for (let g = 0; g < liveGems.length; g++) {
          const gem = liveGems[g];
          if (claimedGems.has(gem)) continue;
          const dx = gem.x - c.x;
          const dy = gem.y - c.y;
          const d2 = dx * dx + dy * dy;
          if (d2 <= seek2 && d2 < bestD2) {
            bestD2 = d2;
            target = gem;
          }
        }
      }
      if (!target) {
        bestD2 = Infinity;
        for (let g = 0; g < liveGems.length; g++) {
          const gem = liveGems[g];
          const dx = gem.x - c.x;
          const dy = gem.y - c.y;
          const d2 = dx * dx + dy * dy;
          if (d2 <= seek2 && d2 < bestD2) {
            bestD2 = d2;
            target = gem;
          }
        }
      }
    }
    c.targetGem = target || null;
    if (target) claimedGems.add(target);

    c.angle += c.orbitSpeed * dt;
    const bob = Math.sin(c.angle * 1.7 + i) * 3;
    const orbitX = player.x + Math.cos(c.angle) * c.orbitRadius;
    const orbitY = player.y + Math.sin(c.angle) * c.orbitRadius + bob;
    const targetX = target ? target.x : orbitX;
    const targetY = target ? target.y : orbitY;
    const baseSpeed = Math.max(0, player.speed || 0) * 1.1;
    const returnSpeed = (c.returnSpeed && c.returnSpeed > 0) ? c.returnSpeed : baseSpeed * 1.8;
    const speed = forceReturn ? returnSpeed : (target ? baseSpeed : baseSpeed * 0.85);

    const dx = targetX - c.x;
    const dy = targetY - c.y;
    const d = Math.hypot(dx, dy) || 0;
    if (d > 0) {
      const step = speed * dt;
      if (d <= step) {
        c.x = targetX;
        c.y = targetY;
      } else {
        c.x += (dx / d) * step;
        c.y += (dy / d) * step;
      }
    }
  }
}

export function formatCompanionPills() {
  if (!companions.length) return `<span class="pill">None</span>`;
  return companions
    .map((c) => `<span class="pill">${c.name}</span>`)
    .join("");
}
