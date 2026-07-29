import { COLORS } from "./config.js";
import { TAU, rand } from "./math.js";
import { particles } from "./state.js";
import { partPool } from "./pools.js";
import { visualSettings } from "./visual_settings.js";

export const PARTICLE_BUDGET = 640;
const MAX_PARTICLES_PER_EMISSION = 72;

function availableCount(amount) {
  const motionScale = visualSettings.reducedMotion ? 0.72 : 1;
  const scale = visualSettings.particleScale * motionScale;
  const budget = Math.max(180, Math.floor(PARTICLE_BUDGET * visualSettings.particleScale));
  const emissionLimit = Math.max(24, Math.floor(MAX_PARTICLES_PER_EMISSION * scale));
  const free = Math.max(0, budget - particles.length);
  return Math.min(Math.max(0, Math.ceil(amount * scale)), emissionLimit, free);
}

export function addParticles(x, y, color, amount = 10, spread = 250) {
  const count = availableCount(amount);
  for (let i = 0; i < count; i++) {
    const p = partPool.get();
    p.alive = true;
    p.x = x; p.y = y;
    const a = rand(TAU, 0);
    const s = rand(spread, spread * 0.25);
    p.vx = Math.cos(a) * s;
    p.vy = Math.sin(a) * s;
    p.maxLife = rand(0.42, 0.18);
    p.life = p.maxLife;
    p.r = rand(3.2, 1.2);
    p.color = color || COLORS.gem;
    p.shape = "dot";
    p.stretch = 1;
    particles.push(p);
  }
}

export function addDirectionalParticles(
  x,
  y,
  color,
  amount,
  spread,
  dirX,
  dirY,
  crit = false
) {
  const count = availableCount(amount);
  let nx = dirX || 0;
  let ny = dirY || 0;
  const length = Math.hypot(nx, ny);
  if (length > 0.001) {
    nx /= length;
    ny /= length;
  } else {
    const fallback = rand(TAU, 0);
    nx = Math.cos(fallback);
    ny = Math.sin(fallback);
  }
  const baseAngle = Math.atan2(ny, nx);
  const cone = crit ? 0.62 : 0.82;
  for (let i = 0; i < count; i++) {
    const p = partPool.get();
    const angle = baseAngle + rand(cone, -cone);
    const speed = rand(spread, spread * 0.38);
    p.alive = true;
    p.x = x + rand(3, -3);
    p.y = y + rand(3, -3);
    p.vx = Math.cos(angle) * speed;
    p.vy = Math.sin(angle) * speed;
    p.maxLife = crit ? rand(0.34, 0.2) : rand(0.26, 0.14);
    p.life = p.maxLife;
    p.r = crit ? rand(3.8, 2.2) : rand(2.8, 1.4);
    p.color = color || COLORS.gem;
    p.shape = crit ? "shard" : "streak";
    p.stretch = crit ? rand(3.6, 2.4) : rand(3, 1.8);
    particles.push(p);
  }
}

export function updateParticles(dt) {
  for (let i = particles.length - 1; i >= 0; i--) {
    const p = particles[i];
    if (!p.alive) { particles[i] = particles[particles.length - 1]; particles.pop(); partPool.put(p); continue; }

    p.life -= dt;
    p.x += p.vx * dt;
    p.y += p.vy * dt;
    p.vx *= Math.pow(0.02, dt);
    p.vy *= Math.pow(0.02, dt);

    if (p.life <= 0) p.alive = false;

    if (!p.alive) {
      particles[i] = particles[particles.length - 1];
      particles.pop();
      partPool.put(p);
    }
  }
}

export function resetParticles() {
  for (let i = particles.length - 1; i >= 0; i--) {
    partPool.put(particles.pop());
  }
}
