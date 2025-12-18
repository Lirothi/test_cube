import {
  COLORS,
  PLAYER_CONFIG,
  XP_CONFIG,
  BUFF_EFFECTS,
  LOOT_CONFIG,
  LOOP_CONFIG,
} from "./config.js";
import { rand, hypot } from "./math.js";
import { sound } from "./audio.js";
import { setupInput, clearDirectionalInput } from "./input.js";
import { resetPlayer, updatePlayer, addXP, setPlayerRuntime } from "./player.js";
import { spawnController, getActiveBoss, resetSpawnState } from "./spawn.js";
import { updateTelegraphs } from "./telegraph.js";
import { renderFrame } from "./render.js";
import {
  ui,
  isMobile,
  updateGodButton,
  updateMuteButton,
  updateMenuStats,
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
import { spawnShockwave, damageEnemy, updateEnemyShots, updateVoidZones, updateEnemies } from "./enemies.js";
import { addParticles, updateParticles, resetParticles } from "./particles.js";
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
  updateWeapons,
} from "./weapons.js";
import {
  spawnObstacles,
  updateActiveObstacles,
  damageObstacle,
  damageObstaclesInRadius,
  setObstacleRuntime,
} from "./obstacles.js";
import {
  WORLD,
  BASE_STATS,
  player,
  buffs,
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
  spawn,
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
    const STATE = { PLAYING:"playing", LEVELUP:"levelup", GAMEOVER:"gameover", MENU:"menu" };
    let state = STATE.PLAYING;
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
      currentCards = pickUpgrades(XP_CONFIG.cardChoices);
      renderUpgradeCards(currentCards, (u) => {
        if (state !== STATE.LEVELUP) return;
        u.apply();
        closeLevelUp();
      });
      openLevelUpUI();
      sound.play("level");
    }
    function closeLevelUp(){
      closeLevelUpUI();
      state = STATE.PLAYING;
      focusCanvas();
    }
    setPlayerRuntime({ openLevelUp });
    setChestRuntime({ addXP });

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
      resetWeapons();
      resetUpgradeState();
      resetDps();
      startNoticeT = START_NOTICE_TIME;

      closeGameOverUI();
      closeLevelUpUI();
      closeMenuUI();
      clearDirectionalInput();

      state = STATE.PLAYING;
      last = performance.now();
      focusCanvas();
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

    /* ============================
       Buffs / Player / Enemies / Projectiles
       ============================ */
    function updateBuffs(dt){
      buffs.magnet = Math.max(0, buffs.magnet - dt);
      buffs.shield = Math.max(0, buffs.shield - dt);
      buffs.slow = Math.max(0, buffs.slow - dt);
      buffs.power = Math.max(0, buffs.power - dt);
      buffs.haste = Math.max(0, buffs.haste - dt);
      buffs.xp = Math.max(0, buffs.xp - dt);
    }


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

    function updateTexts(dt){
      for (let i=dmgTexts.length-1;i>=0;i--){
        const d = dmgTexts[i];
        if (!d.alive){ dmgTexts[i] = dmgTexts[dmgTexts.length-1]; dmgTexts.pop(); dmgPool.put(d); continue; }
        d.life -= dt;
        d.x += d.vx * dt;
        d.y += d.vy * dt;
        d.vx *= Math.pow(0.12, dt);
        d.vy *= Math.pow(0.10, dt);
        if (d.life <= 0) d.alive = false;
        if (!d.alive){
          dmgTexts[i] = dmgTexts[dmgTexts.length-1];
          dmgTexts.pop();
          dmgPool.put(d);
        }
      }

      for (let i=floatTexts.length-1;i>=0;i--){
        const t = floatTexts[i];
        if (!t.alive){ floatTexts[i] = floatTexts[floatTexts.length-1]; floatTexts.pop(); textPool.put(t); continue; }
        t.life -= dt;
        t.x += t.vx * dt;
        t.y += t.vy * dt;
        t.vx *= Math.pow(0.12, dt);
        t.vy *= Math.pow(0.10, dt);
        if (t.life <= 0) t.alive = false;
        if (!t.alive){
          floatTexts[i] = floatTexts[floatTexts.length-1];
          floatTexts.pop();
          textPool.put(t);
        }
      }
    }

    function cleanupDeadEnemies(camX, camY){
      const minX = camX - WORLD.despawnPad;
      const maxX = camX + W + WORLD.despawnPad;
      const minY = camY - WORLD.despawnPad;
      const maxY = camY + H + WORLD.despawnPad;
      const worldLim = WORLD.halfSize + WORLD.despawnPad;

      for (let i=enemies.length-1;i>=0;i--){
        const e = enemies[i];
        const outWorld = (e.x < -worldLim || e.x > worldLim || e.y < -worldLim || e.y > worldLim);
        if (!e.alive || e.x < minX || e.x > maxX || e.y < minY || e.y > maxY || outWorld){
          if (e.boss) continue; // never despawn the boss offscreen
          enemies[i] = enemies[enemies.length-1];
          enemies.pop();
          if (e.alive) e.alive = false;
          if (e.boss) spawn.bossAlive = false;
          enemyPool.put(e);
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
      updateActiveObstacles(camX, camY, W, H, ACTIVE_OBSTACLE_PAD);

      updateWeapons(dt);
      updateEnemies(dt, godMode);
      updateTelegraphs(dt);
      updateVoidZones(dt, godMode);
      updateEnemyShots(dt);
      updateGems(dt);
      updateParticles(dt);
      updateTexts(dt);

      cleanupDeadEnemies(camX, camY);

      if (player.hp <= 0){
        player.hp = 0;
        openGameOver();
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
    if (ui.btnMobileMenu){
      ui.btnMobileMenu.addEventListener("click", (e) => {
        e.stopPropagation();
        if (state === STATE.PLAYING) openMenu();
        else if (state === STATE.MENU) closeMenu();
      }, { passive:true });
    }

  })();
  


