import { TRINKET_CONFIG } from "./config.js";
import { randi } from "./math.js";
import { player, trinkets, trinketBonuses } from "./state.js";

const applyMaxHp = (amount) => {
  player.maxHp += amount;
  player.hp = Math.min(player.maxHp, player.hp + amount);
};

const TRINKETS = [
  {
    id: "flux_boots",
    title: "Flux Boots",
    desc: "Move speed +15%.",
    tag: () => "+15% Speed",
    apply: () => { player.speed *= 1.15; }
  },
  {
    id: "ironheart_core",
    title: "Ironheart Core",
    desc: "Max HP +40.",
    tag: () => "+40 Max HP",
    apply: () => { applyMaxHp(40); }
  },
  {
    id: "aegis_plate",
    title: "Aegis Plate",
    desc: "Armor +3.",
    tag: () => "+3 Armor",
    apply: () => { player.armor += 3; }
  },
  {
    id: "second_chance",
    title: "Second Chance",
    desc: "Revive once upon death.",
    tag: () => "Revive once",
    apply: () => { trinketBonuses.reviveCharges = Math.max(trinketBonuses.reviveCharges, 1); }
  },
  {
    id: "elemental_ward",
    title: "Elemental Ward",
    desc: "All elemental resistances +12%.",
    tag: () => "+6% All Res",
    apply: () => { player.resists.all += 0.12; }
  },
  {
    id: "thermal_shield",
    title: "Thermal Shield",
    desc: "Fire resistance +24%.",
    tag: () => "+12% Fire Res",
    apply: () => { player.resists.fire += 0.24; }
  },
  {
    id: "toxin_filter",
    title: "Toxin Filter",
    desc: "Poison resistance +24%.",
    tag: () => "+12% Poison Res",
    apply: () => { player.resists.poison += 0.24; }
  },
  {
    id: "void_lattice",
    title: "Void Lattice",
    desc: "Void resistance +24%.",
    tag: () => "+12% Void Res",
    apply: () => { player.resists.void += 0.24; }
  },
  {
    id: "vacuum_coil",
    title: "Vacuum Coil",
    desc: "Pickup range +40.",
    tag: () => "+40 Pickup",
    apply: () => { player.pickup += 40; }
  },
  {
    id: "scholar_sigil",
    title: "Scholar Sigil",
    desc: "XP gain +15%.",
    tag: () => "+15% XP",
    apply: () => { trinketBonuses.xpMult *= 1.15; }
  },
  {
    id: "coolant_core",
    title: "Coolant Core",
    desc: "Cooldowns -10%.",
    tag: () => "-10% CD",
    apply: () => { trinketBonuses.cdMult *= 0.9; }
  },
  {
    id: "deadeye_lens",
    title: "Deadeye Lens",
    desc: "Critical chance +6%.",
    tag: () => "+6% Crit",
    apply: () => { trinketBonuses.critChance += 0.06; }
  },
  {
    id: "serrated_fang",
    title: "Serrated Fang",
    desc: "Critical damage +0.25x.",
    tag: () => "+0.25 Crit",
    apply: () => { trinketBonuses.critMult += 0.25; }
  },
  {
    id: "reactor_spark",
    title: "Reactor Spark",
    desc: "Damage +12%.",
    tag: () => "+12% DMG",
    apply: () => { trinketBonuses.dmgMult *= 1.12; }
  },
  {
    id: "guardian_crest",
    title: "Guardian Crest",
    desc: "Max HP +25 and Armor +2.",
    tag: () => "+25 HP, +2 Armor",
    apply: () => { applyMaxHp(25); player.armor += 2; }
  },
];

const TRINKET_LOOKUP = new Map(TRINKETS.map((t) => [t.id, t]));

export function resetTrinkets() {
  trinkets.length = 0;
  trinketBonuses.dmgMult = 1;
  trinketBonuses.cdMult = 1;
  trinketBonuses.xpMult = 1;
  trinketBonuses.critChance = 0;
  trinketBonuses.critMult = 0;
  trinketBonuses.reviveCharges = 0;
  trinketBonuses.secondChanceOffered = false;
}

export function trinketSlotsFull() {
  return trinkets.length >= TRINKET_CONFIG.slots;
}

export function hasTrinket(id) {
  return trinkets.includes(id);
}

export function addTrinket(id) {
  if (trinketSlotsFull()) return false;
  if (hasTrinket(id)) return false;
  const t = TRINKET_LOOKUP.get(id);
  if (!t) return false;
  trinkets.push(id);
  t.apply();
  return true;
}

export function pickTrinkets(count = TRINKET_CONFIG.choices) {
  const available = TRINKETS.filter((t) => {
    if (t.id === "second_chance" && trinketBonuses.secondChanceOffered) return false;
    return !hasTrinket(t.id);
  });
  if (!available.length) return [];
  const picks = [];
  const used = new Set();
  const max = Math.min(count, available.length);
  while (picks.length < max) {
    const idx = randi(available.length);
    const t = available[idx];
    if (used.has(t.id)) continue;
    used.add(t.id);
    picks.push(t);
  }
  if (picks.some((t) => t.id === "second_chance")) trinketBonuses.secondChanceOffered = true;
  return picks;
}

export function formatTrinketPills() {
  if (!trinkets.length) return `<span class="pill">None</span>`;
  return trinkets
    .map((id) => {
      const t = TRINKET_LOOKUP.get(id);
      const label = t ? t.title : id;
      return `<span class="pill">${label}</span>`;
    })
    .join("");
}

export function formatTrinketBonusPills() {
  if (!trinkets.length) return `<span class="pill">No trinkets</span>`;
  return trinkets
    .map((id) => {
      const t = TRINKET_LOOKUP.get(id);
      const label = t ? t.tag() : id;
      return `<span class="pill">${label}</span>`;
    })
    .join("");
}

export { TRINKETS };
