import { TELEGRAPH_CONFIG } from "./config.js";
import { COLORS } from "./colors.js";
import { telegraphs } from "./state.js";

export function addTelegraph({ x, y, dx=0, dy=0, radius=TELEGRAPH_CONFIG.radius, color=COLORS.warn, time=TELEGRAPH_CONFIG.time, fire=null, label=null, width=3, follow=null }){
  telegraphs.push({ x, y, dx, dy, radius, color, t:time, max:time, fire, label, width, follow });
}

export function updateTelegraphs(dt){
  for (let i=telegraphs.length-1;i>=0;i--){
    const tg = telegraphs[i];
    if (tg.follow) tg.follow(tg, dt);
    tg.t -= dt;
    if (tg.t <= 0){
      const fn = tg.fire;
      telegraphs[i] = telegraphs[telegraphs.length-1];
      telegraphs.pop();
      if (fn) fn();
    }
  }
}
