import { clamp } from "./math.js";
import { player } from "./state.js";

export const combatFx = {
  damageT: 0,
  damageStrength: 0,
  blockT: 0,
  blockCooldown: 0,
  healT: 0,
  healStrength: 0,
  healCooldown: 0,
};

const DAMAGE_PULSE_TIME = 0.18;
const BLOCK_PULSE_TIME = 0.2;
const HEAL_PULSE_TIME = 0.3;

export function triggerPlayerDamageFx(amount = 0) {
  const relative = player.maxHp > 0 ? amount / player.maxHp : 0;
  const strength = clamp(0.55 + relative * 3.2, 0.55, 1);
  combatFx.damageT = DAMAGE_PULSE_TIME;
  combatFx.damageStrength = Math.max(combatFx.damageStrength, strength);
}

export function triggerShieldBlockFx() {
  if (combatFx.blockCooldown > 0) return;
  combatFx.blockT = BLOCK_PULSE_TIME;
  combatFx.blockCooldown = 0.12;
}

export function triggerHealFx(amount = 0, strength = 1) {
  if (!(amount > 0) || combatFx.healCooldown > 0) return;
  combatFx.healT = HEAL_PULSE_TIME;
  combatFx.healStrength = Math.max(combatFx.healStrength, clamp(strength, 0.35, 1));
  combatFx.healCooldown = 0.1;
}

export function updateCombatFx(dt) {
  combatFx.damageT = Math.max(0, combatFx.damageT - dt);
  combatFx.blockT = Math.max(0, combatFx.blockT - dt);
  combatFx.blockCooldown = Math.max(0, combatFx.blockCooldown - dt);
  combatFx.healT = Math.max(0, combatFx.healT - dt);
  combatFx.healCooldown = Math.max(0, combatFx.healCooldown - dt);

  if (combatFx.damageT <= 0) combatFx.damageStrength = 0;
  if (combatFx.healT <= 0) combatFx.healStrength = 0;
}

export function resetCombatFx() {
  combatFx.damageT = 0;
  combatFx.damageStrength = 0;
  combatFx.blockT = 0;
  combatFx.blockCooldown = 0;
  combatFx.healT = 0;
  combatFx.healStrength = 0;
  combatFx.healCooldown = 0;
}
