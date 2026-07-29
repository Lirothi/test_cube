import { COLORS, CHEST_CONFIG, WEAPON_CONFIG, ELITE_CONFIG, MAX_WEAPONS, ENEMY_TYPES, TRINKET_CONFIG } from "./config.js";
import { VISUAL_TOKENS } from "./visual_theme.js";
import { clamp, TAU, fmtFloat } from "./math.js";
import { weapons, auraStats } from "./weapons.js";
import { getQuestHudText } from "./quests.js";
import { combatFx } from "./combat_fx.js";
import { visualSettings } from "./visual_settings.js";
import {
  WORLD,
  player,
  activeObstacles,
  obstacles,
  voidZones,
  chests,
  gems,
  enemies,
  enemyShots,
  rails,
  bullets,
  missiles,
  turrets,
  toxinPools,
  axes,
  orbs,
  arcs,
  telegraphs,
  particles,
  dmgTexts,
  floatTexts,
  buffs,
  quest,
  questItems,
  companions,
  trinkets,
} from "./state.js";

const UI_COLORS = VISUAL_TOKENS.canvas;
const BASE_GLOW = VISUAL_TOKENS.glow;
function usesLowResGlow() {
  return visualSettings.glowLayerMode === "lowres" && visualSettings.glowScale > 0;
}
const GLOW = {
  get low() { return usesLowResGlow() ? 0 : BASE_GLOW.low * visualSettings.glowScale; },
  get medium() { return usesLowResGlow() ? 0 : BASE_GLOW.medium * visualSettings.glowScale; },
  get high() { return usesLowResGlow() ? 0 : BASE_GLOW.high * visualSettings.glowScale; },
};
const OPACITY = VISUAL_TOKENS.opacity;

const enemySpriteCache = new Map();
const gemSpriteCache = new Map();
const questSpriteCache = new Map();
const obstacleSpriteCache = new Map();
const lakeGroupSpriteCache = new Map();
const neonCircleSpriteCache = new Map();
const projectileSpriteCache = new Map();
const particleSpriteCache = new Map();
const hudTextCache = new WeakMap();
const hudHtmlCache = new WeakMap();
const hudWidthCache = new WeakMap();
const bossesScratch = [];
const buffPartsScratch = [];
const visibleFrame = {
  lakes: [],
  obstacles: [],
  voidZones: [],
  chests: [],
  questItems: [],
  gems: [],
  enemies: [],
  telegraphs: [],
  enemyShots: [],
  arcs: [],
  rails: [],
  bullets: [],
  missiles: [],
  turrets: [],
  toxinPools: [],
  axes: [],
  orbs: [],
  particles: [],
  companions: [],
  damageTexts: [],
  floatTexts: [],
};
let lakeTextureTile = null;
let cachedVisualSignature = "";
let nextHudUpdateAt = 0;
let glowMaskCanvas = null;
let glowMaskCtx = null;
let glowBlurCanvas = null;
let glowBlurCtx = null;
let glowBackingWidth = 0;
let glowBackingHeight = 0;

const OBSTACLE_SPRITE_RADIUS = 48;
const OBSTACLE_SPRITE_PAD = 8;
const LAKE_GROUP_CACHE_LIMIT = 32;
const LAKE_GROUP_PAD = 18;
const LAKE_OUTLINE_WIDTH = 4;
const NO_LINE_DASH = [];
const TELEGRAPH_DASH_NARROW = [10, 6];
const TELEGRAPH_DASH_WIDE = [16, 9];
const HUD_UPDATE_INTERVAL_MS = 1000 / 30;
const LOW_RES_GLOW_SCALE = 0.5;
const GLOW_FILTER_TO_SHADOW_SCALE = 0.44;
const LOW_RES_GLOW_PASSES = [
  { tier: "low", radius: BASE_GLOW.low },
  { tier: "medium", radius: BASE_GLOW.medium },
  { tier: "high", radius: BASE_GLOW.high },
];
const DYNAMIC_SPRITE_CACHE_LIMIT = 192;

function syncVisualCaches() {
  const signature = `${visualSettings.glowScale}|${visualSettings.glowLayerMode}`;
  if (cachedVisualSignature === signature) return;
  enemySpriteCache.clear();
  gemSpriteCache.clear();
  questSpriteCache.clear();
  obstacleSpriteCache.clear();
  lakeGroupSpriteCache.clear();
  neonCircleSpriteCache.clear();
  projectileSpriteCache.clear();
  particleSpriteCache.clear();
  lakeTextureTile = null;
  cachedVisualSignature = signature;
}

function visualTime() {
  return visualSettings.reducedMotion ? 0 : performance.now();
}

function setHudText(element, value) {
  if (!element || hudTextCache.get(element) === value) return;
  hudTextCache.set(element, value);
  element.textContent = value;
}

function setHudHtml(element, value) {
  if (!element || hudHtmlCache.get(element) === value) return;
  hudHtmlCache.set(element, value);
  element.innerHTML = value;
}

function setHudWidth(element, value) {
  if (!element || hudWidthCache.get(element) === value) return;
  hudWidthCache.set(element, value);
  element.style.width = value;
}

function makeOffscreenCanvas(w, h) {
  if (typeof OffscreenCanvas !== "undefined") return new OffscreenCanvas(w, h);
  const c = document.createElement("canvas");
  c.width = w;
  c.height = h;
  return c;
}

const visibleArrays = Object.values(visibleFrame);

function resetVisibleFrame() {
  for (let i = 0; i < visibleArrays.length; i++) visibleArrays[i].length = 0;
}

function circleIntersectsView(x, y, radius, minX, maxX, minY, maxY) {
  return x + radius >= minX && x - radius <= maxX
    && y + radius >= minY && y - radius <= maxY;
}

function pointsIntersectView(points, minX, maxX, minY, maxY, padding = 0) {
  if (!points || !points.length) return false;
  let pointsMinX = Infinity;
  let pointsMaxX = -Infinity;
  let pointsMinY = Infinity;
  let pointsMaxY = -Infinity;
  for (let i = 0; i < points.length; i++) {
    const point = points[i];
    pointsMinX = Math.min(pointsMinX, point.x);
    pointsMaxX = Math.max(pointsMaxX, point.x);
    pointsMinY = Math.min(pointsMinY, point.y);
    pointsMaxY = Math.max(pointsMaxY, point.y);
  }
  return pointsMaxX + padding >= minX && pointsMinX - padding <= maxX
    && pointsMaxY + padding >= minY && pointsMinY - padding <= maxY;
}

function segmentIntersectsView(x1, y1, x2, y2, minX, maxX, minY, maxY, padding = 0) {
  return Math.max(x1, x2) + padding >= minX && Math.min(x1, x2) - padding <= maxX
    && Math.max(y1, y2) + padding >= minY && Math.min(y1, y2) - padding <= maxY;
}

function collectVisibleFrame(camX, camY, W, H) {
  resetVisibleFrame();
  const minX = camX - WORLD.spawnPad;
  const maxX = camX + W + WORLD.spawnPad;
  const minY = camY - WORLD.spawnPad;
  const maxY = camY + H + WORLD.spawnPad;

  for (let i = 0; i < activeObstacles.length; i++) {
    const obstacle = obstacles[activeObstacles[i]];
    if (!obstacle || !circleIntersectsView(obstacle.x, obstacle.y, obstacle.r + 20, minX, maxX, minY, maxY)) continue;
    (obstacle.type === "lake" ? visibleFrame.lakes : visibleFrame.obstacles).push(obstacle);
  }
  for (let i = 0; i < voidZones.length; i++) {
    const zone = voidZones[i];
    if (circleIntersectsView(zone.x, zone.y, zone.radius + 24, minX, maxX, minY, maxY)) visibleFrame.voidZones.push(zone);
  }
  for (let i = 0; i < chests.length; i++) {
    const chest = chests[i];
    if (circleIntersectsView(chest.x, chest.y, chest.r * 2.2, minX, maxX, minY, maxY)) visibleFrame.chests.push(chest);
  }
  for (let i = 0; i < questItems.length; i++) {
    const item = questItems[i];
    if (item.alive && circleIntersectsView(item.x, item.y, item.r + 24, minX, maxX, minY, maxY)) visibleFrame.questItems.push(item);
  }
  for (let i = 0; i < gems.length; i++) {
    const gem = gems[i];
    if (circleIntersectsView(gem.x, gem.y, gem.r + 24, minX, maxX, minY, maxY)) visibleFrame.gems.push(gem);
  }
  for (let i = 0; i < enemies.length; i++) {
    const enemy = enemies[i];
    if (enemy.alive && circleIntersectsView(enemy.x, enemy.y, enemy.r + 40, minX, maxX, minY, maxY)) visibleFrame.enemies.push(enemy);
  }
  for (let i = 0; i < telegraphs.length; i++) {
    const telegraph = telegraphs[i];
    const length = Math.max(0, telegraph.length || 0);
    const endX = telegraph.x + (telegraph.dx || 0) * length;
    const endY = telegraph.y + (telegraph.dy || 0) * length;
    const padding = Math.max(telegraph.radius || 0, telegraph.dangerWidth || 0, 32);
    if (segmentIntersectsView(telegraph.x, telegraph.y, endX, endY, minX, maxX, minY, maxY, padding)) {
      visibleFrame.telegraphs.push(telegraph);
    }
  }
  for (let i = 0; i < enemyShots.length; i++) {
    const shot = enemyShots[i];
    if (circleIntersectsView(shot.x, shot.y, shot.r * 4 + 12, minX, maxX, minY, maxY)) visibleFrame.enemyShots.push(shot);
  }
  for (let i = 0; i < arcs.length; i++) {
    const arc = arcs[i];
    if (pointsIntersectView(arc.points, minX, maxX, minY, maxY, 28)) visibleFrame.arcs.push(arc);
  }
  for (let i = 0; i < rails.length; i++) {
    const rail = rails[i];
    const trailVisible = pointsIntersectView(rail.trail, minX, maxX, minY, maxY, rail.r * 4 + 16);
    if (trailVisible || circleIntersectsView(rail.x, rail.y, rail.r * 4 + 16, minX, maxX, minY, maxY)) visibleFrame.rails.push(rail);
  }
  for (let i = 0; i < bullets.length; i++) {
    const bullet = bullets[i];
    const radius = bullet.isExplosion ? Math.max(bullet.r || 0, 24) : (bullet.r || 0) + 16;
    if (circleIntersectsView(bullet.x, bullet.y, radius, minX, maxX, minY, maxY)) visibleFrame.bullets.push(bullet);
  }
  for (let i = 0; i < missiles.length; i++) {
    const missile = missiles[i];
    if (circleIntersectsView(missile.x, missile.y, missile.r * 4 + 16, minX, maxX, minY, maxY)) visibleFrame.missiles.push(missile);
  }
  for (let i = 0; i < turrets.length; i++) {
    const turret = turrets[i];
    const endX = turret.x + Math.cos(turret.angle || 0) * (turret.streamLength || 0);
    const endY = turret.y + Math.sin(turret.angle || 0) * (turret.streamLength || 0);
    if (segmentIntersectsView(turret.x, turret.y, endX, endY, minX, maxX, minY, maxY, Math.max(28, turret.width || 0))) {
      visibleFrame.turrets.push(turret);
    }
  }
  for (let i = 0; i < toxinPools.length; i++) {
    const pool = toxinPools[i];
    if (pool.alive && circleIntersectsView(pool.x, pool.y, pool.radius + 20, minX, maxX, minY, maxY)) visibleFrame.toxinPools.push(pool);
  }
  for (let i = 0; i < axes.length; i++) {
    const axe = axes[i];
    if (circleIntersectsView(axe.x, axe.y, Math.max(24, axe.r * 2), minX, maxX, minY, maxY)) visibleFrame.axes.push(axe);
  }
  for (let i = 0; i < orbs.length; i++) {
    const orb = orbs[i];
    if (circleIntersectsView(orb.x, orb.y, Math.max(orb.r || 0, orb.radius || 0) + 24, minX, maxX, minY, maxY)) visibleFrame.orbs.push(orb);
  }
  for (let i = 0; i < particles.length; i++) {
    const particle = particles[i];
    const radius = particle.r * Math.max(1, particle.stretch || 1) + 12;
    if (circleIntersectsView(particle.x, particle.y, radius, minX, maxX, minY, maxY)) visibleFrame.particles.push(particle);
  }
  for (let i = 0; i < companions.length; i++) {
    const companion = companions[i];
    if (circleIntersectsView(companion.x, companion.y, companion.r + 24, minX, maxX, minY, maxY)) visibleFrame.companions.push(companion);
  }
  for (let i = 0; i < dmgTexts.length; i++) {
    const text = dmgTexts[i];
    if (circleIntersectsView(text.x, text.y, Math.max(80, text.size * 3), minX, maxX, minY, maxY)) visibleFrame.damageTexts.push(text);
  }
  for (let i = 0; i < floatTexts.length; i++) {
    const text = floatTexts[i];
    if (circleIntersectsView(text.x, text.y, Math.max(80, text.size * 3), minX, maxX, minY, maxY)) visibleFrame.floatTexts.push(text);
  }

  return { minX, maxX, minY, maxY };
}

function tracePoints(g, cx, cy, r, points) {
  g.beginPath();
  for (let i=0;i<points.length;i+=2) {
    const x = cx + points[i] * r;
    const y = cy + points[i + 1] * r;
    if (i === 0) g.moveTo(x, y);
    else g.lineTo(x, y);
  }
  g.closePath();
}

function tracePolygon(g, cx, cy, r, sides, rotation = -Math.PI * 0.5) {
  g.beginPath();
  for (let i=0;i<sides;i++) {
    const a = rotation + i * TAU / sides;
    const x = cx + Math.cos(a) * r;
    const y = cy + Math.sin(a) * r;
    if (i === 0) g.moveTo(x, y);
    else g.lineTo(x, y);
  }
  g.closePath();
}

function fillSilhouette(g, color, glow = GLOW.medium, outlineWidth = 1) {
  g.fillStyle = color;
  g.shadowColor = color;
  g.shadowBlur = glow;
  g.fill();
  g.shadowBlur = 0;
  g.strokeStyle = UI_COLORS.enemyOutline;
  g.lineWidth = outlineWidth;
  g.stroke();
}

function strokeSilhouette(g, color, width, glow = GLOW.medium) {
  g.strokeStyle = color;
  g.lineWidth = width;
  g.shadowColor = color;
  g.shadowBlur = glow;
  g.stroke();
  g.shadowBlur = 0;
  g.strokeStyle = UI_COLORS.enemyOutline;
  g.lineWidth = Math.max(1, width * 0.16);
  g.stroke();
}

function fillCircle(g, x, y, r, color, glow = GLOW.medium) {
  g.beginPath();
  g.arc(x, y, r, 0, TAU);
  fillSilhouette(g, color, glow);
}

function carveCircle(g, x, y, r) {
  g.save();
  g.globalCompositeOperation = "destination-out";
  g.shadowBlur = 0;
  g.fillStyle = "#000";
  g.beginPath();
  g.arc(x, y, r, 0, TAU);
  g.fill();
  g.restore();
}

function carveEllipse(g, x, y, rx, ry, rotation = 0) {
  g.save();
  g.globalCompositeOperation = "destination-out";
  g.shadowBlur = 0;
  g.fillStyle = "#000";
  g.beginPath();
  g.ellipse(x, y, rx, ry, rotation, 0, TAU);
  g.fill();
  g.restore();
}

function strokeDetail(g, r, width = 0.09) {
  g.shadowBlur = 0;
  g.strokeStyle = UI_COLORS.enemyDetail;
  g.lineWidth = Math.max(1, r * width);
  g.stroke();
}

function drawDrop(g, x, y, r, rotation, color) {
  g.save();
  g.translate(x, y);
  g.rotate(rotation);
  g.beginPath();
  g.moveTo(0, -r);
  g.bezierCurveTo(r * 0.68, -r * 0.18, r * 0.58, r * 0.72, 0, r * 0.78);
  g.bezierCurveTo(-r * 0.58, r * 0.72, -r * 0.68, -r * 0.18, 0, -r);
  g.closePath();
  fillSilhouette(g, color);
  g.restore();
}

function drawEnemyBody(g, type, cx, cy, r, color) {
  g.lineJoin = "round";
  g.lineCap = "round";

  switch (type) {
    case "B": {
      tracePoints(g, cx, cy, r, [
        0, -0.98,
        0.2, -0.22,
        0.98, 0,
        0.2, 0.22,
        0, 0.98,
        -0.2, 0.22,
        -0.98, 0,
        -0.2, -0.22,
      ]);
      fillSilhouette(g, color);
      tracePolygon(g, cx, cy, r * 0.24, 4, Math.PI * 0.25);
      strokeDetail(g, r, 0.1);
      break;
    }
    case "C": {
      tracePolygon(g, cx, cy, r * 0.96, 6, Math.PI / 6);
      fillSilhouette(g, color, GLOW.medium, 1.2);
      tracePolygon(g, cx, cy, r * 0.58, 6, Math.PI / 6);
      strokeDetail(g, r, 0.1);
      break;
    }
    case "S": {
      tracePoints(g, cx, cy, r, [
        -0.78, -0.72,
        0.78, -0.72,
        0.98, -0.42,
        0.98, 0.42,
        0.78, 0.72,
        -0.78, 0.72,
        -0.98, 0.42,
        -0.98, -0.42,
      ]);
      fillSilhouette(g, color, GLOW.medium, 1.2);
      g.beginPath();
      g.moveTo(cx - r * 0.66, cy - r * 0.48);
      g.lineTo(cx - r * 0.38, cy - r * 0.34);
      g.lineTo(cx - r * 0.38, cy + r * 0.34);
      g.lineTo(cx - r * 0.66, cy + r * 0.48);
      g.closePath();
      g.moveTo(cx + r * 0.66, cy - r * 0.48);
      g.lineTo(cx + r * 0.38, cy - r * 0.34);
      g.lineTo(cx + r * 0.38, cy + r * 0.34);
      g.lineTo(cx + r * 0.66, cy + r * 0.48);
      g.closePath();
      strokeDetail(g, r, 0.13);
      break;
    }
    case "R": {
      g.beginPath();
      g.arc(cx, cy, r * 0.5, 0, TAU);
      strokeSilhouette(g, color, r * 0.3);
      for (let i=0;i<4;i++) {
        g.save();
        g.translate(cx, cy);
        g.rotate(i * Math.PI * 0.5);
        g.beginPath();
        g.rect(r * 0.42, -r * 0.14, r * 0.52, r * 0.28);
        fillSilhouette(g, color);
        g.restore();
      }
      break;
    }
    case "M": {
      g.beginPath();
      g.arc(cx, cy, r * 0.69, 0, TAU);
      strokeSilhouette(g, color, r * 0.13);
      for (let i=0;i<3;i++) {
        const a = -Math.PI * 0.5 + i * TAU / 3;
        fillCircle(g, cx + Math.cos(a) * r * 0.69, cy + Math.sin(a) * r * 0.69, r * 0.15, color);
      }
      tracePolygon(g, cx, cy, r * 0.43, 6, 0);
      fillSilhouette(g, color);
      tracePolygon(g, cx, cy, r * 0.2, 6, 0);
      strokeDetail(g, r, 0.08);
      break;
    }
    case "P": {
      const orbit = r * 0.3;
      const dropR = r * 0.48;
      for (let i=0;i<3;i++) {
        const a = -Math.PI * 0.5 + i * TAU / 3;
        drawDrop(
          g,
          cx + Math.cos(a) * orbit,
          cy + Math.sin(a) * orbit,
          dropR,
          a + Math.PI * 0.5,
          color
        );
      }
      break;
    }
    case "F": {
      const flamePoints = [];
      for (let i=0;i<12;i++) {
        const a = -Math.PI * 0.5 + i * TAU / 12;
        const fr = i % 2 === 0 ? 0.98 : 0.54;
        flamePoints.push(Math.cos(a) * fr, Math.sin(a) * fr);
      }
      tracePoints(g, cx, cy, r, flamePoints);
      fillSilhouette(g, color);
      tracePolygon(g, cx, cy, r * 0.34, 6, 0);
      strokeDetail(g, r, 0.1);
      break;
    }
    case "V": {
      for (let i=0;i<3;i++) {
        const a = i * TAU / 3;
        g.beginPath();
        g.arc(cx, cy, r * 0.62, a - 0.48, a + 0.48);
        strokeSilhouette(g, color, r * 0.28);
      }
      g.beginPath();
      g.arc(cx, cy, r * 0.2, 0, TAU);
      strokeDetail(g, r, 0.08);
      break;
    }
    case "X": {
      const points = [];
      for (let i=0;i<16;i++) {
        const a = -Math.PI * 0.5 + i * TAU / 16;
        const pr = i % 2 === 0 ? 0.98 : 0.76;
        points.push(Math.cos(a) * pr, Math.sin(a) * pr);
      }
      tracePoints(g, cx, cy, r, points);
      fillSilhouette(g, color, GLOW.high, 1.4);
      carveEllipse(g, cx, cy, r * 0.52, r * 0.23);
      fillCircle(g, cx, cy, r * 0.12, color, GLOW.low);
      break;
    }
    case "Y": {
      fillCircle(g, cx - r * 0.08, cy, r * 0.86, color, GLOW.high);
      carveCircle(g, cx + r * 0.28, cy - r * 0.06, r * 0.66);
      g.beginPath();
      g.arc(cx, cy, r * 0.91, -1.22, 1.12);
      strokeSilhouette(g, color, r * 0.09, GLOW.low);
      tracePolygon(g, cx + r * 0.38, cy, r * 0.19, 4, 0);
      fillSilhouette(g, color, GLOW.low);
      break;
    }
    case "Z": {
      tracePoints(g, cx, cy, r, [
        -0.96, -0.52,
        -0.48, -0.9,
        0.48, -0.9,
        0.96, -0.52,
        0.96, 0.52,
        0.48, 0.9,
        -0.48, 0.9,
        -0.96, 0.52,
      ]);
      fillSilhouette(g, color, GLOW.high, 1.5);
      g.beginPath();
      g.moveTo(cx - r * 0.52, cy - r * 0.28);
      g.lineTo(cx, cy + r * 0.2);
      g.lineTo(cx + r * 0.52, cy - r * 0.28);
      g.moveTo(cx - r * 0.38, cy + r * 0.08);
      g.lineTo(cx, cy + r * 0.48);
      g.lineTo(cx + r * 0.38, cy + r * 0.08);
      strokeDetail(g, r, 0.11);
      break;
    }
    case "W": {
      tracePoints(g, cx, cy, r, [
        0, -0.98,
        0.22, -0.46,
        0.68, -0.68,
        0.46, -0.22,
        0.98, 0,
        0.46, 0.22,
        0.68, 0.68,
        0.22, 0.46,
        0, 0.98,
        -0.22, 0.46,
        -0.68, 0.68,
        -0.46, 0.22,
        -0.98, 0,
        -0.46, -0.22,
        -0.68, -0.68,
        -0.22, -0.46,
      ]);
      fillSilhouette(g, color, GLOW.high, 1.4);
      tracePolygon(g, cx, cy, r * 0.42, 4, Math.PI * 0.25);
      g.save();
      g.globalCompositeOperation = "destination-out";
      g.fillStyle = "#000";
      g.fill();
      g.restore();
      fillCircle(g, cx, cy, r * 0.12, color, GLOW.low);
      break;
    }
    case "Q": {
      g.globalAlpha = 0.66;
      tracePolygon(g, cx - r * 0.2, cy, r * 0.74, 4, 0);
      fillSilhouette(g, color, GLOW.high, 1.3);
      g.globalAlpha = 1;
      tracePolygon(g, cx + r * 0.2, cy, r * 0.74, 4, 0);
      fillSilhouette(g, color, GLOW.high, 1.3);
      carveCircle(g, cx, cy, r * 0.25);
      g.beginPath();
      g.arc(cx, cy, r * 0.36, 0, TAU);
      strokeDetail(g, r, 0.07);
      fillCircle(g, cx, cy - r * 0.47, r * 0.09, color, GLOW.low);
      fillCircle(g, cx, cy + r * 0.47, r * 0.09, color, GLOW.low);
      break;
    }
    case "A":
    default: {
      tracePolygon(g, cx, cy, r * 0.96, 4, 0);
      fillSilhouette(g, color);
      tracePolygon(g, cx, cy, r * 0.43, 4, 0);
      strokeDetail(g, r, 0.09);
      break;
    }
  }
}

function traceCornerBrackets(g, cx, cy, extent, arm) {
  g.beginPath();
  g.moveTo(cx - extent + arm, cy - extent);
  g.lineTo(cx - extent, cy - extent);
  g.lineTo(cx - extent, cy - extent + arm);
  g.moveTo(cx + extent - arm, cy - extent);
  g.lineTo(cx + extent, cy - extent);
  g.lineTo(cx + extent, cy - extent + arm);
  g.moveTo(cx - extent, cy + extent - arm);
  g.lineTo(cx - extent, cy + extent);
  g.lineTo(cx - extent + arm, cy + extent);
  g.moveTo(cx + extent, cy + extent - arm);
  g.lineTo(cx + extent, cy + extent);
  g.lineTo(cx + extent - arm, cy + extent);
}

function drawEliteMarker(g, cx, cy, r) {
  const color = ELITE_CONFIG.markerColor;
  const outer = r * 1.22;
  const inner = r * 1.06;
  g.lineJoin = "miter";
  g.lineCap = "square";
  g.strokeStyle = color;
  g.shadowColor = color;
  g.shadowBlur = GLOW.medium;
  g.lineWidth = Math.max(1.6, r * 0.12);
  traceCornerBrackets(g, cx, cy, outer, Math.max(4, r * 0.34));
  g.stroke();
  g.shadowBlur = 0;
  g.lineWidth = Math.max(1, r * 0.07);
  traceCornerBrackets(g, cx, cy, inner, Math.max(3, r * 0.24));
  g.stroke();

  const crownW = Math.max(5, r * 0.34);
  const crownH = Math.max(4, r * 0.28);
  const top = cy - outer - crownH * 0.25;
  g.beginPath();
  g.moveTo(cx - crownW, top + crownH);
  g.lineTo(cx - crownW, top + crownH * 0.38);
  g.lineTo(cx - crownW * 0.45, top + crownH * 0.72);
  g.lineTo(cx, top);
  g.lineTo(cx + crownW * 0.45, top + crownH * 0.72);
  g.lineTo(cx + crownW, top + crownH * 0.38);
  g.lineTo(cx + crownW, top + crownH);
  g.closePath();
  fillSilhouette(g, color, GLOW.low, 1);
}

export const getEnemySprite = (type, r, color, visualVariant = "base") => {
  const variant = visualVariant === "elite" ? "elite" : "base";
  const key = `${type}|${r}|${color}|${variant}`;
  let sprite = enemySpriteCache.get(key);
  if (sprite) return sprite;
  const boss = ENEMY_TYPES[type]?.boss === true;
  const glow = boss ? GLOW.high : GLOW.medium;
  const pad = Math.ceil(glow + (variant === "elite" ? 14 : 6));
  const size = Math.ceil(r * 2);
  const c = makeOffscreenCanvas(size + pad * 2, size + pad * 2);
  const g = c.getContext("2d");
  const cx = pad + r;
  const cy = pad + r;
  drawEnemyBody(g, type, cx, cy, r, color);
  if (variant === "elite") drawEliteMarker(g, cx, cy, r);
  sprite = { canvas: c, pad, r, size };
  enemySpriteCache.set(key, sprite);
  return sprite;
};

const getGemSprite = (r) => {
  const key = `${r}`;
  let sprite = gemSpriteCache.get(key);
  if (sprite) return sprite;
  const pad = 12;
  const size = Math.ceil(r * 2);
  const c = makeOffscreenCanvas(size + pad * 2, size + pad * 2);
  const g = c.getContext("2d");
  const cx = pad + r;
  const cy = pad + r;
  g.shadowColor = COLORS.gem;
  g.shadowBlur = GLOW.low;
  g.fillStyle = COLORS.gem;
  tracePoints(g, cx, cy, r, [
    0, -1,
    0.72, 0,
    0, 1,
    -0.72, 0,
  ]);
  g.fill();
  g.globalAlpha = 0.85;
  g.fill();
  g.globalAlpha = 1;
  g.shadowBlur = 0;
  g.strokeStyle = UI_COLORS.enemyDetail;
  g.lineWidth = 1;
  g.stroke();

  g.globalAlpha = 0.72;
  g.strokeStyle = UI_COLORS.enemyDetail;
  g.lineWidth = 0.8;
  g.beginPath();
  g.moveTo(cx, cy - r * 0.76);
  g.lineTo(cx, cy + r * 0.76);
  g.moveTo(cx, cy);
  g.lineTo(cx + r * 0.5, cy);
  g.stroke();
  sprite = { canvas: c, pad, r };
  gemSpriteCache.set(key, sprite);
  return sprite;
};

const getQuestSprite = (r, color) => {
  const key = `${r}|${color}`;
  let sprite = questSpriteCache.get(key);
  if (sprite) return sprite;
  const pad = Math.ceil(18 + r * 0.25);
  const size = Math.ceil(r * 2);
  const c = makeOffscreenCanvas(size + pad * 2, size + pad * 2);
  const g = c.getContext("2d");
  const cx = pad + r;
  const cy = pad + r;
  g.shadowColor = color;
  g.shadowBlur = GLOW.medium;
  g.fillStyle = UI_COLORS.questFill;
  tracePolygon(g, cx, cy, r, 6, Math.PI / 6);
  g.fill();
  g.shadowBlur = 0;
  g.strokeStyle = color;
  g.lineWidth = Math.max(1.5, r * 0.16);
  g.stroke();
  drawQuestMark(g, cx, cy, r * 0.58, color);

  g.globalAlpha = 0.7;
  g.beginPath();
  g.arc(cx, cy, r * 1.28, 0, TAU);
  g.lineWidth = 1.5;
  g.stroke();
  sprite = { canvas: c, pad, r, size };
  questSpriteCache.set(key, sprite);
  return sprite;
};

function stableCellHash(x, y) {
  let h = Math.imul(x | 0, 374761393) ^ Math.imul(y | 0, 668265263);
  h = Math.imul(h ^ (h >>> 13), 1274126177);
  return (h ^ (h >>> 16)) >>> 0;
}

function obstacleVariant(o) {
  if (Number.isInteger(o.visualVariant)) return ((o.visualVariant % 4) + 4) % 4;
  return stableCellHash(Math.floor(o.x * 0.125), Math.floor(o.y * 0.125)) % 4;
}

function obstacleRotation(o) {
  if (Number.isInteger(o.visualRotation)) return ((o.visualRotation % 8) + 8) % 8;
  const hash = stableCellHash(Math.floor(o.x * 0.125), Math.floor(o.y * 0.125));
  return (hash >>> 2) % 8;
}

function obstacleDamageStage(o) {
  if (o.type === "lake" || !(o.maxHp > 0)) return 0;
  const hpT = clamp(o.hp / o.maxHp, 0, 1);
  if (hpT > 0.75) return 0;
  if (hpT > 0.5) return 1;
  if (hpT > 0.25) return 2;
  return 3;
}

function drawObstacleDamage(ctx, cx, cy, r, stage, variant) {
  if (stage <= 0) return;
  ctx.save();
  ctx.strokeStyle = COLORS.obstacleDamage;
  ctx.lineWidth = 1.6;
  ctx.lineCap = "round";
  for (let i = 0; i < stage + 1; i++) {
    const a = variant * 0.71 + i * 2.17 - Math.PI * 0.5;
    const inner = r * (0.08 + i * 0.04);
    const outer = r * (0.38 + stage * 0.1);
    const sx = cx + Math.cos(a) * inner;
    const sy = cy + Math.sin(a) * inner;
    const mx = cx + Math.cos(a + 0.16) * outer * 0.58;
    const my = cy + Math.sin(a + 0.16) * outer * 0.58;
    const ex = cx + Math.cos(a) * outer;
    const ey = cy + Math.sin(a) * outer;
    ctx.beginPath();
    ctx.moveTo(sx, sy);
    ctx.lineTo(mx, my);
    ctx.lineTo(ex, ey);
    if (stage >= 2) {
      ctx.moveTo(mx, my);
      ctx.lineTo(
        mx + Math.cos(a - 1.05) * r * 0.16,
        my + Math.sin(a - 1.05) * r * 0.16
      );
    }
    ctx.stroke();
  }
  ctx.restore();
}

function traceLakeMask(ctx, segments, offsetX = 0, offsetY = 0) {
  ctx.beginPath();
  for (let i = 0; i < segments.length; i++) {
    const o = segments[i];
    const x = o.x + offsetX;
    const y = o.y + offsetY;
    ctx.moveTo(x + o.r, y);
    ctx.arc(x, y, o.r, 0, TAU);
  }
}

function getLakeTextureTile() {
  if (lakeTextureTile) return lakeTextureTile;

  const tile = makeOffscreenCanvas(128, 96);
  const g = tile.getContext("2d");
  g.strokeStyle = COLORS.lakeRipple;
  g.lineWidth = 1.25;
  g.lineCap = "round";

  const ripples = [
    [8, 21, 22, 15, 41, 18, 56, 16],
    [69, 43, 82, 48, 101, 45, 119, 48],
    [20, 76, 34, 70, 52, 74, 66, 71],
  ];
  for (let i = 0; i < ripples.length; i++) {
    const p = ripples[i];
    g.beginPath();
    g.moveTo(p[0], p[1]);
    g.bezierCurveTo(p[2], p[3], p[4], p[5], p[6], p[7]);
    g.stroke();
  }

  g.fillStyle = COLORS.lakeHighlight;
  const glints = [[15, 50, 1.1], [62, 8, 0.8], [93, 72, 1], [123, 25, 0.7]];
  for (let i = 0; i < glints.length; i++) {
    const [x, y, r] = glints[i];
    g.beginPath();
    g.arc(x, y, r, 0, TAU);
    g.fill();
  }

  lakeTextureTile = tile;
  return lakeTextureTile;
}

function lakeGroupCacheKey(group) {
  let hash = 2166136261;
  for (let i = 0; i < group.segments.length; i++) {
    const o = group.segments[i];
    hash ^= Math.round(o.x * 4);
    hash = Math.imul(hash, 16777619);
    hash ^= Math.round(o.y * 4);
    hash = Math.imul(hash, 16777619);
    hash ^= Math.round(o.r * 4);
    hash = Math.imul(hash, 16777619);
  }
  return `${group.segments.length}|${hash >>> 0}`;
}

function getLakeGroupSprite(group) {
  const key = lakeGroupCacheKey(group);
  let sprite = lakeGroupSpriteCache.get(key);
  if (sprite) {
    lakeGroupSpriteCache.delete(key);
    lakeGroupSpriteCache.set(key, sprite);
    return sprite;
  }

  const pad = LAKE_GROUP_PAD;
  const worldWidth = Math.max(1, group.maxX - group.minX);
  const worldHeight = Math.max(1, group.maxY - group.minY);
  const canvasWidth = Math.ceil(worldWidth + pad * 2);
  const canvasHeight = Math.ceil(worldHeight + pad * 2);
  const canvas = makeOffscreenCanvas(canvasWidth, canvasHeight);
  const g = canvas.getContext("2d");
  const mask = makeOffscreenCanvas(canvasWidth, canvasHeight);
  const maskCtx = mask.getContext("2d");
  const offsetX = -group.minX + pad;
  const offsetY = -group.minY + pad;
  const minX = pad;
  const minY = pad;
  const maxX = pad + worldWidth;
  const maxY = pad + worldHeight;

  maskCtx.fillStyle = "#fff";
  traceLakeMask(maskCtx, group.segments, offsetX, offsetY);
  maskCtx.fill();

  const outline = makeOffscreenCanvas(canvasWidth, canvasHeight);
  const outlineCtx = outline.getContext("2d");
  const outlineSteps = 16;
  for (let i = 0; i < outlineSteps; i++) {
    const angle = i * TAU / outlineSteps;
    outlineCtx.drawImage(
      mask,
      Math.cos(angle) * LAKE_OUTLINE_WIDTH,
      Math.sin(angle) * LAKE_OUTLINE_WIDTH
    );
  }
  outlineCtx.globalCompositeOperation = "destination-out";
  outlineCtx.drawImage(mask, 0, 0);
  outlineCtx.globalCompositeOperation = "source-in";
  outlineCtx.fillStyle = COLORS.lakeOutline;
  outlineCtx.fillRect(0, 0, canvasWidth, canvasHeight);

  g.save();
  g.shadowColor = COLORS.lakeEdge;
  g.shadowBlur = 10;
  g.drawImage(outline, 0, 0);
  g.restore();

  const water = makeOffscreenCanvas(canvasWidth, canvasHeight);
  const waterCtx = water.getContext("2d");
  const shade = waterCtx.createLinearGradient(minX, minY, minX, maxY);
  shade.addColorStop(0, COLORS.lakeShallow);
  shade.addColorStop(0.42, COLORS.lake);
  shade.addColorStop(1, COLORS.lakeDeep);
  waterCtx.fillStyle = shade;
  waterCtx.fillRect(0, 0, canvasWidth, canvasHeight);

  const texture = waterCtx.createPattern(getLakeTextureTile(), "repeat");
  if (texture) {
    waterCtx.globalAlpha = 0.72;
    waterCtx.fillStyle = texture;
    waterCtx.fillRect(0, 0, canvasWidth, canvasHeight);
    waterCtx.globalAlpha = 1;
  }

  waterCtx.globalCompositeOperation = "destination-in";
  waterCtx.drawImage(mask, 0, 0);
  g.drawImage(water, 0, 0);

  sprite = {
    canvas,
    x: group.minX - pad,
    y: group.minY - pad,
  };
  lakeGroupSpriteCache.set(key, sprite);
  while (lakeGroupSpriteCache.size > LAKE_GROUP_CACHE_LIMIT) {
    const oldestKey = lakeGroupSpriteCache.keys().next().value;
    lakeGroupSpriteCache.delete(oldestKey);
  }
  return sprite;
}

function drawMergedLakes(ctx, lakeSegments, visMinX, visMaxX, visMinY, visMaxY) {
  if (!lakeSegments.length) return;
  const groups = [];
  for (let i = 0; i < lakeSegments.length; i++) {
    const o = lakeSegments[i];
    const touchingGroups = [];
    for (let g = 0; g < groups.length; g++) {
      const touches = groups[g].segments.some((other) => {
        const dx = o.x - other.x;
        const dy = o.y - other.y;
        const reach = o.r + other.r + 2;
        return dx * dx + dy * dy <= reach * reach;
      });
      if (touches) touchingGroups.push(g);
    }

    let group;
    if (!touchingGroups.length) {
      group = {
        segments: [],
        minX: Infinity,
        minY: Infinity,
        maxX: -Infinity,
        maxY: -Infinity,
      };
      groups.push(group);
    } else {
      group = groups[touchingGroups[0]];
      for (let g = touchingGroups.length - 1; g >= 1; g--) {
        const merged = groups[touchingGroups[g]];
        group.segments.push(...merged.segments);
        group.minX = Math.min(group.minX, merged.minX);
        group.minY = Math.min(group.minY, merged.minY);
        group.maxX = Math.max(group.maxX, merged.maxX);
        group.maxY = Math.max(group.maxY, merged.maxY);
        groups.splice(touchingGroups[g], 1);
      }
    }
    group.segments.push(o);
    group.minX = Math.min(group.minX, o.x - o.r);
    group.minY = Math.min(group.minY, o.y - o.r);
    group.maxX = Math.max(group.maxX, o.x + o.r);
    group.maxY = Math.max(group.maxY, o.y + o.r);
  }

  for (const group of groups) {
    if (
      group.maxX < visMinX || group.minX > visMaxX
      || group.maxY < visMinY || group.minY > visMaxY
    ) continue;

    const sprite = getLakeGroupSprite(group);
    ctx.drawImage(sprite.canvas, sprite.x, sprite.y);
  }
}

const FOREST_CROWNS = [
  [[0, -0.2, 0.58], [-0.38, 0.14, 0.52], [0.38, 0.14, 0.52]],
  [[-0.3, -0.18, 0.5], [0.28, -0.2, 0.54], [-0.26, 0.28, 0.48], [0.3, 0.3, 0.46]],
  [[0, -0.34, 0.48], [-0.4, 0, 0.48], [0.4, 0.02, 0.48], [0, 0.34, 0.5]],
  [[0, -0.18, 0.6], [-0.42, 0.2, 0.44], [0.42, 0.2, 0.44], [0, 0.4, 0.42]],
];

function drawForestObstacle(ctx, cx, cy, r, variant, damageStage) {
  ctx.fillStyle = COLORS.forestGround;
  ctx.beginPath();
  ctx.arc(cx, cy, r, 0, TAU);
  ctx.fill();

  ctx.fillStyle = COLORS.forestTrunk;
  ctx.fillRect(cx - r * 0.13, cy - r * 0.12, r * 0.26, r * 0.72);
  ctx.beginPath();
  ctx.moveTo(cx, cy + r * 0.06);
  ctx.lineTo(cx - r * 0.34, cy - r * 0.16);
  ctx.moveTo(cx, cy + r * 0.12);
  ctx.lineTo(cx + r * 0.34, cy - r * 0.12);
  ctx.strokeStyle = COLORS.forestTrunk;
  ctx.lineWidth = r * 0.1;
  ctx.lineCap = "round";
  ctx.stroke();

  ctx.globalAlpha = 1 - damageStage * 0.12;
  ctx.fillStyle = COLORS.forest;
  const crown = FOREST_CROWNS[variant];
  for (let i = 0; i < crown.length; i++) {
    const lobe = crown[i];
    ctx.beginPath();
    ctx.arc(cx + lobe[0] * r, cy + lobe[1] * r, lobe[2] * r, 0, TAU);
    ctx.fill();
  }
  ctx.globalAlpha = 1;

  ctx.strokeStyle = COLORS.forestShade;
  ctx.lineWidth = 1.4;
  ctx.beginPath();
  ctx.arc(cx, cy, r * 0.98, 0, TAU);
  ctx.stroke();

  if (damageStage >= 2) {
    ctx.fillStyle = COLORS.forestShade;
    ctx.beginPath();
    ctx.arc(
      cx + (variant % 2 ? -1 : 1) * r * 0.36,
      cy - r * 0.16,
      r * (0.12 + damageStage * 0.025),
      0,
      TAU
    );
    ctx.fill();
  }
  drawObstacleDamage(ctx, cx, cy, r, damageStage, variant);
}

function traceRock(ctx, cx, cy, r, variant, inset = 1) {
  const sides = 6 + (variant % 3);
  ctx.beginPath();
  for (let i = 0; i < sides; i++) {
    const a = -Math.PI * 0.5 + i * TAU / sides;
    const wobble = 0.86 + 0.1 * (0.5 + 0.5 * Math.sin((i + 1) * (variant + 2) * 1.73));
    const rr = r * wobble * inset;
    const x = cx + Math.cos(a) * rr;
    const y = cy + Math.sin(a) * rr;
    if (i === 0) ctx.moveTo(x, y);
    else ctx.lineTo(x, y);
  }
  ctx.closePath();
}

function drawRockObstacle(ctx, cx, cy, r, variant, damageStage) {
  ctx.fillStyle = COLORS.rockShadow;
  ctx.beginPath();
  ctx.arc(cx + r * 0.08, cy + r * 0.12, r * 0.96, 0, TAU);
  ctx.fill();

  ctx.globalAlpha = 1 - damageStage * 0.1;
  ctx.fillStyle = COLORS.rock;
  traceRock(ctx, cx, cy - r * 0.04, r, variant);
  ctx.fill();
  ctx.globalAlpha = 1;
  ctx.strokeStyle = "rgba(230,235,245,0.22)";
  ctx.lineWidth = 1.5;
  ctx.stroke();

  ctx.fillStyle = COLORS.rockFacet;
  ctx.beginPath();
  ctx.moveTo(cx - r * 0.46, cy - r * 0.12);
  ctx.lineTo(cx - r * 0.05, cy - r * 0.66);
  ctx.lineTo(cx + r * 0.3, cy - r * 0.18);
  ctx.lineTo(cx + r * 0.08, cy + r * 0.08);
  ctx.closePath();
  ctx.fill();

  ctx.strokeStyle = COLORS.rockShadow;
  ctx.lineWidth = r * 0.1;
  ctx.lineCap = "round";
  ctx.beginPath();
  ctx.moveTo(cx - r * 0.28, cy + r * 0.54);
  ctx.lineTo(cx + r * 0.38, cy + r * 0.48);
  ctx.stroke();
  drawObstacleDamage(ctx, cx, cy, r, damageStage, variant);
}

function getObstacleSprite(type, variant, damageStage, rotation) {
  const safeVariant = ((variant % 4) + 4) % 4;
  const safeDamage = clamp(damageStage | 0, 0, 3);
  const safeRotation = ((rotation % 8) + 8) % 8;
  const key = `${type}|${safeVariant}|${safeDamage}|${safeRotation}`;
  let sprite = obstacleSpriteCache.get(key);
  if (sprite) return sprite;

  const r = OBSTACLE_SPRITE_RADIUS;
  const pad = OBSTACLE_SPRITE_PAD;
  const size = r * 2 + pad * 2;
  const canvas = makeOffscreenCanvas(size, size);
  const g = canvas.getContext("2d");
  const cx = pad + r;
  const cy = pad + r;
  g.save();
  g.translate(cx, cy);
  g.rotate(safeRotation * TAU / 8);
  if (type === "forest") drawForestObstacle(g, 0, 0, r, safeVariant, safeDamage);
  else drawRockObstacle(g, 0, 0, r, safeVariant, safeDamage);
  g.restore();
  sprite = { canvas, r, pad };
  obstacleSpriteCache.set(key, sprite);
  return sprite;
}

function drawObstacleSprite(ctx, o) {
  const sprite = getObstacleSprite(
    o.type,
    obstacleVariant(o),
    obstacleDamageStage(o),
    obstacleRotation(o)
  );
  const scale = o.r / sprite.r;
  const drawSize = sprite.canvas.width * scale;
  const offset = (sprite.r + sprite.pad) * scale;
  ctx.drawImage(sprite.canvas, o.x - offset, o.y - offset, drawSize, drawSize);
}

function drawBackgroundBase(ctx, W, H) {
  ctx.fillStyle = COLORS.bg;
  ctx.fillRect(0, 0, W, H);
}

function drawGridLayer(ctx, W, H, camX, camY, step, color, lineWidth) {
  const x0 = Math.floor(camX / step) * step;
  const y0 = Math.floor(camY / step) * step;

  ctx.save();
  ctx.strokeStyle = color;
  ctx.lineWidth = lineWidth;

  ctx.beginPath();
  for (let x=x0; x<camX+W+step; x+=step){ ctx.moveTo(x, camY); ctx.lineTo(x, camY+H); }
  for (let y=y0; y<camY+H+step; y+=step){ ctx.moveTo(camX, y); ctx.lineTo(camX+W, y); }
  ctx.stroke();
  ctx.restore();
}

function drawWorldGrid(ctx, W, H, camX, camY) {
  drawGridLayer(ctx, W, H, camX, camY, 64, COLORS.gridMinor, 1);
  drawGridLayer(ctx, W, H, camX, camY, 256, COLORS.gridMajor, 1.15);
}

function drawBackgroundAccents(ctx, W, H, camX, camY) {
  const tile = 512;
  const tx0 = Math.floor(camX / tile) - 1;
  const tx1 = Math.ceil((camX + W) / tile) + 1;
  const ty0 = Math.floor(camY / tile) - 1;
  const ty1 = Math.ceil((camY + H) / tile) + 1;

  ctx.save();
  ctx.strokeStyle = COLORS.worldAccent;
  ctx.fillStyle = COLORS.worldAccent;
  ctx.lineWidth = 1;
  for (let ty = ty0; ty <= ty1; ty++) {
    for (let tx = tx0; tx <= tx1; tx++) {
      const hash = stableCellHash(tx, ty);
      if (hash % 5 !== 0) continue;
      const x = tx * tile + 96 + ((hash >>> 8) % 320);
      const y = ty * tile + 96 + ((hash >>> 17) % 320);
      const s = 7 + (hash % 7);
      ctx.beginPath();
      if (hash & 1) {
        ctx.moveTo(x - s, y);
        ctx.lineTo(x + s, y);
        ctx.moveTo(x, y - s);
        ctx.lineTo(x, y + s);
      } else {
        ctx.moveTo(x, y - s);
        ctx.lineTo(x + s, y);
        ctx.lineTo(x, y + s);
        ctx.lineTo(x - s, y);
        ctx.closePath();
      }
      ctx.stroke();
    }
  }
  ctx.restore();
}

function drawWorldBounds(ctx) {
  const bLeft = -WORLD.halfSize;
  const bTop = -WORLD.halfSize;
  const bSize = WORLD.halfSize * 2;
  ctx.save();
  ctx.globalAlpha = OPACITY.strong;
  ctx.strokeStyle = COLORS.worldBoundary;
  ctx.lineWidth = 6;
  ctx.shadowColor = COLORS.worldBoundary;
  ctx.shadowBlur = GLOW.high;
  ctx.strokeRect(bLeft, bTop, bSize, bSize);
  ctx.globalAlpha = OPACITY.soft;
  ctx.lineWidth = 4;
  ctx.shadowBlur = GLOW.low;
  ctx.strokeRect(bLeft + 10, bTop + 10, bSize - 20, bSize - 20);
  ctx.restore();
}

function neonCircle(ctx, x,y,r,fill,glow=GLOW.medium,alpha=1){
  ctx.save();
  ctx.globalAlpha = alpha;
  ctx.shadowColor = fill;
  ctx.shadowBlur = glow;
  ctx.fillStyle = fill;
  ctx.beginPath();
  ctx.arc(x,y,r,0,TAU);
  ctx.fill();
  ctx.shadowBlur = 0;
  ctx.strokeStyle = UI_COLORS.strokeDim;
  ctx.lineWidth = 1;
  ctx.stroke();
  ctx.restore();
}

function neonRing(ctx, x,y,r,stroke,glow=GLOW.medium,lw=2,alpha=1){
  ctx.save();
  ctx.globalAlpha = alpha;
  ctx.shadowColor = stroke;
  ctx.shadowBlur = glow;
  ctx.strokeStyle = stroke;
  ctx.lineWidth = lw;
  ctx.beginPath();
  ctx.arc(x,y,r,0,TAU);
  ctx.stroke();
  ctx.restore();
}

function neonRect(ctx, x,y,w,h,fill,glow=GLOW.medium){
  ctx.save();
  ctx.shadowColor = fill;
  ctx.shadowBlur = glow;
  ctx.fillStyle = fill;
  ctx.fillRect(x,y,w,h);
  ctx.shadowBlur = 0;
  ctx.strokeStyle = UI_COLORS.strokeDim;
  ctx.lineWidth = 1;
  ctx.strokeRect(x,y,w,h);
  ctx.restore();
}

function cacheDynamicSprite(cache, key, create) {
  let sprite = cache.get(key);
  if (sprite) return sprite;
  sprite = create();
  cache.set(key, sprite);
  while (cache.size > DYNAMIC_SPRITE_CACHE_LIMIT) {
    cache.delete(cache.keys().next().value);
  }
  return sprite;
}

function quantizeSpriteValue(value, quantum = 0.5) {
  return Math.max(quantum, Math.round(value / quantum) * quantum);
}

function getNeonCircleSprite(radius, color, glow = GLOW.medium, ringWidth = 0) {
  const r = quantizeSpriteValue(radius);
  const width = ringWidth > 0 ? quantizeSpriteValue(ringWidth, 0.25) : 0;
  const key = `${width > 0 ? "ring" : "circle"}|${r}|${color}|${glow}|${width}`;
  return cacheDynamicSprite(neonCircleSpriteCache, key, () => {
    const pad = Math.ceil(glow + width + 3);
    const size = Math.ceil((r + pad) * 2);
    const canvas = makeOffscreenCanvas(size, size);
    const g = canvas.getContext("2d");
    const center = size * 0.5;
    g.shadowColor = color;
    g.shadowBlur = glow;
    if (width > 0) {
      g.strokeStyle = color;
      g.lineWidth = width;
      g.beginPath();
      g.arc(center, center, r, 0, TAU);
      g.stroke();
    } else {
      g.fillStyle = color;
      g.beginPath();
      g.arc(center, center, r, 0, TAU);
      g.fill();
      g.shadowBlur = 0;
      g.strokeStyle = UI_COLORS.strokeDim;
      g.lineWidth = 1;
      g.stroke();
    }
    return { canvas, center };
  });
}

function drawCachedNeonCircle(ctx, x, y, radius, color, glow = GLOW.medium, alpha = 1) {
  const sprite = getNeonCircleSprite(radius, color, glow, 0);
  const previousAlpha = ctx.globalAlpha;
  ctx.globalAlpha = alpha;
  ctx.drawImage(sprite.canvas, x - sprite.center, y - sprite.center);
  ctx.globalAlpha = previousAlpha;
}

function drawCachedNeonRing(ctx, x, y, radius, color, glow = GLOW.medium, width = 2, alpha = 1) {
  const sprite = getNeonCircleSprite(radius, color, glow, width);
  const previousAlpha = ctx.globalAlpha;
  ctx.globalAlpha = alpha;
  ctx.drawImage(sprite.canvas, x - sprite.center, y - sprite.center);
  ctx.globalAlpha = previousAlpha;
}

function getSingularityOrbBodySprite(radius) {
  const r = quantizeSpriteValue(radius);
  const key = `singularity-body|${r}|${GLOW.medium}`;
  return cacheDynamicSprite(projectileSpriteCache, key, () => {
    const glow = GLOW.medium;
    const pad = Math.ceil(glow + r * 0.3 + 4);
    const size = Math.ceil((r + pad) * 2);
    const canvas = makeOffscreenCanvas(size, size);
    const g = canvas.getContext("2d");
    const center = size * 0.5;
    g.translate(center, center);

    const accretion = g.createRadialGradient(
      -r * 0.18,
      -r * 0.2,
      r * 0.08,
      0,
      0,
      r
    );
    accretion.addColorStop(0, "rgba(215,246,255,.98)");
    accretion.addColorStop(0.16, "rgba(37,240,255,.92)");
    accretion.addColorStop(0.42, "rgba(177,96,255,.9)");
    accretion.addColorStop(0.72, "rgba(73,24,126,.72)");
    accretion.addColorStop(1, "rgba(10,3,24,0)");
    g.shadowColor = COLORS.bullet;
    g.shadowBlur = glow;
    g.fillStyle = accretion;
    g.beginPath();
    g.arc(0, 0, r, 0, TAU);
    g.fill();

    g.lineCap = "round";
    g.lineWidth = Math.max(1.1, r * 0.11);
    for (let i = 0; i < 3; i++) {
      const start = i * TAU / 3 - 0.42;
      g.strokeStyle = i === 1 ? COLORS.arc : COLORS.bullet;
      g.beginPath();
      g.arc(0, 0, r * 0.66, start, start + 0.86);
      g.stroke();
    }

    g.shadowBlur = 0;
    g.fillStyle = "#03030a";
    g.beginPath();
    g.arc(0, 0, r * 0.34, 0, TAU);
    g.fill();
    g.strokeStyle = "rgba(124,246,255,.96)";
    g.lineWidth = Math.max(1, r * 0.08);
    g.beginPath();
    g.arc(0, 0, r * 0.39, 0, TAU);
    g.stroke();

    const core = g.createRadialGradient(
      -r * 0.08,
      -r * 0.1,
      0,
      0,
      0,
      r * 0.3
    );
    core.addColorStop(0, "rgba(14,10,28,.96)");
    core.addColorStop(0.52, "rgba(3,3,10,1)");
    core.addColorStop(1, "rgba(0,0,3,1)");
    g.fillStyle = core;
    g.beginPath();
    g.arc(0, 0, r * 0.29, 0, TAU);
    g.fill();

    return { canvas, center };
  });
}

function getSingularityOrbFieldSprite(radius) {
  const r = quantizeSpriteValue(radius);
  const key = `singularity-field|${r}|${GLOW.high}`;
  return cacheDynamicSprite(projectileSpriteCache, key, () => {
    const glow = GLOW.high;
    const pad = Math.ceil(glow + 5);
    const size = Math.ceil((r + pad) * 2);
    const canvas = makeOffscreenCanvas(size, size);
    const g = canvas.getContext("2d");
    const center = size * 0.5;
    g.translate(center, center);
    g.lineCap = "round";

    g.shadowColor = COLORS.bullet;
    g.shadowBlur = glow;
    g.strokeStyle = COLORS.bullet;
    g.lineWidth = 1.8;
    for (let i = 0; i < 3; i++) {
      const start = i * TAU / 3 + 0.12;
      g.beginPath();
      g.arc(0, 0, r, start, start + 1.28);
      g.stroke();
    }

    g.shadowColor = COLORS.arc;
    g.shadowBlur = GLOW.medium;
    g.strokeStyle = "rgba(124,246,255,.78)";
    g.lineWidth = 1.2;
    for (let i = 0; i < 3; i++) {
      const start = i * TAU / 3 + 0.78;
      g.beginPath();
      g.arc(0, 0, r * 0.72, start, start + 0.78);
      g.stroke();
    }

    g.shadowBlur = 0;
    g.fillStyle = COLORS.arc;
    for (let i = 0; i < 3; i++) {
      const a = i * TAU / 3 + 0.12;
      const x = Math.cos(a) * r;
      const y = Math.sin(a) * r;
      const marker = Math.max(1.5, Math.min(3, r * 0.025));
      g.save();
      g.translate(x, y);
      g.rotate(a);
      g.beginPath();
      g.moveTo(marker, 0);
      g.lineTo(0, marker * 0.58);
      g.lineTo(-marker, 0);
      g.lineTo(0, -marker * 0.58);
      g.closePath();
      g.fill();
      g.restore();
    }

    return { canvas, center };
  });
}

function getHolyAuraFieldSprite(radius) {
  const r = quantizeSpriteValue(radius, 1);
  const key = `holy-aura|${r}|${GLOW.medium}`;
  return cacheDynamicSprite(projectileSpriteCache, key, () => {
    const glow = GLOW.medium;
    const pad = Math.ceil(glow + 5);
    const size = Math.ceil((r + pad) * 2);
    const canvas = makeOffscreenCanvas(size, size);
    const g = canvas.getContext("2d");
    const center = size * 0.5;
    g.translate(center, center);

    const field = g.createRadialGradient(0, 0, r * 0.08, 0, 0, r);
    field.addColorStop(0, "rgba(70,255,143,.01)");
    field.addColorStop(0.55, "rgba(70,255,143,.025)");
    field.addColorStop(0.84, "rgba(37,240,255,.018)");
    field.addColorStop(1, "rgba(70,255,143,.075)");
    g.fillStyle = field;
    g.beginPath();
    g.arc(0, 0, r, 0, TAU);
    g.fill();

    g.lineCap = "round";
    g.shadowColor = COLORS.heal;
    g.shadowBlur = glow;
    g.strokeStyle = "rgba(70,255,143,.58)";
    g.lineWidth = 1.8;
    g.beginPath();
    g.arc(0, 0, r * 0.96, 0, TAU);
    g.stroke();

    g.shadowColor = COLORS.arc;
    g.shadowBlur = GLOW.low;
    g.strokeStyle = "rgba(124,246,255,.2)";
    g.lineWidth = 1;
    g.beginPath();
    g.arc(0, 0, r * 0.68, 0, TAU);
    g.stroke();

    g.shadowBlur = 0;
    g.strokeStyle = "rgba(215,255,236,.42)";
    g.lineWidth = 1.2;
    for (let i = 0; i < 4; i++) {
      const a = i * TAU / 4;
      g.beginPath();
      g.moveTo(Math.cos(a) * r * 0.82, Math.sin(a) * r * 0.82);
      g.lineTo(Math.cos(a) * r * 0.93, Math.sin(a) * r * 0.93);
      g.stroke();
    }

    return { canvas, center };
  });
}

function getProjectileSprite(kind, radius, color) {
  const r = quantizeSpriteValue(radius);
  const key = `${kind}|${r}|${color}|${GLOW.medium}`;
  return cacheDynamicSprite(projectileSpriteCache, key, () => {
    const glow = kind === "missile" ? GLOW.low : GLOW.medium;
    const extent = kind === "magic"
      ? r * 3.4
      : kind === "rail"
        ? r * 3.2
        : kind === "axe"
          ? r * 2.45
          : r * 2.8;
    const pad = Math.ceil(glow + 4);
    const size = Math.ceil((extent + pad) * 2);
    const canvas = makeOffscreenCanvas(size, size);
    const g = canvas.getContext("2d");
    const center = size * 0.5;
    g.save();
    g.translate(center, center);
    g.shadowColor = color;
    g.shadowBlur = glow;
    if (kind === "magic") {
      const body = g.createLinearGradient(-r * 2.8, 0, r * 2.3, 0);
      body.addColorStop(0, "rgba(177,96,255,0)");
      body.addColorStop(0.28, "rgba(177,96,255,.62)");
      body.addColorStop(0.7, "rgba(37,240,255,.92)");
      body.addColorStop(1, "rgba(233,252,255,1)");
      g.fillStyle = body;
      g.beginPath();
      g.moveTo(-r * 2.9, 0);
      g.lineTo(-r * 0.25, -r * 0.82);
      g.lineTo(r * 2.35, 0);
      g.lineTo(-r * 0.25, r * 0.82);
      g.closePath();
      g.fill();

      g.shadowBlur = 0;
      g.fillStyle = "rgba(235,253,255,.96)";
      g.beginPath();
      g.moveTo(-r * 0.45, 0);
      g.lineTo(r * 0.35, -r * 0.3);
      g.lineTo(r * 1.78, 0);
      g.lineTo(r * 0.35, r * 0.3);
      g.closePath();
      g.fill();
      g.strokeStyle = "rgba(177,96,255,.78)";
      g.lineWidth = Math.max(0.8, r * 0.24);
      g.beginPath();
      g.moveTo(-r * 2.15, -r * 0.62);
      g.lineTo(-r * 0.65, 0);
      g.lineTo(-r * 2.15, r * 0.62);
      g.stroke();
    } else if (kind === "rail") {
      const beam = g.createLinearGradient(-r * 2.75, 0, r * 2.65, 0);
      beam.addColorStop(0, "rgba(177,96,255,0)");
      beam.addColorStop(0.25, "rgba(177,96,255,.62)");
      beam.addColorStop(0.68, "rgba(154,194,255,.96)");
      beam.addColorStop(1, "rgba(239,253,255,1)");
      g.fillStyle = beam;
      g.beginPath();
      g.moveTo(-r * 2.8, 0);
      g.lineTo(-r * 1.2, -r * 0.62);
      g.lineTo(r * 1.55, -r * 0.5);
      g.lineTo(r * 2.65, 0);
      g.lineTo(r * 1.55, r * 0.5);
      g.lineTo(-r * 1.2, r * 0.62);
      g.closePath();
      g.fill();

      g.shadowBlur = 0;
      g.strokeStyle = "rgba(215,250,255,.96)";
      g.lineWidth = Math.max(1, r * 0.28);
      g.beginPath();
      g.moveTo(-r * 1.5, 0);
      g.lineTo(r * 2.1, 0);
      g.stroke();
      g.strokeStyle = "rgba(37,240,255,.86)";
      g.lineWidth = Math.max(0.8, r * 0.18);
      for (const side of [-1, 1]) {
        g.beginPath();
        g.moveTo(-r * 1.15, side * r * 0.65);
        g.lineTo(-r * 0.25, side * r * 0.18);
        g.lineTo(r * 0.75, side * r * 0.46);
        g.stroke();
      }
    } else if (kind === "axe") {
      const blade = g.createLinearGradient(-r * 1.8, 0, r * 1.8, 0);
      blade.addColorStop(0, "rgba(177,96,255,.96)");
      blade.addColorStop(0.5, "rgba(215,246,255,.9)");
      blade.addColorStop(1, "rgba(37,240,255,.96)");
      g.fillStyle = blade;
      g.beginPath();
      g.moveTo(-r * 0.18, -r * 1.46);
      g.lineTo(-r * 0.85, -r * 1.6);
      g.lineTo(-r * 1.78, -r * 0.98);
      g.lineTo(-r * 1.35, -r * 0.24);
      g.lineTo(-r * 0.2, -r * 0.5);
      g.lineTo(r * 0.2, -r * 0.5);
      g.lineTo(r * 1.35, -r * 0.24);
      g.lineTo(r * 1.78, -r * 0.98);
      g.lineTo(r * 0.85, -r * 1.6);
      g.lineTo(r * 0.18, -r * 1.46);
      g.closePath();
      g.fill();

      g.shadowBlur = 0;
      g.fillStyle = "rgba(12,17,28,.96)";
      g.fillRect(-r * 0.22, -r * 1.42, r * 0.44, r * 2.82);
      g.strokeStyle = UI_COLORS.axeEdge;
      g.lineWidth = Math.max(0.8, r * 0.13);
      g.strokeRect(-r * 0.22, -r * 1.42, r * 0.44, r * 2.82);
      g.fillStyle = UI_COLORS.axeBody;
      g.beginPath();
      g.arc(0, -r * 0.72, r * 0.3, 0, TAU);
      g.fill();
      g.fillStyle = UI_COLORS.axeEdge;
      g.beginPath();
      g.moveTo(0, r * 1.7);
      g.lineTo(-r * 0.38, r * 1.28);
      g.lineTo(0, r * 0.96);
      g.lineTo(r * 0.38, r * 1.28);
      g.closePath();
      g.fill();
    } else if (kind === "missile") {
      const flame = g.createLinearGradient(-r * 2.7, 0, -r * 0.9, 0);
      flame.addColorStop(0, "rgba(177,96,255,0)");
      flame.addColorStop(0.45, "rgba(177,96,255,.72)");
      flame.addColorStop(1, "rgba(255,217,74,.95)");
      g.fillStyle = flame;
      g.beginPath();
      g.moveTo(-r * 2.65, 0);
      g.lineTo(-r * 1.05, -r * 0.48);
      g.lineTo(-r * 0.72, 0);
      g.lineTo(-r * 1.05, r * 0.48);
      g.closePath();
      g.fill();

      const hull = g.createLinearGradient(-r * 1.2, -r * 0.7, r * 2.15, r * 0.55);
      hull.addColorStop(0, "rgba(117,50,22,.96)");
      hull.addColorStop(0.42, COLORS.missile);
      hull.addColorStop(0.78, "rgba(255,217,74,.98)");
      hull.addColorStop(1, "rgba(237,252,255,.98)");
      g.fillStyle = hull;
      g.beginPath();
      g.moveTo(-r * 1.25, -r * 0.62);
      g.lineTo(r * 1.2, -r * 0.58);
      g.lineTo(r * 2.2, 0);
      g.lineTo(r * 1.2, r * 0.58);
      g.lineTo(-r * 1.25, r * 0.62);
      g.closePath();
      g.fill();

      g.fillStyle = "rgba(177,96,255,.88)";
      for (const side of [-1, 1]) {
        g.beginPath();
        g.moveTo(-r * 0.85, side * r * 0.42);
        g.lineTo(-r * 1.55, side * r * 1.12);
        g.lineTo(r * 0.15, side * r * 0.58);
        g.closePath();
        g.fill();
      }
      g.shadowBlur = 0;
      g.fillStyle = "rgba(215,253,255,.94)";
      g.fillRect(-r * 0.15, -r * 0.12, r * 1.55, r * 0.24);
      g.strokeStyle = COLORS.missileStroke;
      g.lineWidth = 1;
      g.beginPath();
      g.moveTo(-r * 1.25, -r * 0.62);
      g.lineTo(r * 1.2, -r * 0.58);
      g.lineTo(r * 2.2, 0);
      g.lineTo(r * 1.2, r * 0.58);
      g.lineTo(-r * 1.25, r * 0.62);
      g.closePath();
      g.stroke();
    } else {
      const length = kind === "missile" ? r * 3 : r * 3.2;
      const height = kind === "missile" ? r : r * 1.2;
      g.fillStyle = color;
      g.strokeStyle = kind === "missile" ? COLORS.missileStroke : UI_COLORS.strokeDim;
      g.lineWidth = kind === "missile" ? 1.4 : 1;
      if (kind === "missile") {
        g.beginPath();
        g.roundRect(-length * 0.5, -height * 0.6, length, height * 1.2, height * 0.6);
        g.fill();
        g.stroke();
      } else {
        g.fillRect(-length * 0.5, -height * 0.5, length, height);
        g.shadowBlur = 0;
        g.strokeRect(-length * 0.5, -height * 0.5, length, height);
      }
    }
    g.restore();
    return { canvas, center };
  });
}

function getParticleSprite(shape, radius, stretch, color) {
  const r = quantizeSpriteValue(radius);
  const safeStretch = quantizeSpriteValue(Math.max(1, stretch || 1), 0.25);
  const key = `${shape}|${r}|${safeStretch}|${color}|${GLOW.low}`;
  return cacheDynamicSprite(particleSpriteCache, key, () => {
    const glow = GLOW.low;
    const length = r * safeStretch;
    const pad = Math.ceil(glow + 4);
    const halfWidth = shape === "dot" ? r : length;
    const halfHeight = shape === "shard" ? r * 0.72 : r;
    const canvasWidth = Math.ceil((halfWidth + pad) * 2);
    const canvasHeight = Math.ceil((halfHeight + pad) * 2);
    const canvas = makeOffscreenCanvas(canvasWidth, canvasHeight);
    const g = canvas.getContext("2d");
    const centerX = canvasWidth * 0.5;
    const centerY = canvasHeight * 0.5;
    g.translate(centerX, centerY);
    g.fillStyle = color;
    g.strokeStyle = color;
    g.shadowColor = color;
    g.shadowBlur = glow;
    if (shape === "streak") {
      g.lineCap = "round";
      g.lineWidth = Math.max(1, r * 0.9);
      g.beginPath();
      g.moveTo(-length, 0);
      g.lineTo(length * 0.24, 0);
      g.stroke();
    } else if (shape === "shard") {
      const width = r * 0.72;
      g.beginPath();
      g.moveTo(length, 0);
      g.lineTo(0, width);
      g.lineTo(-length * 0.48, 0);
      g.lineTo(0, -width);
      g.closePath();
      g.fill();
    } else {
      g.beginPath();
      g.arc(0, 0, r, 0, TAU);
      g.fill();
    }
    return { canvas, centerX, centerY };
  });
}

function drawQuestMark(ctx, x, y, size, color) {
  ctx.save();
  ctx.strokeStyle = color;
  ctx.fillStyle = color;
  ctx.lineWidth = Math.max(1.4, size * 0.22);
  ctx.lineCap = "round";
  ctx.beginPath();
  ctx.moveTo(x, y - size * 0.72);
  ctx.lineTo(x, y + size * 0.18);
  ctx.stroke();
  ctx.beginPath();
  ctx.arc(x, y + size * 0.62, Math.max(1, size * 0.13), 0, TAU);
  ctx.fill();
  ctx.restore();
}

function chestVisual(kind) {
  if (kind === "trinket") return { color: COLORS.trinket, fill: UI_COLORS.trinketFill };
  if (kind === "aug") return { color: COLORS.aug, fill: UI_COLORS.augFill };
  if (kind === "companion") return { color: COLORS.companionCage, fill: UI_COLORS.companionFill };
  return { color: COLORS.chest, fill: UI_COLORS.chestFill };
}

function drawChestGlyph(ctx, kind, x, y, r, color) {
  ctx.save();
  ctx.translate(x, y);
  ctx.strokeStyle = color;
  ctx.fillStyle = color;
  ctx.lineWidth = Math.max(1.5, r * 0.16);
  ctx.lineCap = "round";
  ctx.lineJoin = "round";

  if (kind === "bonus") {
    const arm = r * 0.56;
    ctx.beginPath();
    ctx.moveTo(-arm, 0);
    ctx.lineTo(arm, 0);
    ctx.moveTo(0, -arm);
    ctx.lineTo(0, arm);
    ctx.stroke();
  } else if (kind === "trinket") {
    tracePoints(ctx, 0, 0, r * 0.68, [
      0, -1,
      0.76, 0,
      0, 1,
      -0.76, 0,
    ]);
    ctx.stroke();
    ctx.beginPath();
    ctx.arc(0, 0, r * 0.16, 0, TAU);
    ctx.fill();
  } else if (kind === "aug") {
    const orbit = r * 0.52;
    ctx.beginPath();
    for (let i=0;i<3;i++) {
      const a = -Math.PI * 0.5 + i * TAU / 3;
      const px = Math.cos(a) * orbit;
      const py = Math.sin(a) * orbit;
      if (i === 0) ctx.moveTo(px, py);
      else ctx.lineTo(px, py);
    }
    ctx.closePath();
    ctx.stroke();
    for (let i=0;i<3;i++) {
      const a = -Math.PI * 0.5 + i * TAU / 3;
      ctx.beginPath();
      ctx.arc(Math.cos(a) * orbit, Math.sin(a) * orbit, r * 0.13, 0, TAU);
      ctx.fill();
    }
  } else {
    ctx.beginPath();
    ctx.arc(0, -r * 0.2, r * 0.22, 0, TAU);
    ctx.fill();
    ctx.beginPath();
    ctx.arc(-r * 0.42, r * 0.3, r * 0.17, 0, TAU);
    ctx.arc(r * 0.42, r * 0.3, r * 0.17, 0, TAU);
    ctx.fill();
    ctx.beginPath();
    ctx.moveTo(-r * 0.42, r * 0.3);
    ctx.lineTo(0, -r * 0.2);
    ctx.lineTo(r * 0.42, r * 0.3);
    ctx.stroke();
  }
  ctx.restore();
}

function drawChestBody(ctx, c, pulse) {
  const visual = chestVisual(c.kind);
  const color = visual.color;
  const rr = c.r * (1.0 + 0.05 * Math.sin(c.pulse * 1.7));

  ctx.save();
  ctx.shadowColor = color;
  ctx.shadowBlur = GLOW.high;
  ctx.fillStyle = visual.fill;
  ctx.strokeStyle = color;
  ctx.lineWidth = 2;
  ctx.fillRect(c.x - rr, c.y - rr, rr * 2, rr * 2);
  ctx.strokeRect(c.x - rr, c.y - rr, rr * 2, rr * 2);
  ctx.shadowBlur = 0;
  ctx.globalAlpha = 0.8;
  ctx.beginPath();
  ctx.moveTo(c.x - rr, c.y - rr * 0.35);
  ctx.lineTo(c.x + rr, c.y - rr * 0.35);
  ctx.stroke();
  ctx.restore();

  drawChestGlyph(ctx, c.kind, c.x, c.y + rr * 0.14, rr * 0.68, color);

  if (c.kind === "bonus") {
    neonRing(ctx, c.x, c.y, rr * 1.55, color, GLOW.medium, 2, 0.72 * pulse);
  } else if (c.kind === "trinket") {
    neonRing(ctx, c.x, c.y, rr * 1.45, color, GLOW.medium, 2, 0.78 * pulse);
    neonRing(ctx, c.x, c.y, rr * 1.92, color, GLOW.low, 1.4, 0.42 * pulse);
  } else if (c.kind === "aug") {
    ctx.save();
    ctx.strokeStyle = color;
    ctx.shadowColor = color;
    ctx.shadowBlur = GLOW.medium;
    ctx.lineWidth = 2;
    ctx.globalAlpha = 0.7 * pulse;
    ctx.setLineDash([rr * 0.54, rr * 0.32]);
    ctx.beginPath();
    ctx.arc(c.x, c.y, rr * 1.7, 0, TAU);
    ctx.stroke();
    ctx.restore();
  } else {
    neonRing(ctx, c.x, c.y, rr * 1.64, color, GLOW.medium, 1.5, 0.56 * pulse);
    ctx.save();
    ctx.fillStyle = color;
    ctx.shadowColor = color;
    ctx.shadowBlur = GLOW.low;
    ctx.globalAlpha = 0.76 * pulse;
    for (let i=0;i<4;i++) {
      const a = Math.PI * 0.25 + i * Math.PI * 0.5;
      ctx.fillRect(
        c.x + Math.cos(a) * rr * 1.62 - 1.5,
        c.y + Math.sin(a) * rr * 1.62 - 1.5,
        3,
        3
      );
    }
    ctx.restore();
  }
}

function drawPlayerGlyph(ctx, x, y, r) {
  ctx.save();
  ctx.shadowColor = COLORS.player;
  ctx.shadowBlur = GLOW.low;
  ctx.fillStyle = COLORS.bg;
  ctx.beginPath();
  ctx.arc(x, y, r * 0.76, 0, TAU);
  ctx.fill();

  ctx.fillStyle = COLORS.player;
  tracePoints(ctx, x, y, r * 0.9, [
    0, -1,
    0.2, -0.2,
    1, 0,
    0.2, 0.2,
    0, 1,
    -0.2, 0.2,
    -1, 0,
    -0.2, -0.2,
  ]);
  ctx.fill();
  ctx.shadowBlur = 0;
  ctx.strokeStyle = UI_COLORS.enemyDetail;
  ctx.lineWidth = 1.4;
  ctx.stroke();
  ctx.fillStyle = UI_COLORS.enemyDetail;
  tracePolygon(ctx, x, y, r * 0.25, 4, Math.PI * 0.25);
  ctx.fill();
  ctx.restore();
}

function drawCompanionGlyph(ctx, c) {
  const r = c.r;
  const glyph = c.visualGlyph || c.id || "cross";
  ctx.save();
  ctx.translate(c.x, c.y);
  ctx.shadowColor = c.color;
  ctx.shadowBlur = GLOW.medium;
  ctx.fillStyle = c.color;
  ctx.strokeStyle = c.color;
  ctx.lineWidth = Math.max(1.2, r * 0.25);
  ctx.lineJoin = "round";
  ctx.lineCap = "round";

  if (glyph === "moth") {
    ctx.beginPath();
    ctx.moveTo(0, -r * 0.2);
    ctx.lineTo(-r * 0.95, -r * 0.72);
    ctx.lineTo(-r * 0.68, r * 0.72);
    ctx.lineTo(0, r * 0.24);
    ctx.lineTo(r * 0.68, r * 0.72);
    ctx.lineTo(r * 0.95, -r * 0.72);
    ctx.closePath();
    ctx.fill();
    ctx.shadowBlur = 0;
    ctx.strokeStyle = UI_COLORS.enemyOutline;
    ctx.lineWidth = 1;
    ctx.stroke();
    ctx.fillStyle = UI_COLORS.questFill;
    ctx.fillRect(-r * 0.12, -r * 0.74, r * 0.24, r * 1.48);
  } else if (glyph === "triad") {
    const orbit = r * 0.58;
    ctx.beginPath();
    for (let i=0;i<3;i++) {
      const a = -Math.PI * 0.5 + i * TAU / 3;
      const px = Math.cos(a) * orbit;
      const py = Math.sin(a) * orbit;
      if (i === 0) ctx.moveTo(px, py);
      else ctx.lineTo(px, py);
    }
    ctx.closePath();
    ctx.stroke();
    for (let i=0;i<3;i++) {
      const a = -Math.PI * 0.5 + i * TAU / 3;
      ctx.beginPath();
      ctx.arc(Math.cos(a) * orbit, Math.sin(a) * orbit, r * 0.22, 0, TAU);
      ctx.fill();
    }
  } else if (glyph === "pixels") {
    const s = r * 0.6;
    ctx.fillRect(-r * 0.86, -r * 0.86, s, s);
    ctx.fillRect(r * 0.26, -r * 0.86, s, s);
    ctx.fillRect(-r * 0.86, r * 0.26, s, s);
    ctx.fillRect(r * 0.26, r * 0.26, s, s);
    ctx.shadowBlur = 0;
    ctx.strokeStyle = UI_COLORS.enemyOutline;
    ctx.lineWidth = 1;
    ctx.strokeRect(-r * 0.86, -r * 0.86, s, s);
    ctx.strokeRect(r * 0.26, -r * 0.86, s, s);
    ctx.strokeRect(-r * 0.86, r * 0.26, s, s);
    ctx.strokeRect(r * 0.26, r * 0.26, s, s);
  } else if (glyph === "aegis") {
    tracePolygon(ctx, 0, 0, r * 0.96, 6, Math.PI / 6);
    ctx.stroke();
    tracePolygon(ctx, 0, 0, r * 0.54, 6, Math.PI / 6);
    ctx.fill();
    ctx.shadowBlur = 0;
    ctx.strokeStyle = UI_COLORS.enemyOutline;
    ctx.lineWidth = 1;
    ctx.stroke();
  } else {
    tracePoints(ctx, 0, 0, r, [
      0, -1,
      0.24, -0.24,
      1, 0,
      0.24, 0.24,
      0, 1,
      -0.24, 0.24,
      -1, 0,
      -0.24, -0.24,
    ]);
    ctx.fill();
    ctx.shadowBlur = 0;
    ctx.strokeStyle = UI_COLORS.enemyOutline;
    ctx.lineWidth = 1;
    ctx.stroke();
  }
  ctx.restore();
}

function drawEnemyShot(ctx, s) {
  if (s.homing) {
    const ang = Math.atan2(s.vy, s.vx);
    const sprite = getProjectileSprite("enemy-homing", s.r, s.color);
    ctx.save();
    ctx.translate(s.x, s.y);
    ctx.rotate(ang);
    ctx.drawImage(sprite.canvas, -sprite.center, -sprite.center);
    ctx.restore();
    return;
  }
  drawCachedNeonCircle(ctx, s.x, s.y, s.r, s.color, GLOW.medium);
}

function drawTextWorld(ctx, x,y,text,color,size,alpha){
  ctx.save();
  ctx.globalAlpha = alpha;
  ctx.font = `700 ${size}px ui-sans-serif, system-ui, -apple-system, Segoe UI, Roboto, Arial`;
  ctx.textAlign = "center";
  ctx.textBaseline = "middle";
  ctx.shadowColor = color;
  ctx.shadowBlur = GLOW.medium;
  ctx.fillStyle = color;
  ctx.fillText(text, x, y);
  ctx.shadowBlur = 0;
  ctx.strokeStyle = UI_COLORS.textStroke;
  ctx.lineWidth = 3;
  ctx.strokeText(text, x, y);
  ctx.restore();
}

function drawDamageText(ctx, d, alpha) {
  if (!d.crit) {
    drawTextWorld(ctx, d.x, d.y, d.text, d.color, d.size, alpha);
    return;
  }
  const progress = 1 - alpha;
  const pop = 1 + Math.sin(Math.min(1, progress * 2.4) * Math.PI) * 0.24;
  ctx.save();
  ctx.translate(d.x, d.y);
  ctx.scale(pop, pop);
  ctx.globalAlpha = alpha;
  ctx.font = `850 ${d.size}px ui-sans-serif, system-ui, -apple-system, Segoe UI, Roboto, Arial`;
  ctx.textAlign = "center";
  ctx.textBaseline = "middle";
  ctx.strokeStyle = UI_COLORS.textStroke;
  ctx.lineWidth = 4;
  ctx.strokeText(d.text, 0, 0);
  ctx.shadowColor = d.color;
  ctx.shadowBlur = GLOW.high;
  ctx.fillStyle = d.color;
  ctx.fillText(d.text, 0, 0);
  ctx.restore();
}

function drawEnemyHitFeedback(ctx, e, sprite) {
  if (!(e.hitFlash > 0)) return;
  const life = Math.max(0.001, e.hitFlashMax || 0.085);
  const t = clamp(e.hitFlash / life, 0, 1);
  const progress = 1 - t;
  ctx.save();
  ctx.globalCompositeOperation = "lighter";
  ctx.globalAlpha = (e.hitCrit ? 0.62 : 0.34) * t;
  ctx.drawImage(sprite.canvas, e.x - sprite.r - sprite.pad, e.y - sprite.r - sprite.pad);
  ctx.restore();

  ctx.save();
  ctx.globalAlpha = t;
  ctx.strokeStyle = e.hitCrit ? COLORS.crit : COLORS.dmg;
  ctx.shadowColor = ctx.strokeStyle;
  ctx.shadowBlur = e.hitCrit ? GLOW.high : GLOW.low;
  ctx.lineCap = "round";
  if (!e.hitCrit) {
    ctx.lineWidth = 1.6;
    ctx.beginPath();
    ctx.arc(e.x, e.y, e.r * (1.04 + progress * 0.34), 0, TAU);
    ctx.stroke();
  } else {
    const base = Math.atan2(e.hitFxDy || 0, e.hitFxDx || 1);
    const inner = e.r * (0.72 + progress * 0.14);
    const outer = e.r * (1.42 + progress * 0.42);
    ctx.lineWidth = 2.4;
    ctx.beginPath();
    for (let i = 0; i < 4; i++) {
      const angle = base + i * Math.PI * 0.5;
      const dx = Math.cos(angle);
      const dy = Math.sin(angle);
      ctx.moveTo(e.x + dx * inner, e.y + dy * inner);
      ctx.lineTo(e.x + dx * outer, e.y + dy * outer);
    }
    ctx.stroke();
  }
  ctx.restore();
}

function drawCombatScreenFx(ctx, W, H, camX, camY) {
  const reduced = visualSettings.reducedMotion;
  if (combatFx.damageT > 0) {
    const t = clamp(combatFx.damageT / 0.18, 0, 1);
    const alpha = 0.2 * t * (combatFx.damageStrength || 0.6) * (reduced ? 0.28 : 1);
    const edge = reduced ? 6 : 18 + 18 * t;
    ctx.save();
    ctx.globalAlpha = alpha;
    ctx.fillStyle = COLORS.warnHit;
    ctx.fillRect(0, 0, W, edge);
    ctx.fillRect(0, H - edge, W, edge);
    ctx.fillRect(0, edge, edge, H - edge * 2);
    ctx.fillRect(W - edge, edge, edge, H - edge * 2);
    if (!reduced) {
      ctx.globalAlpha = alpha * 1.5;
      ctx.strokeStyle = COLORS.warnHit;
      ctx.lineWidth = 2;
      ctx.strokeRect(3, 3, W - 6, H - 6);
    }
    ctx.restore();
  }

  const px = player.x - camX;
  const py = player.y - camY;
  if (combatFx.blockT > 0) {
    const t = clamp(combatFx.blockT / 0.2, 0, 1);
    const progress = 1 - t;
    ctx.save();
    ctx.globalAlpha = (reduced ? 0.42 : 0.78) * t;
    ctx.strokeStyle = COLORS.shieldBlock;
    ctx.shadowColor = COLORS.shieldBlock;
    ctx.shadowBlur = GLOW.high;
    ctx.lineWidth = 3;
    ctx.beginPath();
    ctx.arc(px, py, player.r + 15 + progress * 34, 0, TAU);
    ctx.stroke();
    ctx.shadowBlur = 0;
    if (!reduced) {
      ctx.globalAlpha = 0.34 * t;
      ctx.lineWidth = 4;
      ctx.strokeRect(6, 6, W - 12, H - 12);
    }
    ctx.restore();
  }

  if (combatFx.healT > 0) {
    const t = clamp(combatFx.healT / 0.3, 0, 1);
    const progress = 1 - t;
    const strength = combatFx.healStrength || 0.5;
    ctx.save();
    ctx.globalAlpha = (reduced ? 0.4 : 0.62) * t * strength;
    ctx.strokeStyle = COLORS.heal;
    ctx.shadowColor = COLORS.heal;
    ctx.shadowBlur = GLOW.medium;
    ctx.lineWidth = 2.6;
    ctx.beginPath();
    ctx.arc(px, py, player.r + 22 + progress * 76, 0, TAU);
    ctx.stroke();
    ctx.shadowBlur = 0;
    if (!reduced) {
      ctx.globalAlpha = 0.16 * t * strength;
      ctx.lineWidth = 8;
      ctx.strokeRect(5, 5, W - 10, H - 10);
    }
    ctx.restore();
  }
}

function drawChestIndicators(ctx, W, H, camX, camY){
  const cx = W * 0.5, cy = H * 0.5;
  const margin = CHEST_CONFIG.indicatorMargin;
  const minX = margin, maxX = W - margin;
  const minY = margin, maxY = H - margin;
  const size = CHEST_CONFIG.indicatorSize;
  const pulse = 0.65 + 0.35 * Math.sin(visualTime() * 0.008);

  ctx.save();
  ctx.lineWidth = 2;
  ctx.shadowBlur = GLOW.medium;

  for (let i=0;i<chests.length;i++){
    const c = chests[i];
    if (!c.alive) continue;
    const visual = chestVisual(c.kind);
    const chestColor = visual.color;
    ctx.shadowColor = chestColor;
    ctx.strokeStyle = chestColor;
    ctx.fillStyle = visual.fill;
    const sx = c.x - camX, sy = c.y - camY;
    if (sx >= -c.r && sx <= W + c.r && sy >= -c.r && sy <= H + c.r) continue;

    const dx = sx - cx, dy = sy - cy;
    if (dx === 0 && dy === 0) continue;

    let tx = Infinity, ty = Infinity;
    if (dx > 0) tx = (maxX - cx) / dx; else if (dx < 0) tx = (minX - cx) / dx;
    if (dy > 0) ty = (maxY - cy) / dy; else if (dy < 0) ty = (minY - cy) / dy;
    let t = Math.min(tx, ty);
    if (!isFinite(t) || t <= 0) t = 1;

    const px = cx + dx * t;
    const py = cy + dy * t;
    const ang = Math.atan2(dy, dx);
    const scale = c.kind === "companion" ? 1.35 : 1;
    const s = size * pulse * scale;

    ctx.save();
    ctx.translate(px, py);
    ctx.rotate(ang);
    ctx.beginPath();
    ctx.moveTo(s * 1.35, 0);
    ctx.lineTo(s * 0.88, s * 0.3);
    ctx.lineTo(s * 0.88, -s * 0.3);
    ctx.closePath();
    ctx.fill();
    ctx.stroke();
    ctx.rotate(-ang);
    ctx.fillRect(-s * 0.72, -s * 0.72, s * 1.44, s * 1.44);
    ctx.strokeRect(-s * 0.72, -s * 0.72, s * 1.44, s * 1.44);
    drawChestGlyph(ctx, c.kind, 0, 0, s * 0.58, chestColor);
    ctx.restore();
  }

  ctx.restore();
}

function drawOffscreenIndicator(ctx, W, H, camX, camY, x, y, color, fill, sizeScale = 1, kind = "quest") {
  const cx = W * 0.5, cy = H * 0.5;
  const margin = CHEST_CONFIG.indicatorMargin;
  const minX = margin, maxX = W - margin;
  const minY = margin, maxY = H - margin;
  const size = CHEST_CONFIG.indicatorSize * sizeScale;
  const sx = x - camX, sy = y - camY;
  if (sx >= -size && sx <= W + size && sy >= -size && sy <= H + size) return;

  const dx = sx - cx, dy = sy - cy;
  if (dx === 0 && dy === 0) return;

  let tx = Infinity, ty = Infinity;
  if (dx > 0) tx = (maxX - cx) / dx; else if (dx < 0) tx = (minX - cx) / dx;
  if (dy > 0) ty = (maxY - cy) / dy; else if (dy < 0) ty = (minY - cy) / dy;
  let t = Math.min(tx, ty);
  if (!isFinite(t) || t <= 0) t = 1;

  const px = cx + dx * t;
  const py = cy + dy * t;
  const ang = Math.atan2(dy, dx);

  ctx.save();
  ctx.translate(px, py);
  ctx.rotate(ang);
  ctx.lineWidth = 2;
  ctx.shadowBlur = GLOW.medium;
  ctx.shadowColor = color;
  ctx.strokeStyle = color;
  ctx.fillStyle = fill || color;
  ctx.beginPath();
  ctx.moveTo(size * 1.42, 0);
  ctx.lineTo(size * 0.92, size * 0.32);
  ctx.lineTo(size * 0.92, -size * 0.32);
  ctx.closePath();
  ctx.fill();
  ctx.stroke();
  ctx.rotate(-ang);

  if (kind === "zone") {
    ctx.beginPath();
    ctx.arc(0, 0, size * 0.72, 0, TAU);
    ctx.fill();
    ctx.stroke();
    ctx.beginPath();
    ctx.arc(0, 0, size * 0.32, 0, TAU);
    ctx.moveTo(-size * 0.86, 0);
    ctx.lineTo(size * 0.86, 0);
    ctx.moveTo(0, -size * 0.86);
    ctx.lineTo(0, size * 0.86);
    ctx.stroke();
  } else if (kind === "item") {
    tracePolygon(ctx, 0, 0, size * 0.78, 6, Math.PI / 6);
    ctx.fill();
    ctx.stroke();
    drawQuestMark(ctx, 0, 0, size * 0.48, color);
  } else {
    tracePolygon(ctx, 0, 0, size * 0.8, 4, 0);
    ctx.fill();
    ctx.stroke();
    drawQuestMark(ctx, 0, 0, size * 0.48, color);
  }
  ctx.restore();
}

function drawQuestIndicators(ctx, W, H, camX, camY) {
  const pulse = 0.65 + 0.35 * Math.sin(visualTime() * 0.008);
  if (quest.giverActive && (!quest.active || quest.completed)) {
    const qColor = quest.completed ? COLORS.gold : COLORS.quest;
    drawOffscreenIndicator(ctx, W, H, camX, camY, quest.giverX, quest.giverY, qColor, UI_COLORS.questFill, 1.2 * pulse, "quest");
  }
  if (!quest.active || quest.completed) return;
  if (quest.type === "perfect_sweep") {
    drawOffscreenIndicator(ctx, W, H, camX, camY, quest.zoneX, quest.zoneY, COLORS.warn, UI_COLORS.questFill, 1.0 * pulse, "zone");
    return;
  }
  if (quest.type !== "scavenge" && quest.type !== "drop") return;
  for (let i = 0; i < questItems.length; i++) {
    const it = questItems[i];
    if (!it.alive) continue;
    const color = it.type === "drop" ? COLORS.gold : COLORS.quest;
    drawOffscreenIndicator(ctx, W, H, camX, camY, it.x, it.y, color, UI_COLORS.questFill, 0.9 * pulse, "item");
  }
}

function getTelegraphProgress(tg) {
  if (Number.isFinite(tg.progress)) return clamp(tg.progress, 0, 1);
  if (tg.max > 0) return clamp(1 - tg.t / tg.max, 0, 1);
  return 1;
}

function drawTelegraphElementMark(ctx, tg, scale, phase) {
  const x = tg.x;
  const y = tg.y;
  const element = tg.element || "";
  ctx.lineWidth = Math.max(1.4, scale * 0.08);
  ctx.setLineDash(NO_LINE_DASH);

  if (element === "poison") {
    for (let i = 0; i < 3; i++) {
      const a = phase * 0.15 + i * TAU / 3 - Math.PI * 0.5;
      ctx.beginPath();
      ctx.arc(
        x + Math.cos(a) * scale * 0.3,
        y + Math.sin(a) * scale * 0.3,
        scale * (0.13 + i * 0.025),
        0,
        TAU
      );
      ctx.stroke();
    }
    return;
  }

  if (element === "fire") {
    for (let i = -1; i <= 1; i++) {
      const ox = i * scale * 0.28;
      ctx.beginPath();
      ctx.moveTo(x + ox - scale * 0.12, y + scale * 0.28);
      ctx.lineTo(x + ox, y - scale * (0.28 + 0.06 * (i + 1)));
      ctx.lineTo(x + ox + scale * 0.12, y + scale * 0.28);
      ctx.stroke();
    }
    return;
  }

  if (element === "void") {
    ctx.setLineDash(TELEGRAPH_DASH_NARROW);
    ctx.beginPath();
    ctx.arc(x, y, scale * 0.36, phase * 0.25, phase * 0.25 + TAU);
    ctx.stroke();
    ctx.setLineDash(NO_LINE_DASH);
    ctx.beginPath();
    ctx.moveTo(x - scale * 0.34, y);
    ctx.lineTo(x + scale * 0.34, y);
    ctx.moveTo(x, y - scale * 0.34);
    ctx.lineTo(x, y + scale * 0.34);
    ctx.stroke();
  }
}

function drawCircularTelegraph(ctx, tg, progress, phase) {
  const x = tg.x;
  const y = tg.y;
  const r = Math.max(4, tg.radius);
  const kind = tg.kind || "impact";
  const urgency = 0.3 + progress * 0.7;
  const countdownR = r * (1.34 - 0.34 * progress);

  ctx.globalAlpha = 0.05 + progress * 0.08;
  ctx.fillStyle = tg.color;
  ctx.beginPath();
  ctx.arc(x, y, r, 0, TAU);
  ctx.fill();

  ctx.globalAlpha = 0.35 + urgency * 0.45;
  ctx.strokeStyle = tg.color;
  ctx.lineWidth = Math.max(1.5, tg.width || 3);
  ctx.beginPath();
  ctx.arc(x, y, r, 0, TAU);
  ctx.stroke();

  ctx.globalAlpha = 0.2 + progress * 0.6;
  ctx.lineWidth = 1.5 + progress * 1.5;
  ctx.beginPath();
  ctx.arc(x, y, countdownR, 0, TAU);
  ctx.stroke();

  ctx.globalAlpha = 0.45 + progress * 0.5;
  ctx.lineWidth = 3;
  ctx.beginPath();
  ctx.arc(x, y, r + 6, -Math.PI * 0.5, -Math.PI * 0.5 + TAU * progress);
  ctx.stroke();

  if (kind === "aoe") {
    ctx.globalAlpha = 0.35 + progress * 0.45;
    ctx.setLineDash(TELEGRAPH_DASH_WIDE);
    ctx.lineWidth = 1.5;
    ctx.beginPath();
    ctx.arc(x, y, r * 0.72, phase * 0.18, phase * 0.18 + TAU);
    ctx.stroke();
    ctx.setLineDash(NO_LINE_DASH);
    drawTelegraphElementMark(ctx, tg, r * 0.5, phase);
    return;
  }

  if (kind === "nova") {
    ctx.globalAlpha = 0.45 + progress * 0.45;
    ctx.lineWidth = 1.7;
    for (let i = 0; i < 8; i++) {
      const a = i * TAU / 8 + phase * 0.08;
      const inner = r * (0.18 + progress * 0.08);
      const outer = r * 0.68;
      ctx.beginPath();
      ctx.moveTo(x + Math.cos(a) * inner, y + Math.sin(a) * inner);
      ctx.lineTo(x + Math.cos(a) * outer, y + Math.sin(a) * outer);
      ctx.stroke();
    }
    return;
  }

  if (kind === "mine") {
    ctx.globalAlpha = 0.55 + progress * 0.4;
    ctx.lineWidth = 2;
    for (let i = 0; i < 8; i++) {
      const a = i * TAU / 8;
      ctx.beginPath();
      ctx.moveTo(x + Math.cos(a) * r * 0.72, y + Math.sin(a) * r * 0.72);
      ctx.lineTo(x + Math.cos(a) * r, y + Math.sin(a) * r);
      ctx.stroke();
    }
    const cross = r * (0.18 + 0.08 * progress);
    ctx.beginPath();
    ctx.moveTo(x - cross, y - cross);
    ctx.lineTo(x + cross, y + cross);
    ctx.moveTo(x + cross, y - cross);
    ctx.lineTo(x - cross, y + cross);
    ctx.stroke();
    return;
  }

  if (kind === "blink") {
    ctx.globalAlpha = 0.55 + progress * 0.4;
    ctx.lineWidth = 2.2;
    ctx.setLineDash(TELEGRAPH_DASH_WIDE);
    ctx.beginPath();
    ctx.arc(x, y, r * 0.72, -phase * 0.22, TAU - phase * 0.22);
    ctx.stroke();
    ctx.setLineDash(NO_LINE_DASH);
    const d = r * (0.3 - progress * 0.08);
    ctx.beginPath();
    ctx.moveTo(x, y - d);
    ctx.lineTo(x + d, y);
    ctx.lineTo(x, y + d);
    ctx.lineTo(x - d, y);
    ctx.closePath();
    ctx.stroke();
    return;
  }

  if (kind === "rift") {
    ctx.globalAlpha = 0.5 + progress * 0.4;
    ctx.lineWidth = 2;
    for (let i = 0; i < 3; i++) {
      const rr = r * (0.25 + i * 0.18);
      const start = phase * (i % 2 ? -0.2 : 0.2) + i * 1.7;
      ctx.beginPath();
      ctx.arc(x, y, rr, start, start + Math.PI * 1.18);
      ctx.stroke();
    }
    for (let i = 0; i < 4; i++) {
      const a = i * TAU / 4 + phase * 0.08;
      ctx.beginPath();
      ctx.moveTo(x + Math.cos(a) * r * 0.78, y + Math.sin(a) * r * 0.78);
      ctx.lineTo(x + Math.cos(a) * r * 0.5, y + Math.sin(a) * r * 0.5);
      ctx.stroke();
    }
    return;
  }

  if (kind === "spawn") {
    ctx.globalAlpha = 0.5 + progress * 0.45;
    ctx.lineWidth = 2;
    tracePolygon(ctx, x, y, r * (0.42 + progress * 0.12), 6, Math.PI / 6);
    ctx.stroke();
    const bracket = r * 0.22;
    for (let i = 0; i < 4; i++) {
      const a = i * Math.PI * 0.5 + Math.PI * 0.25;
      const cx = x + Math.cos(a) * r * 0.72;
      const cy = y + Math.sin(a) * r * 0.72;
      ctx.beginPath();
      ctx.moveTo(cx - bracket, cy);
      ctx.lineTo(cx, cy);
      ctx.lineTo(cx, cy - bracket);
      ctx.stroke();
    }
    return;
  }

  ctx.globalAlpha = 0.5 + progress * 0.45;
  ctx.lineWidth = 2;
  for (let i = 0; i < 4; i++) {
    const a = i * Math.PI * 0.5;
    ctx.beginPath();
    ctx.moveTo(x + Math.cos(a) * r * 0.78, y + Math.sin(a) * r * 0.78);
    ctx.lineTo(x + Math.cos(a) * r * 0.5, y + Math.sin(a) * r * 0.5);
    ctx.stroke();
  }
  const mark = r * (0.12 + progress * 0.1);
  ctx.beginPath();
  ctx.moveTo(x, y - mark);
  ctx.lineTo(x + mark, y);
  ctx.lineTo(x, y + mark);
  ctx.lineTo(x - mark, y);
  ctx.closePath();
  ctx.stroke();
}

function drawDirectionalTelegraph(ctx, tg, progress, phase) {
  const x = tg.x;
  const y = tg.y;
  const dx = tg.dx || 0;
  const dy = tg.dy || 0;
  const directionLength = Math.hypot(dx, dy);
  const r = Math.max(8, tg.radius || 20);
  const urgency = 0.3 + progress * 0.7;

  if (directionLength <= 0.0001) {
    drawCircularTelegraph(ctx, tg, progress, phase);
    const count = Math.max(1, tg.count || 1);
    ctx.globalAlpha = 0.5 + progress * 0.45;
    ctx.lineWidth = 2;
    for (let i = 0; i < count; i++) {
      const a = i * TAU / count - Math.PI * 0.5;
      const px = x + Math.cos(a) * r * 0.65;
      const py = y + Math.sin(a) * r * 0.65;
      ctx.beginPath();
      ctx.moveTo(px - Math.cos(a - 0.55) * r * 0.22, py - Math.sin(a - 0.55) * r * 0.22);
      ctx.lineTo(px, py);
      ctx.lineTo(px - Math.cos(a + 0.55) * r * 0.22, py - Math.sin(a + 0.55) * r * 0.22);
      ctx.stroke();
    }
    return;
  }

  const nx = dx / directionLength;
  const ny = dy / directionLength;
  const px = -ny;
  const py = nx;
  const length = Math.max(r * 2.5, tg.length || r * 4);
  const halfWidth = Math.max(2.5, (tg.dangerWidth || tg.width || 4) * 0.5);
  const endX = x + nx * length;
  const endY = y + ny * length;

  if (tg.kind === "projectile") {
    const dash = Math.max(7, r * 0.34);
    const startX = x + nx * r * 0.72;
    const startY = y + ny * r * 0.72;
    ctx.strokeStyle = UI_COLORS.rangedAim;
    ctx.globalAlpha = 0.15 + progress * 0.18;
    ctx.lineWidth = 1.15;
    ctx.setLineDash([dash, dash * 1.35]);
    ctx.lineDashOffset = -phase * 1.6;
    ctx.beginPath();
    ctx.moveTo(startX, startY);
    ctx.lineTo(endX, endY);
    ctx.stroke();
    ctx.setLineDash(NO_LINE_DASH);

    const bracketLength = Math.max(5, r * 0.22);
    ctx.strokeStyle = UI_COLORS.rangedAimHot;
    ctx.globalAlpha = 0.22 + progress * 0.34;
    ctx.lineWidth = 1.25;
    for (const side of [-1, 1]) {
      const bx = endX + px * halfWidth * side;
      const by = endY + py * halfWidth * side;
      ctx.beginPath();
      ctx.moveTo(bx - nx * bracketLength, by - ny * bracketLength);
      ctx.lineTo(bx, by);
      ctx.lineTo(
        bx - px * side * bracketLength * 0.72,
        by - py * side * bracketLength * 0.72
      );
      ctx.stroke();
    }

    ctx.strokeStyle = UI_COLORS.rangedAim;
    ctx.globalAlpha = 0.12 + progress * 0.18;
    ctx.lineWidth = 1;
    ctx.setLineDash([4, 6]);
    ctx.beginPath();
    ctx.arc(x, y, r * (1.16 - 0.1 * progress), 0, TAU);
    ctx.stroke();
    ctx.setLineDash(NO_LINE_DASH);
    return;
  }

  ctx.globalAlpha = 0.05 + progress * 0.1;
  ctx.fillStyle = tg.color;
  ctx.beginPath();
  ctx.moveTo(x + px * halfWidth, y + py * halfWidth);
  ctx.lineTo(endX + px * halfWidth, endY + py * halfWidth);
  ctx.lineTo(endX - px * halfWidth, endY - py * halfWidth);
  ctx.lineTo(x - px * halfWidth, y - py * halfWidth);
  ctx.closePath();
  ctx.fill();

  ctx.globalAlpha = 0.35 + urgency * 0.55;
  ctx.strokeStyle = tg.color;
  ctx.lineWidth = 1.5;
  ctx.beginPath();
  ctx.moveTo(x + px * halfWidth, y + py * halfWidth);
  ctx.lineTo(endX + px * halfWidth, endY + py * halfWidth);
  ctx.moveTo(x - px * halfWidth, y - py * halfWidth);
  ctx.lineTo(endX - px * halfWidth, endY - py * halfWidth);
  ctx.stroke();

  const arrow = Math.max(7, r * 0.35);
  ctx.lineWidth = 2;
  ctx.beginPath();
  ctx.moveTo(endX - nx * arrow + px * arrow * 0.65, endY - ny * arrow + py * arrow * 0.65);
  ctx.lineTo(endX, endY);
  ctx.lineTo(endX - nx * arrow - px * arrow * 0.65, endY - ny * arrow - py * arrow * 0.65);
  ctx.stroke();

  const markerDistance = length * progress;
  const markerX = x + nx * markerDistance;
  const markerY = y + ny * markerDistance;
  ctx.globalAlpha = 0.55 + progress * 0.4;
  ctx.lineWidth = 2.5;
  ctx.beginPath();
  ctx.moveTo(markerX + px * (halfWidth + 5), markerY + py * (halfWidth + 5));
  ctx.lineTo(markerX - px * (halfWidth + 5), markerY - py * (halfWidth + 5));
  ctx.stroke();

  ctx.globalAlpha = 0.3 + progress * 0.6;
  ctx.lineWidth = 2;
  ctx.beginPath();
  ctx.arc(x, y, r * (1.25 - 0.25 * progress), 0, TAU);
  ctx.stroke();
}

function drawAttackTelegraph(ctx, tg, index, now) {
  const progress = getTelegraphProgress(tg);
  const phase = now * 0.006 + index * 0.73;
  const isProjectileAim = tg.kind === "projectile";
  const telegraphColor = isProjectileAim ? UI_COLORS.rangedAim : tg.color;
  ctx.save();
  ctx.strokeStyle = telegraphColor;
  ctx.fillStyle = telegraphColor;
  ctx.shadowColor = telegraphColor;
  ctx.shadowBlur = 0;
  ctx.lineCap = "round";
  ctx.lineJoin = "round";

  if (tg.kind === "projectile" || tg.kind === "line") {
    drawDirectionalTelegraph(ctx, tg, progress, phase);
  } else {
    drawCircularTelegraph(ctx, tg, progress, phase);
  }

  if (!isProjectileAim) {
    ctx.setLineDash(NO_LINE_DASH);
    ctx.globalAlpha = 0.18 + progress * 0.42;
    ctx.shadowBlur = GLOW.low;
    ctx.lineWidth = 1.5;
    ctx.beginPath();
    ctx.arc(tg.x, tg.y, Math.max(8, tg.radius || 20), 0, TAU);
    ctx.stroke();
  }

  if (tg.label) {
    ctx.shadowBlur = 0;
    ctx.globalAlpha = OPACITY.strong;
    ctx.fillStyle = tg.color;
    ctx.font = "700 14px ui-sans-serif, system-ui, -apple-system, Segoe UI, Roboto, Arial";
    ctx.textAlign = "center";
    ctx.textBaseline = "middle";
    ctx.fillText(tg.label, tg.x, tg.y);
  }
  ctx.restore();
}

function drawHazardZone(ctx, z, index, now) {
  const lifeT = clamp(z.life / z.maxLife, 0, 1);
  const fade = clamp(lifeT / 0.22, 0, 1);
  const phase = now * 0.004 + index * 0.91;
  const r = z.radius;
  const type = z.type || "poison";

  ctx.save();
  ctx.fillStyle = z.color;
  ctx.strokeStyle = z.color;
  ctx.lineCap = "round";
  ctx.lineJoin = "round";

  ctx.globalAlpha = (0.08 + lifeT * 0.12) * fade;
  ctx.beginPath();
  ctx.arc(z.x, z.y, r, 0, TAU);
  ctx.fill();

  ctx.globalAlpha = (0.5 + lifeT * 0.3) * fade;
  ctx.shadowColor = z.color;
  ctx.shadowBlur = GLOW.medium;
  ctx.lineWidth = 2.2;
  ctx.beginPath();
  ctx.arc(z.x, z.y, r, 0, TAU);
  ctx.stroke();
  ctx.shadowBlur = 0;

  if (type === "fire") {
    ctx.globalAlpha = (0.35 + lifeT * 0.35) * fade;
    ctx.lineWidth = 1.8;
    for (let i = 0; i < 7; i++) {
      const a = i * TAU / 7;
      const cx = z.x + Math.cos(a) * r * 0.55;
      const cy = z.y + Math.sin(a) * r * 0.55;
      const lift = r * (0.1 + 0.04 * Math.sin(phase * 1.7 + i));
      ctx.beginPath();
      ctx.moveTo(cx - r * 0.06, cy + lift);
      ctx.lineTo(cx, cy - lift);
      ctx.lineTo(cx + r * 0.06, cy + lift);
      ctx.stroke();
    }
  } else if (type === "void") {
    ctx.globalAlpha = (0.38 + lifeT * 0.32) * fade;
    ctx.lineWidth = 1.8;
    ctx.setLineDash(TELEGRAPH_DASH_NARROW);
    for (let i = 0; i < 3; i++) {
      const rr = r * (0.28 + i * 0.18);
      const spin = phase * (i % 2 ? -0.32 : 0.32) + i;
      ctx.beginPath();
      ctx.arc(z.x, z.y, rr, spin, spin + TAU);
      ctx.stroke();
    }
    ctx.setLineDash(NO_LINE_DASH);
    for (let i = 0; i < 4; i++) {
      const a = i * TAU / 4 + phase * 0.2;
      ctx.beginPath();
      ctx.moveTo(z.x + Math.cos(a) * r * 0.82, z.y + Math.sin(a) * r * 0.82);
      ctx.lineTo(z.x + Math.cos(a) * r * 0.58, z.y + Math.sin(a) * r * 0.58);
      ctx.stroke();
    }
  } else {
    ctx.globalAlpha = (0.32 + lifeT * 0.35) * fade;
    ctx.lineWidth = 1.5;
    for (let i = 0; i < 8; i++) {
      const a = i * 2.399963 + phase * 0.16;
      const orbit = r * (0.22 + (i % 3) * 0.2);
      const bubbleR = r * (0.035 + (i % 2) * 0.018);
      ctx.beginPath();
      ctx.arc(
        z.x + Math.cos(a) * orbit,
        z.y + Math.sin(a) * orbit,
        bubbleR,
        0,
        TAU
      );
      ctx.stroke();
    }
  }

  ctx.restore();
}

function turretColor(turret) {
  if (turret.element === "fire") return COLORS.turretFire;
  if (turret.element === "poison") return COLORS.turretPoison;
  return COLORS.turret;
}

function turretStreamOffset(turret, t, now) {
  if (visualSettings.reducedMotion) return Math.sin(turret.id * 1.7 + t * 8) * turret.width * 0.035 * t;
  const phase = now * 0.009 + turret.id * 1.7;
  return (
    Math.sin(phase + t * 10.5) * 0.15
    + Math.sin(phase * 0.63 - t * 16) * 0.08
  ) * turret.width * t;
}

function traceTurretStreamCenter(ctx, turret, now) {
  const length = turret.streamLength || 0;
  if (length <= turret.r + 2) return false;
  const segments = 7;
  ctx.beginPath();
  ctx.moveTo(turret.r * 0.72, 0);
  for (let i = 1; i <= segments; i++) {
    const t = i / segments;
    ctx.lineTo(length * t, turretStreamOffset(turret, t, now));
  }
  return true;
}

function drawTurretStream(ctx, turret, now) {
  if (!turret.firing || turret.streamLength <= turret.r + 2) return;
  const lifeFade = clamp(turret.life / 0.5, 0, 1);
  const color = turretColor(turret);
  const length = turret.streamLength;
  const segments = 7;

  ctx.save();
  ctx.translate(turret.x, turret.y);
  ctx.rotate(turret.angle);
  ctx.lineCap = "round";
  ctx.lineJoin = "round";
  ctx.globalCompositeOperation = "lighter";

  ctx.beginPath();
  for (let i = 0; i <= segments; i++) {
    const t = i / segments;
    const x = Math.max(turret.r * 0.72, length * t);
    const y = turretStreamOffset(turret, t, now);
    const envelope = Math.pow(Math.sin(Math.PI * Math.min(0.98, t)), 0.62);
    const halfWidth = 1.5 + turret.width * (0.08 + envelope * 0.42);
    if (i === 0) ctx.moveTo(x, y - halfWidth);
    else ctx.lineTo(x, y - halfWidth);
  }
  for (let i = segments; i >= 0; i--) {
    const t = i / segments;
    const x = Math.max(turret.r * 0.72, length * t);
    const y = turretStreamOffset(turret, t, now);
    const envelope = Math.pow(Math.sin(Math.PI * Math.min(0.98, t)), 0.62);
    const halfWidth = 1.5 + turret.width * (0.08 + envelope * 0.42);
    ctx.lineTo(x, y + halfWidth);
  }
  ctx.closePath();
  ctx.globalAlpha = 0.27 * lifeFade;
  ctx.fillStyle = color;
  ctx.shadowColor = color;
  ctx.shadowBlur = GLOW.medium;
  ctx.fill();

  ctx.globalAlpha = 0.56 * lifeFade;
  ctx.shadowBlur = GLOW.low;
  ctx.strokeStyle = color;
  ctx.lineWidth = Math.max(2, turret.width * 0.17);
  if (traceTurretStreamCenter(ctx, turret, now)) ctx.stroke();

  ctx.globalAlpha = 0.68 * lifeFade;
  ctx.shadowBlur = 0;
  ctx.strokeStyle = "rgba(239,254,255,.92)";
  ctx.lineWidth = 1.05;
  if (traceTurretStreamCenter(ctx, turret, now)) ctx.stroke();

  for (let i = 0; i < 3; i++) {
    const t = visualSettings.reducedMotion ? (0.28 + i * 0.21) : ((now * 0.00048 + i * 0.29 + turret.id * 0.11) % 0.86);
    if (t < 0.12) continue;
    const x = length * t;
    const y = turretStreamOffset(turret, t, now);
    const moteR = 1.2 + (i % 2) * 0.6;
    ctx.globalAlpha = (0.34 + (1 - t) * 0.4) * lifeFade;
    ctx.fillStyle = color;
    ctx.beginPath();
    ctx.arc(x, y, moteR, 0, TAU);
    ctx.fill();
  }
  ctx.restore();
}

function drawTurretBody(ctx, turret) {
  const color = turretColor(turret);
  const fade = clamp(turret.life / 0.5, 0, 1);
  const r = turret.r;
  ctx.save();
  ctx.translate(turret.x, turret.y);
  ctx.globalAlpha = fade;

  ctx.strokeStyle = "rgba(88,247,255,.34)";
  ctx.lineWidth = 1.25;
  for (let i = 0; i < 3; i++) {
    const angle = i * TAU / 3 + Math.PI * 0.5;
    ctx.beginPath();
    ctx.moveTo(Math.cos(angle) * r * 0.55, Math.sin(angle) * r * 0.55);
    ctx.lineTo(Math.cos(angle) * r * 1.22, Math.sin(angle) * r * 1.22);
    ctx.stroke();
  }

  tracePolygon(ctx, 0, 0, r * 0.78, 6, Math.PI / 6);
  ctx.fillStyle = "rgba(8,16,25,.92)";
  ctx.fill();
  ctx.shadowColor = color;
  ctx.shadowBlur = GLOW.medium;
  ctx.strokeStyle = color;
  ctx.lineWidth = 1.6;
  ctx.stroke();

  ctx.rotate(turret.angle);
  ctx.fillStyle = "rgba(8,15,24,.96)";
  ctx.strokeStyle = color;
  ctx.lineWidth = 1.4;
  ctx.beginPath();
  ctx.moveTo(-r * 0.42, -r * 0.44);
  ctx.lineTo(r * 0.72, -r * 0.34);
  ctx.lineTo(r * 1.26, -r * 0.18);
  ctx.lineTo(r * 1.26, r * 0.18);
  ctx.lineTo(r * 0.72, r * 0.34);
  ctx.lineTo(-r * 0.42, r * 0.44);
  ctx.closePath();
  ctx.fill();
  ctx.stroke();

  ctx.shadowBlur = 0;
  ctx.fillStyle = "rgba(235,253,255,.94)";
  ctx.beginPath();
  ctx.arc(0, 0, r * 0.22, 0, TAU);
  ctx.fill();
  ctx.strokeStyle = color;
  ctx.lineWidth = 1.1;
  ctx.beginPath();
  ctx.arc(r * 1.25, 0, r * 0.24, 0, TAU);
  ctx.stroke();
  ctx.restore();
}

function drawFriendlyToxinPool(ctx, pool, now) {
  const lifeT = clamp(pool.life / Math.max(0.001, pool.maxLife), 0, 1);
  const fade = Math.min(1, lifeT * 4, (1 - lifeT) * 6 + 0.2);
  const phase = visualSettings.reducedMotion ? pool.phase * 0 : now * 0.00022 + pool.ownerId;
  const radius = pool.radius;
  ctx.save();
  ctx.translate(pool.x, pool.y);
  ctx.rotate(phase);
  ctx.globalAlpha = fade * 0.16;
  ctx.fillStyle = COLORS.turretPoison;
  ctx.beginPath();
  for (let i = 0; i < 12; i++) {
    const angle = i * TAU / 12;
    const rr = radius * (0.88 + ((i * 17 + pool.ownerId) % 5) * 0.025);
    const x = Math.cos(angle) * rr;
    const y = Math.sin(angle) * rr;
    if (i === 0) ctx.moveTo(x, y);
    else ctx.lineTo(x, y);
  }
  ctx.closePath();
  ctx.fill();

  ctx.globalAlpha = fade * 0.62;
  ctx.strokeStyle = COLORS.turretPoison;
  ctx.lineWidth = 1.5;
  ctx.stroke();
  ctx.globalAlpha = fade * 0.38;
  ctx.strokeStyle = COLORS.turret;
  ctx.lineWidth = 0.9;
  ctx.beginPath();
  ctx.arc(0, 0, radius * 0.72, 0, TAU);
  ctx.stroke();

  ctx.globalAlpha = fade * 0.26;
  ctx.strokeStyle = COLORS.turretPoison;
  ctx.lineWidth = 1;
  for (let i = -2; i <= 2; i++) {
    const y = i * radius * 0.22;
    const span = Math.sqrt(Math.max(0, radius * radius * 0.55 - y * y));
    ctx.beginPath();
    ctx.moveTo(-span, y);
    ctx.lineTo(span, y);
    ctx.stroke();
  }
  ctx.restore();
}

function ensureGlowBuffers(W, H, renderDpr) {
  const bufferScale = Math.max(0.25, renderDpr * LOW_RES_GLOW_SCALE);
  const width = Math.max(1, Math.ceil(W * bufferScale));
  const height = Math.max(1, Math.ceil(H * bufferScale));
  if (width === glowBackingWidth && height === glowBackingHeight && glowMaskCanvas && glowBlurCanvas) {
    return bufferScale;
  }

  glowBackingWidth = width;
  glowBackingHeight = height;
  glowMaskCanvas = makeOffscreenCanvas(width, height);
  glowMaskCtx = glowMaskCanvas.getContext("2d");
  glowBlurCanvas = makeOffscreenCanvas(width, height);
  glowBlurCtx = glowBlurCanvas.getContext("2d");
  return bufferScale;
}

function glowCircle(ctx, x, y, radius, color, alpha = 1) {
  ctx.globalAlpha = alpha;
  ctx.fillStyle = color;
  ctx.beginPath();
  ctx.arc(x, y, Math.max(1, radius), 0, TAU);
  ctx.fill();
}

function glowRing(ctx, x, y, radius, color, width = 2, alpha = 1) {
  ctx.globalAlpha = alpha;
  ctx.strokeStyle = color;
  ctx.lineWidth = Math.max(1, width);
  ctx.beginPath();
  ctx.arc(x, y, Math.max(1, radius), 0, TAU);
  ctx.stroke();
}

function tracePolyline(ctx, points, reverse = false) {
  if (!points || points.length < 2) return false;
  ctx.beginPath();
  const start = reverse ? points.length - 1 : 0;
  const end = reverse ? -1 : points.length;
  const step = reverse ? -1 : 1;
  ctx.moveTo(points[start].x, points[start].y);
  for (let i = start + step; i !== end; i += step) {
    ctx.lineTo(points[i].x, points[i].y);
  }
  return true;
}

function glowDirectedProjectile(ctx, projectile, length, width, color, alpha = 1) {
  const speed = Math.hypot(projectile.vx || 0, projectile.vy || 0) || 1;
  const nx = (projectile.vx || 0) / speed;
  const ny = (projectile.vy || 0) / speed;
  ctx.globalAlpha = alpha;
  ctx.strokeStyle = color;
  ctx.lineWidth = Math.max(1, width);
  ctx.beginPath();
  ctx.moveTo(projectile.x - nx * length * 0.62, projectile.y - ny * length * 0.62);
  ctx.lineTo(projectile.x + nx * length * 0.38, projectile.y + ny * length * 0.38);
  ctx.stroke();
}

function drawLowResGlowSource(ctx, camX, camY, W, H, tier) {
  const minX = camX - WORLD.spawnPad;
  const maxX = camX + W + WORLD.spawnPad;
  const minY = camY - WORLD.spawnPad;
  const maxY = camY + H + WORLD.spawnPad;
  ctx.save();
  ctx.translate(-camX, -camY);
  ctx.globalCompositeOperation = "lighter";
  ctx.lineCap = "round";
  ctx.lineJoin = "round";

  if (tier === "medium") {
    for (let i = 0; i < visibleFrame.voidZones.length; i++) {
      const zone = visibleFrame.voidZones[i];
      glowCircle(ctx, zone.x, zone.y, zone.radius, zone.color, 0.12);
      glowRing(ctx, zone.x, zone.y, zone.radius, zone.color, 3, 0.7);
    }
  }
  if (tier === "high") {
    for (let i = 0; i < visibleFrame.chests.length; i++) {
      const chest = visibleFrame.chests[i];
      const visual = chestVisual(chest.kind);
      glowCircle(ctx, chest.x, chest.y, chest.r * 1.35, visual.color, 0.9);
    }
    if (
      quest.giverActive
      && circleIntersectsView(quest.giverX, quest.giverY, quest.giverR * 2, minX, maxX, minY, maxY)
    ) {
      glowCircle(ctx, quest.giverX, quest.giverY, quest.giverR * 1.35, quest.completed ? COLORS.gold : COLORS.quest, 0.9);
    }
  }
  const sweepRadius = Math.max(40, quest.zoneR || 180);
  if (
    tier === "high"
    &&
    quest.active && !quest.completed && quest.type === "perfect_sweep"
    && circleIntersectsView(quest.zoneX, quest.zoneY, sweepRadius + 24, minX, maxX, minY, maxY)
  ) {
    glowRing(ctx, quest.zoneX, quest.zoneY, sweepRadius, COLORS.warn, 4, 0.7);
  }
  if (tier === "medium") {
    for (let i = 0; i < visibleFrame.questItems.length; i++) {
      const item = visibleFrame.questItems[i];
      glowCircle(ctx, item.x, item.y, item.r * 1.2, COLORS.quest, 0.85);
    }
  }
  if (tier === "low") {
    for (let i = 0; i < visibleFrame.gems.length; i++) {
      const gem = visibleFrame.gems[i];
      const lifeT = gem.maxLife > 0 ? clamp(gem.life / gem.maxLife, 0, 1) : 1;
      glowCircle(ctx, gem.x, gem.y, gem.r * 1.1, COLORS.gem, lifeT > 0.2 ? 0.9 : lifeT * 4.5);
    }
  }
  for (let i = 0; i < visibleFrame.enemies.length; i++) {
    const enemy = visibleFrame.enemies[i];
    const enemyTier = ENEMY_TYPES[enemy.type]?.boss === true ? "high" : "medium";
    if (tier === enemyTier) {
      glowCircle(ctx, enemy.x, enemy.y, enemy.r * (enemy.elite ? 1.06 : 0.94), enemy.color, enemy.elite ? 0.9 : 0.68);
    }
    const hitTier = enemy.hitCrit ? "high" : "low";
    if (enemy.hitFlash > 0 && tier === hitTier) {
      glowRing(ctx, enemy.x, enemy.y, enemy.r * 1.25, enemy.hitCrit ? COLORS.crit : COLORS.dmg, 3, 0.8);
    }
  }
  if (tier === "medium") {
    for (let i = 0; i < visibleFrame.telegraphs.length; i++) {
      const telegraph = visibleFrame.telegraphs[i];
      const progress = getTelegraphProgress(telegraph);
      if (telegraph.kind === "projectile") {
        const length = Math.max(24, telegraph.length || telegraph.radius * 4);
        const startOffset = Math.max(8, telegraph.radius || 20) * 0.72;
        ctx.globalAlpha = 0.1 + progress * 0.12;
        ctx.strokeStyle = UI_COLORS.rangedAim;
        ctx.lineWidth = 1.2;
        ctx.setLineDash([8, 13]);
        ctx.beginPath();
        ctx.moveTo(
          telegraph.x + (telegraph.dx || 0) * startOffset,
          telegraph.y + (telegraph.dy || 0) * startOffset
        );
        ctx.lineTo(
          telegraph.x + (telegraph.dx || 0) * length,
          telegraph.y + (telegraph.dy || 0) * length
        );
        ctx.stroke();
        ctx.setLineDash(NO_LINE_DASH);
      } else if (telegraph.kind === "line") {
        const length = Math.max(24, telegraph.length || telegraph.radius * 4);
        ctx.globalAlpha = 0.35 + progress * 0.45;
        ctx.strokeStyle = telegraph.color;
        ctx.lineWidth = Math.max(2, telegraph.dangerWidth || telegraph.width || 3);
        ctx.beginPath();
        ctx.moveTo(telegraph.x, telegraph.y);
        ctx.lineTo(telegraph.x + (telegraph.dx || 0) * length, telegraph.y + (telegraph.dy || 0) * length);
        ctx.stroke();
      } else {
        glowRing(ctx, telegraph.x, telegraph.y, Math.max(8, telegraph.radius || 20), telegraph.color, 3, 0.35 + progress * 0.45);
      }
    }
  }
  if (tier === "medium") {
    for (let i = 0; i < visibleFrame.enemyShots.length; i++) {
      const shot = visibleFrame.enemyShots[i];
      glowCircle(ctx, shot.x, shot.y, shot.r * (shot.homing ? 1.35 : 1), shot.color, 0.85);
    }
  }
  if (tier === "high") {
    for (let i = 0; i < visibleFrame.arcs.length; i++) {
      const arc = visibleFrame.arcs[i];
      if (!arc.points || arc.points.length < 2) continue;
      ctx.globalAlpha = clamp(arc.life / (arc.maxLife || 0.0001), 0, 1);
      ctx.strokeStyle = arc.color || COLORS.arc;
      ctx.lineWidth = 4 * (arc.intensity || 1);
      if (tracePolyline(ctx, arc.points)) ctx.stroke();
    }
    for (let i = 0; i < visibleFrame.rails.length; i++) {
      const rail = visibleFrame.rails[i];
      ctx.globalAlpha = 0.8;
      ctx.strokeStyle = COLORS.rail;
      ctx.lineWidth = Math.max(3, rail.r * 2.4);
      if (tracePolyline(ctx, rail.trail, true)) ctx.stroke();
      glowCircle(ctx, rail.x, rail.y, rail.r * 1.45, COLORS.rail, 0.95);
    }
  }
  if (tier === "medium") {
    for (let i = 0; i < visibleFrame.arcs.length; i++) {
      const arc = visibleFrame.arcs[i];
      if (!arc.points || arc.points.length < 2) continue;
      ctx.globalAlpha = clamp(arc.life / (arc.maxLife || 0.0001), 0, 1) * 0.88;
      ctx.strokeStyle = COLORS.player;
      ctx.lineWidth = 1.5 * (arc.intensity || 1);
      if (tracePolyline(ctx, arc.points)) ctx.stroke();
    }
    for (let i = 0; i < visibleFrame.rails.length; i++) {
      const rail = visibleFrame.rails[i];
      ctx.globalAlpha = 0.82;
      ctx.strokeStyle = COLORS.arc;
      ctx.lineWidth = Math.max(1.5, rail.r * 0.8);
      if (tracePolyline(ctx, rail.trail, true)) ctx.stroke();
      glowCircle(ctx, rail.x, rail.y, rail.r * 0.72, COLORS.arc, 0.92);
    }
  }
  for (let i = 0; i < visibleFrame.bullets.length; i++) {
    const bullet = visibleFrame.bullets[i];
    if (bullet.isExplosion && tier === "high") {
      const lifeT = clamp(bullet.life / (bullet.maxLife || 0.0001), 0, 1);
      glowRing(ctx, bullet.x, bullet.y, bullet.r * (1 - lifeT), bullet.color || COLORS.bullet, 4, lifeT);
    } else if (!bullet.isExplosion && tier === "medium") {
      glowDirectedProjectile(ctx, bullet, bullet.r * 4.6, bullet.r * 1.35, COLORS.bullet, 0.9);
    }
  }
  if (tier === "medium") {
    for (let i = 0; i < visibleFrame.toxinPools.length; i++) {
      const pool = visibleFrame.toxinPools[i];
      const lifeT = clamp(pool.life / Math.max(0.001, pool.maxLife), 0, 1);
      glowCircle(ctx, pool.x, pool.y, pool.radius * 0.82, COLORS.turretPoison, lifeT * 0.16);
      glowRing(ctx, pool.x, pool.y, pool.radius, COLORS.turretPoison, 2, lifeT * 0.58);
    }
    for (let i = 0; i < visibleFrame.turrets.length; i++) {
      const turret = visibleFrame.turrets[i];
      const color = turretColor(turret);
      glowCircle(ctx, turret.x, turret.y, turret.r * 0.78, color, 0.82);
      if (!turret.firing || turret.streamLength <= turret.r) continue;
      ctx.globalAlpha = 0.58;
      ctx.strokeStyle = color;
      ctx.lineWidth = Math.max(2, turret.width * 0.34);
      ctx.beginPath();
      ctx.moveTo(turret.x + Math.cos(turret.angle) * turret.r, turret.y + Math.sin(turret.angle) * turret.r);
      ctx.lineTo(
        turret.x + Math.cos(turret.angle) * turret.streamLength,
        turret.y + Math.sin(turret.angle) * turret.streamLength
      );
      ctx.stroke();
    }
  }
  if (tier === "high") {
    for (let i = 0; i < visibleFrame.turrets.length; i++) {
      const turret = visibleFrame.turrets[i];
      if (!turret.firing || turret.streamLength <= turret.r) continue;
      ctx.globalAlpha = 0.76;
      ctx.strokeStyle = "rgba(239,254,255,.96)";
      ctx.lineWidth = 1.5;
      ctx.beginPath();
      ctx.moveTo(turret.x + Math.cos(turret.angle) * turret.r, turret.y + Math.sin(turret.angle) * turret.r);
      ctx.lineTo(
        turret.x + Math.cos(turret.angle) * turret.streamLength,
        turret.y + Math.sin(turret.angle) * turret.streamLength
      );
      ctx.stroke();
    }
  }
  if (tier === "low") {
    for (let i = 0; i < visibleFrame.missiles.length; i++) {
      const missile = visibleFrame.missiles[i];
      glowDirectedProjectile(ctx, missile, missile.r * 4.2, missile.r * 1.5, COLORS.missile, 0.88);
    }
  }
  if (tier === "medium") {
    for (let i = 0; i < visibleFrame.axes.length; i++) {
      const axe = visibleFrame.axes[i];
      glowCircle(ctx, axe.x, axe.y, Math.max(7, axe.r * 0.9), UI_COLORS.axeShadow, 0.72);
    }
  }
  for (let i = 0; i < visibleFrame.orbs.length; i++) {
    const orb = visibleFrame.orbs[i];
    const coreRadius = orb.state === "fly" ? orb.r : orb.radius * 0.4;
    if (tier === "medium") {
      glowCircle(ctx, orb.x, orb.y, coreRadius * 0.86, COLORS.bullet, orb.state === "park" ? 0.9 : 0.78);
    }
    if (tier === "high") {
      const fieldAlpha = orb.state === "park" ? 0.64 : 0.4;
      glowRing(ctx, orb.x, orb.y, orb.radius, COLORS.bullet, 2.6, fieldAlpha);
      glowRing(ctx, orb.x, orb.y, orb.radius * 0.72, COLORS.arc, 1.6, fieldAlpha * 0.72);
    }
  }
  if (weapons.aura.unlocked && tier === "high") {
    const stats = auraStats();
    glowCircle(ctx, player.x, player.y, stats.radius, UI_COLORS.auraStroke, 0.07);
    glowRing(ctx, player.x, player.y, stats.radius, UI_COLORS.auraStroke, 2.6, 0.46);
  }
  if (weapons.aura.unlocked && tier === "medium") {
    const stats = auraStats();
    glowRing(ctx, player.x, player.y, stats.radius * 0.68, COLORS.arc, 1.2, 0.2);
  }
  if (tier === "low") {
    for (let i = 0; i < visibleFrame.particles.length; i++) {
      const particle = visibleFrame.particles[i];
      const lifeT = clamp(particle.life / particle.maxLife, 0, 1);
      glowCircle(ctx, particle.x, particle.y, particle.r, particle.color, lifeT * 0.72);
    }
  }
  if (tier === "high") {
    glowCircle(ctx, player.x, player.y, player.r, COLORS.player, 1);
    if (buffs.shield > 0) glowRing(ctx, player.x, player.y, player.r + 10, UI_COLORS.shieldRing, 3, 0.8);
  }
  if (tier === "medium") {
    if (buffs.magnet > 0) glowRing(ctx, player.x, player.y, player.r + 18, UI_COLORS.magnetRing, 3, 0.65);
    for (let i = 0; i < visibleFrame.companions.length; i++) {
      const companion = visibleFrame.companions[i];
      glowCircle(ctx, companion.x, companion.y, companion.r, companion.color, 0.82);
    }
  }
  for (let i = 0; i < visibleFrame.damageTexts.length; i++) {
    const text = visibleFrame.damageTexts[i];
    if (tier !== (text.crit ? "high" : "medium")) continue;
    ctx.globalAlpha = clamp(text.life / text.maxLife, 0, 1) * 0.8;
    ctx.fillStyle = text.color;
    ctx.font = `${text.crit ? 850 : 700} ${text.size}px ui-sans-serif, system-ui, sans-serif`;
    ctx.textAlign = "center";
    ctx.textBaseline = "middle";
    ctx.fillText(text.text, text.x, text.y);
  }

  ctx.restore();
  ctx.globalAlpha = 1;
  ctx.globalCompositeOperation = "source-over";
}

function glowRadiusAtTarget(logicalRadius, bufferScale) {
  return Math.max(
    0.5,
    logicalRadius * visualSettings.glowScale * bufferScale * GLOW_FILTER_TO_SHADOW_SCALE
  );
}

function drawLowResGlowLayer(ctx, W, H, camX, camY, renderDpr) {
  if (!usesLowResGlow()) return;
  const bufferScale = ensureGlowBuffers(W, H, renderDpr);

  ctx.save();
  ctx.setTransform(renderDpr, 0, 0, renderDpr, 0, 0);
  ctx.globalCompositeOperation = "lighter";
  ctx.globalAlpha = Math.min(1, 0.9 * visualSettings.glowScale);
  for (let i = 0; i < LOW_RES_GLOW_PASSES.length; i++) {
    const pass = LOW_RES_GLOW_PASSES[i];
    glowMaskCtx.setTransform(1, 0, 0, 1, 0, 0);
    glowMaskCtx.clearRect(0, 0, glowBackingWidth, glowBackingHeight);
    glowMaskCtx.setTransform(bufferScale, 0, 0, bufferScale, 0, 0);
    drawLowResGlowSource(glowMaskCtx, camX, camY, W, H, pass.tier);

    glowBlurCtx.setTransform(1, 0, 0, 1, 0, 0);
    glowBlurCtx.clearRect(0, 0, glowBackingWidth, glowBackingHeight);
    glowBlurCtx.globalCompositeOperation = "source-over";
    glowBlurCtx.globalAlpha = 1;
    glowBlurCtx.filter = `blur(${glowRadiusAtTarget(pass.radius, bufferScale)}px)`;
    glowBlurCtx.drawImage(glowMaskCanvas, 0, 0);
    glowBlurCtx.filter = "none";

    ctx.drawImage(glowBlurCanvas, 0, 0, W, H);
  }
  ctx.restore();
}

export function renderFrame({
  ctx,
  W,
  H,
  camX,
  camY,
  boss,
  startNoticeT,
  START_NOTICE_TIME,
  isMobile,
  state,
  STATE,
  fmtTime,
  ui,
  frameNow,
  renderDpr = 1,
}){
  syncVisualCaches();
  const visualNow = visualTime();
  const viewBounds = collectVisibleFrame(camX, camY, W, H);
  drawBackgroundBase(ctx, W, H);

  ctx.save();
  ctx.translate(-camX, -camY);

  drawWorldGrid(ctx, W, H, camX, camY);
  drawBackgroundAccents(ctx, W, H, camX, camY);
  drawLowResGlowLayer(ctx, W, H, camX, camY, renderDpr);
  drawWorldBounds(ctx);

  const visMinX = viewBounds.minX, visMaxX = viewBounds.maxX;
  const visMinY = viewBounds.minY, visMaxY = viewBounds.maxY;

  // Obstacles
  ctx.save();
  drawMergedLakes(ctx, visibleFrame.lakes, visMinX, visMaxX, visMinY, visMaxY);
  for (let i=0;i<visibleFrame.obstacles.length;i++) drawObstacleSprite(ctx, visibleFrame.obstacles[i]);
  ctx.restore();

  for (let i=0;i<visibleFrame.voidZones.length;i++){
    drawHazardZone(ctx, visibleFrame.voidZones[i], i, visualNow);
  }

  for (let i=0;i<visibleFrame.toxinPools.length;i++){
    drawFriendlyToxinPool(ctx, visibleFrame.toxinPools[i], visualNow);
  }

  for (let i=0;i<visibleFrame.chests.length;i++){
    const c = visibleFrame.chests[i];
    const pulse = (Math.sin(c.pulse) * 0.15 + 0.85);
    drawChestBody(ctx, c, pulse);
  }

  if (
    quest.giverActive
    && circleIntersectsView(quest.giverX, quest.giverY, quest.giverR * 2, visMinX, visMaxX, visMinY, visMaxY)
  ) {
    const qPulse = 0.7 + 0.3 * Math.sin(visualTime() * 0.006);
    const qColor = quest.completed ? COLORS.gold : COLORS.quest;
    const qr = quest.giverR * (1.0 + 0.08 * qPulse);
    const exBob = Math.sin(visualTime() * 0.006) * 3;

    ctx.save();
    ctx.translate(quest.giverX, quest.giverY);
    ctx.shadowColor = qColor;
    ctx.shadowBlur = GLOW.medium;
    ctx.fillStyle = UI_COLORS.questFill;
    ctx.strokeStyle = qColor;
    ctx.lineWidth = 2;
    tracePolygon(ctx, 0, 0, qr, 4, 0);
    ctx.fill();
    ctx.shadowBlur = 0;
    ctx.stroke();
    ctx.restore();
    drawQuestMark(ctx, quest.giverX, quest.giverY, qr * 0.58, qColor);

    neonRing(ctx, quest.giverX, quest.giverY, qr * 1.5, qColor, GLOW.high, 2, 0.8 * qPulse);

    if (!quest.active || quest.completed) {
      const mark = quest.completed ? "✓" : "!";
      ctx.save();
      ctx.font = "700 20px ui-sans-serif, system-ui, -apple-system, Segoe UI, Roboto, Arial";
      ctx.textAlign = "center";
      ctx.textBaseline = "middle";
      ctx.shadowColor = qColor;
      ctx.shadowBlur = GLOW.medium;
      ctx.fillStyle = qColor;
      ctx.fillText(mark, quest.giverX, quest.giverY - qr * 1.9 + exBob);
      ctx.restore();
    }
  }

  const sweepRadius = Math.max(40, quest.zoneR || 180);
  if (
    quest.active && !quest.completed && quest.type === "perfect_sweep"
    && circleIntersectsView(quest.zoneX, quest.zoneY, sweepRadius + 24, visMinX, visMaxX, visMinY, visMaxY)
  ) {
    const sweepPulse = 0.6 + 0.4 * Math.sin(visualTime() * 0.01);
    const zr = sweepRadius;
    neonRing(ctx, quest.zoneX, quest.zoneY, zr, COLORS.warn, GLOW.high, 2.4, 0.55 * sweepPulse);
    neonRing(ctx, quest.zoneX, quest.zoneY, zr * 0.74, COLORS.quest, GLOW.low, 1.8, 0.25 * sweepPulse);
  }

  for (let i=0;i<visibleFrame.questItems.length;i++){
    const it = visibleFrame.questItems[i];
    const color = COLORS.quest;
    const sprite = getQuestSprite(it.r, color);
    ctx.drawImage(sprite.canvas, it.x - sprite.r - sprite.pad, it.y - sprite.r - sprite.pad);
  }

  for (let i=0;i<visibleFrame.gems.length;i++){
    const g = visibleFrame.gems[i];
    const lifeT = g.maxLife > 0 ? clamp(g.life / g.maxLife, 0, 1) : 1;
    const a = lifeT > 0.2 ? 1 : (lifeT / 0.2);
    ctx.save();
    ctx.globalAlpha = a;
    const sprite = getGemSprite(g.r);
    ctx.drawImage(sprite.canvas, g.x - sprite.r - sprite.pad, g.y - sprite.r - sprite.pad);
    ctx.restore();
  }

  for (let i=0;i<visibleFrame.turrets.length;i++){
    drawTurretBody(ctx, visibleFrame.turrets[i]);
  }

  // Enemies: cached sprite draw (reduces per-frame shadowBlur cost)
  for (let i=0;i<visibleFrame.enemies.length;i++){
    const e = visibleFrame.enemies[i];
    const sprite = getEnemySprite(e.type, e.r, e.color, e.elite ? "elite" : "base");
    ctx.drawImage(sprite.canvas, e.x - sprite.r - sprite.pad, e.y - sprite.r - sprite.pad);
    drawEnemyHitFeedback(ctx, e, sprite);
  }

  for (let i=0;i<visibleFrame.turrets.length;i++){
    drawTurretStream(ctx, visibleFrame.turrets[i], visualNow);
  }

  // Enemy HP bars
  ctx.save();
  ctx.fillStyle = UI_COLORS.hpBarBg;
  for (let i=0;i<visibleFrame.enemies.length;i++){
    const e = visibleFrame.enemies[i];
    const hpT = clamp(e.hp / e.maxHp, 0, 1);
    if (hpT >= 0.999) continue;
    const size = e.r * 2;
    const barW = size + 8;
    const barH = 4;
    const bx = e.x - barW * 0.5;
    const by = e.y - e.r - 10;
    ctx.fillRect(bx, by, barW, barH);
  }
  ctx.shadowColor = UI_COLORS.hpBarShadow;
  ctx.shadowBlur = 0;
  ctx.fillStyle = UI_COLORS.hpBarFill;
  for (let i=0;i<visibleFrame.enemies.length;i++){
    const e = visibleFrame.enemies[i];
    const hpT = clamp(e.hp / e.maxHp, 0, 1);
    if (hpT >= 0.999) continue;
    const size = e.r * 2;
    const barW = size + 8;
    const barH = 4;
    const bx = e.x - barW * 0.5;
    const by = e.y - e.r - 10;
    ctx.fillRect(bx, by, barW * hpT, barH);
  }
  ctx.restore();

  // Telegraphs
  for (let i=0;i<visibleFrame.telegraphs.length;i++){
    drawAttackTelegraph(ctx, visibleFrame.telegraphs[i], i, visualNow);
  }

  // Ranged enemy projectiles
  for (let i=0;i<visibleFrame.enemyShots.length;i++){
    const s = visibleFrame.enemyShots[i];
    drawEnemyShot(ctx, s);
  }

  // Arc Lances
  for (let i=0;i<visibleFrame.arcs.length;i++){
    const a = visibleFrame.arcs[i];
    if (!a.points || a.points.length < 2) continue;
    const alpha = clamp(a.life / (a.maxLife || 0.0001), 0, 1);
    const intensity = a.intensity || 1;
    ctx.save();
    ctx.lineCap = "round";
    ctx.lineJoin = "round";

    ctx.globalAlpha = alpha * 0.34;
    ctx.shadowColor = COLORS.bullet;
    ctx.shadowBlur = GLOW.high * intensity;
    ctx.strokeStyle = COLORS.bullet;
    ctx.lineWidth = 9 * intensity;
    if (tracePolyline(ctx, a.points)) ctx.stroke();

    ctx.globalAlpha = alpha;
    ctx.shadowColor = a.color || COLORS.arc;
    ctx.shadowBlur = GLOW.medium * intensity;
    ctx.strokeStyle = a.color || COLORS.arc;
    ctx.lineWidth = 4.2 * intensity;
    if (tracePolyline(ctx, a.points)) ctx.stroke();

    ctx.shadowBlur = 0;
    ctx.globalAlpha = alpha * 0.94;
    ctx.strokeStyle = "rgba(235,253,255,.98)";
    ctx.lineWidth = 1.25 * intensity;
    if (tracePolyline(ctx, a.points)) ctx.stroke();

    ctx.fillStyle = "rgba(235,253,255,.96)";
    for (let j = 0; j < a.points.length; j += 12) {
      const node = a.points[j];
      const nodeR = 2.4 * intensity;
      ctx.save();
      ctx.translate(node.x, node.y);
      ctx.rotate(Math.PI * 0.25);
      ctx.fillRect(-nodeR * 0.5, -nodeR * 0.5, nodeR, nodeR);
      ctx.restore();
    }
    ctx.restore();
  }

  // Rails
  for (let i=0;i<visibleFrame.rails.length;i++){
    const r = visibleFrame.rails[i];
    ctx.save();
    if (r.trail && r.trail.length > 1){
      ctx.lineCap = "round";
      ctx.lineJoin = "round";
      ctx.globalAlpha = 0.2;
      ctx.shadowColor = COLORS.bullet;
      ctx.shadowBlur = GLOW.high;
      ctx.strokeStyle = COLORS.bullet;
      ctx.lineWidth = r.r * 3.6;
      if (tracePolyline(ctx, r.trail, true)) ctx.stroke();

      ctx.globalAlpha = 0.58;
      ctx.shadowColor = COLORS.rail;
      ctx.shadowBlur = GLOW.medium;
      ctx.strokeStyle = COLORS.rail;
      ctx.lineWidth = r.r * 1.75;
      if (tracePolyline(ctx, r.trail, true)) ctx.stroke();

      ctx.globalAlpha = 0.92;
      ctx.shadowBlur = 0;
      ctx.strokeStyle = "rgba(235,253,255,.94)";
      ctx.lineWidth = Math.max(1.2, r.r * 0.42);
      if (tracePolyline(ctx, r.trail, true)) ctx.stroke();
    }
    const railSprite = getProjectileSprite("rail", r.r, COLORS.rail);
    ctx.globalAlpha = 1;
    ctx.translate(r.x, r.y);
    ctx.rotate(Math.atan2(r.vy, r.vx));
    ctx.drawImage(railSprite.canvas, -railSprite.center, -railSprite.center);
    ctx.restore();
  }

  // Bullets
  for (let i=0;i<visibleFrame.bullets.length;i++){
    const b = visibleFrame.bullets[i];
    if (b.isExplosion){
      const a = clamp((b.life) / (b.maxLife || 0.0001), 0, 1);
      const p = 1 - a;
      const rad = (b.r || 0) * p;
      ctx.save();
      ctx.globalAlpha = a * 0.22;
      ctx.fillStyle = b.color || COLORS.bullet;
      ctx.beginPath();
      ctx.arc(b.x, b.y, rad * 0.88, 0, TAU);
      ctx.fill();
      ctx.globalAlpha = a;
      ctx.shadowColor = b.color || COLORS.bullet;
      ctx.shadowBlur = GLOW.high;
      ctx.strokeStyle = b.color || COLORS.bullet;
      ctx.lineWidth = 3.4;
      ctx.beginPath();
      ctx.arc(b.x, b.y, rad, 0, TAU);
      ctx.stroke();
      ctx.shadowBlur = 0;
      ctx.globalAlpha = a * 0.88;
      ctx.strokeStyle = COLORS.arc;
      ctx.lineWidth = 1.2;
      ctx.beginPath();
      ctx.arc(b.x, b.y, rad * 0.72, 0, TAU);
      ctx.stroke();
      ctx.restore();
      continue;
    }
    const magicSprite = getProjectileSprite("magic", b.r, COLORS.bullet);
    ctx.save();
    ctx.translate(b.x, b.y);
    ctx.rotate(Math.atan2(b.vy, b.vx));
    ctx.drawImage(magicSprite.canvas, -magicSprite.center, -magicSprite.center);
    ctx.restore();
  }

  // Missiles
  for (let i=0;i<visibleFrame.missiles.length;i++){
    const m = visibleFrame.missiles[i];
    const ang = Math.atan2(m.vy, m.vx);
    const sprite = getProjectileSprite("missile", m.r, COLORS.missile);
    ctx.save();
    ctx.translate(m.x, m.y);
    ctx.rotate(ang);
    ctx.drawImage(sprite.canvas, -sprite.center, -sprite.center);
    ctx.restore();
  }

  // Axes
  for (let i=0;i<visibleFrame.axes.length;i++){
    const a = visibleFrame.axes[i];
    const sprite = getProjectileSprite("axe", a.r || 12, UI_COLORS.axeShadow);
    ctx.save();
    ctx.translate(a.x, a.y);
    ctx.rotate(a.rot);
    ctx.drawImage(sprite.canvas, -sprite.center, -sprite.center);
    ctx.restore();
  }

  // Orbs
  for (let i=0;i<visibleFrame.orbs.length;i++){
    const o = visibleFrame.orbs[i];
    const pulse = 0.75 + 0.25 * Math.sin(visualTime() * 0.006 + i);
    const r = o.state === "fly" ? o.r : o.radius * 0.4;
    const phase = visualSettings.reducedMotion ? 0 : visualTime() * 0.00042;
    const field = getSingularityOrbFieldSprite(o.radius);
    const body = getSingularityOrbBodySprite(r);

    ctx.save();
    ctx.translate(o.x, o.y);
    ctx.rotate(-phase - i * 0.37);
    ctx.globalAlpha = (o.state === "park" ? 0.52 : 0.3) + pulse * 0.2;
    ctx.drawImage(field.canvas, -field.center, -field.center);
    ctx.restore();

    ctx.save();
    ctx.translate(o.x, o.y);
    ctx.rotate(phase * 2.4 + i * 0.61);
    const bodyScale = o.state === "park" ? 0.97 + pulse * 0.03 : 1;
    ctx.scale(bodyScale, bodyScale);
    ctx.drawImage(body.canvas, -body.center, -body.center);
    ctx.restore();
  }

  // Aura
  if (weapons.aura.unlocked){
    const s = auraStats();
    const auraField = getHolyAuraFieldSprite(s.radius);
    const auraPhase = visualSettings.reducedMotion ? 0 : visualTime() * 0.000155;
    ctx.save();
    ctx.globalAlpha = 0.68;
    ctx.translate(player.x, player.y);
    ctx.rotate(auraPhase);
    ctx.drawImage(auraField.canvas, -auraField.center, -auraField.center);
    ctx.restore();

    if (!visualSettings.reducedMotion && weapons.aura.pulseFx > 0) {
      const maxFx = weapons.aura.pulseFxMax || 0.25;
      const t = clamp(weapons.aura.pulseFx / maxFx, 0, 1);
      const progress = 1 - t;
      const pulseR = s.radius * (0.88 + 0.12 * progress);
      neonRing(ctx, player.x, player.y, pulseR, UI_COLORS.auraStroke, GLOW.medium, 1.8, 0.3 * t);
    }
  }

  // Particles
  ctx.save();
  ctx.lineCap = "round";
  for (let i=0;i<visibleFrame.particles.length;i++){
    const p = visibleFrame.particles[i];
    const a = clamp(p.life / p.maxLife, 0, 1);
    const shape = p.shape === "streak" || p.shape === "shard" ? p.shape : "dot";
    const sprite = getParticleSprite(shape, p.r, p.stretch || 1, p.color);
    if (p.shape === "streak" || p.shape === "shard") {
      ctx.save();
      ctx.translate(p.x, p.y);
      ctx.rotate(Math.atan2(p.vy, p.vx));
      ctx.globalAlpha = a;
      ctx.drawImage(sprite.canvas, -sprite.centerX, -sprite.centerY);
      ctx.restore();
    } else {
      ctx.globalAlpha = a;
      ctx.drawImage(sprite.canvas, p.x - sprite.centerX, p.y - sprite.centerY);
    }
  }
  ctx.restore();

  // Player + buffs
  const flicker = player.iFrame > 0 ? (Math.sin(visualTime() * 0.03) * 0.25 + 0.75) : 1;
  ctx.save();
  ctx.globalAlpha = flicker;
  neonCircle(ctx, player.x, player.y, player.r, COLORS.player, GLOW.high);
  drawPlayerGlyph(ctx, player.x, player.y, player.r);
  ctx.restore();

  if (buffs.shield > 0){
    const a = 0.55 + 0.25 * Math.sin(visualTime()*0.004);
    neonRing(ctx, player.x, player.y, player.r + 10, UI_COLORS.shieldRing, GLOW.high, 2.5, a);
  }
  if (!visualSettings.reducedMotion && buffs.reviveFlash > 0) {
    const t = clamp(buffs.reviveFlash / 1.6, 0, 1);
    const pulse = 0.6 + 0.4 * Math.sin(visualTime() * 0.012);
    const ringR = player.r + 60 + (1 - t) * 120;
    neonRing(ctx, player.x, player.y, ringR, COLORS.heal, GLOW.high, 3.2, 0.7 * t * pulse);
  }
  if (buffs.magnet > 0){
    neonRing(ctx, player.x, player.y, player.r + 18, UI_COLORS.magnetRing, GLOW.medium, 2, 0.55);
  }

  // Player HP bar (always visible)
  const playerHp = clamp(player.hp, 0, player.maxHp);
  const playerHpT = player.maxHp > 0 ? clamp(playerHp / player.maxHp, 0, 1) : 0;
  const playerBarW = player.r * 2 + 18;
  const playerBarH = 5;
  const playerBarX = player.x - playerBarW * 0.5;
  const playerBarY = player.y - player.r - 18;
  ctx.save();
  ctx.fillStyle = UI_COLORS.hpBarBg;
  ctx.fillRect(playerBarX, playerBarY, playerBarW, playerBarH);
  ctx.shadowColor = UI_COLORS.hpBarShadow;
  ctx.shadowBlur = 0;
  ctx.fillStyle = UI_COLORS.hpBarPlayerFill;
  ctx.fillRect(playerBarX, playerBarY, playerBarW * playerHpT, playerBarH);
  ctx.shadowBlur = 0;
  ctx.strokeStyle = UI_COLORS.strokeDim;
  ctx.lineWidth = 1;
  ctx.strokeRect(playerBarX, playerBarY, playerBarW, playerBarH);
  ctx.restore();

  for (let i=0;i<visibleFrame.companions.length;i++){
    const c = visibleFrame.companions[i];
    const pulse = 0.7 + 0.3 * Math.sin(visualTime() * 0.006 + i);
    drawCompanionGlyph(ctx, c);
    neonRing(ctx, c.x, c.y, c.r + 6, c.color, GLOW.medium, 1.5, 0.5 * pulse);
  }

  for (let i=0;i<visibleFrame.damageTexts.length;i++){
    const d = visibleFrame.damageTexts[i];
    const a = clamp(d.life / d.maxLife, 0, 1);
    drawDamageText(ctx, d, a);
  }
  for (let i=0;i<visibleFrame.floatTexts.length;i++){
    const t = visibleFrame.floatTexts[i];
    const a = clamp(t.life / t.maxLife, 0, 1);
    drawTextWorld(ctx, t.x, t.y, t.text, t.color, t.size, a);
  }

  ctx.restore();

  drawCombatScreenFx(ctx, W, H, camX, camY);

  if (!visualSettings.reducedMotion && buffs.reviveFlash > 0) {
    const t = clamp(buffs.reviveFlash / 1.6, 0, 1);
    ctx.save();
    ctx.globalAlpha = 0.32 * t;
    ctx.strokeStyle = COLORS.heal;
    ctx.shadowColor = COLORS.heal;
    ctx.shadowBlur = GLOW.high;
    ctx.lineWidth = 10 * t + 2;
    ctx.strokeRect(7, 7, W - 14, H - 14);
    ctx.restore();
  }

  if (startNoticeT > 0){
    const a = clamp(startNoticeT / START_NOTICE_TIME, 0, 1);
    ctx.save();
    ctx.globalAlpha = a;
    ctx.font = `700 ${isMobile ? 16 : 20}px ui-sans-serif, system-ui, -apple-system, Segoe UI, Roboto, Arial`;
    ctx.textAlign = "center";
    ctx.textBaseline = "middle";
    ctx.fillStyle = COLORS.text;
    ctx.shadowColor = COLORS.player;
    ctx.shadowBlur = GLOW.medium * a;
    const lineGap = isMobile ? 20 : 26;
    ctx.fillText(`Remember you are limited to ${MAX_WEAPONS} weapons and to ${TRINKET_CONFIG.slots} trinkets`, W * 0.5, H * 0.4);
    ctx.fillText("Bonus chests more likely to heal at low HP.", W * 0.5, H * 0.4 + lineGap);
    ctx.restore();
  }

  drawChestIndicators(ctx, W, H, camX, camY);
  drawQuestIndicators(ctx, W, H, camX, camY);

  if (state === STATE.LEVELUP || state === STATE.TRINKET || state === STATE.AUG || state === STATE.COMPANION){
    ctx.save();
    ctx.fillStyle = UI_COLORS.overlayDim;
    ctx.fillRect(0,0,W,H);
    ctx.restore();
  }

  // HUD
  const hudNow = Number.isFinite(frameNow) ? frameNow : performance.now();
  if (hudNow < nextHudUpdateAt) return;
  nextHudUpdateAt = hudNow + HUD_UPDATE_INTERVAL_MS;

  setHudText(ui.time, fmtTime(player.time));
  setHudText(ui.level, String(player.level));
  setHudText(ui.kills, String(player.kills));
  setHudText(ui.armor, String(Math.round(player.armor || 0)));
  const hudResists = player.resists || {};
  const allResist = hudResists.all || 0;
  setHudText(ui.resFire, `${Math.round(clamp(allResist + (hudResists.fire || 0), 0, 1) * 100)}%`);
  setHudText(ui.resPoison, `${Math.round(clamp(allResist + (hudResists.poison || 0), 0, 1) * 100)}%`);
  setHudText(ui.resVoid, `${Math.round(clamp(allResist + (hudResists.void || 0), 0, 1) * 100)}%`);
  if (ui.mTime){
    setHudText(ui.mTime, fmtTime(player.time));
    setHudText(ui.mLevel, `Lv ${player.level}`);
    setHudText(ui.mKills, `K ${player.kills}`);
  }

  const hp = clamp(player.hp, 0, player.maxHp);
  setHudText(ui.hp, `${Math.ceil(hp)} / ${Math.ceil(player.maxHp)}`);
  const hpT = player.maxHp > 0 ? clamp(hp / player.maxHp, 0, 1) : 0;
  setHudText(ui.hpPct, `${Math.round(hpT * 100)}%`);
  setHudWidth(ui.hpFill, `${(hpT*100).toFixed(2)}%`);
  if (ui.mHp){
    setHudText(ui.mHp, `${Math.ceil(hp)}/${Math.ceil(player.maxHp)}`);
    setHudText(ui.mHpPct, `${Math.round(hpT * 100)}%`);
    setHudWidth(ui.mHpFill, `${(hpT*100).toFixed(2)}%`);
  }

  const bosses = bossesScratch;
  bosses.length = 0;
  for (let i = 0; i < enemies.length; i++) {
    const e = enemies[i];
    if (e.alive && e.boss) bosses.push(e);
  }
  const primaryBoss = bosses[0] || boss || null;
  const bossHp = primaryBoss ? clamp(primaryBoss.hp, 0, primaryBoss.maxHp) : 0;
  const bossHpT = primaryBoss && primaryBoss.maxHp > 0 ? clamp(bossHp / primaryBoss.maxHp, 0, 1) : 0;
  const bossName = primaryBoss ? (ENEMY_TYPES[primaryBoss.type]?.name || "Boss") : "";
  const bossOn = bosses.length > 0;
  if (ui.hud) ui.hud.classList.toggle("boss-active", bossOn);
  if (ui.bossWrap) ui.bossWrap.classList.toggle("on", bossOn);
  if (ui.bossCard){
    ui.bossCard.classList.toggle("on", bossOn);
    if (bossOn){
      if (ui.bossBars){
        const bossMarkup = bosses.map((b) => {
          const hp = clamp(b.hp, 0, b.maxHp);
          const hpT = b.maxHp > 0 ? clamp(hp / b.maxHp, 0, 1) : 0;
          const name = ENEMY_TYPES[b.type]?.name || "Boss";
          return `
            <div class="bossRow">
              <div class="barRow">
                <div class="barLabel">${name}</div>
                <div class="barValue"><span>${Math.round(hpT * 100)}%</span></div>
              </div>
              <div class="barOuter"><div class="barInner boss" style="width:${(hpT*100).toFixed(2)}%"></div></div>
              <div class="barValue sub"><span>${Math.ceil(hp)} / ${Math.ceil(b.maxHp)}</span></div>
            </div>
          `;
        }).join("");
        setHudHtml(ui.bossBars, bossMarkup);
      }
      setHudText(ui.bossName, bossName);
      setHudText(ui.bossHp, `${Math.ceil(bossHp)} / ${Math.ceil(primaryBoss.maxHp)}`);
      setHudText(ui.bossHpPct, `${Math.round(bossHpT * 100)}%`);
      setHudWidth(ui.bossHpFill, `${(bossHpT*100).toFixed(2)}%`);
    } else if (ui.bossBars) {
      setHudHtml(ui.bossBars, "");
    }
  }
  if (ui.mBossBar){
    ui.mBossBar.classList.toggle("on", bossOn);
    if (bossOn){
      const label = bosses.length > 1 ? `Bosses (${bosses.length})` : (bossName || "Boss");
      setHudText(ui.mBossName, label);
      setHudText(ui.mBossPct, `${Math.round(bossHpT * 100)}%`);
      setHudWidth(ui.mBossFill, `${(bossHpT*100).toFixed(2)}%`);
    } else {
      setHudText(ui.mBossName, "Boss");
      setHudText(ui.mBossPct, "0%");
      setHudWidth(ui.mBossFill, "0%");
    }
  }

  setHudText(ui.xp, fmtFloat(player.xp, 1));
  setHudText(ui.xpNeed, fmtFloat(player.xpNeed, 1));
  const xpT = player.xpNeed > 0 ? clamp(player.xp / player.xpNeed, 0, 1) : 0;
  setHudWidth(ui.xpFill, `${(xpT*100).toFixed(2)}%`);
  if (ui.mXp){
    setHudText(ui.mXp, fmtFloat(player.xp, 1));
    setHudText(ui.mXpNeed, fmtFloat(player.xpNeed, 1));
    setHudWidth(ui.mXpFill, `${(xpT*100).toFixed(2)}%`);
  }

  if (ui.buffs){
    const buffParts = buffPartsScratch;
    buffParts.length = 0;
    if (buffs.shield > 0) buffParts.push(`Shield ${fmtFloat(buffs.shield, 1)}s`);
    if (buffs.magnet > 0) buffParts.push(`Magnet ${fmtFloat(buffs.magnet, 1)}s`);
    if (buffs.slow > 0) buffParts.push(`Freeze ${fmtFloat(buffs.slow, 1)}s`);
    if (buffs.power > 0) buffParts.push(`Overcharge ${fmtFloat(buffs.power, 1)}s`);
    if (buffs.haste > 0) buffParts.push(`Haste ${fmtFloat(buffs.haste, 1)}s`);
    if (buffs.xp > 0) buffParts.push(`XP Boost ${fmtFloat(buffs.xp, 1)}s`);
    const buffText = buffParts.length ? buffParts.join(" | ") : "-";
    setHudHtml(ui.buffs, `<b>Buffs:</b> ${buffText}`);
    ui.buffs.classList.toggle("empty", buffParts.length === 0);
    if (ui.mBuffs) {
      setHudText(ui.mBuffs, `Buffs: ${buffText}`);
      ui.mBuffs.classList.toggle("empty", buffParts.length === 0);
    }
  } else if (ui.mBuffs) {
    setHudText(ui.mBuffs, "Buffs: -");
    ui.mBuffs.classList.add("empty");
  }
  if (ui.quest){
    const questText = getQuestHudText();
    setHudHtml(ui.quest, `<b>Quest:</b> ${questText}`);
    ui.quest.classList.toggle("empty", questText === "-");
    if (ui.mQuest) {
      setHudText(ui.mQuest, `Quest: ${questText}`);
      ui.mQuest.classList.toggle("empty", questText === "-");
    }
  } else if (ui.mQuest) {
    setHudText(ui.mQuest, "Quest: -");
    ui.mQuest.classList.add("empty");
  }
  if (ui.mMeta){
    setHudText(ui.mMeta, `Trinkets ${trinkets.length} | Companions ${companions.length}`);
  }
}
