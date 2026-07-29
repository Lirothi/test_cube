import { BUILD, CRIT_UPGRADES, UPGRADE_CONFIG, WEAPON_CONFIG } from "./config.js";
import { fmtFloat } from "./math.js";
import { sound } from "./audio.js";
import { DPS_TRACKER, MASTERY_INFO, upgradeState } from "./upgrade.js";
import { weapons, magicStats, arcStats, auraStats, railStats, axeStats, orbStats, missileStats, turretStats } from "./weapons.js";
import { player, BASE_STATS, dmgTexts, floatTexts, trinkets, trinketBonuses, companions } from "./state.js";
import { dmgPool, textPool } from "./pools.js";
import { getAugmentById } from "./augments.js";
import { TRINKETS } from "./trinkets.js";
import { COMPANIONS } from "./companions.js";
import {
  cycleGlowMode,
  cycleGlowLayerMode,
  cycleMotionMode,
  cycleQualityMode,
  getVisualSettingsLabels,
  subscribeVisualSettings,
  visualSettings,
} from "./visual_settings.js";

export const ui = {
  hud: document.getElementById("hud"),
  time: document.getElementById("uiTime"),
  level: document.getElementById("uiLevel"),
  hp: document.getElementById("uiHp"),
  hpPct: document.getElementById("uiHpPct"),
  kills: document.getElementById("uiKills"),
  armor: document.getElementById("uiArmor"),
  resFire: document.getElementById("uiResFire"),
  resPoison: document.getElementById("uiResPoison"),
  resVoid: document.getElementById("uiResVoid"),
  xp: document.getElementById("uiXp"),
  xpNeed: document.getElementById("uiXpNeed"),
  xpFill: document.getElementById("xpFill"),
  hpFill: document.getElementById("hpFill"),
  bossWrap: document.getElementById("bossBarWrap"),
  bossCard: document.getElementById("bossBarCard"),
  bossBars: document.getElementById("bossBars"),
  bossName: document.getElementById("bossName"),
  bossHp: document.getElementById("bossHp"),
  bossHpPct: document.getElementById("bossHpPct"),
  bossHpFill: document.getElementById("bossHpFill"),
  buffs: document.getElementById("uiBuffs"),
  quest: document.getElementById("uiQuest"),
  levelup: document.getElementById("levelup"),
  levelupTitle: document.getElementById("levelupTitle"),
  levelupSub: document.getElementById("levelupSub"),
  upgradeCards: document.getElementById("upgradeCards"),
  levelupPanel: document.getElementById("levelupPanel"),
  devPanel: document.getElementById("devPanel"),
  devWeaponSelect: document.getElementById("devWeaponSelect"),
  devWeaponAdd: document.getElementById("devWeaponAdd"),
  devWeaponUp: document.getElementById("devWeaponUp"),
  devWeaponMax: document.getElementById("devWeaponMax"),
  devWeaponMastery: document.getElementById("devWeaponMastery"),
  devTrinketSelect: document.getElementById("devTrinketSelect"),
  devTrinketAdd: document.getElementById("devTrinketAdd"),
  devHeal: document.getElementById("devHeal"),
  devShield: document.getElementById("devShield"),
  devAddLevel: document.getElementById("devAddLevel"),
  devClearEnemies: document.getElementById("devClearEnemies"),
  devScaleHpInput: document.getElementById("devScaleHpInput"),
  devScaleSpeedInput: document.getElementById("devScaleSpeedInput"),
  devScaleDmgInput: document.getElementById("devScaleDmgInput"),
  devScaleApply: document.getElementById("devScaleApply"),
  devStatus: document.getElementById("devStatus"),
  uiBuild: document.getElementById("uiBuild"),
  gameover: document.getElementById("gameover"),
  summary: document.getElementById("uiSummary"),
  goTime: document.getElementById("uiGoTime"),
  goLvl: document.getElementById("uiGoLvl"),
  goKills: document.getElementById("uiGoKills"),
  goUpgrades: document.getElementById("uiGoUpgrades"),
  goDps: document.getElementById("uiGoDps"),
  btnRestart: document.getElementById("btnRestart"),
  menu: document.getElementById("menu"),
  menuPanel: document.getElementById("menuPanel"),
  menuBuild: document.getElementById("uiMenuBuild"),
  menuPlayerStats: document.getElementById("menuPlayerStats"),
  menuWeaponStats: document.getElementById("menuWeaponStats"),
  menuBuildDetails: document.getElementById("menuBuildDetails"),
  btnResume: document.getElementById("btnResume"),
  btnMenuRestart: document.getElementById("btnMenuRestart"),
  btnGod: document.getElementById("btnGod"),
  btnMute: document.getElementById("btnMute"),
  btnGlow: document.getElementById("btnGlow"),
  btnGlowLayer: document.getElementById("btnGlowLayer"),
  btnMotion: document.getElementById("btnMotion"),
  btnQuality: document.getElementById("btnQuality"),
  btnScreenShake: document.getElementById("btnScreenShake"),
  btnMobileMenu: document.getElementById("btnMobileMenu"),
  hint: document.getElementById("hint"),
  loadout: document.getElementById("uiLoadout"),
  trinkets: document.getElementById("uiTrinkets"),
  companions: document.getElementById("uiCompanions"),
  bonuses: document.getElementById("uiBonuses"),
  mTime: document.getElementById("mUiTime"),
  mLevel: document.getElementById("mUiLevel"),
  mKills: document.getElementById("mUiKills"),
  mFps: document.getElementById("mFps"),
  mHp: document.getElementById("mUiHp"),
  mHpPct: document.getElementById("mUiHpPct"),
  mHpFill: document.getElementById("mHpFill"),
  mXp: document.getElementById("mUiXp"),
  mXpNeed: document.getElementById("mUiXpNeed"),
  mXpFill: document.getElementById("mXpFill"),
  mBossBar: document.getElementById("mBossBar"),
  mBossName: document.getElementById("mBossName"),
  mBossPct: document.getElementById("mBossPct"),
  mBossFill: document.getElementById("mBossFill"),
  mWeapons: document.getElementById("mWeapons"),
  mMeta: document.getElementById("mMeta"),
  mBuffs: document.getElementById("mBuffs"),
  mQuest: document.getElementById("mQuest"),
  fps: document.getElementById("fps"),
};

if (ui.uiBuild) ui.uiBuild.textContent = BUILD;
if (ui.menuBuild) ui.menuBuild.textContent = BUILD;

const queryParams = new URLSearchParams(window.location.search);
const cvdMode = queryParams.get("cvd");
export const isMobile = queryParams.get("mobile") === "1"
  || /Mobi|Android|iPhone|iPad|iPod|Touch/i.test(navigator.userAgent)
  || (navigator.maxTouchPoints || 0) > 0;
export const isDevMode = queryParams.get("dev") === "1";
export const isPerfMode = queryParams.get("perf") === "1";
export const isCanvasSyncMode = queryParams.get("canvasSync") !== "0";
if (!isMobile && ui.hint) {
  ui.hint.textContent = "Move: WASD/Arrows | Chests: touch to open | Auto-attacks | ESC: Menu (Click/tap the canvas to focus keys)";
}
if (isMobile) document.body.classList.add("mobile");
if (isDevMode || isPerfMode) document.body.classList.add("dev-mode");
if (isPerfMode) document.body.classList.add("perf-mode");
if (["protanopia", "deuteranopia", "tritanopia"].includes(cvdMode)) {
  document.body.dataset.cvd = cvdMode;
}

function syncVisualSettingsButtons() {
  const labels = getVisualSettingsLabels();
  if (ui.btnGlow) {
    ui.btnGlow.textContent = `Glow: ${labels.glow}`;
    ui.btnGlow.dataset.mode = visualSettings.glowMode;
  }
  if (ui.btnGlowLayer) {
    ui.btnGlowLayer.textContent = `Layer: ${labels.glowLayer}`;
    ui.btnGlowLayer.dataset.mode = visualSettings.glowLayerMode;
  }
  if (ui.btnMotion) {
    ui.btnMotion.textContent = `Motion: ${labels.motion}`;
    ui.btnMotion.dataset.mode = visualSettings.motionMode;
  }
  if (ui.btnQuality) {
    ui.btnQuality.textContent = `Effects: ${labels.quality}`;
    ui.btnQuality.dataset.mode = visualSettings.qualityMode;
  }
  if (ui.btnScreenShake) ui.btnScreenShake.textContent = "Screen Shake: Off";
}

subscribeVisualSettings(syncVisualSettingsButtons);

const CARD_WEAPON_KEYS = new Set(["magic", "arc", "aura", "rail", "axe", "orb", "missile", "turret"]);
const PASSIVE_CARD_META = {
  speed: { category: "Passive · Mobility", theme: "utility", glyph: "SPD", levelKey: "speedLv", max: UPGRADE_CONFIG.passiveMaxLevel },
  hp: { category: "Passive · Defense", theme: "defense", glyph: "HP", levelKey: "hpLv", max: UPGRADE_CONFIG.passiveMaxLevel },
  armor: { category: "Passive · Defense", theme: "defense", glyph: "ARM", levelKey: "armorLv", max: UPGRADE_CONFIG.passiveMaxLevel },
  resAll: { category: "Passive · Defense", theme: "defense", glyph: "RES", levelKey: "resAllLv", max: UPGRADE_CONFIG.passiveMaxLevel },
  resFire: { category: "Passive · Defense", theme: "fire", glyph: "FIR", levelKey: "resFireLv", max: UPGRADE_CONFIG.passiveMaxLevel },
  resPoison: { category: "Passive · Defense", theme: "poison", glyph: "TOX", levelKey: "resPoisonLv", max: UPGRADE_CONFIG.passiveMaxLevel },
  resVoid: { category: "Passive · Defense", theme: "void", glyph: "VOI", levelKey: "resVoidLv", max: UPGRADE_CONFIG.passiveMaxLevel },
  pickup: { category: "Passive · Utility", theme: "utility", glyph: "MAG", levelKey: "pickupLv", max: UPGRADE_CONFIG.passiveMaxLevel },
  xp: { category: "Passive · Growth", theme: "growth", glyph: "XP", levelKey: "xpLv", max: UPGRADE_CONFIG.passiveMaxLevel },
  cdr: { category: "Passive · Offense", theme: "offense", glyph: "CD", levelKey: "cdLv", max: UPGRADE_CONFIG.passiveMaxLevel },
  critChance: { category: "Passive · Offense", theme: "offense", glyph: "%", levelKey: "critChanceLv", max: CRIT_UPGRADES.maxLevels },
  critMult: { category: "Passive · Offense", theme: "offense", glyph: "×", levelKey: "critMultLv", max: CRIT_UPGRADES.maxLevels },
};
const TRINKET_CARD_IDS = new Set(TRINKETS.map((item) => item.id));
const COMPANION_CARD_IDS = new Set(COMPANIONS.map((item) => item.id));

function escapeHtml(value) {
  return String(value ?? "").replace(/[&<>"']/g, (char) => ({
    "&": "&amp;",
    "<": "&lt;",
    ">": "&gt;",
    '"': "&quot;",
    "'": "&#39;",
  })[char]);
}

function itemInitials(title) {
  return String(title || "?")
    .split(/\s+/)
    .filter(Boolean)
    .slice(0, 2)
    .map((word) => word[0])
    .join("")
    .toUpperCase();
}

function getCardPresentation(card) {
  const id = String(card.id || "");
  const augmentWeapon = id.split("_")[0];
  const passive = PASSIVE_CARD_META[id];
  let kind = card.cardKind || "";
  if (!kind && CARD_WEAPON_KEYS.has(id)) kind = "weapon";
  if (!kind && passive) kind = "passive";
  if (!kind && TRINKET_CARD_IDS.has(id)) kind = "trinket";
  if (!kind && COMPANION_CARD_IDS.has(id)) kind = "companion";
  if (!kind && CARD_WEAPON_KEYS.has(augmentWeapon)) kind = "augment";
  if (!kind) kind = "upgrade";

  let category = "Upgrade";
  let theme = "default";
  let iconClass = "";
  let iconText = itemInitials(card.title);
  let levelText = "Choose";

  if (kind === "weapon") {
    const weapon = weapons[id];
    const max = WEAPON_CONFIG[id]?.maxLevel || weapon?.level || 1;
    category = "Weapon";
    theme = id;
    iconClass = `weapon-${id}`;
    iconText = "";
    if (!weapon?.unlocked) levelText = "Unlock · Lv 1";
    else if (weapon.level < max) levelText = `Lv ${weapon.level} → ${weapon.level + 1}`;
    else levelText = `Mastery ${weapon.mastery || 0} → ${(weapon.mastery || 0) + 1}`;
  } else if (kind === "passive") {
    const current = upgradeState[passive.levelKey] || 0;
    category = passive.category;
    theme = passive.theme;
    iconText = passive.glyph;
    levelText = `Lv ${current} → ${Math.min(passive.max, current + 1)}`;
  } else if (kind === "trinket") {
    category = "Trinket";
    theme = "trinket";
    levelText = "Acquire";
  } else if (kind === "augment") {
    category = "Weapon Augment";
    theme = "augment";
    iconClass = `weapon-${augmentWeapon}`;
    iconText = "";
    levelText = "Install";
  } else if (kind === "companion") {
    category = "Companion";
    theme = "companion";
    iconClass = `companion-${card.visualGlyph || "cross"}`;
    iconText = "";
    levelText = card.disabled ? "Owned" : "Recruit";
  } else if (kind === "skip") {
    category = "No Reward";
    theme = "skip";
    iconText = "—";
    levelText = "Continue";
  }

  const rawTag = typeof card.tag === "function" ? String(card.tag()) : "";
  let details = rawTag.split(/\s*\|\s*/).filter(Boolean);
  if (kind === "weapon" || kind === "passive") {
    details = details.filter((part) => !/^Lv\s/i.test(part) && !/^Weapon\s*-\s*Unlock$/i.test(part));
  }
  if (kind === "weapon") {
    const weapon = weapons[id];
    const max = WEAPON_CONFIG[id]?.maxLevel || 1;
    if (weapon?.unlocked && weapon.level >= max) details.unshift(`Mastery rank: ${MASTERY_INFO}`);
    else if (!details.length) details.push(weapon?.unlocked ? "Improves weapon stats" : "Adds a new auto-attack");
  }
  if (card.disabled && !details.some((part) => /owned|unavailable/i.test(part))) details.push("Unavailable");

  return { kind, category, theme, iconClass, iconText, levelText, details };
}

export function updateGodButton(godMode) {
  if (ui.btnGod) ui.btnGod.textContent = `God Mode: ${godMode ? "On" : "Off"}`;
}

export function updateMuteButton() {
  if (ui.btnMute) ui.btnMute.textContent = `Sound: ${sound.enabled ? "On" : "Off"}`;
}

export function updateMenuStats() {
  if (!ui.menuPlayerStats || !ui.menuWeaponStats) return;
  const hp = `${Math.ceil(player.hp)} / ${Math.ceil(player.maxHp)}`;
  const speedPct = Math.round((player.speed / BASE_STATS.speed) * 100);
  const pickupPct = Math.round((player.pickup / BASE_STATS.pickup) * 100);
  const critBonusChance = Math.round(upgradeState.critChanceLv * CRIT_UPGRADES.chancePerLevel * 100);
  const critBonusMult = fmtFloat(1 + upgradeState.critMultLv * CRIT_UPGRADES.multPerLevel, 2);
  const cdBonusPct = Math.round(upgradeState.cdLv * UPGRADE_CONFIG.cdReduction * 100);
  const xpBonusPct = Math.round(upgradeState.xpLv * UPGRADE_CONFIG.xpMultGain * 100);
  const statRow = (label, value) => `<div class="pauseStatRow"><span>${label}</span><strong>${value}</strong></div>`;
  ui.menuPlayerStats.innerHTML = [
    `<section class="pauseStatGroup pauseStatGroupPrimary">
      <div class="pauseStatGroupTitle">Run</div>
      <div class="pauseStatHighlights">
        <div><span>Level</span><strong>${player.level}</strong></div>
        <div><span>HP</span><strong>${hp}</strong></div>
        <div><span>Kills</span><strong>${player.kills}</strong></div>
      </div>
    </section>`,
    `<section class="pauseStatGroup">
      <div class="pauseStatGroupTitle">Defense</div>
      ${statRow("Armor", Math.round(player.armor))}
      ${statRow("All Res", `${Math.round((player.resists?.all || 0) * 100)}%`)}
      ${statRow("Fire · Poison · Void", `${Math.round((player.resists?.fire || 0) * 100)}% · ${Math.round((player.resists?.poison || 0) * 100)}% · ${Math.round((player.resists?.void || 0) * 100)}%`)}
    </section>`,
    `<section class="pauseStatGroup">
      <div class="pauseStatGroupTitle">Offense & Growth</div>
      ${statRow("Cooldown", `-${cdBonusPct}%`)}
      ${statRow("Crit Bonus", `+${critBonusChance}% · ×${critBonusMult}`)}
      ${statRow("XP Gain", `+${xpBonusPct}%`)}
    </section>`,
    `<section class="pauseStatGroup">
      <div class="pauseStatGroupTitle">Mobility</div>
      ${statRow("Move Speed", `${Math.round(player.speed)} · ${speedPct}% base`)}
      ${statRow("Pickup", `${Math.round(player.pickup)} · ${pickupPct}% base`)}
    </section>`,
  ].join("");

  const rows = [];
  const addWeapon = (label, key, fn) => {
    const w = weapons[key];
    if (!w.unlocked) return;
    const s = fn();
    const parts = [];
    if (s.dmg) parts.push(`DMG ${Math.round(s.dmg)}`);
    if (s.count) parts.push(`Count ${s.count}`);
    if (s.chains) parts.push(`Chains ${s.chains}`);
    if (s.cd) parts.push(`CD ${fmtFloat(s.cd, 2)}s`);
    if (s.pierce) parts.push(`Pierce ${Math.round(s.pierce)}`);
    if (s.tick) parts.push(`Tick ${fmtFloat(s.tick, 2)}s`);
    if (s.chainRange) parts.push(`Chain ${Math.round(s.chainRange)}`);
    if (key === "aura" && s.radius) parts.push(`Radius ${Math.round(s.radius)}`);
    const dps = fmtFloat((DPS_TRACKER[key] || 0) / Math.max(player.time, 0.1), 1);
    parts.push(`Crit ${Math.round((s.critChance || 0) * 100)}% x${fmtFloat(s.critMult || 1, 2)}`);
    rows.push(`<article class="pauseWeaponRow">
      <div class="pauseWeaponHeading">
        <span class="pauseWeaponIcon weapon-${key}"><span class="slotGlyph"></span></span>
        <span><strong>${label}</strong><small>Lv ${w.level}${w.mastery ? ` · M${w.mastery}` : ""}</small></span>
        <span class="pauseWeaponDps"><strong>${dps}</strong><small>DPS</small></span>
      </div>
      <div class="pauseWeaponMeta">${parts.join(" · ")}</div>
    </article>`);
  };
  addWeapon("Magic Bullet", "magic", magicStats);
  addWeapon("Arc Lance", "arc", arcStats);
  addWeapon("Holy Aura", "aura", auraStats);
  addWeapon("Railgun", "rail", railStats);
  addWeapon("Axe Throw", "axe", axeStats);
  addWeapon("Singularity Orb", "orb", orbStats);
  addWeapon("Homing Missiles", "missile", missileStats);
  addWeapon("Flux Turret", "turret", turretStats);
  ui.menuWeaponStats.innerHTML = rows.length ? rows.join("") : `<div class="pauseEmpty">No weapons unlocked</div>`;

  if (ui.menuBuildDetails) {
    const trinketInfo = trinkets.map((id) => {
      const item = TRINKETS.find((entry) => entry.id === id);
      return item ? `<div><b>${item.title}</b><span>${item.desc}</span></div>` : `<div><b>${id}</b></div>`;
    });
    const companionInfo = companions.map((companion) => {
      const item = COMPANIONS.find((entry) => entry.id === companion.id);
      const name = item?.name || companion.name || companion.id;
      return item ? `<div><b>${name}</b><span>${item.desc}</span></div>` : `<div><b>${name}</b></div>`;
    });
    ui.menuBuildDetails.innerHTML = [
      `<section><h4>Trinkets</h4>${trinketInfo.length ? trinketInfo.join("") : "<p>None</p>"}</section>`,
      `<section><h4>Companions</h4>${companionInfo.length ? companionInfo.join("") : "<p>None</p>"}</section>`,
      `<section><h4>Bonuses</h4><p>Complete derived values are listed in Player Stats.</p></section>`,
    ].join("");
  }
}

const WEAPON_LABELS = [
  { key: "magic", label: "Magic Bullet" },
  { key: "arc", label: "Arc Lance" },
  { key: "aura", label: "Holy Aura" },
  { key: "rail", label: "Railgun" },
  { key: "axe", label: "Axe Throw" },
  { key: "orb", label: "Singularity Orb" },
  { key: "missile", label: "Homing Missiles" },
  { key: "turret", label: "Flux Turret" },
];
const TRINKET_LABELS = new Map(TRINKETS.map((t) => [t.id, t.title]));

function formatBuildSummary() {
  const sections = [];
  const section = (title, items, className = "") => {
    const content = items.length
      ? items.map((item) => `<span class="summaryChip">${item}</span>`).join("")
      : `<span class="summaryEmpty">None</span>`;
    return `<section class="summarySection ${className}"><h4>${title}</h4><div class="summaryChips">${content}</div></section>`;
  };

  const weaponParts = [];
  for (const { key, label } of WEAPON_LABELS) {
    const w = weapons[key];
    if (!w.unlocked) continue;
    const mastery = w.mastery ? ` · M${w.mastery}` : "";
    const augTitle = w.aug ? (getAugmentById(w.aug)?.title || "Aug") : "";
    const augText = augTitle ? `<small>${augTitle}</small>` : "";
    weaponParts.push(`<span class="summaryWeaponIcon weapon-${key}"><span class="slotGlyph"></span></span><span>${label}<b>Lv ${w.level}${mastery}</b>${augText}</span>`);
  }
  sections.push(section("Weapons", weaponParts, "summaryWeapons"));

  const trinketParts = trinkets.map((id) => escapeHtml(TRINKET_LABELS.get(id) || id));
  sections.push(section("Trinkets", trinketParts));

  const companionParts = companions.map((companion) => escapeHtml(companion.name || companion.id));
  sections.push(section("Companions", companionParts));

  const passiveParts = [];
  const passiveRows = [
    ["Speed", upgradeState.speedLv],
    ["Max HP", upgradeState.hpLv],
    ["Armor", upgradeState.armorLv],
    ["Pickup", upgradeState.pickupLv],
    ["XP", upgradeState.xpLv],
    ["CDR", upgradeState.cdLv],
    ["Crit Chance", upgradeState.critChanceLv],
    ["Crit Damage", upgradeState.critMultLv],
    ["All Res", upgradeState.resAllLv],
    ["Fire Res", upgradeState.resFireLv],
    ["Poison Res", upgradeState.resPoisonLv],
    ["Void Res", upgradeState.resVoidLv],
  ];
  for (const [label, level] of passiveRows) {
    if (level) passiveParts.push(`${label} <b>Lv ${level}</b>`);
  }
  const dmgPct = Math.round(((trinketBonuses.dmgMult || 1) - 1) * 100);
  const xpMult = (1 + upgradeState.xpLv * UPGRADE_CONFIG.xpMultGain) * (trinketBonuses.xpMult || 1);
  const xpPct = Math.round((xpMult - 1) * 100);
  const cdMult = Math.max(0, 1 - upgradeState.cdLv * UPGRADE_CONFIG.cdReduction) * (trinketBonuses.cdMult || 1);
  const cdPct = Math.round((1 - cdMult) * 100);
  const critChance = (upgradeState.critChanceLv * CRIT_UPGRADES.chancePerLevel) + (trinketBonuses.critChance || 0);
  const critMult = 1 + upgradeState.critMultLv * CRIT_UPGRADES.multPerLevel + (trinketBonuses.critMult || 0);
  const bonusParts = [
    `DMG ${dmgPct >= 0 ? "+" : ""}${dmgPct}%`,
    `XP ${xpPct >= 0 ? "+" : ""}${xpPct}%`,
    `CDR ${cdPct >= 0 ? "-" : "+"}${Math.abs(cdPct)}%`,
    `Crit ${Math.round(critChance * 100)}% ×${fmtFloat(critMult, 2)}`,
  ];
  sections.push(section("Passives", passiveParts));
  sections.push(section("Final Bonuses", bonusParts, "summaryBonuses"));
  return sections.join("");
}

function formatDpsRows() {
  const elapsed = Math.max(player.time, 0.1);
  const entries = [];
  for (const { key, label } of WEAPON_LABELS) {
    if (!weapons[key]?.unlocked) continue;
    const damage = DPS_TRACKER[key] || 0;
    entries.push({ key, label, damage, dps: damage / elapsed });
  }
  if (!entries.length) return `<div class="summaryEmpty">No weapon damage</div>`;
  const maxDps = Math.max(...entries.map((entry) => entry.dps), 1);
  return entries
    .sort((a, b) => b.dps - a.dps)
    .map((entry) => {
      const fill = Math.max(1, Math.min(10, Math.ceil((entry.dps / maxDps) * 10)));
      return `<div class="dpsRow">
        <div class="dpsHeading">
          <span class="dpsWeaponIcon weapon-${entry.key}"><span class="slotGlyph"></span></span>
          <span>${entry.label}</span>
          <strong>${Math.round(entry.dps)} <small>DPS</small></strong>
        </div>
        <div class="dpsTrack"><span class="dpsFill dpsFill${fill}"></span></div>
        <div class="dpsDamage">${Math.round(entry.damage)} total damage</div>
      </div>`;
    })
    .join("");
}

export function updateTexts(dt) {
  for (let i = dmgTexts.length - 1; i >= 0; i--) {
    const d = dmgTexts[i];
    if (!d.alive) { dmgTexts[i] = dmgTexts[dmgTexts.length - 1]; dmgTexts.pop(); dmgPool.put(d); continue; }
    d.life -= dt;
    d.x += d.vx * dt;
    d.y += d.vy * dt;
    d.vx *= Math.pow(0.12, dt);
    d.vy *= Math.pow(0.10, dt);
    if (d.life <= 0) d.alive = false;
    if (!d.alive) {
      dmgTexts[i] = dmgTexts[dmgTexts.length - 1];
      dmgTexts.pop();
      dmgPool.put(d);
    }
  }

  for (let i = floatTexts.length - 1; i >= 0; i--) {
    const t = floatTexts[i];
    if (!t.alive) { floatTexts[i] = floatTexts[floatTexts.length - 1]; floatTexts.pop(); textPool.put(t); continue; }
    t.life -= dt;
    t.x += t.vx * dt;
    t.y += t.vy * dt;
    t.vx *= Math.pow(0.12, dt);
    t.vy *= Math.pow(0.10, dt);
    if (t.life <= 0) t.alive = false;
    if (!t.alive) {
      floatTexts[i] = floatTexts[floatTexts.length - 1];
      floatTexts.pop();
      textPool.put(t);
    }
  }
}

export function renderUpgradeCards(cards, onPick) {
  ui.upgradeCards.innerHTML = "";
  for (const u of cards) {
    const el = document.createElement("button");
    const disabled = !!u.disabled;
    const view = getCardPresentation(u);
    el.type = "button";
    el.className = `card type-${view.kind} theme-${view.theme}${disabled ? " disabled" : ""}`;
    el.disabled = disabled;
    el.setAttribute("aria-disabled", disabled ? "true" : "false");
    el.setAttribute("aria-label", `${u.title}. ${view.levelText}. ${u.desc}`);
    const icon = view.iconText
      ? `<span class="cardGlyphText">${escapeHtml(view.iconText)}</span>`
      : `<span class="slotGlyph"></span>`;
    const details = view.details.length
      ? view.details.map((detail) => `<span class="cardDetail">${escapeHtml(detail)}</span>`).join("")
      : `<span class="cardDetail cardDetailMuted">No additional modifiers</span>`;
    el.innerHTML = `
      <span class="cardCategory">${escapeHtml(view.category)}</span>
      <span class="cardTop">
        <span class="cardIcon ${view.iconClass}">${icon}</span>
        <span class="cardHeading">
          <span class="cardLevel">${escapeHtml(view.levelText)}</span>
          <span class="cardTitle">${escapeHtml(u.title)}</span>
        </span>
      </span>
      <span class="cardEffect">${escapeHtml(u.desc)}</span>
      <span class="cardDetails">${details}</span>
      ${disabled ? `<span class="cardLock">Unavailable</span>` : ""}
    `;
    if (!disabled) {
      el.addEventListener("click", () => {
        if (typeof onPick === "function") onPick(u);
      });
      el.addEventListener("keydown", (event) => {
        if (event.key !== "Enter" && event.key !== " ") return;
        event.preventDefault();
        el.click();
      });
    }
    ui.upgradeCards.appendChild(el);
  }
}

export function setLevelUpHeader(title, subtitle) {
  if (ui.levelupTitle) ui.levelupTitle.textContent = title;
  if (ui.levelupSub) ui.levelupSub.textContent = subtitle;
}

export function openMenuUI() {
  if (!ui.menu) return;
  ui.menu.classList.add("on");
  ui.menu.style.pointerEvents = "auto";
  if (ui.menuPanel) ui.menuPanel.scrollTop = 0;
}

export function closeMenuUI() {
  if (!ui.menu) return;
  ui.menu.classList.remove("on");
  ui.menu.style.pointerEvents = "none";
}

export function openLevelUpUI() {
  if (!ui.levelup) return;
  ui.levelup.classList.add("on");
  ui.levelup.style.pointerEvents = "auto";
  if (ui.levelupPanel) ui.levelupPanel.scrollTop = 0;
  if (ui.upgradeCards) ui.upgradeCards.scrollLeft = 0;
  requestAnimationFrame(() => {
    ui.upgradeCards?.querySelector(".card:not(:disabled)")?.focus({ preventScroll: true });
  });
}

export function closeLevelUpUI() {
  if (!ui.levelup) return;
  ui.levelup.classList.remove("on");
  ui.levelup.style.pointerEvents = "none";
}

export function openGameOverUI() {
  if (!ui.gameover) return;
  ui.gameover.classList.add("on");
  ui.gameover.style.pointerEvents = "auto";
  const panel = ui.gameover.querySelector(".panel");
  if (panel) panel.scrollTop = 0;
}

export function closeGameOverUI() {
  if (!ui.gameover) return;
  ui.gameover.classList.remove("on");
  ui.gameover.style.pointerEvents = "none";
}

export function wireUiEvents({ restart, closeMenu, openMenu, toggleGod, toggleMute, toggleMobileMenu }) {
  if (ui.btnRestart) ui.btnRestart.addEventListener("click", restart, { passive: true });
  if (ui.btnResume) ui.btnResume.addEventListener("click", closeMenu, { passive: true });
  if (ui.btnMenuRestart) ui.btnMenuRestart.addEventListener("click", restart, { passive: true });
  if (ui.btnGod) ui.btnGod.addEventListener("click", toggleGod, { passive: true });
  if (ui.btnMute) ui.btnMute.addEventListener("click", toggleMute, { passive: true });
  if (ui.btnGlow) ui.btnGlow.addEventListener("click", cycleGlowMode, { passive: true });
  if (ui.btnGlowLayer) ui.btnGlowLayer.addEventListener("click", cycleGlowLayerMode, { passive: true });
  if (ui.btnMotion) ui.btnMotion.addEventListener("click", cycleMotionMode, { passive: true });
  if (ui.btnQuality) ui.btnQuality.addEventListener("click", cycleQualityMode, { passive: true });
  if (ui.levelup) ui.levelup.addEventListener("click", (e) => e.stopPropagation(), { passive: true });
  if (ui.gameover) ui.gameover.addEventListener("click", (e) => e.stopPropagation(), { passive: true });
  if (ui.menu) ui.menu.addEventListener("click", (e) => e.stopPropagation(), { passive: true });
  if (ui.btnMobileMenu) {
    ui.btnMobileMenu.addEventListener("click", (e) => {
      e.stopPropagation();
      if (typeof toggleMobileMenu === "function") toggleMobileMenu();
    }, { passive: true });
  }
}

export function fmtTime(sec) {
  sec = Math.max(0, sec);
  const m = (sec / 60) | 0;
  const s = (sec - m * 60) | 0;
  return String(m).padStart(2, "0") + ":" + String(s).padStart(2, "0");
}

export function updateGameOverSummary() {
  const t = fmtTime(player.time);
  if (ui.summary) ui.summary.textContent = `You survived ${t}.`;
  if (ui.goTime) ui.goTime.textContent = t;
  if (ui.goLvl) ui.goLvl.textContent = String(player.level);
  if (ui.goKills) ui.goKills.textContent = String(player.kills);
  if (ui.goUpgrades) ui.goUpgrades.innerHTML = formatBuildSummary();
  if (ui.goDps) ui.goDps.innerHTML = formatDpsRows();
  return t;
}
