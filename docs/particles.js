import { COLORS } from "./config.js";
import { TAU, rand } from "./math.js";
import { particles } from "./state.js";
import { partPool } from "./pools.js";

export function addParticles(x, y, color, amount = 10, spread = 250) {
  for (let i = 0; i < amount; i++) {
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
