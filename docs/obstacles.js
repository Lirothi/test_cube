import { OBSTACLE_CONFIG } from "./config.js";
import { COLORS } from "./colors.js";
import { rand, randi, TAU } from "./math.js";
import { WORLD, obstacles, activeObstacles } from "./state.js";

export const OBSTACLE_TYPE = { LAKE:"lake", FOREST:"forest", ROCK:"rock" };

let runtime = { addParticles: null };

function stableVisualHash(type, x, y, r) {
  const typeSeed = type === OBSTACLE_TYPE.LAKE ? 17 : (type === OBSTACLE_TYPE.FOREST ? 31 : 47);
  const xi = Math.floor(x * 0.125);
  const yi = Math.floor(y * 0.125);
  const ri = Math.floor(r * 4);
  let h = Math.imul(xi, 374761393) ^ Math.imul(yi, 668265263) ^ Math.imul(ri + typeSeed, 1274126177);
  h = Math.imul(h ^ (h >>> 13), 1274126177);
  return (h ^ (h >>> 16)) >>> 0;
}

export function setObstacleRuntime({ addParticles }) {
  runtime.addParticles = addParticles;
}

function requireRuntime() {
  if (!runtime.addParticles) throw new Error("Obstacle runtime missing; call setObstacleRuntime({ addParticles }) first.");
  return runtime;
}

export function spawnObstacles(){
  obstacles.length = 0;
  const radius = Math.max(200, Math.min(OBSTACLE_CONFIG.spawnRadius, WORLD.halfSize + 400));
  const addCircle = (type, x, y, r, hp=0, visualGroup=0) => {
    const visualHash = stableVisualHash(type, x, y, r);
    obstacles.push({
      type,
      x,
      y,
      r,
      hp,
      maxHp: hp,
      visualGroup,
      visualVariant: visualHash % 4,
      visualRotation: (visualHash >>> 2) % 8,
    });
  };
  const groups = [];
  const groupPad = 80;
  const placeGroup = (groupR) => {
    const lim = radius - groupR;
    if (lim <= 0) return null;
    for (let t=0;t<40;t++){
      const x = rand(lim, -lim);
      const y = rand(lim, -lim);
      let ok = true;
      for (let g=0; g<groups.length; g++){
        const dx = x - groups[g].x;
        const dy = y - groups[g].y;
        const reach = groupR + groups[g].r + groupPad;
        if (dx*dx + dy*dy < reach*reach){
          ok = false;
          break;
        }
      }
      if (ok){
        groups.push({ x, y, r: groupR });
        return { x, y };
      }
    }
    return null;
  };

  // Lakes: few big oval blobs
  for (let i=0;i<OBSTACLE_CONFIG.lakes.count;i++){
    const angle = rand(TAU, 0);
    const dirx = Math.cos(angle), diry = Math.sin(angle);
    const major = rand(OBSTACLE_CONFIG.lakes.sizeMax * 2.2, OBSTACLE_CONFIG.lakes.sizeMax * 1.2);
    const minor = rand(OBSTACLE_CONFIG.lakes.sizeMin * 1.3, OBSTACLE_CONFIG.lakes.sizeMin * 0.8);
    const bulge = rand(0.32, 0.12);
    const segments = OBSTACLE_CONFIG.lakes.blobs || 3;
    const segs = [];
    let maxReach = 0;
    for (let s=0;s<segments;s++){
      const t = segments === 1 ? 0 : (s/(segments-1) - 0.5);
      const jitter = rand(minor * 0.4, -minor * 0.4);
      const ox = dirx * major * t + -diry * jitter * 0.2;
      const oy = diry * major * t + dirx * jitter * 0.2;
      const blend = Math.abs(t);
      const baseR = (major * 0.35) + (minor * 0.8 - major * 0.35) * blend;
      const wobble = 1 + rand(bulge, -bulge);
      const r = Math.max(minor * 0.4, baseR * wobble);
      segs.push({ ox, oy, r });
      const reach = Math.hypot(ox, oy) + r;
      if (reach > maxReach) maxReach = reach;
    }
    const center = placeGroup(maxReach);
    if (!center) continue;
    for (let s=0;s<segs.length;s++){
      addCircle(OBSTACLE_TYPE.LAKE, center.x + segs[s].ox, center.y + segs[s].oy, segs[s].r, 0, i + 1);
    }
  }

  // Forests: small clusters of scattered trees around a base
  for (let i=0;i<OBSTACLE_CONFIG.forests.count;i++){
    const trees = randi(8, 4);
    const segs = [];
    let maxReach = 0;
    for (let t=0;t<trees;t++){
      const ang = rand(TAU, 0);
      const dist = rand(58, 12);
      const r = rand(OBSTACLE_CONFIG.forests.sizeMax, OBSTACLE_CONFIG.forests.sizeMin);
      const ox = Math.cos(ang) * dist;
      const oy = Math.sin(ang) * dist;
      segs.push({ ox, oy, r });
      const reach = Math.hypot(ox, oy) + r;
      if (reach > maxReach) maxReach = reach;
    }
    const center = placeGroup(maxReach);
    if (!center) continue;
    for (let t=0;t<segs.length;t++){
      addCircle(OBSTACLE_TYPE.FOREST, center.x + segs[t].ox, center.y + segs[t].oy, segs[t].r, OBSTACLE_CONFIG.forests.hp);
    }
  }

  // Rocks: mix of long ridges and clustered piles
  const rockCfg = OBSTACLE_CONFIG.rocks;
  const randomInclusive = (min, max) => randi(max + 1, min);
  for (let i=0;i<OBSTACLE_CONFIG.rocks.count;i++){
    if (Math.random() < rockCfg.ridgeChance){
      const angle = rand(TAU, 0);
      const dirx = Math.cos(angle), diry = Math.sin(angle);
      const largeRidge = Math.random() < rockCfg.largeRidgeChance;
      const nodes = largeRidge
        ? randomInclusive(rockCfg.largeRidgeNodesMin, rockCfg.largeRidgeNodesMax)
        : randomInclusive(rockCfg.ridgeNodesMin, rockCfg.ridgeNodesMax);
      const step = largeRidge
        ? rand(rockCfg.largeRidgeStepMax, rockCfg.largeRidgeStepMin)
        : rand(rockCfg.ridgeStepMax, rockCfg.ridgeStepMin);
      const jitterRange = largeRidge ? rockCfg.largeRidgeJitter : rockCfg.ridgeJitter;
      const segs = [];
      let maxReach = 0;
      for (let n=0;n<nodes;n++){
        const offset = (n - (nodes-1)/2) * step;
        const jitter = rand(jitterRange, -jitterRange);
        const ox = dirx * offset + -diry * jitter;
        const oy = diry * offset + dirx * jitter;
        const r = rand(rockCfg.sizeMax, rockCfg.sizeMin);
        segs.push({ ox, oy, r });
        const reach = Math.hypot(ox, oy) + r;
        if (reach > maxReach) maxReach = reach;
      }
      const center = placeGroup(maxReach);
      if (!center) continue;
      for (let n=0;n<segs.length;n++){
        addCircle(OBSTACLE_TYPE.ROCK, center.x + segs[n].ox, center.y + segs[n].oy, segs[n].r, OBSTACLE_CONFIG.rocks.hp);
      }
    } else {
      const largePile = Math.random() < rockCfg.largePileChance;
      const nodes = largePile
        ? randomInclusive(rockCfg.largePileNodesMin, rockCfg.largePileNodesMax)
        : randomInclusive(rockCfg.pileNodesMin, rockCfg.pileNodesMax);
      const spreadMax = largePile ? rockCfg.largePileSpreadMax : rockCfg.pileSpreadMax;
      const segs = [];
      let maxReach = 0;
      for (let n=0;n<nodes;n++){
        const ang = rand(TAU, 0);
        const dist = rand(spreadMax, rockCfg.pileSpreadMin);
        const r = rand(rockCfg.sizeMax, rockCfg.sizeMin);
        const ox = Math.cos(ang) * dist;
        const oy = Math.sin(ang) * dist;
        segs.push({ ox, oy, r });
        const reach = Math.hypot(ox, oy) + r;
        if (reach > maxReach) maxReach = reach;
      }
      const center = placeGroup(maxReach);
      if (!center) continue;
      for (let n=0;n<segs.length;n++){
        addCircle(OBSTACLE_TYPE.ROCK, center.x + segs[n].ox, center.y + segs[n].oy, segs[n].r, OBSTACLE_CONFIG.rocks.hp);
      }
    }
  }
}

export function updateActiveObstacles(camX, camY, W, H, pad){
  activeObstacles.length = 0;
  const minX = camX - pad;
  const maxX = camX + W + pad;
  const minY = camY - pad;
  const maxY = camY + H + pad;
  for (let i=0;i<obstacles.length;i++){
    const o = obstacles[i];
    const r = o.r;
    if (o.x + r < minX || o.x - r > maxX || o.y + r < minY || o.y - r > maxY) continue;
    activeObstacles.push(i);
  }
}

export function damageObstacle(idx, dmg, isExplosion=false){
  const { addParticles } = requireRuntime();
  const o = obstacles[idx];
  if (!o) return;
  if (o.type === OBSTACLE_TYPE.LAKE) return;
  if (o.type === OBSTACLE_TYPE.ROCK && !isExplosion) return;
  o.hp = Math.max(0, o.hp - dmg);
  if (o.hp <= 0){
    if (o.type === OBSTACLE_TYPE.FOREST){
      addParticles(o.x, o.y, COLORS.forest, 10, 280);
    } else if (o.type === OBSTACLE_TYPE.ROCK){
      addParticles(o.x, o.y, COLORS.rock, 10, 240);
    }
    obstacles[idx] = obstacles[obstacles.length-1];
    obstacles.pop();
  }
}

export function damageObstaclesInRadius(x,y,r,dmg,isExplosion=false){
  for (let i=0;i<activeObstacles.length;i++){
    const idx = activeObstacles[i];
    const o = obstacles[idx];
    if (!o) continue;
    const dx = o.x - x;
    const dy = o.y - y;
    const reach = o.r + r;
    if (dx*dx + dy*dy <= reach*reach){
      damageObstacle(idx, dmg, isExplosion);
    }
  }
}

export function resolveObstacles(entity, radius){
  for (let i=0;i<activeObstacles.length;i++){
    const o = obstacles[activeObstacles[i]];
    if (!o) continue;
    const dx = entity.x - o.x;
    const dy = entity.y - o.y;
    const dist2 = dx*dx + dy*dy;
    const minD = radius + o.r;
    if (dist2 < minD*minD){
      const dist = Math.sqrt(dist2) || 1;
      const push = (minD - dist) + 0.1;
      entity.x += (dx / dist) * push;
      entity.y += (dy / dist) * push;
    }
  }
}

export function obstacleAvoidance(x,y,r){
  let ax = 0, ay = 0;
  for (let i=0;i<activeObstacles.length;i++){
    const o = obstacles[activeObstacles[i]];
    if (!o) continue;
    const dx = x - o.x;
    const dy = y - o.y;
    const dist2 = dx*dx + dy*dy;
    const minD = r + o.r + 30;
    if (dist2 < minD*minD){
      const dist = Math.sqrt(dist2) || 1;
      const strength = (minD - dist) / minD;
      ax += (dx / dist) * strength;
      ay += (dy / dist) * strength;
    }
  }
  return { ax, ay };
}
