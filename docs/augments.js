import { AUGMENT_CONFIG } from "./config.js";

const AUGMENTS = {
  magic: [
    {
      id: "magic_cryo",
      title: "Cryo Rounds",
      desc: "Hits slow enemies for a short time.",
      tag: () => `Slow ${Math.round((1 - AUGMENT_CONFIG.magic.slow.mult) * 100)}%`,
    },
    {
      id: "magic_prism",
      title: "Prism Split",
      desc: `Every ${AUGMENT_CONFIG.magic.prism.every}th volley splits into extra bolts.`,
      tag: () => `+2 Bolts @ ${Math.round(AUGMENT_CONFIG.magic.prism.dmgMult * 100)}%`,
    },
  ],
  aura: [
    {
      id: "aura_pulse",
      title: "Sanctified Pulse",
      desc: "Periodic pulse deals heavy damage and knockback.",
      tag: () => `Pulse ${AUGMENT_CONFIG.aura.pulse.cd.toFixed(1)}s`,
    },
    {
      id: "aura_leech",
      title: "Leech Field",
      desc: "Aura ticks heal a fraction of max HP.",
      tag: () => `Heal ${Math.round(AUGMENT_CONFIG.aura.leechPct * 1000) / 10}%`,
    },
  ],
  rail: [
    {
      id: "rail_fire",
      title: "Incendiary Rail",
      desc: "Hits ignite enemies with burn damage.",
      tag: () => `Burn ${Math.round(AUGMENT_CONFIG.rail.burn.dpsPct * 100)}%`,
    },
    {
      id: "rail_overpen",
      title: "Overpenetrator",
      desc: "Higher damage and pierce, slower cooldown.",
      tag: () => `+${AUGMENT_CONFIG.rail.overpen.pierce} Pierce`,
    },
  ],
  axe: [
    {
      id: "axe_bleed",
      title: "Serrated Edge",
      desc: "Hits apply bleed damage over time.",
      tag: () => `Bleed ${Math.round(AUGMENT_CONFIG.axe.bleed.dpsPct * 100)}%`,
    },
    {
      id: "axe_boomerang",
      title: "Boomerang Axes",
      desc: "Axes return once and can hit again.",
      tag: () => `Return ${Math.round(AUGMENT_CONFIG.axe.boomerang.dmgMult * 100)}%`,
    },
  ],
  orb: [
    {
      id: "orb_event_horizon",
      title: "Event Horizon",
      desc: "Longer park time and stronger pull.",
      tag: () => `Pull +${Math.round((AUGMENT_CONFIG.orb.eventHorizon.pullMult - 1) * 100)}%`,
    },
    {
      id: "orb_dark_burst",
      title: "Dark Burst",
      desc: "Bigger blast, stronger damage, weaker pull.",
      tag: () => `Blast +${Math.round((AUGMENT_CONFIG.orb.darkBurst.radiusMult - 1) * 100)}%`,
    },
  ],
  missile: [
    {
      id: "missile_swarm",
      title: "Seeker Swarm",
      desc: "Launch one extra missile per volley. Missiles do 80% damage.",
      tag: () => `+${AUGMENT_CONFIG.missile.swarm.extra} Missile`,
    },
    {
      id: "missile_concussive",
      title: "Concussive Warhead",
      desc: "Explosions slow and knock back enemies.",
      tag: () => `Slow ${Math.round((1 - AUGMENT_CONFIG.missile.concussive.slowMult) * 100)}%`,
    },
  ],
};

const AUGMENT_LOOKUP = new Map();
for (const key of Object.keys(AUGMENTS)) {
  for (const aug of AUGMENTS[key]) AUGMENT_LOOKUP.set(aug.id, aug);
}

export function getAugmentsForWeapon(weaponKey) {
  return AUGMENTS[weaponKey] ? [...AUGMENTS[weaponKey]] : [];
}

export function getAugmentById(id) {
  return AUGMENT_LOOKUP.get(id) || null;
}

export { AUGMENTS };
