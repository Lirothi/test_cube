import { BUILD, CRIT_UPGRADES, UPGRADE_CONFIG } from "./config.js";
import { fmtFloat } from "./math.js";
import { sound } from "./audio.js";
import { DPS_TRACKER, upgradeState, listUpgradeSummary, formatDpsSummary } from "./upgrade.js";
import { weapons, magicStats, auraStats, railStats, axeStats, orbStats, missileStats } from "./weapons.js";
import { player, BASE_STATS } from "./state.js";

export const ui = {
  time: document.getElementById("uiTime"),
  level: document.getElementById("uiLevel"),
  hp: document.getElementById("uiHp"),
  hpPct: document.getElementById("uiHpPct"),
  kills: document.getElementById("uiKills"),
  xp: document.getElementById("uiXp"),
  xpNeed: document.getElementById("uiXpNeed"),
  xpFill: document.getElementById("xpFill"),
  hpFill: document.getElementById("hpFill"),
  bossWrap: document.getElementById("bossBarWrap"),
  bossCard: document.getElementById("bossBarCard"),
  bossName: document.getElementById("bossName"),
  bossHp: document.getElementById("bossHp"),
  bossHpPct: document.getElementById("bossHpPct"),
  bossHpFill: document.getElementById("bossHpFill"),
  buffs: document.getElementById("uiBuffs"),
  levelup: document.getElementById("levelup"),
  upgradeCards: document.getElementById("upgradeCards"),
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
  menuBuild: document.getElementById("uiMenuBuild"),
  menuPlayerStats: document.getElementById("menuPlayerStats"),
  menuWeaponStats: document.getElementById("menuWeaponStats"),
  btnResume: document.getElementById("btnResume"),
  btnMenuRestart: document.getElementById("btnMenuRestart"),
  btnGod: document.getElementById("btnGod"),
  btnMute: document.getElementById("btnMute"),
  btnMobileMenu: document.getElementById("btnMobileMenu"),
  hint: document.getElementById("hint"),
  loadout: document.getElementById("uiLoadout"),
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
  fps: document.getElementById("fps"),
};

if (ui.uiBuild) ui.uiBuild.textContent = BUILD;
if (ui.menuBuild) ui.menuBuild.textContent = BUILD;

export const isMobile = /Mobi|Android|iPhone|iPad|iPod|Touch/i.test(navigator.userAgent) || (navigator.maxTouchPoints || 0) > 0;
if (!isMobile && ui.hint) {
  ui.hint.textContent = "Move: WASD/Arrows | Chests: touch to open | Auto-attacks | ESC: Menu (Click/tap the canvas to focus keys)";
}
if (isMobile) document.body.classList.add("mobile");

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
  ui.menuPlayerStats.innerHTML = [
    `<div class="kv"><span>Level</span><span>Lv ${player.level}</span></div>`,
    `<div class="kv"><span>HP</span><span>${hp}</span></div>`,
    `<div class="kv"><span>Armor</span><span>${Math.round(player.armor)}</span></div>`,
    `<div class="kv"><span>Move Speed</span><span>${Math.round(player.speed)} (${speedPct}% base)</span></div>`,
    `<div class="kv"><span>Pickup</span><span>${Math.round(player.pickup)} (${pickupPct}% base)</span></div>`,
    `<div class="kv"><span>Cooldown Reduction</span><span>-${cdBonusPct}%</span></div>`,
    `<div class="kv"><span>XP Gain</span><span>+${xpBonusPct}%</span></div>`,
    `<div class="kv"><span>Crit Bonus</span><span>+${critBonusChance}% / x${critBonusMult}</span></div>`,
    `<div class="kv"><span>Kills</span><span>${player.kills}</span></div>`,
  ].join("");

  const rows = [];
  const addWeapon = (label, key, fn) => {
    const w = weapons[key];
    if (!w.unlocked) return;
    const s = fn();
    const parts = [];
    if (s.dmg) parts.push(`DMG ${Math.round(s.dmg)}`);
    if (s.count) parts.push(`Count ${s.count}`);
    if (s.cd) parts.push(`CD ${fmtFloat(s.cd, 2)}s`);
    if (s.pierce) parts.push(`Pierce ${Math.round(s.pierce)}`);
    if (s.tick) parts.push(`Tick ${fmtFloat(s.tick, 2)}s`);
    if (key === "aura" && s.radius) parts.push(`Radius ${Math.round(s.radius)}`);
    const dps = fmtFloat((DPS_TRACKER[key] || 0) / Math.max(player.time, 0.1), 1);
    parts.push(`Crit ${Math.round((s.critChance || 0) * 100)}% x${fmtFloat(s.critMult || 1, 2)}`);
    parts.push(`DPS ${dps}`);
    rows.push(`<div class="kv"><span>${label} Lv ${w.level}${w.mastery ? ` (M${w.mastery})` : ""}</span><span>${parts.join(" | ")}</span></div>`);
  };
  addWeapon("Magic", "magic", magicStats);
  addWeapon("Aura", "aura", auraStats);
  addWeapon("Railgun", "rail", railStats);
  addWeapon("Axe", "axe", axeStats);
  addWeapon("Orb", "orb", orbStats);
  addWeapon("Missiles", "missile", missileStats);
  ui.menuWeaponStats.innerHTML = rows.length ? rows.join("") : `<div class="kv"><span>Weapons</span><span>None unlocked</span></div>`;
}

export function renderUpgradeCards(cards, onPick) {
  ui.upgradeCards.innerHTML = "";
  for (const u of cards) {
    const el = document.createElement("div");
    el.className = "card";
    el.innerHTML = `
      <h3>${u.title}</h3>
      <p>${u.desc}</p>
      <div class="pill">${u.tag()}</div>
    `;
    el.addEventListener("click", () => {
      if (typeof onPick === "function") onPick(u);
    }, { passive: true });
    ui.upgradeCards.appendChild(el);
  }
}

export function openMenuUI() {
  if (!ui.menu) return;
  ui.menu.classList.add("on");
  ui.menu.style.pointerEvents = "auto";
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
  if (ui.goUpgrades) ui.goUpgrades.textContent = listUpgradeSummary();
  if (ui.goDps) ui.goDps.textContent = formatDpsSummary();
  return t;
}
