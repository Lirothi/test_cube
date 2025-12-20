import { AUGMENT_CONFIG } from "./config.js";

const AUGMENTS = {
  magic: [
    {
      id: "magic_cryo",
      title: "Cryo Rounds",
      desc: "Hits slow enemies for a short time.",
      tag: () => `Slow ${Math.round((1 - AUGMENT_CONFIG.magic.slow.mult) * 100)}% / ${AUGMENT_CONFIG.magic.slow.duration.toFixed(1)}s`,
    },
    {
      id: "magic_prism",
      title: "Prism Split",
      desc: `Every ${AUGMENT_CONFIG.magic.prism.every}th volley splits into extra bolts.`,
      tag: () => `Every ${AUGMENT_CONFIG.magic.prism.every}th / +2 Bolts @ ${Math.round(AUGMENT_CONFIG.magic.prism.dmgMult * 100)}% / Angle ${AUGMENT_CONFIG.magic.prism.angle.toFixed(2)}`,
    },
  ],
  aura: [
    {
      id: "aura_pulse",
      title: "Sanctified Pulse",
      desc: "Periodic pulse deals heavy damage and knockback.",
      tag: () => `Pulse ${AUGMENT_CONFIG.aura.pulse.cd.toFixed(1)}s / DMG +${Math.round((AUGMENT_CONFIG.aura.pulse.dmgMult - 1) * 100)}% / Knock +${Math.round((AUGMENT_CONFIG.aura.pulse.knockMult - 1) * 100)}%`,
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
      tag: () => `Burn ${Math.round(AUGMENT_CONFIG.rail.burn.dpsPct * 100)}% / ${AUGMENT_CONFIG.rail.burn.duration.toFixed(1)}s`,
    },
    {
      id: "rail_overpen",
      title: "Overpenetrator",
      desc: "Higher damage and pierce, slower cooldown.",
      tag: () => `+${AUGMENT_CONFIG.rail.overpen.pierce} Pierce / DMG +${Math.round((AUGMENT_CONFIG.rail.overpen.dmgMult - 1) * 100)}% / CD +${Math.round((AUGMENT_CONFIG.rail.overpen.cdMult - 1) * 100)}%`,
    },
  ],
  axe: [
    {
      id: "axe_bleed",
      title: "Serrated Edge",
      desc: "Hits apply bleed damage over time.",
      tag: () => `Bleed ${Math.round(AUGMENT_CONFIG.axe.bleed.dpsPct * 100)}% / ${AUGMENT_CONFIG.axe.bleed.duration.toFixed(1)}s`,
    },
    {
      id: "axe_boomerang",
      title: "Boomerang Axes",
      desc: "Axes return once and can hit again.",
      tag: () => `Return DMG ${Math.round(AUGMENT_CONFIG.axe.boomerang.dmgMult * 100)}% / Life ${Math.round(AUGMENT_CONFIG.axe.boomerang.returnLifeMult * 100)}%`,
    },
  ],
  orb: [
    {
      id: "orb_event_horizon",
      title: "Event Horizon",
      desc: `Longer park time, stronger pull, and +${Math.round((AUGMENT_CONFIG.orb.eventHorizon.tickDmgMult - 1) * 100)}% tick damage.`,
      tag: () => `Park +${AUGMENT_CONFIG.orb.eventHorizon.park.toFixed(1)}s / Pull +${Math.round((AUGMENT_CONFIG.orb.eventHorizon.pullMult - 1) * 100)}% / Tick +${Math.round((AUGMENT_CONFIG.orb.eventHorizon.tickDmgMult - 1) * 100)}%`,
    },
    {
      id: "orb_dark_burst",
      title: "Dark Burst",
      desc: "Bigger blast, stronger damage, weaker pull.",
      tag: () => `Blast +${Math.round((AUGMENT_CONFIG.orb.darkBurst.radiusMult - 1) * 100)}% / DMG +${Math.round((AUGMENT_CONFIG.orb.darkBurst.dmgMult - 1) * 100)}% / Park ${AUGMENT_CONFIG.orb.darkBurst.park >= 0 ? "+" : ""}${AUGMENT_CONFIG.orb.darkBurst.park.toFixed(1)}s / Pull ${Math.round((AUGMENT_CONFIG.orb.darkBurst.pullMult - 1) * 100)}%`,
    },
  ],
  missile: [
    {
      id: "missile_swarm",
      title: "Seeker Swarm",
      desc: "Launch one extra missile per volley. Missiles do 80% damage.",
      tag: () => `+${AUGMENT_CONFIG.missile.swarm.extra} Missile / DMG ${Math.round(AUGMENT_CONFIG.missile.swarm.dmgMult * 100)}%`,
    },
    {
      id: "missile_concussive",
      title: "Concussive Warhead",
      desc: "Explosions slow, knock back, and burn enemies.",
      tag: () => `Slow ${Math.round((1 - AUGMENT_CONFIG.missile.concussive.slowMult) * 100)}% ${AUGMENT_CONFIG.missile.concussive.slowDuration.toFixed(1)}s / Knock ${Math.round(AUGMENT_CONFIG.missile.concussive.knock)} / Burn ${Math.round(AUGMENT_CONFIG.missile.concussive.burn.dpsPct * 100)}% ${AUGMENT_CONFIG.missile.concussive.burn.duration.toFixed(1)}s`,
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
