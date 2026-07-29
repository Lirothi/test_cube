import {
  COLORS,
  PLAYER_CONFIG,
  XP_CONFIG,
  BUFF_EFFECTS,
  LOOT_CONFIG,
  LOOP_CONFIG,
  SPAWN_CONFIG,
  UPGRADE_CONFIG,
  CRIT_UPGRADES,
  WEAPON_CONFIG,
} from "./config.js";
import { rand, randi, hypot, fmtFloat } from "./math.js";
import { sound } from "./audio.js";
import { setupInput, clearDirectionalInput } from "./input.js";
import { resetPlayer, updatePlayer, addXP, setPlayerRuntime, xpNeedForLevel } from "./player.js";
import { spawnController, getActiveBoss, resetSpawnState } from "./spawn.js";
import { updateTelegraphs } from "./telegraph.js";
import { renderFrame } from "./render.js";
import {
  ui,
  isMobile,
  isDevMode,
  isPerfMode,
  isCanvasSyncMode,
  updateGodButton,
  updateMuteButton,
  updateMenuStats,
  updateTexts,
  setLevelUpHeader,
  renderUpgradeCards,
  fmtTime,
  updateGameOverSummary,
  openMenuUI,
  closeMenuUI,
  openLevelUpUI,
  closeLevelUpUI,
  openGameOverUI,
  closeGameOverUI,
  wireUiEvents,
} from "./ui.js";
import { subscribeVisualSettings, visualSettings } from "./visual_settings.js";
import { updateChests, resetChests, setChestRuntime } from "./chests.js";
import { damageEnemy, updateEnemyShots, updateVoidZones, updateEnemies, cleanupDeadEnemies, setEnemyRuntime } from "./enemies.js";
import { resetCompanions, updateCompanions, addCompanion, companionSlotsFull, COMPANIONS } from "./companions.js";
import { addParticles, updateParticles, resetParticles } from "./particles.js";
import { resetQuests, updateQuests, setQuestRuntime, onEnemyKilled, onEnemyDamaged, onGemCollected, onChestOpened } from "./quests.js";
import {
  upgradeState,
  resetUpgradeState,
  resetDps,
  pickUpgrades,
} from "./upgrade.js";
import {
  weapons,
  resetWeapons,
  weaponCount,
  magicStats,
  auraStats,
  axeStats,
  railStats,
  orbStats,
  missileStats,
  turretStats,
  setWeaponContext,
  setWeaponRuntime,
  spawnShockwave,
  updateWeapons,
} from "./weapons.js";
import { pickTrinkets, addTrinket, resetTrinkets, trinketSlotsFull, TRINKETS } from "./trinkets.js";
import { getAugmentsForWeapon, getAugmentById } from "./augments.js";
import {
  spawnObstacles,
  updateActiveObstacles,
  damageObstacle,
  damageObstaclesInRadius,
  setObstacleRuntime,
} from "./obstacles.js";
import {
  BASE_STATS,
  player,
  buffs,
  trinkets,
  trinketBonuses,
  updateBuffs,
  input,
  enemies,
  bullets,
  missiles,
  turrets,
  toxinPools,
  rails,
  axes,
  orbs,
  arcs,
  enemyShots,
  telegraphs,
  voidZones,
  gems,
  companions,
  particles,
  chests,
  dmgTexts,
  floatTexts,
  obstacles,
  activeObstacles,
  clampPointToWorld,
  clampEntityToWorld,
} from "./state.js";
import {
  enemyPool,
  bulletPool,
  missilePool,
  turretPool,
  toxinPool,
  railPool,
  axePool,
  shotPool,
  voidPool,
  orbPool,
  arcPool,
  gemPool,
  chestPool,
  dmgPool,
  textPool,
} from "./pools.js";
import { popFloatText } from "./float_text.js";
import {
  resetCombatFx,
  triggerHealFx,
  updateCombatFx,
} from "./combat_fx.js";

(() => {
    "use strict";

    /* ============================
       Canvas + DPR
       ============================ */
    const canvas = document.getElementById("c");
    const ctx = canvas.getContext("2d", {
      alpha: false,
      desynchronized: !isCanvasSyncMode,
    });

    let W = 0, H = 0, DPR = 1;
    let resizeRaf = 0;
    const MAX_RENDER_DPR = 2;
    const DPR_QUANTUM = 1 / 8;
    const RENDER_PIXEL_BUDGET = {
      high: 3600000,
      efficient: 2400000,
    };

    function chooseRenderDpr(width, height) {
      const area = Math.max(1, width * height);
      const nativeDpr = Math.min(MAX_RENDER_DPR, window.devicePixelRatio || 1);
      const budget = visualSettings.qualityMode === "efficient"
        ? RENDER_PIXEL_BUDGET.efficient
        : RENDER_PIXEL_BUDGET.high;
      const minimumDpr = visualSettings.qualityMode === "efficient" ? 0.75 : 1;
      const budgetDpr = Math.sqrt(budget / area);
      const desiredDpr = Math.max(minimumDpr, Math.min(nativeDpr, budgetDpr));
      return Math.max(minimumDpr, Math.floor(desiredDpr / DPR_QUANTUM) * DPR_QUANTUM);
    }

    function resize() {
      resizeRaf = 0;
      const nextW = Math.max(1, Math.floor(innerWidth));
      const nextH = Math.max(1, Math.floor(innerHeight));
      const nextDpr = chooseRenderDpr(nextW, nextH);
      const backingW = Math.max(1, Math.floor(nextW * nextDpr));
      const backingH = Math.max(1, Math.floor(nextH * nextDpr));
      const logicalSizeChanged = nextW !== W || nextH !== H || nextDpr !== DPR;
      const backingSizeChanged = canvas.width !== backingW || canvas.height !== backingH;

      W = nextW;
      H = nextH;
      DPR = nextDpr;
      if (canvas.width !== backingW) canvas.width = backingW;
      if (canvas.height !== backingH) canvas.height = backingH;
      if (canvas.style.width !== `${W}px`) canvas.style.width = `${W}px`;
      if (canvas.style.height !== `${H}px`) canvas.style.height = `${H}px`;
      if (logicalSizeChanged || backingSizeChanged) ctx.setTransform(DPR, 0, 0, DPR, 0, 0);
    }

    function scheduleResize() {
      if (resizeRaf) return;
      resizeRaf = requestAnimationFrame(resize);
    }

    addEventListener("resize", scheduleResize, { passive: true });
    resize();
    subscribeVisualSettings(scheduleResize);

    // Ensure keyboard focus (fixes WASD not working)
    function focusCanvas(){ try { canvas.focus({ preventScroll:true }); } catch {} }
    canvas.addEventListener("pointerdown", (e) => { sound.unlock(); focusCanvas(e); }, { passive:true });
    addEventListener("load", focusCanvas, { passive:true });

    /* ============================
       Helpers
       ============================ */
    let godMode = false;
    updateGodButton(godMode);
    updateMuteButton();

    const ACTIVE_OBSTACLE_PAD = 1200;
    const STATE = { PLAYING:"playing", LEVELUP:"levelup", TRINKET:"trinket", AUG:"aug", COMPANION:"companion", GAMEOVER:"gameover", MENU:"menu" };
    let state = STATE.PLAYING;
    let devMenuOpen = false;
    let fpsAccum = 0, fpsCount = 0;
    let updateCpuAccum = 0, renderCpuAccum = 0;
    const START_NOTICE_TIME = 8.0;
    let startNoticeT = 0;
    function updateFps(dt, updateCpuMs = 0, renderCpuMs = 0){
      if ((!isDevMode && !isPerfMode) || !ui.fps) return;
      fpsAccum += (dt > 0 ? (1/dt) : 0);
      updateCpuAccum += updateCpuMs;
      renderCpuAccum += renderCpuMs;
      fpsCount++;
      if (fpsCount >= 15){
        const fps = fpsAccum / fpsCount;
        const fpsLabel = `${Math.round(fps)} fps`;
        if (isPerfMode) {
          const backingMp = canvas.width * canvas.height / 1000000;
          const projectileCount = bullets.length + missiles.length + rails.length
            + axes.length + orbs.length + arcs.length + enemyShots.length;
          const updateCpu = updateCpuAccum / fpsCount;
          const renderCpu = renderCpuAccum / fpsCount;
          const contextMode = isCanvasSyncMode ? "sync" : "desync";
          const glowLayerMode = visualSettings.glowLayerMode === "lowres" ? "glow½" : "glow-inline";
          ui.fps.textContent = `${fpsLabel} · ${DPR.toFixed(3)}x · ${backingMp.toFixed(2)}MP · U ${updateCpu.toFixed(2)} · Rcpu ${renderCpu.toFixed(2)} · E ${enemies.length} · FX ${particles.length} · P ${projectileCount} · T ${turrets.length}/${toxinPools.length} · ${glowLayerMode} · ${contextMode}`;
          ui.fps.title = "U/Rcpu are CPU milliseconds; GPU completion is not included.";
          if (ui.mFps) ui.mFps.textContent = `${fpsLabel} · ${DPR.toFixed(2)}x · ${backingMp.toFixed(1)}MP`;
        } else {
          ui.fps.textContent = fpsLabel;
          if (ui.mFps) ui.mFps.textContent = fpsLabel;
        }
        fpsAccum = 0;
        fpsCount = 0;
        updateCpuAccum = 0;
        renderCpuAccum = 0;
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
    const WEAPON_LABEL_MAP = new Map(WEAPON_LABELS.map((item) => [item.key, item.label]));
    const TRINKET_LABEL_MAP = new Map(TRINKETS.map((item) => [item.id, item.title]));
    const TRINKET_DATA_MAP = new Map(TRINKETS.map((item) => [item.id, item]));
    const COMPANION_DATA_MAP = new Map(COMPANIONS.map((item) => [item.id, item]));

    function escapeHudText(value){
      return String(value)
        .replaceAll("&", "&amp;")
        .replaceAll('"', "&quot;")
        .replaceAll("<", "&lt;")
        .replaceAll(">", "&gt;");
    }

    function formatWeaponPills(){
      const pills = [];
      for (const { key, label } of WEAPON_LABELS){
        const w = weapons[key];
        if (!w || !w.unlocked) continue;
        const mastery = w.mastery ? ` M${w.mastery}` : "";
        const aug = w.aug ? (getAugmentById(w.aug)?.title || "Aug") : "";
        const augText = aug ? ` Aug: ${aug}` : "";
        pills.push(`<span class="pill">${label} Lv ${w.level}${mastery}${augText}</span>`);
      }
      return pills.length ? pills.join("") : `<span class="pill">None</span>`;
    }

    function formatWeaponSlots(){
      const slots = [];
      for (const { key, label } of WEAPON_LABELS){
        const w = weapons[key];
        if (!w || !w.unlocked) continue;
        const mastery = w.mastery ? ` · Mastery ${w.mastery}` : "";
        const aug = w.aug ? (getAugmentById(w.aug)?.title || "Augmentation") : "";
        const augText = aug ? ` · ${aug}` : "";
        const title = escapeHudText(`${label} · Level ${w.level}${mastery}${augText}`);
        slots.push(
          `<span class="hudSlot weaponSlot weapon-${key}" title="${title}" aria-label="${title}">` +
            `<span class="slotGlyph" aria-hidden="true"></span>` +
            `<span class="slotLevel">${w.level}</span>` +
          `</span>`
        );
      }
      return slots.length
        ? slots.join("")
        : `<span class="hudCounter empty" title="No weapons"><span class="counterMark">0</span></span>`;
    }

    function formatTrinketSlots(){
      if (!trinkets.length) {
        return `<span class="hudCounter empty" title="No trinkets"><span class="counterMark">0</span></span>`;
      }
      const labels = trinkets.map((id) => {
        const item = TRINKET_DATA_MAP.get(id);
        return item ? `${item.title} · ${item.desc}` : id;
      });
      const title = escapeHudText(labels.join(" · "));
      return (
        `<span class="hudCounter trinketCounter" title="${title}" aria-label="${title}">` +
          `<span class="slotGlyphText" aria-hidden="true">TK</span>` +
          `<span class="counterMark">${trinkets.length}</span>` +
        `</span>`
      );
    }

    function formatCompanionSlots(){
      if (!companions.length) {
        return `<span class="hudCounter empty" title="No companions"><span class="counterMark">0</span></span>`;
      }
      return companions.map((companion) => {
        const item = COMPANION_DATA_MAP.get(companion.id);
        const label = companion.name || item?.name || companion.id;
        const glyph = item?.visualGlyph || companion.visualGlyph || "core";
        const title = escapeHudText(`${label}${item?.desc ? ` · ${item.desc}` : ""}`);
        return (
          `<span class="hudSlot companionSlot companion-${glyph}" title="${title}" aria-label="${title}">` +
            `<span class="slotGlyph" aria-hidden="true"></span>` +
          `</span>`
        );
      }).join("");
    }

    function getBonusLabels(){
      const labels = [];
      const add = (text) => labels.push(text);

      const speedPct = Math.round(((player.speed / BASE_STATS.speed) - 1) * 100);
      if (speedPct) add(`Speed ${speedPct > 0 ? "+" : ""}${speedPct}%`);

      const hpBonus = Math.round(player.maxHp - BASE_STATS.hp);
      if (hpBonus) add(`Max HP +${hpBonus}`);

      const armorBonus = Math.round(player.armor || 0);
      if (armorBonus) add(`Armor +${armorBonus}`);

      const resists = player.resists || {};
      const allResPct = Math.round((resists.all || 0) * 100);
      if (allResPct) add(`All Res +${allResPct}%`);
      const fireResPct = Math.round((resists.fire || 0) * 100);
      if (fireResPct) add(`Fire Res +${fireResPct}%`);
      const poisonResPct = Math.round((resists.poison || 0) * 100);
      if (poisonResPct) add(`Poison Res +${poisonResPct}%`);
      const voidResPct = Math.round((resists.void || 0) * 100);
      if (voidResPct) add(`Void Res +${voidResPct}%`);

      const pickupBonus = Math.round(player.pickup - BASE_STATS.pickup);
      if (pickupBonus) add(`Pickup +${pickupBonus}`);

      const dmgPct = Math.round(((trinketBonuses.dmgMult || 1) - 1) * 100);
      if (dmgPct) add(`Damage +${dmgPct}%`);

      const xpMult = (1 + upgradeState.xpLv * UPGRADE_CONFIG.xpMultGain) * (trinketBonuses.xpMult || 1);
      const xpPct = Math.round((xpMult - 1) * 100);
      if (xpPct) add(`XP +${xpPct}%`);

      const cdMult = Math.max(0, 1 - upgradeState.cdLv * UPGRADE_CONFIG.cdReduction) * (trinketBonuses.cdMult || 1);
      const cdPct = Math.round((1 - cdMult) * 100);
      if (cdPct) add(`CD ${cdPct > 0 ? "-" : "+"}${Math.abs(cdPct)}%`);

      const critChance = (upgradeState.critChanceLv * CRIT_UPGRADES.chancePerLevel) + (trinketBonuses.critChance || 0);
      if (critChance) add(`Crit +${Math.round(critChance * 100)}%`);

      const critMult = (upgradeState.critMultLv * CRIT_UPGRADES.multPerLevel) + (trinketBonuses.critMult || 0);
      if (critMult) add(`Crit Dmg +${fmtFloat(critMult, 2)}x`);

      return labels;
    }

    function formatBonusCounter(){
      const labels = getBonusLabels();
      const title = escapeHudText(labels.length ? labels.join(" · ") : "No active bonuses");
      return (
        `<span class="hudCounter bonusCounter${labels.length ? "" : " empty"}" title="${title}" aria-label="${title}">` +
          `<span class="bonusGlyph" aria-hidden="true"></span>` +
          `<span class="counterMark">${labels.length}</span>` +
        `</span>`
      );
    }

    function updateLoadoutUi(){
      if (ui.loadout) ui.loadout.innerHTML = formatWeaponSlots();
      if (ui.trinkets) ui.trinkets.innerHTML = formatTrinketSlots();
      if (ui.companions) ui.companions.innerHTML = formatCompanionSlots();
      if (ui.bonuses) ui.bonuses.innerHTML = formatBonusCounter();
      if (ui.mWeapons) ui.mWeapons.innerHTML = formatWeaponSlots();
    }

    const AUG_FLOAT_LIFE = 3;

    /* ============================
       Input setup
       ============================ */
    setupInput({
      canvas,
      ui,
      sound,
      isMobile,
      STATE,
      getState: () => state,
      openMenu,
       closeMenu,
    });

    /* ============================
       Upgrades
       ============================ */
    setWeaponContext({ buffs, upgradeState });

    /* ============================
       Combat / Drops / Particles
       ============================ */

    setObstacleRuntime({ addParticles });
    setWeaponRuntime({ damageEnemy, damageObstacle, damageObstaclesInRadius, addParticles, spawnShockwave });

    /* ============================
       Level up UI
       ============================ */
    let currentCards = [];

    function openLevelUp(){
      if (state !== STATE.PLAYING) return;
      state = STATE.LEVELUP;
      if (ui.levelup) ui.levelup.classList.remove("trinket");
      if (ui.levelup) ui.levelup.classList.remove("aug");
      setLevelUpHeader("Level Up", "Choose 1 upgrade. Game is paused.");
      currentCards = pickUpgrades(XP_CONFIG.cardChoices);
      renderUpgradeCards(currentCards, (u) => {
        if (state !== STATE.LEVELUP) return;
        u.apply();
        updateLoadoutUi();
        closeLevelUp();
      });
      openLevelUpUI();
      sound.play("level");
    }
    function closeLevelUp(){
      closeLevelUpUI();
      setDevMenuVisible(false);
      if (ui.levelup) ui.levelup.classList.remove("trinket");
      if (ui.levelup) ui.levelup.classList.remove("aug");
      state = STATE.PLAYING;
      focusCanvas();
    }

    function openTrinket(){
      if (state !== STATE.PLAYING) return false;
      if (trinketSlotsFull()) return false;
      const picks = pickTrinkets();
      if (!picks.length) return false;
      picks.push({
        id: "skip_trinket",
        title: "Skip",
        desc: "Leave this trinket behind.",
        tag: () => "No effect",
        cardKind: "skip",
      });
      state = STATE.TRINKET;
      if (ui.levelup) ui.levelup.classList.add("trinket");
      if (ui.levelup) ui.levelup.classList.remove("aug");
      setLevelUpHeader("Trinket Found", "Choose 1 trinket. Game is paused.");
      renderUpgradeCards(picks, (t) => {
        if (state !== STATE.TRINKET) return;
        if (t.id === "skip_trinket") {
          closeTrinket();
          return;
        }
        addTrinket(t.id);
        updateLoadoutUi();
        closeTrinket();
      });
      openLevelUpUI();
      sound.play("level");
      return true;
    }

    function closeTrinket(){
      closeLevelUpUI();
      setDevMenuVisible(false);
      setLevelUpHeader("Level Up", "Choose 1 upgrade. Game is paused.");
      if (ui.levelup) ui.levelup.classList.remove("trinket");
      if (ui.levelup) ui.levelup.classList.remove("aug");
      state = STATE.PLAYING;
      focusCanvas();
    }

    function openCompanion(){
      if (state !== STATE.PLAYING) return false;
      if (companionSlotsFull()) return false;
      const choices = COMPANIONS.map((c) => {
        const owned = companions.some((p) => p.id === c.id);
        return {
          id: c.id,
          title: c.name,
          desc: owned ? `${c.desc} Already recruited.` : c.desc,
          tag: () => owned ? `${c.tag()} | Owned` : c.tag(),
          disabled: owned,
          cardKind: "companion",
          visualGlyph: c.visualGlyph,
        };
      });
      state = STATE.COMPANION;
      if (ui.levelup) {
        ui.levelup.classList.remove("trinket");
        ui.levelup.classList.remove("aug");
      }
      setLevelUpHeader("Companion Cage", "Choose 1 companion. Game is paused.");
      renderUpgradeCards(choices, (c) => {
        if (state !== STATE.COMPANION) return;
        if (c.disabled) return;
        addCompanion(c.id);
        updateLoadoutUi();
        closeCompanion();
      });
      openLevelUpUI();
      sound.play("level");
      return true;
    }

    function closeCompanion(){
      closeLevelUpUI();
      setDevMenuVisible(false);
      setLevelUpHeader("Level Up", "Choose 1 upgrade. Game is paused.");
      if (ui.levelup) ui.levelup.classList.remove("trinket");
      if (ui.levelup) ui.levelup.classList.remove("aug");
      state = STATE.PLAYING;
      focusCanvas();
    }

    function openAug(){
      if (state !== STATE.PLAYING) return false;
      const options = WEAPON_LABELS.filter(({ key }) => weapons[key]?.unlocked && !weapons[key]?.aug);
      if (!options.length) {
        const unlocked = WEAPON_LABELS.filter(({ key }) => weapons[key]?.unlocked);
        if (!unlocked.length) return false;
        const pick = unlocked[randi(unlocked.length)];
        const w = weapons[pick.key];
        const maxLevel = WEAPON_CONFIG[pick.key]?.maxLevel || w.level;
        if (w.level < maxLevel) {
          w.level++;
          popFloatText(player.x, player.y - 18, `${pick.label} +1`, COLORS.aug, 20, AUG_FLOAT_LIFE);
        } else {
          w.mastery++;
          popFloatText(player.x, player.y - 18, `${pick.label} Mastery +1`, COLORS.aug, 20, AUG_FLOAT_LIFE);
        }
        updateLoadoutUi();
        return true;
      }
      const pick = options[randi(options.length)];
      const choices = getAugmentsForWeapon(pick.key);
      if (!choices.length) return false;
      state = STATE.AUG;
      if (ui.levelup) {
        ui.levelup.classList.add("aug");
        ui.levelup.classList.remove("trinket");
      }
      setLevelUpHeader(`${pick.label} Augmentation`, "Choose 1 augmentation. Game is paused.");
      renderUpgradeCards(choices, (aug) => {
        if (state !== STATE.AUG) return;
        weapons[pick.key].aug = aug.id;
        updateLoadoutUi();
        closeAug();
      });
      openLevelUpUI();
      sound.play("level");
      return true;
    }

    function closeAug(){
      closeLevelUpUI();
      setDevMenuVisible(false);
      setLevelUpHeader("Level Up", "Choose 1 upgrade. Game is paused.");
      if (ui.levelup) ui.levelup.classList.remove("aug");
      state = STATE.PLAYING;
      focusCanvas();
    }

    /* ============================
       Dev Menu
       ============================ */
    function isDevMenuAllowed(){
      return state === STATE.LEVELUP || state === STATE.TRINKET || state === STATE.AUG || state === STATE.COMPANION || state === STATE.MENU;
    }

    function getDevPanelHost(){
      if (state === STATE.MENU) return ui.menuPanel;
      return ui.levelupPanel;
    }

    function setDevMenuVisible(next){
      devMenuOpen = !!next;
      if (!ui.devPanel) return;
      const host = getDevPanelHost();
      if (host && ui.devPanel.parentElement !== host) {
        host.appendChild(ui.devPanel);
      }
      ui.devPanel.classList.toggle("on", devMenuOpen);
      ui.devPanel.setAttribute("aria-hidden", devMenuOpen ? "false" : "true");
      if (devMenuOpen) refreshDevOptions();
    }

    function toggleDevMenu(){
      if (!isDevMenuAllowed()) return;
      setDevMenuVisible(!devMenuOpen);
    }

    function setDevStatus(text){
      if (ui.devStatus) ui.devStatus.textContent = text;
    }

    function getWeaponLabel(key){
      return WEAPON_LABEL_MAP.get(key) || key;
    }

    function refreshDevWeaponOptions(){
      if (!ui.devWeaponSelect) return;
      const current = ui.devWeaponSelect.value;
      ui.devWeaponSelect.innerHTML = WEAPON_LABELS.map(({ key, label }) => {
        const w = weapons[key];
        const status = w.unlocked ? `Lv ${w.level}` : "Locked";
        const mastery = w.mastery ? ` M${w.mastery}` : "";
        return `<option value="${key}">${label} (${status}${mastery})</option>`;
      }).join("");
      if (current) ui.devWeaponSelect.value = current;
    }

    function refreshDevTrinketOptions(){
      if (!ui.devTrinketSelect) return;
      const current = ui.devTrinketSelect.value;
      ui.devTrinketSelect.innerHTML = TRINKETS.map((t) => {
        const owned = trinkets.includes(t.id);
        return `<option value="${t.id}">${t.title}${owned ? " (Owned)" : ""}</option>`;
      }).join("");
      if (current) ui.devTrinketSelect.value = current;
    }

    function refreshDevScaling(){
      const hpPct = SPAWN_CONFIG.scaling.hp * 10000;
      const spdPct = SPAWN_CONFIG.scaling.speed * 10000;
      const dmgPct = SPAWN_CONFIG.scaling.dmg * 10000;
      if (ui.devScaleHpInput && document.activeElement !== ui.devScaleHpInput) ui.devScaleHpInput.value = fmtFloat(hpPct, 1);
      if (ui.devScaleSpeedInput && document.activeElement !== ui.devScaleSpeedInput) ui.devScaleSpeedInput.value = fmtFloat(spdPct, 1);
      if (ui.devScaleDmgInput && document.activeElement !== ui.devScaleDmgInput) ui.devScaleDmgInput.value = fmtFloat(dmgPct, 1);
    }

    function refreshDevOptions(){
      refreshDevWeaponOptions();
      refreshDevTrinketOptions();
      refreshDevScaling();
    }

    function devUnlockWeapon(key){
      const w = weapons[key];
      if (!w) return;
      if (!w.unlocked) {
        w.unlocked = true;
        w.level = Math.max(1, w.level);
      } else if (w.level < 1) {
        w.level = 1;
      }
      updateLoadoutUi();
      refreshDevWeaponOptions();
      setDevStatus(`${getWeaponLabel(key)} unlocked`);
    }

    function devUpgradeWeapon(key){
      const w = weapons[key];
      if (!w) return;
      if (!w.unlocked) {
        w.unlocked = true;
        w.level = 1;
        updateLoadoutUi();
        refreshDevWeaponOptions();
        setDevStatus(`${getWeaponLabel(key)} unlocked`);
        return;
      }
      const maxLevel = WEAPON_CONFIG[key]?.maxLevel || w.level;
      if (w.level < maxLevel) {
        w.level++;
        setDevStatus(`${getWeaponLabel(key)} Lv ${w.level}`);
      } else {
        w.mastery = (w.mastery || 0) + 1;
        setDevStatus(`${getWeaponLabel(key)} Mastery ${w.mastery}`);
      }
      updateLoadoutUi();
      refreshDevWeaponOptions();
    }

    function devMaxWeapon(key){
      const w = weapons[key];
      if (!w) return;
      const maxLevel = WEAPON_CONFIG[key]?.maxLevel || w.level;
      w.unlocked = true;
      w.level = Math.max(w.level, maxLevel);
      updateLoadoutUi();
      refreshDevWeaponOptions();
      setDevStatus(`${getWeaponLabel(key)} maxed`);
    }

    function devMasteryWeapon(key){
      const w = weapons[key];
      if (!w) return;
      w.unlocked = true;
      w.level = Math.max(1, w.level);
      w.mastery = (w.mastery || 0) + 1;
      updateLoadoutUi();
      refreshDevWeaponOptions();
      setDevStatus(`${getWeaponLabel(key)} Mastery ${w.mastery}`);
    }

    function devAddTrinket(id){
      const title = TRINKET_LABEL_MAP.get(id) || id;
      const added = addTrinket(id);
      updateLoadoutUi();
      refreshDevTrinketOptions();
      setDevStatus(added ? `Added ${title}` : `Could not add ${title}`);
    }

    function devHeal(){
      const hpBefore = player.hp;
      player.hp = player.maxHp;
      triggerHealFx(player.hp - hpBefore);
      setDevStatus("HP full");
    }

    function devShield(){
      buffs.shield = Math.max(buffs.shield, 10);
      setDevStatus("Shield 10s");
    }

    function devAddLevel(){
      player.level++;
      player.xp = 0;
      player.xpNeed = xpNeedForLevel(player.level);
      setDevStatus(`Level ${player.level}`);
      if (state === STATE.MENU) {
        closeMenu();
      } else if (state !== STATE.PLAYING) {
        closeLevelUpUI();
        setDevMenuVisible(false);
        if (ui.levelup) {
          ui.levelup.classList.remove("trinket");
          ui.levelup.classList.remove("aug");
        }
        state = STATE.PLAYING;
      }
      openLevelUp();
    }

    function devClearEnemies(){
      for (let i=enemies.length-1;i>=0;i--) enemyPool.put(enemies.pop());
      for (let i=enemyShots.length-1;i>=0;i--) shotPool.put(enemyShots.pop());
      for (let i=voidZones.length-1;i>=0;i--) voidPool.put(voidZones.pop());
      telegraphs.length = 0;
      setDevStatus("Enemies cleared");
    }

    function devApplyScaling(){
      const hpPct = Math.max(0, parseFloat(ui.devScaleHpInput?.value || ""));
      const spdPct = Math.max(0, parseFloat(ui.devScaleSpeedInput?.value || ""));
      const dmgPct = Math.max(0, parseFloat(ui.devScaleDmgInput?.value || ""));
      if (!Number.isFinite(hpPct) || !Number.isFinite(spdPct) || !Number.isFinite(dmgPct)) {
        setDevStatus("Scaling values invalid");
        refreshDevScaling();
        return;
      }
      SPAWN_CONFIG.scaling.hp = hpPct / 10000;
      SPAWN_CONFIG.scaling.speed = spdPct / 10000;
      SPAWN_CONFIG.scaling.dmg = dmgPct / 10000;
      refreshDevScaling();
      setDevStatus("Scaling updated");
    }

    function wireDevMenu(){
      if (!ui.devPanel) return;
      refreshDevOptions();
      if (ui.devWeaponAdd) ui.devWeaponAdd.addEventListener("click", () => {
        const key = ui.devWeaponSelect?.value;
        if (key) devUnlockWeapon(key);
      }, { passive:true });
      if (ui.devWeaponUp) ui.devWeaponUp.addEventListener("click", () => {
        const key = ui.devWeaponSelect?.value;
        if (key) devUpgradeWeapon(key);
      }, { passive:true });
      if (ui.devWeaponMax) ui.devWeaponMax.addEventListener("click", () => {
        const key = ui.devWeaponSelect?.value;
        if (key) devMaxWeapon(key);
      }, { passive:true });
      if (ui.devWeaponMastery) ui.devWeaponMastery.addEventListener("click", () => {
        const key = ui.devWeaponSelect?.value;
        if (key) devMasteryWeapon(key);
      }, { passive:true });
      if (ui.devTrinketAdd) ui.devTrinketAdd.addEventListener("click", () => {
        const id = ui.devTrinketSelect?.value;
        if (id) devAddTrinket(id);
      }, { passive:true });
      if (ui.devHeal) ui.devHeal.addEventListener("click", devHeal, { passive:true });
      if (ui.devShield) ui.devShield.addEventListener("click", devShield, { passive:true });
      if (ui.devAddLevel) ui.devAddLevel.addEventListener("click", devAddLevel, { passive:true });
      if (ui.devClearEnemies) ui.devClearEnemies.addEventListener("click", devClearEnemies, { passive:true });
      if (ui.devScaleApply) ui.devScaleApply.addEventListener("click", devApplyScaling, { passive:true });
    }

    addEventListener("keydown", (e) => {
      if (e.repeat) return;
      if (!e.ctrlKey) return;
      if (typeof e.key !== "string") return;
      if (e.key.toLowerCase() !== "d") return;
      if (!isDevMenuAllowed()) return;
      e.preventDefault();
      toggleDevMenu();
    });
    setPlayerRuntime({ openLevelUp });
    setChestRuntime({ addXP, openTrinket, openAug, openCompanion, onChestOpened });
    setEnemyRuntime({ openAug, onEnemyKilled, onEnemyDamaged });
    setQuestRuntime({ addXP, openAug, openTrinket, addParticles });

    /* ============================
       Main Menu / Pause
       ============================ */
    function openMenu(){
      if (state !== STATE.PLAYING) return;
      state = STATE.MENU;
      updateMenuStats();
      openMenuUI();
    }

    function closeMenu(){
      if (state !== STATE.MENU) return;
      setDevMenuVisible(false);
      closeMenuUI();
      state = STATE.PLAYING;
      focusCanvas();
    }

    /* ============================
       Game Over
       ============================ */
    function openGameOver(){
      state = STATE.GAMEOVER;
      openGameOverUI();
      updateGameOverSummary();
    }

    function restart(){
      for (let i=enemies.length-1;i>=0;i--) enemyPool.put(enemies.pop());
      for (let i=bullets.length-1;i>=0;i--) bulletPool.put(bullets.pop());
      for (let i=missiles.length-1;i>=0;i--) missilePool.put(missiles.pop());
      for (let i=turrets.length-1;i>=0;i--) turretPool.put(turrets.pop());
      for (let i=toxinPools.length-1;i>=0;i--) toxinPool.put(toxinPools.pop());
      for (let i=rails.length-1;i>=0;i--) railPool.put(rails.pop());
      for (let i=axes.length-1;i>=0;i--) axePool.put(axes.pop());
      for (let i=orbs.length-1;i>=0;i--) orbPool.put(orbs.pop());
      for (let i=arcs.length-1;i>=0;i--) arcPool.put(arcs.pop());
      obstacles.length = 0;
      for (let i=enemyShots.length-1;i>=0;i--) shotPool.put(enemyShots.pop());
      for (let i=voidZones.length-1;i>=0;i--) voidPool.put(voidZones.pop());
      for (let i=gems.length-1;i>=0;i--) gemPool.put(gems.pop());
      resetParticles();
      for (let i=chests.length-1;i>=0;i--) chestPool.put(chests.pop());
      for (let i=dmgTexts.length-1;i>=0;i--) dmgPool.put(dmgTexts.pop());
      for (let i=floatTexts.length-1;i>=0;i--) textPool.put(floatTexts.pop());
      telegraphs.length = 0;

      resetSpawnState();
      resetChests();
      spawnObstacles();

      resetPlayer();
      resetQuests();
      resetWeapons();
      resetUpgradeState();
      resetTrinkets();
      resetCompanions();
      resetDps();
      resetCombatFx();
      startNoticeT = START_NOTICE_TIME;

      closeGameOverUI();
      closeLevelUpUI();
      setDevMenuVisible(false);
      closeMenuUI();
      clearDirectionalInput();

      state = STATE.PLAYING;
      last = performance.now();
      focusCanvas();
      updateLoadoutUi();
    }

    wireUiEvents({
      restart,
      closeMenu,
      openMenu,
      toggleGod: () => {
        godMode = !godMode;
        updateGodButton(godMode);
        if (godMode){
          player.hp = player.maxHp;
          buffs.shield = 9999;
        } else {
          buffs.shield = 0;
        }
      },
      toggleMute: () => {
        sound.enabled = !sound.enabled;
        updateMuteButton();
        if (sound.enabled) sound.unlock();
      },
      toggleMobileMenu: () => {
        if (state === STATE.PLAYING) openMenu();
        else if (state === STATE.MENU) closeMenu();
      }
    });
    wireDevMenu();

    /* ============================
       Buffs / Player / Enemies / Projectiles
       ============================ */
    function updateGems(dt){
      const magnetMul = (buffs.magnet > 0) ? BUFF_EFFECTS.magnetRadiusMult : 1.0; // 3x radius while active
      const pickup = player.pickup * magnetMul;
      const pickupMult = player.pickup / BASE_STATS.pickup;
      const pr2 = pickup * pickup;
      const compCount = companions.length;

      for (let i=gems.length-1;i>=0;i--){
        const g = gems[i];
        if (!g.alive){ gems[i] = gems[gems.length-1]; gems.pop(); gemPool.put(g); continue; }

        g.life -= dt;
        if (g.life <= 0){
          g.alive = false;
        }

        g.vx *= Math.pow(LOOT_CONFIG.frictionBase, dt);
        g.vy *= Math.pow(LOOT_CONFIG.frictionBase, dt);

        const dx = player.x - g.x;
        const dy = player.y - g.y;
        const d2 = dx*dx + dy*dy;
        let collected = false;

        if (d2 < pr2){
          const d = Math.sqrt(d2) || 1;
          const nx = dx / d, ny = dy / d;
          const pullBase = ((buffs.magnet > 0) ? BUFF_EFFECTS.magnetPullPowered : BUFF_EFFECTS.magnetPullBase) * pickupMult; // stronger pickup upgrade = faster pull
          const pull = (1 - d / pickup) * pullBase;
          g.vx += nx * pull * dt;
          g.vy += ny * pull * dt;
        }

        for (let j = 0; j < compCount; j++){
          const c = companions[j];
          const cdx = c.x - g.x;
          const cdy = c.y - g.y;
          const cd2 = cdx * cdx + cdy * cdy;
          const cPickup = c.pickup || 0;
          if (!collected && cPickup > 0 && cd2 < cPickup * cPickup){
            const d = Math.sqrt(cd2) || 1;
            const nx = cdx / d, ny = cdy / d;
            const pull = (1 - d / cPickup) * (c.pull || 0);
            g.vx += nx * pull * dt;
            g.vy += ny * pull * dt;
          }
          const rr = (c.r + g.r + LOOT_CONFIG.pickupPadding);
          if (cd2 <= rr * rr){
            collected = true;
            break;
          }
        }

        g.x += g.vx * dt;
        g.y += g.vy * dt;

        const rr = player.r + g.r + LOOT_CONFIG.pickupPadding;
        if (d2 <= rr*rr || collected){
          g.alive = false;
          addXP(g.v);
          onGemCollected(g.v);
          addParticles(g.x, g.y, COLORS.gem, 2, 220);
          sound.play("pickup");
        }

        if (!g.alive){
          gems[i] = gems[gems.length-1];
          gems.pop();
          gemPool.put(g);
        }
      }
    }

    /* ============================
       Rendering
       ============================ */
    function render(frameNow){
      const camX = player.x - W*0.5;
      const camY = player.y - H*0.5;
      const boss = getActiveBoss();
      if (state !== STATE.PLAYING) {
        updateActiveObstacles(camX, camY, W, H, ACTIVE_OBSTACLE_PAD);
      }
      renderFrame({
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
        renderDpr: DPR,
      });
    }

    /* ============================
       Main Loop
       ============================ */
    let last = performance.now();

    function update(dt){
      player.time += dt;
      if (startNoticeT > 0) startNoticeT = Math.max(0, startNoticeT - dt);

      updateBuffs(dt);
      updateCombatFx(dt);
      updatePlayer(dt);
      updateCompanions(dt);

      const camX = player.x - W*0.5;
      const camY = player.y - H*0.5;

      spawnController(dt, camX, camY, W, H);
      updateChests(dt, camX, camY, W, H);
      updateQuests(dt);
      updateActiveObstacles(camX, camY, W, H, ACTIVE_OBSTACLE_PAD);

      updateWeapons(dt);
      updateEnemies(dt, godMode, W, H);
      updateTelegraphs(dt);
      updateVoidZones(dt, godMode);
      updateEnemyShots(dt);
      updateGems(dt);
      updateParticles(dt);
      updateTexts(dt);

      cleanupDeadEnemies(camX, camY, W, H);

      if (player.hp <= 0){
        if (trinketBonuses.reviveCharges > 0){
          trinketBonuses.reviveCharges--;
          const reviveHp = Math.max(1, Math.ceil(player.maxHp * 0.45));
          player.hp = reviveHp;
          player.iFrame = Math.max(player.iFrame, 1.0);
          buffs.shield = Math.max(buffs.shield, 1.6);
          buffs.reviveFlash = Math.max(buffs.reviveFlash, 1.6);
          triggerHealFx(reviveHp, 1);
          spawnShockwave(player.x, player.y, 220, COLORS.heal);
          addParticles(player.x, player.y, COLORS.heal, 90, 900);
          popFloatText(player.x, player.y - player.r - 26, "SECOND CHANCE!", COLORS.heal, 30, 2.2, 22, 60, 120);
          sound.play("boss");
        } else {
          player.hp = 0;
          openGameOver();
        }
      }
    }

    function frame(now){
      const dt = Math.min(LOOP_CONFIG.maxDt, (now - last) / 1000);
      last = now;
      let updateCpuMs = 0;
      let renderCpuMs = 0;

      if (state === STATE.PLAYING){
        const updateStart = isPerfMode ? performance.now() : 0;
        update(dt);
        if (isPerfMode) updateCpuMs = performance.now() - updateStart;
      } else {
        if (particles.length) updateParticles(dt);
        if (dmgTexts.length || floatTexts.length) updateTexts(dt);
        if (telegraphs.length) updateTelegraphs(dt);
      }

      const renderStart = isPerfMode ? performance.now() : 0;
      render(now);
      if (isPerfMode) renderCpuMs = performance.now() - renderStart;
      if (state === STATE.PLAYING) updateFps(dt, updateCpuMs, renderCpuMs);
      requestAnimationFrame(frame);
    }

    /* ============================
       Boot
       ============================ */
    function boot(){
      restart();
      spawnObstacles();
      for (let i=0;i<30;i++) addParticles(rand(200,-200), rand(120,-120), COLORS.player, 1, 220);
      requestAnimationFrame(frame);
    }

    boot();

    ui.levelup.addEventListener("click", (e)=> e.stopPropagation(), { passive:true });
    ui.gameover.addEventListener("click", (e)=> e.stopPropagation(), { passive:true });
    ui.menu.addEventListener("click", (e)=> e.stopPropagation(), { passive:true });
  })();
  
