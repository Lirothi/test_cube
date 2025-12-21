import {
  COLORS,
  PLAYER_CONFIG,
  XP_CONFIG,
  BUFF_EFFECTS,
  LOOT_CONFIG,
  LOOP_CONFIG,
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
import { updateChests, resetChests, setChestRuntime } from "./chests.js";
import { damageEnemy, updateEnemyShots, updateVoidZones, updateEnemies, cleanupDeadEnemies, setEnemyRuntime } from "./enemies.js";
import { addParticles, updateParticles, resetParticles } from "./particles.js";
import { resetQuests, updateQuests, setQuestRuntime, onEnemyKilled } from "./quests.js";
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
  setWeaponContext,
  setWeaponRuntime,
  spawnShockwave,
  updateWeapons,
} from "./weapons.js";
import { pickTrinkets, addTrinket, resetTrinkets, trinketSlotsFull, formatTrinketPills, TRINKETS } from "./trinkets.js";
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
  rails,
  axes,
  orbs,
  enemyShots,
  telegraphs,
  voidZones,
  gems,
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
  railPool,
  axePool,
  shotPool,
  voidPool,
  orbPool,
  gemPool,
  chestPool,
  dmgPool,
  textPool,
} from "./pools.js";
import { popFloatText } from "./float_text.js";

(() => {
    "use strict";

    /* ============================
       Canvas + DPR
       ============================ */
    const canvas = document.getElementById("c");
    const ctx = canvas.getContext("2d", { alpha: false, desynchronized: true });

    let W = 0, H = 0, DPR = 1;
    const BIG_SCREEN_PIXELS = 2000000; // ~2K and above screens; cap DPR to ease GPU load
    function resize() {
      const area = innerWidth * innerHeight;
      const maxDpr = area > BIG_SCREEN_PIXELS ? 1 : 2;
      DPR = Math.min(maxDpr, window.devicePixelRatio || 1);
      W = Math.floor(innerWidth);
      H = Math.floor(innerHeight);
      canvas.width = Math.floor(W * DPR);
      canvas.height = Math.floor(H * DPR);
      canvas.style.width = W + "px";
      canvas.style.height = H + "px";
      ctx.setTransform(DPR, 0, 0, DPR, 0, 0);
    }
    addEventListener("resize", resize);
    resize();

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
    const STATE = { PLAYING:"playing", LEVELUP:"levelup", TRINKET:"trinket", AUG:"aug", GAMEOVER:"gameover", MENU:"menu" };
    let state = STATE.PLAYING;
    let devMenuOpen = false;
    let fpsAccum = 0, fpsCount = 0;
    const START_NOTICE_TIME = 8.0;
    let startNoticeT = 0;
    function updateFps(dt){
      if (!ui.fps) return;
      fpsAccum += (dt > 0 ? (1/dt) : 0);
      fpsCount++;
      if (fpsCount >= 10){
        const fps = fpsAccum / fpsCount;
        ui.fps.textContent = `${Math.round(fps)} fps`;
        if (ui.mFps) ui.mFps.textContent = `${Math.round(fps)} fps`;
        fpsAccum = 0;
        fpsCount = 0;
      }
    }
    const WEAPON_LABELS = [
      { key: "magic", label: "Magic Bullet" },
      { key: "aura", label: "Holy Aura" },
      { key: "rail", label: "Railgun" },
      { key: "axe", label: "Axe Throw" },
      { key: "orb", label: "Singularity Orb" },
      { key: "missile", label: "Homing Missiles" },
    ];
    const WEAPON_LABEL_MAP = new Map(WEAPON_LABELS.map((item) => [item.key, item.label]));
    const TRINKET_LABEL_MAP = new Map(TRINKETS.map((item) => [item.id, item.title]));

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

    function formatBonusPills(){
      const pills = [];
      const add = (text) => pills.push(`<span class="pill">${text}</span>`);

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

      return pills.length ? pills.join("") : `<span class="pill">No bonuses</span>`;
    }

    function updateLoadoutUi(){
      if (ui.loadout) ui.loadout.innerHTML = formatWeaponPills();
      if (ui.trinkets) ui.trinkets.innerHTML = formatTrinketPills();
      if (ui.bonuses) ui.bonuses.innerHTML = formatBonusPills();
      if (ui.mWeapons) ui.mWeapons.innerHTML = formatWeaponPills();
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
      return state === STATE.LEVELUP || state === STATE.TRINKET || state === STATE.AUG || state === STATE.MENU;
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

    function refreshDevOptions(){
      refreshDevWeaponOptions();
      refreshDevTrinketOptions();
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
      player.hp = player.maxHp;
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
    }

    function devClearEnemies(){
      for (let i=enemies.length-1;i>=0;i--) enemyPool.put(enemies.pop());
      for (let i=enemyShots.length-1;i>=0;i--) shotPool.put(enemyShots.pop());
      for (let i=voidZones.length-1;i>=0;i--) voidPool.put(voidZones.pop());
      telegraphs.length = 0;
      setDevStatus("Enemies cleared");
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
    setChestRuntime({ addXP, openTrinket, openAug });
    setEnemyRuntime({ openAug, onEnemyKilled });
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
      for (let i=rails.length-1;i>=0;i--) railPool.put(rails.pop());
      for (let i=axes.length-1;i>=0;i--) axePool.put(axes.pop());
      for (let i=orbs.length-1;i>=0;i--) orbPool.put(orbs.pop());
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
      resetDps();
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

        if (d2 < pr2){
          const d = Math.sqrt(d2) || 1;
          const nx = dx / d, ny = dy / d;
          const pullBase = ((buffs.magnet > 0) ? BUFF_EFFECTS.magnetPullPowered : BUFF_EFFECTS.magnetPullBase) * pickupMult; // stronger pickup upgrade = faster pull
          const pull = (1 - d / pickup) * pullBase;
          g.vx += nx * pull * dt;
          g.vy += ny * pull * dt;
        }

        g.x += g.vx * dt;
        g.y += g.vy * dt;

        const rr = player.r + g.r + LOOT_CONFIG.pickupPadding;
        if (d2 <= rr*rr){
          g.alive = false;
          addXP(g.v);
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
    function render(){
      const camX = player.x - W*0.5;
      const camY = player.y - H*0.5;
      const boss = getActiveBoss();
      updateActiveObstacles(camX, camY, W, H, ACTIVE_OBSTACLE_PAD);
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
      updatePlayer(dt);

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
          buffs.shield = Math.max(buffs.shield, 1.2);
          popFloatText(player.x, player.y - player.r - 18, "Second Chance!", COLORS.heal, 18, 1.6, 16, 40, 90);
        } else {
          player.hp = 0;
          openGameOver();
        }
      }
    }

    function frame(now){
      const dt = Math.min(LOOP_CONFIG.maxDt, (now - last) / 1000);
      last = now;

      if (state === STATE.PLAYING){
        updateFps(dt);
        update(dt);
      } else {
        if (particles.length) updateParticles(dt);
        if (dmgTexts.length || floatTexts.length) updateTexts(dt);
        if (telegraphs.length) updateTelegraphs(dt);
      }

      render();
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
  
