import { COLORS, CHEST_CONFIG, WEAPON_CONFIG, ELITE_CONFIG, MAX_WEAPONS, ENEMY_TYPES, TRINKET_CONFIG } from "./config.js";
import { clamp, TAU, fmtFloat } from "./math.js";
import { weapons, auraStats } from "./weapons.js";
import { getQuestHudText } from "./quests.js";
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
  axes,
  orbs,
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

const UI_COLORS = {
  strokeDim: "rgba(255,255,255,.10)",
  textStroke: "rgba(0,0,0,.35)",
  chestFill: "rgba(70,255,143,0.18)",
  trinketFill: "rgba(124,255,217,0.18)",
  augFill: "rgba(141,123,255,0.18)",
  companionFill: "rgba(154,255,106,0.18)",
  questFill: "rgba(255,184,74,0.18)",
  auraFill: COLORS.voidAura,
  auraStroke: COLORS.voidAuraStroke,
  playerGlow: COLORS.playerGlow,
  playerCore: COLORS.playerCore,
  shieldRing: COLORS.auraRingShield,
  magnetRing: COLORS.magnetRing,
  overlayDim: COLORS.overlayDim,
  orbRing: "rgba(177,96,255,.35)",
  hpBarBg: "rgba(255,255,255,.12)",
  hpBarShadow: "rgba(37,240,255,.6)",
  hpBarFill: "rgba(255, 37, 37, 0.75)",
  axeShadow: "rgba(177,96,255,.9)",
  axeBody: "rgba(177,96,255,.95)",
  axeEdge: "rgba(37,240,255,.95)",
  railTrailBase: "rgba(154,245,255,", // used with alpha injected
};

const enemySpriteCache = new Map();
const gemSpriteCache = new Map();

function makeOffscreenCanvas(w, h) {
  if (typeof OffscreenCanvas !== "undefined") return new OffscreenCanvas(w, h);
  const c = document.createElement("canvas");
  c.width = w;
  c.height = h;
  return c;
}

const getEnemySprite = (r, color) => {
  const key = `${r}|${color}`;
  let sprite = enemySpriteCache.get(key);
  if (sprite) return sprite;
  const pad = 18;
  const size = Math.ceil(r * 2);
  const c = makeOffscreenCanvas(size + pad * 2, size + pad * 2);
  const g = c.getContext("2d");
  g.shadowColor = color;
  g.shadowBlur = 16;
  g.fillStyle = color;
  g.strokeStyle = UI_COLORS.strokeDim;
  g.lineWidth = 1;
  g.fillRect(pad, pad, size, size);
  g.shadowBlur = 0;
  g.strokeRect(pad, pad, size, size);
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
  g.shadowColor = COLORS.gem;
  g.shadowBlur = 10;
  g.fillStyle = COLORS.gem;
  g.beginPath();
  g.arc(pad + r, pad + r, r, 0, TAU);
  g.fill();
  g.shadowBlur = 0;
  g.strokeStyle = UI_COLORS.strokeDim;
  g.lineWidth = 1;
  g.beginPath();
  g.arc(pad + r, pad + r, r, 0, TAU);
  g.stroke();
  sprite = { canvas: c, pad, r };
  gemSpriteCache.set(key, sprite);
  return sprite;
};

function drawGrid(ctx, W, H, camX, camY) {
  const step = 64;
  const x0 = Math.floor(camX / step) * step;
  const y0 = Math.floor(camY / step) * step;

  ctx.save();
  ctx.strokeStyle = COLORS.grid;
  ctx.lineWidth = 1;

  ctx.beginPath();
  for (let x=x0; x<camX+W+step; x+=step){ ctx.moveTo(x, camY); ctx.lineTo(x, camY+H); }
  for (let y=y0; y<camY+H+step; y+=step){ ctx.moveTo(camX, y); ctx.lineTo(camX+W, y); }
  ctx.stroke();
  ctx.restore();
}

function neonCircle(ctx, x,y,r,fill,glow=16,alpha=1){
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

function neonRing(ctx, x,y,r,stroke,glow=18,lw=2,alpha=1){
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

function neonRect(ctx, x,y,w,h,fill,glow=16){
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

function drawEnemyShot(ctx, s) {
  if (s.homing) {
    const ang = Math.atan2(s.vy, s.vx);
    const w = s.r * 3.2;
    const h = s.r * 1.2;
    ctx.save();
    ctx.translate(s.x, s.y);
    ctx.rotate(ang);
    ctx.shadowColor = s.color;
    ctx.shadowBlur = 18;
    ctx.fillStyle = s.color;
    ctx.fillRect(-w * 0.5, -h * 0.5, w, h);
    ctx.shadowBlur = 0;
    ctx.strokeStyle = UI_COLORS.strokeDim;
    ctx.lineWidth = 1;
    ctx.strokeRect(-w * 0.5, -h * 0.5, w, h);
    ctx.restore();
    return;
  }
  neonCircle(ctx, s.x, s.y, s.r, s.color, 18);
}

function drawTextWorld(ctx, x,y,text,color,size,alpha){
  ctx.save();
  ctx.globalAlpha = alpha;
  ctx.font = `700 ${size}px ui-sans-serif, system-ui, -apple-system, Segoe UI, Roboto, Arial`;
  ctx.textAlign = "center";
  ctx.textBaseline = "middle";
  ctx.shadowColor = color;
  ctx.shadowBlur = 18;
  ctx.fillStyle = color;
  ctx.fillText(text, x, y);
  ctx.shadowBlur = 0;
  ctx.strokeStyle = UI_COLORS.textStroke;
  ctx.lineWidth = 3;
  ctx.strokeText(text, x, y);
  ctx.restore();
}

function drawChestIndicators(ctx, W, H, camX, camY){
  const cx = W * 0.5, cy = H * 0.5;
  const margin = CHEST_CONFIG.indicatorMargin;
  const minX = margin, maxX = W - margin;
  const minY = margin, maxY = H - margin;
  const size = CHEST_CONFIG.indicatorSize;
  const pulse = 0.65 + 0.35 * Math.sin(performance.now() * 0.008);

  ctx.save();
  ctx.lineWidth = 2;
  ctx.shadowBlur = 14;

  for (let i=0;i<chests.length;i++){
    const c = chests[i];
    if (!c.alive) continue;
    const chestColor = c.kind === "trinket"
      ? COLORS.trinket
      : (c.kind === "aug"
        ? COLORS.aug
        : (c.kind === "companion" ? COLORS.companionCage : COLORS.chest));
    ctx.shadowColor = chestColor;
    ctx.strokeStyle = chestColor;
    ctx.fillStyle = c.kind === "trinket"
      ? UI_COLORS.trinketFill
      : (c.kind === "aug"
        ? UI_COLORS.augFill
        : (c.kind === "companion" ? UI_COLORS.companionFill : UI_COLORS.chestFill));
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
    ctx.moveTo(s, 0);
    ctx.lineTo(-s * 0.9, s * 0.78);
    ctx.lineTo(-s * 0.9, -s * 0.78);
    ctx.closePath();
    ctx.fill();
    ctx.stroke();
    ctx.restore();
  }

  ctx.restore();
}

function drawOffscreenIndicator(ctx, W, H, camX, camY, x, y, color, fill, sizeScale = 1) {
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
  ctx.shadowBlur = 14;
  ctx.shadowColor = color;
  ctx.strokeStyle = color;
  ctx.fillStyle = fill || color;
  ctx.beginPath();
  ctx.moveTo(size, 0);
  ctx.lineTo(-size * 0.9, size * 0.7);
  ctx.lineTo(-size * 0.9, -size * 0.7);
  ctx.closePath();
  ctx.fill();
  ctx.stroke();
  ctx.restore();
}

function drawQuestIndicators(ctx, W, H, camX, camY) {
  const pulse = 0.65 + 0.35 * Math.sin(performance.now() * 0.008);
  if (quest.giverActive && (!quest.active || quest.completed)) {
    const qColor = quest.completed ? COLORS.gold : COLORS.quest;
    drawOffscreenIndicator(ctx, W, H, camX, camY, quest.giverX, quest.giverY, qColor, UI_COLORS.questFill, 1.2 * pulse);
  }
  if (!quest.active || quest.completed) return;
  if (quest.type !== "scavenge" && quest.type !== "drop") return;
  for (let i = 0; i < questItems.length; i++) {
    const it = questItems[i];
    if (!it.alive) continue;
    const color = it.type === "drop" ? COLORS.gold : COLORS.quest;
    drawOffscreenIndicator(ctx, W, H, camX, camY, it.x, it.y, color, UI_COLORS.questFill, 0.9 * pulse);
  }
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
}){
  ctx.fillStyle = COLORS.bg;
  ctx.fillRect(0,0,W,H);

  ctx.save();
  ctx.translate(-camX, -camY);

  drawGrid(ctx, W, H, camX, camY);

  // Map bounds glow
  ctx.save();
  const bLeft = -WORLD.halfSize;
  const bTop = -WORLD.halfSize;
  const bSize = WORLD.halfSize * 2;
  ctx.globalAlpha = 0.8;
  ctx.strokeStyle = COLORS.auraRingShield;
  ctx.lineWidth = 6;
  ctx.shadowColor = COLORS.auraRingShield;
  ctx.shadowBlur = 24;
  ctx.strokeRect(bLeft, bTop, bSize, bSize);
  ctx.globalAlpha = 0.5;
  ctx.lineWidth = 4;
  ctx.shadowBlur = 12;
  ctx.strokeRect(bLeft + 10, bTop + 10, bSize - 20, bSize - 20);
  ctx.restore();

  const visMinX = camX - WORLD.spawnPad, visMaxX = camX + W + WORLD.spawnPad;
  const visMinY = camY - WORLD.spawnPad, visMaxY = camY + H + WORLD.spawnPad;

  // Obstacles
  ctx.save();
  for (let i=0;i<activeObstacles.length;i++){
    const o = obstacles[activeObstacles[i]];
    const r = o.r;
    if (o.x + r < visMinX || o.x - r > visMaxX || o.y + r < visMinY || o.y - r > visMaxY) continue;
    if (o.type === "lake"){
      ctx.globalAlpha = 0.98;
      ctx.shadowColor = "transparent";
      ctx.shadowBlur = 0;
      ctx.fillStyle = COLORS.lake;
      ctx.beginPath();
      ctx.arc(o.x, o.y, r, 0, TAU);
      ctx.fill();
    } else if (o.type === "forest"){
      const hpT = o.maxHp > 0 ? clamp(o.hp / o.maxHp, 0.15, 1) : 1;
      ctx.globalAlpha = 0.25 + 0.75 * hpT;
      ctx.shadowColor = "transparent";
      ctx.shadowBlur = 0;
      ctx.fillStyle = COLORS.forest;
      ctx.beginPath();
      ctx.arc(o.x, o.y, r, 0, TAU);
      ctx.fill();
    } else {
      const hpT = o.maxHp > 0 ? clamp(o.hp / o.maxHp, 0.15, 1) : 1;
      ctx.globalAlpha = 0.25 + 0.75 * hpT;
      ctx.shadowColor = "transparent";
      ctx.shadowBlur = 0;
      ctx.fillStyle = COLORS.rock;
      ctx.beginPath();
      ctx.arc(o.x, o.y, r, 0, TAU);
      ctx.fill();
    }
  }
  ctx.restore();

  // cull void zones
  for (let i=0;i<voidZones.length;i++){
    const z = voidZones[i];
    if (z.x < camX - WORLD.spawnPad || z.x > camX + W + WORLD.spawnPad || z.y < camY - WORLD.spawnPad || z.y > camY + H + WORLD.spawnPad) continue;
    const a = clamp(z.life / z.maxLife, 0, 1);
    const pulse = 0.9 + 0.1 * Math.sin(performance.now() * 0.006 + i);
    ctx.save();
    ctx.globalAlpha = 0.25 + 0.55 * a;
    ctx.shadowColor = z.color;
    ctx.shadowBlur = 24;
    ctx.fillStyle = z.color;
    ctx.beginPath();
    ctx.arc(z.x, z.y, z.radius * pulse, 0, TAU);
    ctx.fill();
    ctx.shadowBlur = 0;
    ctx.strokeStyle = z.color;
    ctx.lineWidth = 2;
    ctx.globalAlpha = 0.35 + 0.35 * a;
    ctx.beginPath();
    ctx.arc(z.x, z.y, z.radius, 0, TAU);
    ctx.stroke();
    ctx.restore();
  }

  for (let i=0;i<chests.length;i++){
    const c = chests[i];
    if (c.x < camX - WORLD.spawnPad || c.x > camX + W + WORLD.spawnPad || c.y < camY - WORLD.spawnPad || c.y > camY + H + WORLD.spawnPad) continue;
    const chestColor = c.kind === "trinket"
      ? COLORS.trinket
      : (c.kind === "aug"
        ? COLORS.aug
        : (c.kind === "companion" ? COLORS.companionCage : COLORS.chest));
    const ringColor = c.kind === "trinket"
      ? COLORS.trinket
      : (c.kind === "aug"
        ? COLORS.aug
        : (c.kind === "companion" ? COLORS.companionCage : COLORS.gold));
    const pulse = (Math.sin(c.pulse) * 0.15 + 0.85);
    const rr = c.r * (1.0 + 0.05 * Math.sin(c.pulse * 1.7));
    neonRect(ctx, c.x - rr, c.y - rr, rr*2, rr*2, chestColor, 28);
    neonRing(ctx, c.x, c.y, rr*1.45, ringColor, 32, 2, pulse);
    neonRing(ctx, c.x, c.y, rr*2.0, ringColor, 22, 1.5, 0.45 * pulse);
    if (c.kind === "companion") {
      const bar = rr * 0.7;
      ctx.save();
      ctx.translate(c.x, c.y);
      ctx.strokeStyle = ringColor;
      ctx.shadowColor = ringColor;
      ctx.shadowBlur = 18;
      ctx.lineWidth = 2;
      ctx.beginPath();
      ctx.moveTo(-bar, -bar);
      ctx.lineTo(-bar, bar);
      ctx.moveTo(bar, -bar);
      ctx.lineTo(bar, bar);
      ctx.moveTo(-bar, 0);
      ctx.lineTo(bar, 0);
      ctx.moveTo(0, -bar);
      ctx.lineTo(0, bar);
      ctx.stroke();
      ctx.restore();
    }
  }

  if (quest.giverActive) {
    const qPulse = 0.7 + 0.3 * Math.sin(performance.now() * 0.006);
    const qColor = quest.completed ? COLORS.gold : COLORS.quest;
    const qr = quest.giverR * (1.0 + 0.08 * qPulse);
    const exBob = Math.sin(performance.now() * 0.006) * 3;

    ctx.save();
    ctx.shadowColor = qColor;
    ctx.shadowBlur = 18;
    ctx.fillStyle = UI_COLORS.questFill;
    ctx.beginPath();
    ctx.arc(quest.giverX, quest.giverY, qr, 0, TAU);
    ctx.fill();
    ctx.shadowBlur = 0;
    ctx.strokeStyle = UI_COLORS.strokeDim;
    ctx.lineWidth = 1;
    ctx.stroke();
    ctx.restore();

    ctx.save();
    ctx.translate(quest.giverX, quest.giverY);
    ctx.shadowColor = qColor;
    ctx.shadowBlur = 16;
    ctx.fillStyle = qColor;
    ctx.strokeStyle = UI_COLORS.strokeDim;
    ctx.lineWidth = 1;
    const tri = qr * 0.75;
    ctx.beginPath();
    ctx.moveTo(0, -tri);
    ctx.lineTo(tri * 0.85, tri * 0.7);
    ctx.lineTo(-tri * 0.85, tri * 0.7);
    ctx.closePath();
    ctx.fill();
    ctx.shadowBlur = 0;
    ctx.stroke();
    ctx.restore();

    neonRing(ctx, quest.giverX, quest.giverY, qr * 1.5, qColor, 24, 2, 0.8 * qPulse);

    if (!quest.active || quest.completed) {
      const mark = quest.completed ? "?" : "!";
      ctx.save();
      ctx.font = "700 20px ui-sans-serif, system-ui, -apple-system, Segoe UI, Roboto, Arial";
      ctx.textAlign = "center";
      ctx.textBaseline = "middle";
      ctx.shadowColor = qColor;
      ctx.shadowBlur = 16;
      ctx.fillStyle = qColor;
      ctx.fillText(mark, quest.giverX, quest.giverY - qr * 1.9 + exBob);
      ctx.restore();
    }
  }

  for (let i=0;i<questItems.length;i++){
    const it = questItems[i];
    if (!it.alive) continue;
    if (it.x < camX - WORLD.spawnPad || it.x > camX + W + WORLD.spawnPad || it.y < camY - WORLD.spawnPad || it.y > camY + H + WORLD.spawnPad) continue;
    const color = COLORS.quest;
    const size = it.r;
    neonRect(ctx, it.x - size, it.y - size, size * 2, size * 2, color, 16);
    neonRing(ctx, it.x, it.y, size * 1.3, color, 18, 2, 0.7);
  }

  for (let i=0;i<gems.length;i++){
    const g = gems[i];
    if (g.x < camX - WORLD.spawnPad || g.x > camX + W + WORLD.spawnPad || g.y < camY - WORLD.spawnPad || g.y > camY + H + WORLD.spawnPad) continue;
    const lifeT = g.maxLife > 0 ? clamp(g.life / g.maxLife, 0, 1) : 1;
    const a = lifeT > 0.2 ? 1 : (lifeT / 0.2);
    ctx.save();
    ctx.globalAlpha = a;
    const sprite = getGemSprite(g.r);
    ctx.drawImage(sprite.canvas, g.x - sprite.r - sprite.pad, g.y - sprite.r - sprite.pad);
    ctx.restore();
  }

  // Enemies: cached sprite draw (reduces per-frame shadowBlur cost)
  for (let i=0;i<enemies.length;i++){
    const e = enemies[i];
    if (!e.alive) continue;
    if (e.x < visMinX || e.x > visMaxX || e.y < visMinY || e.y > visMaxY) continue;
    const sprite = getEnemySprite(e.r, e.color);
    ctx.drawImage(sprite.canvas, e.x - sprite.r - sprite.pad, e.y - sprite.r - sprite.pad);
  }

  // Elite outline pass
  ctx.save();
  ctx.shadowColor = ELITE_CONFIG.markerColor;
  ctx.shadowBlur = 18;
  ctx.strokeStyle = ELITE_CONFIG.markerColor;
  ctx.lineWidth = 2.6;
  for (let i=0;i<enemies.length;i++){
    const e = enemies[i];
    if (!e.alive || !e.elite) continue;
    if (e.x < visMinX || e.x > visMaxX || e.y < visMinY || e.y > visMaxY) continue;
    const size = e.r * 1.1;
    ctx.strokeRect(e.x - e.r, e.y - e.r, size * 2, size * 2);
  }
  ctx.restore();

  // Enemy HP bars
  ctx.save();
  ctx.fillStyle = UI_COLORS.hpBarBg;
  for (let i=0;i<enemies.length;i++){
    const e = enemies[i];
    if (!e.alive) continue;
    if (e.x < visMinX || e.x > visMaxX || e.y < visMinY || e.y > visMaxY) continue;
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
  for (let i=0;i<enemies.length;i++){
    const e = enemies[i];
    if (!e.alive) continue;
    if (e.x < visMinX || e.x > visMaxX || e.y < visMinY || e.y > visMaxY) continue;
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
  for (let i=0;i<telegraphs.length;i++){
    const tg = telegraphs[i];
    const a = clamp(tg.t / tg.max, 0, 1);
    const r = tg.radius * (0.9 + 0.15 * Math.sin(performance.now() * 0.008 + i));
    ctx.save();
    ctx.globalAlpha = 0.25 + 0.55 * (1 - a);
    ctx.shadowColor = tg.color;
    ctx.shadowBlur = 20;
    ctx.strokeStyle = tg.color;
    ctx.lineWidth = tg.width || 3;
    ctx.beginPath();
    ctx.arc(tg.x, tg.y, r, 0, TAU);
    ctx.stroke();
    if (tg.dx || tg.dy){
      const ang = Math.atan2(tg.dy, tg.dx);
      ctx.lineWidth = 2;
      ctx.beginPath();
      ctx.moveTo(tg.x, tg.y);
      ctx.lineTo(tg.x + Math.cos(ang) * tg.radius * 0.9, tg.y + Math.sin(ang) * tg.radius * 0.9);
      ctx.stroke();
    }
    if (tg.label){
      ctx.shadowBlur = 0;
      ctx.globalAlpha = 0.9;
      ctx.fillStyle = tg.color;
      ctx.font = "700 14px ui-sans-serif, system-ui, -apple-system, Segoe UI, Roboto, Arial";
      ctx.textAlign = "center";
      ctx.textBaseline = "middle";
      ctx.fillText(tg.label, tg.x, tg.y);
    }
    ctx.restore();
  }

  // Ranged enemy projectiles
  for (let i=0;i<enemyShots.length;i++){
    const s = enemyShots[i];
    drawEnemyShot(ctx, s);
  }

  // Rails
  for (let i=0;i<rails.length;i++){
    const r = rails[i];
    const trailLife = WEAPON_CONFIG.rail.projectile.trailLife;
    ctx.save();
    if (r.trail && r.trail.length > 1){
      ctx.lineCap = "round";
      for (let j=1;j<r.trail.length;j++){
        const a = clamp(r.trail[j].life / trailLife, 0, 1);
        const w = (r.r * 2.2) * a + 2;
        ctx.strokeStyle = `${UI_COLORS.railTrailBase}${0.14 + 0.32 * a})`;
        ctx.shadowColor = COLORS.rail;
        ctx.shadowBlur = 18 * a;
        ctx.lineWidth = w;
        ctx.beginPath();
        ctx.moveTo(r.trail[j-1].x, r.trail[j-1].y);
        ctx.lineTo(r.trail[j].x, r.trail[j].y);
        ctx.stroke();
      }
    }
    ctx.shadowColor = COLORS.rail;
    ctx.shadowBlur = 24;
    ctx.fillStyle = COLORS.rail;
    ctx.beginPath();
    ctx.ellipse(r.x, r.y, r.r * 1.6, r.r, Math.atan2(r.vy, r.vx), 0, TAU);
    ctx.fill();
    ctx.restore();
  }

  // Bullets
  for (let i=0;i<bullets.length;i++){
    const b = bullets[i];
    if (b.isExplosion){
      const a = clamp((b.life) / (b.maxLife || 0.0001), 0, 1);
      const p = 1 - a;
      const rad = (b.r || 0) * p;
      ctx.save();
      ctx.globalAlpha = a;
      ctx.shadowColor = b.color || COLORS.bullet;
      ctx.shadowBlur = 20;
      ctx.strokeStyle = b.color || COLORS.bullet;
      ctx.lineWidth = 3;
      ctx.beginPath();
      ctx.arc(b.x, b.y, rad, 0, TAU);
      ctx.stroke();
      ctx.restore();
      continue;
    }
    neonCircle(ctx, b.x, b.y, b.r, COLORS.bullet, 18);
  }

  // Missiles
  for (let i=0;i<missiles.length;i++){
    const m = missiles[i];
    const ang = Math.atan2(m.vy, m.vx);
    const len = m.r * 3;
    const w = m.r;
    ctx.save();
    ctx.translate(m.x, m.y);
    ctx.rotate(ang);
    ctx.shadowColor = COLORS.missile;
    ctx.shadowBlur = 12;
    ctx.fillStyle = COLORS.missile;
    ctx.strokeStyle = COLORS.missileStroke;
    ctx.lineWidth = 1.4;
    ctx.beginPath();
    ctx.roundRect(-len*0.5, -w*0.6, len, w*1.2, w*0.6);
    ctx.fill();
    ctx.stroke();
    ctx.restore();
  }

  // Axes
  for (let i=0;i<axes.length;i++){
    const a = axes[i];
    ctx.save();
    ctx.translate(a.x, a.y);
    ctx.rotate(a.rot);
    ctx.shadowColor = UI_COLORS.axeShadow;
    ctx.shadowBlur = 18;
    ctx.fillStyle = UI_COLORS.axeBody;
    ctx.fillRect(-10, -3, 20, 6);
    ctx.fillStyle = UI_COLORS.axeEdge;
    ctx.fillRect(-2, -12, 4, 24);
    ctx.shadowBlur = 0;
    ctx.strokeStyle = UI_COLORS.hpBarBg;
    ctx.strokeRect(-10, -3, 20, 6);
    ctx.restore();
  }

  // Orbs
  for (let i=0;i<orbs.length;i++){
    const o = orbs[i];
    const pulse = 0.75 + 0.25 * Math.sin(performance.now() * 0.006 + i);
    const r = o.state === "fly" ? o.r : o.radius * 0.4;
    neonCircle(ctx, o.x, o.y, r, COLORS.bullet, 18, 0.8);
    neonRing(ctx, o.x, o.y, o.radius, UI_COLORS.orbRing, 24, 2, 0.35 * pulse);
    if (o.state === "park"){
      neonRing(ctx, o.x, o.y, o.radius, UI_COLORS.orbRing, 24, 2, 0.6 * pulse);
    }
  }

  // Aura
  if (weapons.aura.unlocked){
    const s = auraStats();
    ctx.save();
    ctx.globalAlpha = 0.92;
    ctx.shadowColor = COLORS.chest;
    ctx.shadowBlur = 26;
    ctx.fillStyle = UI_COLORS.auraFill;
    ctx.beginPath();
    ctx.arc(player.x, player.y, s.radius, 0, TAU);
    ctx.fill();
    ctx.shadowBlur = 16;
    ctx.strokeStyle = UI_COLORS.auraStroke;
    ctx.lineWidth = 2;
    ctx.stroke();
    ctx.restore();

    if (weapons.aura.pulseFx > 0) {
      const maxFx = weapons.aura.pulseFxMax || 0.25;
      const t = clamp(weapons.aura.pulseFx / maxFx, 0, 1);
      const progress = 1 - t;
      const pulseR = s.radius * (0.3 + 0.7 * progress);
      neonRing(ctx, player.x, player.y, pulseR, UI_COLORS.auraStroke, 28, 3, 0.8 * t);
    }
  }

  // Particles
  for (let i=0;i<particles.length;i++){
    const p = particles[i];
    if (p.x < camX - WORLD.spawnPad || p.x > camX + W + WORLD.spawnPad || p.y < camY - WORLD.spawnPad || p.y > camY + H + WORLD.spawnPad) continue;
    const a = clamp(p.life / p.maxLife, 0, 1);
    ctx.save();
    ctx.globalAlpha = a;
    ctx.shadowColor = p.color;
    ctx.shadowBlur = 14;
    ctx.fillStyle = p.color;
    ctx.beginPath();
    ctx.arc(p.x, p.y, p.r, 0, TAU);
    ctx.fill();
    ctx.restore();
  }

  // Player + buffs
  const flicker = player.iFrame > 0 ? (Math.sin(performance.now() * 0.03) * 0.25 + 0.75) : 1;
  ctx.save();
  ctx.globalAlpha = flicker;
  neonCircle(ctx, player.x, player.y, player.r, COLORS.player, 22);
  ctx.shadowColor = UI_COLORS.playerGlow;
  ctx.shadowBlur = 10;
  ctx.fillStyle = UI_COLORS.playerCore;
  ctx.beginPath();
  ctx.arc(player.x, player.y, 5.5, 0, TAU);
  ctx.fill();
  ctx.restore();

  if (buffs.shield > 0){
    const a = 0.55 + 0.25 * Math.sin(performance.now()*0.004);
    neonRing(ctx, player.x, player.y, player.r + 10, UI_COLORS.shieldRing, 26, 2.5, a);
  }
  if (buffs.magnet > 0){
    neonRing(ctx, player.x, player.y, player.r + 18, UI_COLORS.magnetRing, 22, 2, 0.55);
  }

  for (let i=0;i<companions.length;i++){
    const c = companions[i];
    const pulse = 0.7 + 0.3 * Math.sin(performance.now() * 0.006 + i);
    neonCircle(ctx, c.x, c.y, c.r, c.color, 18, 0.9);
    neonRing(ctx, c.x, c.y, c.r + 6, c.color, 18, 1.5, 0.5 * pulse);
  }

  for (let i=0;i<dmgTexts.length;i++){
    const d = dmgTexts[i];
    const a = clamp(d.life / d.maxLife, 0, 1);
    drawTextWorld(ctx, d.x, d.y, d.text, d.color, d.size, a);
  }
  for (let i=0;i<floatTexts.length;i++){
    const t = floatTexts[i];
    const a = clamp(t.life / t.maxLife, 0, 1);
    drawTextWorld(ctx, t.x, t.y, t.text, t.color, t.size, a);
  }

  ctx.restore();

  if (startNoticeT > 0){
    const a = clamp(startNoticeT / START_NOTICE_TIME, 0, 1);
    ctx.save();
    ctx.globalAlpha = a;
    ctx.font = `700 ${isMobile ? 16 : 20}px ui-sans-serif, system-ui, -apple-system, Segoe UI, Roboto, Arial`;
    ctx.textAlign = "center";
    ctx.textBaseline = "middle";
    ctx.fillStyle = COLORS.text;
    ctx.shadowColor = COLORS.player;
    ctx.shadowBlur = 16 * a;
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
  ui.time.textContent = fmtTime(player.time);
  ui.level.textContent = String(player.level);
  ui.kills.textContent = String(player.kills);
  if (ui.mTime){
    ui.mTime.textContent = fmtTime(player.time);
    ui.mLevel.textContent = `Lv ${player.level}`;
    ui.mKills.textContent = `K ${player.kills}`;
  }

  const hp = clamp(player.hp, 0, player.maxHp);
  ui.hp.textContent = `${Math.ceil(hp)} / ${Math.ceil(player.maxHp)}`;
  const hpT = player.maxHp > 0 ? clamp(hp / player.maxHp, 0, 1) : 0;
  ui.hpPct.textContent = `${Math.round(hpT * 100)}%`;
  ui.hpFill.style.width = `${(hpT*100).toFixed(2)}%`;
  if (ui.mHp){
    ui.mHp.textContent = `${Math.ceil(hp)}/${Math.ceil(player.maxHp)}`;
    ui.mHpPct.textContent = `${Math.round(hpT * 100)}%`;
    ui.mHpFill.style.width = `${(hpT*100).toFixed(2)}%`;
  }

  const bosses = [];
  for (let i = 0; i < enemies.length; i++) {
    const e = enemies[i];
    if (e.alive && e.boss) bosses.push(e);
  }
  const primaryBoss = bosses[0] || boss || null;
  const bossHp = primaryBoss ? clamp(primaryBoss.hp, 0, primaryBoss.maxHp) : 0;
  const bossHpT = primaryBoss && primaryBoss.maxHp > 0 ? clamp(bossHp / primaryBoss.maxHp, 0, 1) : 0;
  const bossName = primaryBoss ? (ENEMY_TYPES[primaryBoss.type]?.name || "Boss") : "";
  const bossOn = bosses.length > 0;
  if (ui.bossWrap) ui.bossWrap.classList.toggle("on", bossOn);
  if (ui.bossCard){
    ui.bossCard.classList.toggle("on", bossOn);
    if (bossOn){
      if (ui.bossBars){
        ui.bossBars.innerHTML = bosses.map((b) => {
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
      }
      if (ui.bossName) ui.bossName.textContent = bossName;
      if (ui.bossHp) ui.bossHp.textContent = `${Math.ceil(bossHp)} / ${Math.ceil(primaryBoss.maxHp)}`;
      if (ui.bossHpPct) ui.bossHpPct.textContent = `${Math.round(bossHpT * 100)}%`;
      if (ui.bossHpFill) ui.bossHpFill.style.width = `${(bossHpT*100).toFixed(2)}%`;
    } else if (ui.bossBars) {
      ui.bossBars.innerHTML = "";
    }
  }
  if (ui.mBossBar){
    ui.mBossBar.classList.toggle("on", bossOn);
    if (bossOn){
      const label = bosses.length > 1 ? `Bosses (${bosses.length})` : (bossName || "Boss");
      ui.mBossName.textContent = label;
      ui.mBossPct.textContent = `${Math.round(bossHpT * 100)}%`;
      ui.mBossFill.style.width = `${(bossHpT*100).toFixed(2)}%`;
    } else {
      ui.mBossName.textContent = "Boss";
      ui.mBossPct.textContent = "0%";
      ui.mBossFill.style.width = "0%";
    }
  }

  ui.xp.textContent = fmtFloat(player.xp, 1);
  ui.xpNeed.textContent = fmtFloat(player.xpNeed, 1);
  const xpT = player.xpNeed > 0 ? clamp(player.xp / player.xpNeed, 0, 1) : 0;
  ui.xpFill.style.width = `${(xpT*100).toFixed(2)}%`;
  if (ui.mXp){
    ui.mXp.textContent = fmtFloat(player.xp, 1);
    ui.mXpNeed.textContent = fmtFloat(player.xpNeed, 1);
    ui.mXpFill.style.width = `${(xpT*100).toFixed(2)}%`;
  }

  if (ui.buffs){
    const buffParts = [];
    if (buffs.shield > 0) buffParts.push(`Shield ${fmtFloat(buffs.shield, 1)}s`);
    if (buffs.magnet > 0) buffParts.push(`Magnet ${fmtFloat(buffs.magnet, 1)}s`);
    if (buffs.slow > 0) buffParts.push(`Freeze ${fmtFloat(buffs.slow, 1)}s`);
    if (buffs.power > 0) buffParts.push(`Overcharge ${fmtFloat(buffs.power, 1)}s`);
    if (buffs.haste > 0) buffParts.push(`Haste ${fmtFloat(buffs.haste, 1)}s`);
    if (buffs.xp > 0) buffParts.push(`XP Boost ${fmtFloat(buffs.xp, 1)}s`);
    const buffText = buffParts.length ? buffParts.join(" | ") : "-";
    ui.buffs.innerHTML = `<b>Buffs:</b> ${buffText}`;
    if (ui.mBuffs) ui.mBuffs.textContent = `Buffs: ${buffText}`;
  } else if (ui.mBuffs) {
    ui.mBuffs.textContent = "Buffs: -";
  }
  if (ui.quest){
    const questText = getQuestHudText();
    ui.quest.innerHTML = `<b>Quest:</b> ${questText}`;
    if (ui.mQuest) ui.mQuest.textContent = `Quest: ${questText}`;
  } else if (ui.mQuest) {
    ui.mQuest.textContent = "Quest: -";
  }
  if (ui.mMeta){
    ui.mMeta.textContent = `Trinkets ${trinkets.length} | Companions ${companions.length}`;
  }
}
