import { TELEGRAPH_CONFIG } from "./config.js";
import { COLORS } from "./colors.js";
import { telegraphs } from "./state.js";

export const TELEGRAPH_KIND = Object.freeze({
  IMPACT: "impact",
  AOE: "aoe",
  NOVA: "nova",
  LINE: "line",
  PROJECTILE: "projectile",
  MINE: "mine",
  BLINK: "blink",
  RIFT: "rift",
  SPAWN: "spawn",
});

export function addTelegraph({
  x,
  y,
  dx=0,
  dy=0,
  radius=TELEGRAPH_CONFIG.radius,
  color=COLORS.warn,
  time=TELEGRAPH_CONFIG.time,
  fire=null,
  label=null,
  width=3,
  follow=null,
  kind=TELEGRAPH_KIND.IMPACT,
  element="",
  length=0,
  dangerWidth=0,
  count=1,
  spread=0,
}){
  const directionLength = Math.hypot(dx, dy);
  const ndx = directionLength > 0 ? dx / directionLength : 0;
  const ndy = directionLength > 0 ? dy / directionLength : 0;
  telegraphs.push({
    x,
    y,
    dx: ndx,
    dy: ndy,
    radius,
    color,
    t: time,
    max: time,
    progress: 0,
    fire,
    label,
    width,
    follow,
    kind,
    element,
    length,
    dangerWidth,
    count,
    spread,
  });
}

export function updateTelegraphs(dt){
  for (let i=telegraphs.length-1;i>=0;i--){
    const tg = telegraphs[i];
    if (tg.follow) tg.follow(tg, dt);
    tg.t -= dt;
    tg.progress = tg.max > 0
      ? Math.max(0, Math.min(1, 1 - tg.t / tg.max))
      : 1;
    if (tg.t <= 0){
      tg.progress = 1;
      const fn = tg.fire;
      telegraphs[i] = telegraphs[telegraphs.length-1];
      telegraphs.pop();
      if (fn) fn();
    }
  }
}
