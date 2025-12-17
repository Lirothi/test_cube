import {
  BUILD,
  COLORS,
  PLAYER_CONFIG,
  XP_CONFIG,
  BUFF_EFFECTS,
  UPGRADE_CONFIG,
  CRIT_UPGRADES,
  SPAWN_CONFIG,
  TELEGRAPH_CONFIG,
  CHEST_CONFIG,
  WEAPON_CONFIG,
  WEAPON_MASTERY,
  MAX_WEAPONS,
  DPS_LABELS,
  WEAPON_RIDERS,
  ENEMY_BEHAVIOR,
  RANGED_SHOT_CONFIG,
  LOOT_CONFIG,
  LOOP_CONFIG,
  ELITE_CONFIG,
  BOSS_CONFIG,
  ENEMY_TYPES
} from "./config.js";

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
       UI
       ============================ */
    const ui = {
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
      hint: document.getElementById("hint"),
      loadout: document.getElementById("uiLoadout"),
      bonuses: document.getElementById("uiBonuses"),
      mTime: document.getElementById("mUiTime"),
      mLevel: document.getElementById("mUiLevel"),
      mKills: document.getElementById("mUiKills"),
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
    ui.uiBuild.textContent = BUILD;
    ui.menuBuild.textContent = BUILD;

    /* ============================
       Helpers
       ============================ */
    const TAU = Math.PI * 2;
    const isMobile = /Mobi|Android|iPhone|iPad|iPod|Touch/i.test(navigator.userAgent) || (navigator.maxTouchPoints || 0) > 0;
    ui.hint.textContent = isMobile
      ? "Move: drag to steer | Tap chests to open | Auto-attacks | Tap screen for focus"
      : "Move: WASD/Arrows | Chests: touch to open | Auto-attacks | ESC: Menu (Click/tap the canvas to focus keys)";
    if (isMobile) document.body.classList.add("mobile");
    let godMode = false;
    const UI_COLORS = {
      strokeDim: "rgba(255,255,255,.10)",
      textStroke: "rgba(0,0,0,.35)",
      chestFill: "rgba(70,255,143,0.18)",
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

    const sound = {
      ctx: null,
      unlocked: false,
      enabled: true,
      master: null,
      bank: {
        shoot: { freq: 720, type: "square", dur: 0.06, vol: 0.05 },
        rail:  { freq: 220, type: "sawtooth", dur: 0.18, vol: 0.08, glide: 140 },
        axe:   { freq: 320, type: "triangle", dur: 0.12, vol: 0.06 },
        pickup:{ freq: 960, type: "square", dur: 0.05, vol: 0.04 },
        level: { freq: 560, type: "triangle", dur: 0.25, vol: 0.06, glide: 140 },
        hurt:  { freq: 160, type: "sawtooth", dur: 0.12, vol: 0.08 },
        boss:  { freq: 90,  type: "square", dur: 0.4,  vol: 0.10, glide: -60 },
      },
      unlock(){
        if (this.unlocked) return;
        try {
          this.ctx = this.ctx || new (window.AudioContext || window.webkitAudioContext)();
          this.ctx.resume();
          if (!this.master){
            this.master = this.ctx.createGain();
            this.master.gain.value = 0.35;
            this.master.connect(this.ctx.destination);
          }
          this.unlocked = true;
        } catch {}
      },
      play(name){
        if (!this.enabled) return;
        if (!this.unlocked) this.unlock();
        const cfg = this.bank[name];
        if (!cfg || !this.ctx || !this.master) return;
        const ctx = this.ctx;
        const now = ctx.currentTime;
        const osc = ctx.createOscillator();
        const gain = ctx.createGain();
        osc.type = cfg.type || "sine";
        osc.frequency.value = cfg.freq || 440;
        if (cfg.glide){
          osc.frequency.linearRampToValueAtTime(Math.max(20, (cfg.freq || 440) + cfg.glide), now + Math.max(0.01, cfg.dur || 0.1));
        }
        const vol = cfg.vol || 0.05;
        gain.gain.setValueAtTime(vol, now);
        gain.gain.exponentialRampToValueAtTime(0.0001, now + (cfg.dur || 0.1));
        osc.connect(gain);
        gain.connect(this.master);
        osc.start(now);
        osc.stop(now + (cfg.dur || 0.1));
      }
    };
    function updateGodButton(){
      if (ui.btnGod) ui.btnGod.textContent = `God Mode: ${godMode ? "On" : "Off"}`;
    }
    function updateMuteButton(){
      if (ui.btnMute) ui.btnMute.textContent = `Sound: ${sound.enabled ? "On" : "Off"}`;
    }
    updateGodButton();
    updateMuteButton();
    const rand = (a=1,b=0)=> (Math.random()*(a-b)+b);
    const randi = (a,b=0)=> (Math.random()*(a-b)+b) | 0;
    const clamp = (v, lo, hi) => v < lo ? lo : v > hi ? hi : v;
    const hypot = Math.hypot;
    const fmtFloat = (n, digits=2) => {
      const s = n.toFixed(digits).replace(/\.?0+$/,"");
      return s === "-0" ? "0" : s;
    };
    function calcCrit(dmg, chance, mult){
      const crit = Math.random() < chance;
      return { dmg: crit ? dmg * mult : dmg, crit };
    }

    /* ============================
       Input (robust: uses e.code)
       ============================ */
    const input = { up:false, down:false, left:false, right:false };
    const clearDirectionalInput = () => { input.up = input.down = input.left = input.right = false; };
    const CODE_MAP = new Map([
      ["KeyW", "up"], ["ArrowUp", "up"],
      ["KeyS", "down"], ["ArrowDown", "down"],
      ["KeyA", "left"], ["ArrowLeft", "left"],
      ["KeyD", "right"], ["ArrowRight", "right"],
    ]);

    addEventListener("keydown", (e) => {
      if (e.code === "Escape"){
        if (state === STATE.PLAYING){
          openMenu();
        } else if (state === STATE.MENU){
          closeMenu();
        }
        e.preventDefault();
        return;
      }

      const m = CODE_MAP.get(e.code);
      if (m) { input[m] = true; e.preventDefault(); }
    }, { passive:false });

    addEventListener("keyup", (e) => {
      const m = CODE_MAP.get(e.code);
      if (m) { input[m] = false; e.preventDefault(); }
    }, { passive:false });
    addEventListener("keydown", () => sound.unlock(), { passive:true });

    // Touch drag controls for mobile (simple virtual stick)
    if (isMobile){
      let touchId = null;
      let startX = 0, startY = 0;
      const stick = document.getElementById("stick");
      const stickInner = document.getElementById("stickInner");
      const stickOuter = document.getElementById("stickOuter");
      const resetStick = () => {
        if (stickInner) stickInner.style.transform = "translate(0px,0px)";
        if (stick) stick.classList.remove("on");
      };
      const DEAD = 12;
      const updateTouchDir = (x,y) => {
        const dx = x - startX;
        const dy = y - startY;
        clearDirectionalInput();
        if (Math.abs(dx) > DEAD){
          if (dx > 0) input.right = true; else input.left = true;
        }
        if (Math.abs(dy) > DEAD){
          if (dy > 0) input.down = true; else input.up = true;
        }
        if (stickInner){
          const clampLen = 48;
          const len = Math.min(clampLen, Math.hypot(dx, dy));
          const ang = Math.atan2(dy, dx);
          stickInner.style.transform = `translate(${Math.cos(ang)*len}px, ${Math.sin(ang)*len}px)`;
        }
      };
      const endTouch = () => { touchId = null; clearDirectionalInput(); resetStick(); };
      canvas.addEventListener("touchstart", (e) => {
        if (touchId !== null) return;
        const t = e.changedTouches[0];
        touchId = t.identifier;
        startX = t.clientX;
        startY = t.clientY;
        if (stick){
          stick.classList.add("on");
          stick.style.left = `${startX - 60}px`;
          stick.style.top = `${startY - 60}px`;
        }
        updateTouchDir(startX, startY);
      }, { passive:true });
      canvas.addEventListener("touchmove", (e) => {
        if (touchId === null) return;
        for (let i=0;i<e.changedTouches.length;i++){
          const t = e.changedTouches[i];
          if (t.identifier === touchId){
            updateTouchDir(t.clientX, t.clientY);
            e.preventDefault();
            break;
          }
        }
      }, { passive:false });
      const touchEndHandler = (e) => {
        if (touchId === null) return;
        for (let i=0;i<e.changedTouches.length;i++){
          if (e.changedTouches[i].identifier === touchId){
            endTouch();
            break;
          }
        }
      };
      canvas.addEventListener("touchend", touchEndHandler, { passive:true });
      canvas.addEventListener("touchcancel", touchEndHandler, { passive:true });
    }

    /* ============================
       Pools
       ============================ */
    function makePool(createFn, initial = 256) {
      const free = [];
      for (let i=0;i<initial;i++) free.push(createFn());
      return {
        get() { return free.length ? free.pop() : createFn(); },
        put(o) { free.push(o); }
      };
    }

    /* ============================
       World / State
       ============================ */
    const WORLD = { spawnPad: 80, despawnPad: 240 };
    const STATE = { PLAYING:"playing", LEVELUP:"levelup", GAMEOVER:"gameover", MENU:"menu" };
    let state = STATE.PLAYING;
    let fpsAccum = 0, fpsCount = 0;
    function updateFps(dt){
      if (!ui.fps) return;
      fpsAccum += (dt > 0 ? (1/dt) : 0);
      fpsCount++;
      if (fpsCount >= 10){
        const fps = fpsAccum / fpsCount;
        ui.fps.textContent = `${Math.round(fps)} fps`;
        fpsAccum = 0;
        fpsCount = 0;
      }
    }

    /* ============================
       Player + Buffs
       ============================ */
    const BASE_STATS = { hp:120, speed:230, pickup:130 };

    const player = {
      x:0,y:0, r:PLAYER_CONFIG.radius,
      hp:BASE_STATS.hp, maxHp:BASE_STATS.hp,
      speed:BASE_STATS.speed,
      pickup:BASE_STATS.pickup,
      iFrame:0,
      level:1,
      xp:0,
      xpNeed:XP_CONFIG.baseNeed,
      kills:0,
      time:0,
    };

    const buffs = {
      magnet: 0,
      shield: 0,
      slow: 0,
      power: 0, // damage/haste boost
      haste: 0, // movement burst
      xp: 0,    // XP multiplier
    };

    function xpNeedForLevel(lvl){
      const delta = lvl - 1;
      return Math.floor(
        XP_CONFIG.baseNeed +
        delta * XP_CONFIG.perLevel +
        Math.pow(delta, XP_CONFIG.curvePower) * XP_CONFIG.curveScale
      );
    }

    function resetPlayer() {
      player.x = 0; player.y = 0;
      player.hp = BASE_STATS.hp; player.maxHp = BASE_STATS.hp;
      player.speed = BASE_STATS.speed;
      player.pickup = BASE_STATS.pickup;
      player.iFrame = 0;
      player.level = 1;
      player.xp = 0;
      player.xpNeed = xpNeedForLevel(1);
      player.kills = 0;
      player.time = 0;
      buffs.magnet = 0;
      buffs.shield = 0;
      buffs.slow = 0;
      buffs.power = 0;
      buffs.haste = 0;
      buffs.xp = 0;
    }

    /* ============================
       Entities
       ============================ */
    const enemies = [];
    const bullets = [];
    const rails = [];
    const axes = [];
    const orbs = [];
    const enemyShots = [];
    const telegraphs = [];
    const voidZones = [];
    const gems = [];
    const particles = [];
    const chests = [];
    const dmgTexts = [];
    const floatTexts = [];

    const enemyPool = makePool(() => ({
      alive:false, type:"A",
      x:0,y:0, r:10,
      hp:1, maxHp:1,
      speed:1, dmg:1,
      color:"#fff",
      kx:0, ky:0,
      ranged:false,
      shotCd:0, shotDmg:0, shotSpeed:0, shotRange:0, shotT:0,
      shotSeq:0,
      spitter:false, spitCd:0, spitRange:0, spitRadius:0, spitDuration:0, spitDps:0, spitColor:"#fff", spitTelegraph:0, spitType:"", spitT:0,
      boss:false, novaCd:0, novaT:0, novaShots:0, novaShotSpeed:0, novaShotDmg:0, novaRadius:0, novaTelegraph:0, novaSeq:0,
      slowT:0, slowMul:1,
      burnT:0, burnDps:0,
      bleedT:0, bleedDps:0,
      elite:false, knockResist:0, gemBonus:0,
    }), 260);

    const bulletPool= makePool(() => ({ alive:false, x:0,y:0, vx:0,vy:0, r:3,   dmg:8,  life:0, critChance:0, critMult:1 }), 260);
    const railPool  = makePool(() => ({ alive:false, x:0,y:0, vx:0,vy:0, r:4.4, dmg:60, life:0, pierce:0, trail:[], critChance:0, critMult:1 }), 160);
    const axePool   = makePool(() => ({ alive:false, x:0,y:0, vx:0,vy:0, r:6,   dmg:18, life:0, rot:0, spin:0, critChance:0, critMult:1 }), 120);
    const shotPool  = makePool(() => ({ alive:false, x:0,y:0, vx:0,vy:0, r:3.6, dmg:8,  life:0, color:COLORS.gem }), 240);
    const voidPool  = makePool(() => ({ alive:false, x:0,y:0, radius:0, life:0, maxLife:0, dps:0, tick:0.25, tickT:0, color:"#fff", type:"poison" }), 120);
    const orbPool   = makePool(() => ({ alive:false, x:0,y:0, vx:0,vy:0, r:10, dmg:0, critChance:0, critMult:1, state:"fly", life:0, park:0, tick:0, pull:0, radius:0, explosion:0 }), 80);

    const gemPool   = makePool(() => ({ alive:false, x:0,y:0, vx:0,vy:0, v:1, r:5 }), 260);
    const partPool  = makePool(() => ({ alive:false, x:0,y:0, vx:0,vy:0, life:0, maxLife:0, r:2, color:COLORS.gem }), 520);
    const chestPool = makePool(() => ({ alive:false, x:0,y:0, r:12, pulse:0 }), 24);
    const dmgPool   = makePool(() => ({ alive:false, x:0,y:0, vx:0,vy:0, life:0, maxLife:0, text:"", color:"#fff", size:14 }), 240);
    const textPool  = makePool(() => ({ alive:false, x:0,y:0, vx:0,vy:0, life:0, maxLife:0, text:"", color:"#fff", size:14 }), 120);

    /* ============================
       Weapons
       ============================ */
    const weapons = {
      magic: { unlocked:true, level:1, mastery:0, t:0 },
      aura:  { unlocked:false, level:0, mastery:0, tick:0 },
      rail:  { unlocked:false, level:0, mastery:0, t:0 },
      axe:   { unlocked:false, level:0, mastery:0, t:0 },
      orb:   { unlocked:false, level:0, mastery:0, t:0 },
    };

    function resetWeapons(){
      weapons.magic.unlocked = true; weapons.magic.level = 1; weapons.magic.mastery = 0; weapons.magic.t = 0;
      weapons.aura.unlocked  = false; weapons.aura.level  = 0; weapons.aura.mastery  = 0; weapons.aura.tick = 0;
      weapons.rail.unlocked  = false; weapons.rail.level  = 0; weapons.rail.mastery  = 0; weapons.rail.t = 0;
      weapons.axe.unlocked   = false; weapons.axe.level   = 0; weapons.axe.mastery   = 0; weapons.axe.t = 0;
      weapons.orb.unlocked   = false; weapons.orb.level   = 0; weapons.orb.mastery   = 0; weapons.orb.t = 0;
    }

    function weaponCount(){
      let c = 0;
      if (weapons.magic.unlocked) c++;
      if (weapons.aura.unlocked) c++;
      if (weapons.rail.unlocked) c++;
      if (weapons.axe.unlocked) c++;
      if (weapons.orb.unlocked) c++;
      return c;
    }

    function magicStats(){
      const cfg = WEAPON_CONFIG.magic;
      const lv = weapons.magic.level;
      const mastery = weapons.magic.mastery || 0;
      const masteryDmgMult = 1 + mastery * WEAPON_MASTERY.dmgMult;
      const masteryCrit = mastery * WEAPON_MASTERY.critChance;
      const masteryCritMult = mastery * WEAPON_MASTERY.critMult;
      const powerMul = (buffs.power > 0) ? cfg.powerDmgMult : 1.0;
      const cdMul = (buffs.power > 0) ? cfg.powerCdMult : 1.0;
      const dmg = (cfg.dmgBase + lv * cfg.dmgPerLevel) * powerMul * masteryDmgMult;
      const cd = Math.max(cfg.cdMin, cfg.cdBase - lv * cfg.cdPerLevel) * cdMul;
      const speed = cfg.speedBase + lv * cfg.speedPerLevel;
      const count = 1 + Math.floor((lv-1) / cfg.countInterval);
      const range = cfg.range;
      const knock = (cfg.knockBase + lv * cfg.knockPerLevel) * powerMul;
      const critChance = Math.min(1, cfg.crit.base + lv * cfg.crit.perLevel + masteryCrit);
      const critMult = cfg.crit.multBase + lv * cfg.crit.multPerLevel + masteryCritMult + upgradeState.critMultLv * CRIT_UPGRADES.multPerLevel;
      const critChanceTotal = clamp(critChance + upgradeState.critChanceLv * CRIT_UPGRADES.chancePerLevel, 0, 1);
      return { dmg, cd, speed, count, range, knock, critChance:critChanceTotal, critMult };
    }
    function auraStats(){
      const cfg = WEAPON_CONFIG.aura;
      const lv = weapons.aura.level;
      const mastery = weapons.aura.mastery || 0;
      const masteryDmgMult = 1 + mastery * WEAPON_MASTERY.dmgMult;
      const masteryCrit = mastery * WEAPON_MASTERY.critChance;
      const masteryCritMult = mastery * WEAPON_MASTERY.critMult;
      const powerMul = (buffs.power > 0) ? cfg.powerDmgMult : 1.0;
      const radius = cfg.radiusBase + lv * cfg.radiusPerLevel;
      const tick = cfg.tick;
      const dmg = (cfg.dmgBase + lv * cfg.dmgPerLevel) * powerMul * masteryDmgMult;
      const knock = (cfg.knockBase + lv * cfg.knockPerLevel) * powerMul;
      const critChance = Math.min(1, cfg.crit.base + lv * cfg.crit.perLevel + masteryCrit);
      const critMult = cfg.crit.multBase + lv * cfg.crit.multPerLevel + masteryCritMult + upgradeState.critMultLv * CRIT_UPGRADES.multPerLevel;
      const critChanceTotal = clamp(critChance + upgradeState.critChanceLv * CRIT_UPGRADES.chancePerLevel, 0, 1);
      return { radius, tick, dmg, knock, critChance:critChanceTotal, critMult };
    }
    function axeStats(){
      const cfg = WEAPON_CONFIG.axe;
      const lv = weapons.axe.level;
      const mastery = weapons.axe.mastery || 0;
      const masteryDmgMult = 1 + mastery * WEAPON_MASTERY.dmgMult;
      const masteryCrit = mastery * WEAPON_MASTERY.critChance;
      const masteryCritMult = mastery * WEAPON_MASTERY.critMult;
      const powerMul = (buffs.power > 0) ? cfg.powerDmgMult : 1.0;
      const cdMul = (buffs.power > 0) ? cfg.powerCdMult : 1.0;
      const cd = Math.max(cfg.cdMin, cfg.cdBase - lv * cfg.cdPerLevel) * cdMul;
      const dmg = (cfg.dmgBase + lv * cfg.dmgPerLevel) * powerMul * masteryDmgMult;
      const speed = cfg.speedBase + lv * cfg.speedPerLevel;
      const count = 1 + Math.floor((lv-1) / cfg.countInterval);
      const gravity = cfg.gravity;
      const knock = (cfg.knockBase + lv * cfg.knockPerLevel) * powerMul;
      const critChance = Math.min(1, cfg.crit.base + lv * cfg.crit.perLevel + masteryCrit);
      const critMult = cfg.crit.multBase + lv * cfg.crit.multPerLevel + masteryCritMult + upgradeState.critMultLv * CRIT_UPGRADES.multPerLevel;
      const critChanceTotal = clamp(critChance + upgradeState.critChanceLv * CRIT_UPGRADES.chancePerLevel, 0, 1);
      return { cd, dmg, speed, count, gravity, knock, critChance:critChanceTotal, critMult };
    }
    function railStats(){
      const cfg = WEAPON_CONFIG.rail;
      const lv = weapons.rail.level;
      const mastery = weapons.rail.mastery || 0;
      const masteryDmgMult = 1 + mastery * WEAPON_MASTERY.dmgMult;
      const masteryCrit = mastery * WEAPON_MASTERY.critChance;
      const masteryCritMult = mastery * WEAPON_MASTERY.critMult;
      const powerMul = (buffs.power > 0) ? cfg.powerDmgMult : 1.0;
      const cdMul = (buffs.power > 0) ? cfg.powerCdMult : 1.0;
      const cd = Math.max(cfg.cdMin, cfg.cdBase - lv * cfg.cdPerLevel) * cdMul;
      const dmg = (cfg.dmgBase + lv * cfg.dmgPerLevel) * powerMul * masteryDmgMult;
      const speed = cfg.speedBase + lv * cfg.speedPerLevel;
      const pierce = cfg.pierceBase + Math.floor((lv+1) / cfg.pierceLevelDivisor) + (buffs.power > 0 ? cfg.powerPierceBonus : 0);
      const range = cfg.rangeBase + lv * cfg.rangePerLevel;
      const knock = (cfg.knockBase + lv * cfg.knockPerLevel) * powerMul;
      const critChance = Math.min(1, cfg.crit.base + lv * cfg.crit.perLevel + masteryCrit);
      const critMult = cfg.crit.multBase + lv * cfg.crit.multPerLevel + masteryCritMult + upgradeState.critMultLv * CRIT_UPGRADES.multPerLevel;
      const critChanceTotal = clamp(critChance + upgradeState.critChanceLv * CRIT_UPGRADES.chancePerLevel, 0, 1);
      return { cd, dmg, speed, pierce, range, knock, critChance:critChanceTotal, critMult };
    }
    function orbStats(){
      const cfg = WEAPON_CONFIG.orb;
      const lv = weapons.orb.level;
      const mastery = weapons.orb.mastery || 0;
      const masteryDmgMult = 1 + mastery * WEAPON_MASTERY.dmgMult;
      const masteryCrit = mastery * WEAPON_MASTERY.critChance;
      const masteryCritMult = mastery * WEAPON_MASTERY.critMult;
      const powerMul = (buffs.power > 0) ? cfg.powerDmgMult : 1.0;
      const cdMul = (buffs.power > 0) ? cfg.powerCdMult : 1.0;
      const dmg = (cfg.dmgBase + lv * cfg.dmgPerLevel) * powerMul * masteryDmgMult;
      const cd = Math.max(cfg.cdMin, cfg.cdBase - lv * cfg.cdPerLevel) * cdMul;
      const speed = cfg.speedBase + lv * cfg.speedPerLevel;
      const radius = cfg.pullRadiusBase + lv * cfg.pullRadiusPerLevel;
      const pull = cfg.pullBase + lv * cfg.pullPerLevel;
      const critChance = Math.min(1, cfg.crit.base + lv * cfg.crit.perLevel + masteryCrit);
      const critMult = cfg.crit.multBase + lv * cfg.crit.multPerLevel + masteryCritMult + upgradeState.critMultLv * CRIT_UPGRADES.multPerLevel;
      const critChanceTotal = clamp(critChance + upgradeState.critChanceLv * CRIT_UPGRADES.chancePerLevel, 0, 1);
      const explosion = dmg * cfg.explosionMult;
      const park = cfg.parkTimeBase + lv * cfg.parkTimePerLevel;
      return { dmg, cd, speed, radius, pull, range: cfg.range, tick: cfg.tick, park, explosion, critChance:critChanceTotal, critMult };
    }

    /* ============================
       Upgrades
       ============================ */
    const DPS_TRACKER = { magic:0, aura:0, rail:0, axe:0, orb:0 };

    const upgradeState = { speedLv:0, hpLv:0, pickupLv:0, critChanceLv:0, critMultLv:0 };
    function resetUpgradeState(){
      upgradeState.speedLv=0;
      upgradeState.hpLv=0;
      upgradeState.pickupLv=0;
      upgradeState.critChanceLv=0;
      upgradeState.critMultLv=0;
    }
    function resetDps(){ DPS_TRACKER.magic=0; DPS_TRACKER.aura=0; DPS_TRACKER.rail=0; DPS_TRACKER.axe=0; DPS_TRACKER.orb=0; }

    function weaponTag(weapon, maxLv){
      if (!weapon.unlocked) return "Weapon - Unlock";
      const base = `Weapon - Lv ${weapon.level}/${maxLv}`;
      return weapon.mastery > 0 ? `${base} (M${weapon.mastery})` : base;
    }

            const UPGRADES = [
      {
        id: "magic", title: "Magic Bullet",
        desc: "Shoots the nearest enemy automatically. Max level unlocks mastery ranks that boost damage and crits.",
        tag: () => weaponTag(weapons.magic, WEAPON_CONFIG.magic.maxLevel),
        can: () => true,
        apply: () => {
          weapons.magic.unlocked = true;
          if (weapons.magic.level < WEAPON_CONFIG.magic.maxLevel) weapons.magic.level++;
          else weapons.magic.mastery++;
        }
      },
      {
        id: "aura", title: "Holy Aura",
        desc: "A luminous field around you that damages and pushes enemies back. Extra ranks past max add damage and crit scaling.",
        tag: () => weaponTag(weapons.aura, WEAPON_CONFIG.aura.maxLevel),
        can: () => (weapons.aura.unlocked) || weaponCount() < MAX_WEAPONS,
        apply: () => {
          if (!weapons.aura.unlocked){ weapons.aura.unlocked=true; weapons.aura.level=1; return; }
          if (weapons.aura.level < WEAPON_CONFIG.aura.maxLevel) weapons.aura.level++;
          else weapons.aura.mastery++;
        }
      },
      {
        id: "rail", title: "Railgun",
        desc: "Charges a piercing rail shot that crosses the map with huge damage. Mastery after max level boosts damage/crit.",
        tag: () => weaponTag(weapons.rail, WEAPON_CONFIG.rail.maxLevel),
        can: () => (weapons.rail.unlocked) || weaponCount() < MAX_WEAPONS,
        apply: () => {
          if (!weapons.rail.unlocked){ weapons.rail.unlocked=true; weapons.rail.level=1; return; }
          if (weapons.rail.level < WEAPON_CONFIG.rail.maxLevel) weapons.rail.level++;
          else weapons.rail.mastery++;
        }
      },
      {
        id: "axe", title: "Axe Throw",
        desc: "Throws axes in a neon arc. Strong burst + heavy knockback. Mastery adds damage/crit scaling past max.",
        tag: () => weaponTag(weapons.axe, WEAPON_CONFIG.axe.maxLevel),
        can: () => (weapons.axe.unlocked) || weaponCount() < MAX_WEAPONS,
        apply: () => {
          if (!weapons.axe.unlocked){ weapons.axe.unlocked=true; weapons.axe.level=1; return; }
          if (weapons.axe.level < WEAPON_CONFIG.axe.maxLevel) weapons.axe.level++;
          else weapons.axe.mastery++;
        }
      },
      {
        id: "orb", title: "Singularity Orb",
        desc: "Launch an orb that parks, pulls enemies inward, pulses damage, then explodes. Mastery boosts damage/crit after max.",
        tag: () => weaponTag(weapons.orb, WEAPON_CONFIG.orb.maxLevel),
        can: () => (weapons.orb.unlocked) || weaponCount() < MAX_WEAPONS,
        apply: () => {
          if (!weapons.orb.unlocked){ weapons.orb.unlocked=true; weapons.orb.level=1; return; }
          if (weapons.orb.level < WEAPON_CONFIG.orb.maxLevel) weapons.orb.level++;
          else weapons.orb.mastery++;
        }
      },
      {
        id: "speed", title: "Speed Up",
        desc: "Move faster to kite swarms and reach chests sooner.",
        tag: () => `Passive - Lv ${upgradeState.speedLv}/${UPGRADE_CONFIG.passiveMaxLevel}`,
        can: () => upgradeState.speedLv < UPGRADE_CONFIG.passiveMaxLevel,
        apply: () => { upgradeState.speedLv++; player.speed *= UPGRADE_CONFIG.speedMult; }
      },
      {
        id: "hp", title: "Max HP Up",
        desc: "Increase maximum HP and heal a bit immediately.",
        tag: () => `Passive - Lv ${upgradeState.hpLv}/${UPGRADE_CONFIG.passiveMaxLevel}`,
        can: () => upgradeState.hpLv < UPGRADE_CONFIG.passiveMaxLevel,
        apply: () => {
          upgradeState.hpLv++;
          const add = UPGRADE_CONFIG.hpBaseGain + upgradeState.hpLv * UPGRADE_CONFIG.hpPerLevelGain;
          player.maxHp += add;
          player.hp = Math.min(player.maxHp, player.hp + Math.floor(add * UPGRADE_CONFIG.hpHealPct));
        }
      },
      {
        id: "pickup", title: "Pickup Range",
        desc: "Collect XP gems from farther away and pull them in faster.",
        tag: () => `Passive - Lv ${upgradeState.pickupLv}/${UPGRADE_CONFIG.passiveMaxLevel}`,
        can: () => upgradeState.pickupLv < UPGRADE_CONFIG.passiveMaxLevel,
        apply: () => { upgradeState.pickupLv++; player.pickup += UPGRADE_CONFIG.pickupGain; }
      },
      {
        id: "critChance", title: "Critical Chance",
        desc: "Increase critical strike chance for all weapons.",
        tag: () => `Passive - Lv ${upgradeState.critChanceLv}/${CRIT_UPGRADES.maxLevels}`,
        can: () => upgradeState.critChanceLv < CRIT_UPGRADES.maxLevels,
        apply: () => { upgradeState.critChanceLv = Math.min(CRIT_UPGRADES.maxLevels, upgradeState.critChanceLv + 1); }
      },
      {
        id: "critMult", title: "Critical Damage",
        desc: "Increase critical damage multiplier for all weapons.",
        tag: () => `Passive - Lv ${upgradeState.critMultLv}/${CRIT_UPGRADES.maxLevels}`,
        can: () => upgradeState.critMultLv < CRIT_UPGRADES.maxLevels,
        apply: () => { upgradeState.critMultLv = Math.min(CRIT_UPGRADES.maxLevels, upgradeState.critMultLv + 1); }
      },
    ];

    function pickUpgrades(n=XP_CONFIG.cardChoices){
      const available = [];
      for (let i=0;i<UPGRADES.length;i++) if (UPGRADES[i].can()) available.push(UPGRADES[i]);
      if (!available.length) return [];
      const picks = [];
      const used = new Set();
      for (let k=0;k<n;k++){
        let best = null, bestScore = -1;
        for (let i=0;i<available.length;i++){
          const u = available[i];
          if (used.has(u.id)) continue;
          let w = 1;
          if ((u.id==="aura" && !weapons.aura.unlocked) || (u.id==="axe" && !weapons.axe.unlocked) || (u.id==="rail" && !weapons.rail.unlocked) || (u.id==="orb" && !weapons.orb.unlocked)) w = UPGRADE_CONFIG.weightNewWeapon;
          if (u.id==="pickup") w *= UPGRADE_CONFIG.weightPickup;
          const s = Math.random() * w;
          if (s > bestScore){ bestScore = s; best = u; }
        }
        if (!best) break;
        used.add(best.id);
        picks.push(best);
      }
      while (picks.length < n) picks.push(available[randi(available.length)]);
      return picks;
    }

    function listUpgradeSummary(){
      const parts = [];
      const mTag = (w) => w.mastery ? ` (M${w.mastery})` : "";
      parts.push(`Magic Bullet Lv ${weapons.magic.level}${mTag(weapons.magic)}`);
      if (weapons.aura.unlocked) parts.push(`Holy Aura Lv ${weapons.aura.level}${mTag(weapons.aura)}`);
      if (weapons.rail.unlocked) parts.push(`Railgun Lv ${weapons.rail.level}${mTag(weapons.rail)}`);
      if (weapons.axe.unlocked) parts.push(`Axe Throw Lv ${weapons.axe.level}${mTag(weapons.axe)}`);
      if (weapons.orb.unlocked) parts.push(`Singularity Orb Lv ${weapons.orb.level}${mTag(weapons.orb)}`);
      if (upgradeState.speedLv) parts.push(`Speed +${upgradeState.speedLv}`);
      if (upgradeState.hpLv) parts.push(`Max HP +${upgradeState.hpLv}`);
      if (upgradeState.pickupLv) parts.push(`Pickup +${upgradeState.pickupLv}`);
      return parts.join(" | ");
    }

    function formatDpsSummary(){
      const t = Math.max(player.time, 0.1);
      const entries = [];
      const append = (key, unlocked) => {
        if (!unlocked) return;
        const dmg = DPS_TRACKER[key] || 0;
        const dps = dmg / t;
        entries.push(`${DPS_LABELS[key]}: ${Math.round(dps)} DPS (${Math.round(dmg)} dmg)`);
      };
      append("magic", weapons.magic.unlocked);
      append("aura", weapons.aura.unlocked);
      append("rail", weapons.rail.unlocked);
      append("axe", weapons.axe.unlocked);
      append("orb", weapons.orb.unlocked);
      return entries.length ? entries.join(" | ") : "No weapon damage";
    }

    function updateMenuStats(){
      if (!ui.menuPlayerStats || !ui.menuWeaponStats) return;
      const hp = `${Math.ceil(player.hp)} / ${Math.ceil(player.maxHp)}`;
      const speedPct = Math.round((player.speed / BASE_STATS.speed) * 100);
      const pickupPct = Math.round((player.pickup / BASE_STATS.pickup) * 100);
      const critBonusChance = Math.round(upgradeState.critChanceLv * CRIT_UPGRADES.chancePerLevel * 100);
      const critBonusMult = fmtFloat(1 + upgradeState.critMultLv * CRIT_UPGRADES.multPerLevel, 2);
      ui.menuPlayerStats.innerHTML = [
        `<div class="kv"><span>Level</span><span>Lv ${player.level}</span></div>`,
        `<div class="kv"><span>HP</span><span>${hp}</span></div>`,
        `<div class="kv"><span>Move Speed</span><span>${Math.round(player.speed)} (${speedPct}% base)</span></div>`,
        `<div class="kv"><span>Pickup</span><span>${Math.round(player.pickup)} (${pickupPct}% base)</span></div>`,
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
        parts.push(`Crit ${Math.round((s.critChance || 0) * 100)}% x${fmtFloat(s.critMult || 1, 2)}`);
        rows.push(`<div class="kv"><span>${label} Lv ${w.level}${w.mastery ? ` (M${w.mastery})` : ""}</span><span>${parts.join(" | ")}</span></div>`);
      };
      addWeapon("Magic", "magic", magicStats);
      addWeapon("Aura", "aura", auraStats);
      addWeapon("Railgun", "rail", railStats);
      addWeapon("Axe", "axe", axeStats);
      addWeapon("Orb", "orb", orbStats);
      ui.menuWeaponStats.innerHTML = rows.length ? rows.join("") : `<div class="kv"><span>Weapons</span><span>None unlocked</span></div>`;
    }

    /* ============================
       Spawning (rebalanced)
       ============================ */
    const spawn = {
      acc: 0,
      baseRate: SPAWN_CONFIG.baseRate,
      timeScale: SPAWN_CONFIG.timeScale,
      maxEnemies: SPAWN_CONFIG.maxEnemies,
      squadT: SPAWN_CONFIG.squadInterval,
      eliteT: ELITE_CONFIG.interval,
      bossSpawned: false,
      bossAlive: false,
      bossRef: null,
    };

    function pickSpawnPos(camX, camY){
      const pad = WORLD.spawnPad;
      const side = randi(4);
      const minX = camX - pad, maxX = camX + W + pad;
      const minY = camY - pad, maxY = camY + H + pad;

      let x, y;
      if (side === 0) { x = rand(maxX, minX); y = minY; }
      else if (side === 1) { x = maxX; y = rand(maxY, minY); }
      else if (side === 2) { x = rand(maxX, minX); y = maxY; }
      else { x = minX; y = rand(maxY, minY); }
      return { x, y };
    }

    function spawnEnemy(typeKey, camX, camY, hpMult, spdMult, dmgMult, elite=false, pos=null){
      const info = ENEMY_TYPES[typeKey];
      const e = enemyPool.get();
      e.alive = true;
      e.type = typeKey;
      e.r = info.r;

      e.maxHp = info.hp * hpMult * (elite ? ELITE_CONFIG.hpMult : 1);
      e.hp = e.maxHp;

      e.speed = info.speed * spdMult;

      // contact dmg scales with time
      e.dmg = info.dmg * dmgMult * (elite ? ELITE_CONFIG.dmgMult : 1);

      e.color = info.color;
      e.kx = 0; e.ky = 0;

      // ranged setup
      e.ranged = !!info.ranged;
      e.shotCd = info.shotCd || 0;
      e.shotDmg = (info.shotDmg || 0) * dmgMult; // projectile dmg scales with time too
      e.shotSpeed = info.shotSpeed || 0;
      e.shotRange = info.shotRange || 0;
      e.shotT = e.ranged ? rand(e.shotCd * RANGED_SHOT_CONFIG.startDelayMax, e.shotCd * RANGED_SHOT_CONFIG.startDelayMin) : 0;
      e.shotSeq = 0;
      e.novaSeq = 0;
      e.spitter = !!info.spit;
      e.spitCd = info.spit ? info.spit.cd : 0;
      e.spitRange = info.spit ? info.spit.range : e.shotRange;
      e.spitRadius = info.spit ? info.spit.radius : 0;
      e.spitDuration = info.spit ? info.spit.duration : 0;
      e.spitDps = (info.spit ? info.spit.dps : 0) * dmgMult;
      e.spitTick = info.spit ? (info.spit.tick || 0.25) : 0.25;
      e.spitColor = info.spit ? info.spit.color : COLORS.warn;
      e.spitTelegraph = info.spit ? (info.spit.telegraph || TELEGRAPH_CONFIG.enemyTime) : TELEGRAPH_CONFIG.enemyTime;
      e.spitType = info.spit ? (info.spit.type || "poison") : "";
      e.spitT = e.spitter ? rand((e.spitCd || RANGED_SHOT_CONFIG.defaultCd) * RANGED_SHOT_CONFIG.startDelayMax, (e.spitCd || RANGED_SHOT_CONFIG.defaultCd) * RANGED_SHOT_CONFIG.startDelayMin) : 0;
      e.boss = !!info.boss;
      e.novaCd = info.nova ? info.nova.cd : 0;
      e.novaShots = info.nova ? info.nova.shots : 0;
      e.novaShotSpeed = info.nova ? info.nova.shotSpeed : 0;
      e.novaShotDmg = info.nova ? info.nova.shotDmg * dmgMult : 0;
      e.novaRadius = info.nova ? info.nova.radius : 0;
      e.novaTelegraph = info.nova ? info.nova.telegraph : TELEGRAPH_CONFIG.enemyTime;
      e.novaT = e.boss ? rand(e.novaCd * RANGED_SHOT_CONFIG.startDelayMax, e.novaCd * RANGED_SHOT_CONFIG.startDelayMin) : 0;
      e.slowT = 0; e.slowMul = 1;
      e.burnT = 0; e.burnDps = 0;
      e.bleedT = 0; e.bleedDps = 0;
      e.elite = elite;
      e.knockResist = (info.knockResist || 0) + (elite ? ELITE_CONFIG.knockResist : 0);
      e.gemBonus = (elite ? ELITE_CONFIG.extraGems : 0) + (e.boss ? BOSS_CONFIG.lootGems : 0);
      if (e.boss){
        spawn.bossAlive = true;
        spawn.bossRef = e;
      }

      const p = pos || pickSpawnPos(camX, camY);
      e.x = p.x; e.y = p.y;

      enemies.push(e);
    }

    function getActiveBoss(){
      if (spawn.bossRef && spawn.bossRef.alive) return spawn.bossRef;
      for (let i=0;i<enemies.length;i++){
        const e = enemies[i];
        if (e.alive && e.boss){
          spawn.bossRef = e;
          return e;
        }
      }
      spawn.bossRef = null;
      return null;
    }

    function spawnMixedSquad(t, camX, camY, hpMult, spdMult, dmgMult){
      // Spawn a small mixed pack to keep composition varied.
      if (enemies.length >= spawn.maxEnemies - SPAWN_CONFIG.squadReserve) return;
      const pool = ["A", "B"];
      if (t > SPAWN_CONFIG.mixedPoolTimes.extraFast) pool.push("B");
      if (t > SPAWN_CONFIG.mixedPoolTimes.ranged) pool.push("R");
      if (t > SPAWN_CONFIG.mixedPoolTimes.tank) pool.push("C");
      if (t > SPAWN_CONFIG.mixedPoolTimes.brute) pool.push("S");
      if (t > SPAWN_CONFIG.mixedPoolTimes.void){ pool.push("P"); pool.push("F"); }
      const count = clamp(
        SPAWN_CONFIG.mixedCount.base + Math.floor(t / SPAWN_CONFIG.mixedCount.scaleTime),
        SPAWN_CONFIG.mixedCount.base,
        SPAWN_CONFIG.mixedCount.max
      );
      for (let i=0;i<count;i++){
        const pick = pool[randi(pool.length)];
        spawnEnemy(
          pick,
          camX,
          camY,
          hpMult * SPAWN_CONFIG.mixedStatMult.hp,
          spdMult * SPAWN_CONFIG.mixedStatMult.speed,
          dmgMult * SPAWN_CONFIG.mixedStatMult.dmg
        );
      }
    }

    function spawnController(dt, camX, camY){
      spawn.squadT -= dt;
      spawn.eliteT -= dt;

      const t = player.time;
      const rate = spawn.baseRate + t * spawn.timeScale;
      spawn.acc += dt * rate;

      if (!spawn.bossSpawned && t >= BOSS_CONFIG.spawnTime){
        spawn.bossSpawned = true;
        const pos = pickSpawnPos(camX, camY);
        addTelegraph({
          x: pos.x, y: pos.y,
          radius: BOSS_CONFIG.telegraph.radius,
          color: BOSS_CONFIG.telegraph.color,
          time: BOSS_CONFIG.telegraph.time,
          fire: () => {
            spawn.bossAlive = true;
            spawnEnemy("X", camX, camY, 1 + t * SPAWN_CONFIG.scaling.hp, 1 + t * SPAWN_CONFIG.scaling.speed, 1 + t * SPAWN_CONFIG.scaling.dmg, false, pos);
            sound.play("boss");
          }
        });
      }

      if (enemies.length >= spawn.maxEnemies) return;

      // Requested change:
      //  - less HP scaling
      //  - more damage scaling
      const hpMult  = 1 + t * SPAWN_CONFIG.scaling.hp;
      const spdMult = 1 + t * SPAWN_CONFIG.scaling.speed;
      const dmgMult = 1 + t * SPAWN_CONFIG.scaling.dmg;

      if (t > SPAWN_CONFIG.squadStart && spawn.squadT <= 0){
        spawn.squadT = clamp(
          SPAWN_CONFIG.squadIntervalMax - t * SPAWN_CONFIG.squadIntervalDrop,
          SPAWN_CONFIG.squadIntervalMin,
          SPAWN_CONFIG.squadIntervalMax
        );
        spawnMixedSquad(t, camX, camY, hpMult, spdMult, dmgMult);
      }

      if (spawn.eliteT <= 0 && enemies.length < spawn.maxEnemies){
        spawn.eliteT = ELITE_CONFIG.interval;
        const pos = pickSpawnPos(camX, camY);
        addTelegraph({
          x: pos.x, y: pos.y,
          radius: ELITE_CONFIG.telegraphRadius,
          color: ELITE_CONFIG.telegraphColor,
          time: ELITE_CONFIG.telegraphTime,
          fire: () => {
            const eliteType = (t > SPAWN_CONFIG.thresholds.tank && Math.random() > 0.5) ? "C" : "B";
            spawnEnemy(eliteType, camX, camY, hpMult, spdMult, dmgMult, true, pos);
          }
        });
      }

      // Ranged spawns are intentionally rare (and capped) to avoid projectile spam
      let rangedCount = 0;
      let voidCount = 0;
      for (let i=0;i<enemies.length;i++){
        const e = enemies[i];
        if (!e.alive) continue;
        if (e.type === "R") rangedCount++;
        else if (e.type === "P" || e.type === "F") voidCount++;
      }
      const rangedCap = SPAWN_CONFIG.ranged.capBase + Math.floor(t / SPAWN_CONFIG.ranged.capScaleTime); // slowly grows over time
      const rangedChance = SPAWN_CONFIG.ranged.chance; // chance per spawn (after fast roll)
      const voidCap = SPAWN_CONFIG.voids.capBase + Math.floor(t / SPAWN_CONFIG.voids.capScaleTime);
      const voidChance = SPAWN_CONFIG.voids.chance;


      while (spawn.acc >= 1 && enemies.length < spawn.maxEnemies){
        spawn.acc -= 1;

        const roll = Math.random();
        let typeKey = "A";

        if (t > SPAWN_CONFIG.thresholds.fast && roll < SPAWN_CONFIG.rolls.fast) typeKey = "B";

        // small chance to spawn a ranged kiter (capped)
        if (t > SPAWN_CONFIG.thresholds.ranged && roll >= SPAWN_CONFIG.rolls.fast && roll < (SPAWN_CONFIG.rolls.fast + rangedChance) && rangedCount < rangedCap){
          typeKey = "R";
          rangedCount++;
        }

        if (t > SPAWN_CONFIG.thresholds.tank && roll > SPAWN_CONFIG.rolls.tank) typeKey = "C";

        if (t > SPAWN_CONFIG.thresholds.brute && typeKey === "C" && Math.random() < SPAWN_CONFIG.rolls.brute) typeKey = "S";

        if (t > SPAWN_CONFIG.thresholds.void && roll > SPAWN_CONFIG.rolls.void && voidCount < voidCap && Math.random() < voidChance){
          typeKey = Math.random() < SPAWN_CONFIG.voids.fireBias ? "F" : "P";
          voidCount++;
        }

        if (t > SPAWN_CONFIG.thresholds.lateMix && roll > SPAWN_CONFIG.rolls.lateMix && typeKey !== "P" && typeKey !== "F"){
          const r2 = Math.random();
          if (r2 < SPAWN_CONFIG.rolls.lateTank){
            typeKey = "C";
          } else if (r2 < SPAWN_CONFIG.rolls.lateRanged && rangedCount < rangedCap){
            typeKey = "R";
            rangedCount++;
          } else if (r2 < SPAWN_CONFIG.rolls.lateVoid && voidCount < voidCap){
            typeKey = Math.random() < SPAWN_CONFIG.voids.fireBias ? "F" : "P";
            voidCount++;
          } else if (r2 < SPAWN_CONFIG.rolls.lateBrute){
            typeKey = "S";
          } else {
            typeKey = "B";
          }
        }

        spawnEnemy(typeKey, camX, camY, hpMult, spdMult, dmgMult);
      }
    }

    /* ============================
       Telegraphs (warnings)
       ============================ */
    function addTelegraph({ x, y, dx=0, dy=0, radius=TELEGRAPH_CONFIG.radius, color=COLORS.warn, time=TELEGRAPH_CONFIG.time, fire=null, label=null, width=3, follow=null }){
      telegraphs.push({ x, y, dx, dy, radius, color, t:time, max:time, fire, label, width, follow });
    }

    function updateTelegraphs(dt){
      for (let i=telegraphs.length-1;i>=0;i--){
        const tg = telegraphs[i];
        if (tg.follow) tg.follow(tg, dt);
        tg.t -= dt;
        if (tg.t <= 0){
          const fn = tg.fire;
          telegraphs[i] = telegraphs[telegraphs.length-1];
          telegraphs.pop();
          if (fn) fn();
        }
      }
    }

    /* ============================
       Chests (insta bonuses)
       ============================ */
    const chestSpawn = {
      t: CHEST_CONFIG.timerStart,
      min: CHEST_CONFIG.timerMin,
      max: CHEST_CONFIG.timerMax,
      activeMax: CHEST_CONFIG.activeMax
    };

    function queueChestBomb(x,y){
      const radius = CHEST_CONFIG.bomb.radius;
      const dmg = CHEST_CONFIG.bomb.dmgBase + player.level * CHEST_CONFIG.bomb.dmgPerLevel;
      addTelegraph({
        x, y,
        radius,
        color: COLORS.warn,
        time: CHEST_CONFIG.bomb.telegraphTime,
        fire: () => {
          for (let i=0;i<enemies.length;i++){
            const e = enemies[i];
            if (!e.alive) continue;
            const dx = e.x - x, dy = e.y - y;
            const r2 = radius * radius;
            if (dx*dx + dy*dy <= r2){
              damageEnemy(e, dmg, 0, 0, 0, false);
            }
          }
          addParticles(x, y, COLORS.warn, CHEST_CONFIG.bomb.particles, CHEST_CONFIG.bomb.particleSpread);
        }
      });
    }

    const CHEST_BONUSES = [
      { id:"heal", label:"HEAL", color:COLORS.heal, apply: () => {
          const amt = player.maxHp * CHEST_CONFIG.bonuses.healPct;
          player.hp = Math.min(player.maxHp, player.hp + amt);
          popFloatText(player.x, player.y - 14, `+${Math.ceil(amt)} HP`, COLORS.heal, 16);
        }
      },
      { id:"magnet", label:"MAGNET", color:COLORS.gem, apply: () => {
          buffs.magnet = Math.max(buffs.magnet, CHEST_CONFIG.bonuses.magnet);
          popFloatText(player.x, player.y - 14, "MAGNET!", COLORS.gem, 16);
        }
      },
      { id:"shockwave", label:"SHOCKWAVE", color:COLORS.player, apply: () => {
          const baseKnock = CHEST_CONFIG.shockwave.baseKnock;
          const dmg = CHEST_CONFIG.shockwave.dmgBase + player.level * CHEST_CONFIG.shockwave.dmgPerLevel;
          for (let i=0;i<enemies.length;i++){
            const e = enemies[i];
            if (!e.alive) continue;
            const dx = e.x - player.x, dy = e.y - player.y;
            const d = hypot(dx,dy) || 1;
            const nx = dx / d, ny = dy / d;
            e.kx += nx * baseKnock;
            e.ky += ny * baseKnock;
            if (d < CHEST_CONFIG.shockwave.damageRadius) damageEnemy(e, dmg, nx, ny, CHEST_CONFIG.shockwave.knockPush, false);
          }
          addParticles(player.x, player.y, COLORS.player, 42, 520);
          popFloatText(player.x, player.y - 14, "SHOCKWAVE!", COLORS.player, 16);
        }
      },
      { id:"shield", label:"SHIELD", color:"#7fe7ff", apply: () => {
          buffs.shield = Math.max(buffs.shield, CHEST_CONFIG.bonuses.shield);
          popFloatText(player.x, player.y - 14, "SHIELD!", "#7fe7ff", 16);
        }
      },
      { id:"freeze", label:"FREEZE", color:"#b160ff", apply: () => {
          buffs.slow = Math.max(buffs.slow, CHEST_CONFIG.bonuses.freeze);
          popFloatText(player.x, player.y - 14, "FREEZE!", "#b160ff", 16);
        }
      },
      { id:"xpboost", label:"XP BOOST", color:COLORS.gold, apply: () => {
          buffs.xp = Math.max(buffs.xp, CHEST_CONFIG.bonuses.xp);
          popFloatText(player.x, player.y - 14, "XP BOOST!", COLORS.gold, 16);
        }
      },
      { id:"overcharge", label:"OVERCHARGE", color:"#ff9dfc", apply: () => {
          buffs.power = Math.max(buffs.power, CHEST_CONFIG.bonuses.power);
          popFloatText(player.x, player.y - 14, "OVERCHARGE!", "#ff9dfc", 16);
        }
      },
      { id:"sprint", label:"HYPER SPRINT", color:COLORS.player, apply: () => {
          buffs.haste = Math.max(buffs.haste, CHEST_CONFIG.bonuses.haste);
          popFloatText(player.x, player.y - 14, "SPEED UP!", COLORS.player, 16);
        }
      },
      { id:"bomb", label:"BOMB", color:COLORS.warn, apply: () => {
          queueChestBomb(player.x, player.y);
          popFloatText(player.x, player.y - 14, "BOMB!", COLORS.warn, 16);
        }
      },
    ];

    function spawnChest(camX, camY){
      if (chests.length >= chestSpawn.activeMax) return;

      const c = chestPool.get();
      c.alive = true;
      c.pulse = rand(TAU, 0);

      for (let tries=0; tries<CHEST_CONFIG.spawnTries; tries++){
        const x = player.x + rand(W*CHEST_CONFIG.spawnOffset, -W*CHEST_CONFIG.spawnOffset);
        const y = player.y + rand(H*CHEST_CONFIG.spawnOffset, -H*CHEST_CONFIG.spawnOffset);
        const dx = x - player.x, dy = y - player.y;
        if (dx*dx + dy*dy > CHEST_CONFIG.spawnMinDist * CHEST_CONFIG.spawnMinDist){
          c.x = x;
          c.y = y;
          break;
        }
      }
      chests.push(c);
    }

    function updateChests(dt, camX, camY){
      chestSpawn.t -= dt;
      if (chestSpawn.t <= 0){
        chestSpawn.t = rand(chestSpawn.max, chestSpawn.min);
        spawnChest(camX, camY);
      }

      for (let i=chests.length-1;i>=0;i--){
        const c = chests[i];
        if (!c.alive){ chests[i] = chests[chests.length-1]; chests.pop(); chestPool.put(c); continue; }
        c.pulse += dt * CHEST_CONFIG.pulseSpeed;

        const dx = c.x - player.x, dy = c.y - player.y;
        const rr = c.r + player.r + CHEST_CONFIG.radiusPadding;
        if (dx*dx + dy*dy <= rr*rr){
          c.alive = false;
          let bonus = CHEST_BONUSES[randi(CHEST_BONUSES.length)];
          if (player.hp / player.maxHp < CHEST_CONFIG.healBias.hpPct && Math.random() < CHEST_CONFIG.healBias.chance) bonus = CHEST_BONUSES[0];
          addParticles(c.x, c.y, COLORS.chest, CHEST_CONFIG.openParticles.count, CHEST_CONFIG.openParticles.spread);
          popFloatText(c.x, c.y - 10, bonus.label, bonus.color, 18);
          bonus.apply();
        }

        if (!c.alive){
          chests[i] = chests[chests.length-1];
          chests.pop();
          chestPool.put(c);
        }
      }
    }

    /* ============================
       Combat / Drops / Particles
       ============================ */
    function addParticles(x,y,color,amount=10,spread=250){
      for (let i=0;i<amount;i++){
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

    function spawnDmgText(x,y,amount,color=COLORS.dmg,size=14){
      const d = dmgPool.get();
      d.alive = true;
      d.x = x; d.y = y;
      d.vx = rand(22, -22);
      d.vy = -rand(64, 38);
      d.maxLife = rand(0.62, 0.45);
      d.life = d.maxLife;
      d.text = String(Math.round(amount));
      d.color = color;
      d.size = size;
      dmgTexts.push(d);
    }

    function popFloatText(x,y,text,color="#d7f6ff",size=16){
      const t = textPool.get();
      t.alive = true;
      t.x = x; t.y = y;
      t.vx = rand(16, -16);
      t.vy = -rand(54, 34);
      t.maxLife = 0.90;
      t.life = t.maxLife;
      t.text = text;
      t.color = color;
      t.size = size;
      floatTexts.push(t);
    }

    function dropGem(x,y,value=1){
      const g = gemPool.get();
      g.alive = true;
      g.x = x; g.y = y;
      const a = rand(TAU, 0);
      const s = rand(LOOT_CONFIG.dropSpeedMax, LOOT_CONFIG.dropSpeedMin);
      g.vx = Math.cos(a) * s;
      g.vy = Math.sin(a) * s;
      g.v = value;
      g.r = LOOT_CONFIG.gemRadiusBase + value * LOOT_CONFIG.gemRadiusScale;
      gems.push(g);
    }

    function damageEnemy(e, dmg, pushX, pushY, pushStrength, showText=true, crit=false, source=null){
      if (dmg > 0){
        const inflicted = Math.min(dmg, Math.max(0, e.hp));
        e.hp -= dmg;
        if (source) DPS_TRACKER[source] = (DPS_TRACKER[source] || 0) + inflicted;
        if (showText){
          const color = crit ? COLORS.crit : COLORS.dmg;
          const size = crit ? 18 : 14;
          spawnDmgText(e.x, e.y - e.r - 6, dmg, color, size);
        }
      }
      if (pushStrength > 0){
        const resist = e.knockResist || 0;
        const effPush = pushStrength * (1 - resist);
        e.kx += pushX * effPush;
        e.ky += pushY * effPush;
      }
      if (e.hp > 0 && source){
        if (source === "magic"){
          const cfg = WEAPON_RIDERS.magic.slow;
          e.slowT = Math.max(e.slowT, cfg.duration);
          e.slowMul = cfg.mult;
        } else if (source === "rail"){
          const cfg = WEAPON_RIDERS.rail.burn;
          e.burnT = Math.max(e.burnT, cfg.duration);
          e.burnDps = Math.max(e.burnDps, dmg * cfg.dpsPct);
        } else if (source === "axe"){
          const cfg = WEAPON_RIDERS.axe.bleed;
          e.bleedT = Math.max(e.bleedT, cfg.duration);
          e.bleedDps = Math.max(e.bleedDps, dmg * cfg.dpsPct);
        }
      }
      if (e.hp <= 0){
        player.kills++;
        addParticles(e.x, e.y, e.color, 12 + randi(8), 300);

        const info = ENEMY_TYPES[e.type];
        const extra = e.gemBonus || 0;
        for (let i=0;i<info.gem + extra;i++){
          dropGem(e.x + rand(LOOT_CONFIG.dropJitter, -LOOT_CONFIG.dropJitter), e.y + rand(LOOT_CONFIG.dropJitter, -LOOT_CONFIG.dropJitter), info.xp);
        }
        if (e.boss){
          spawn.bossAlive = false;
          spawn.bossRef = null;
        }
        e.alive = false;
      }
    }

    /* ============================
       Weapons
       ============================ */
    function findNearestEnemy(px,py,maxDist){
      let best = null;
      let bestD = maxDist * maxDist;
      for (let i=0;i<enemies.length;i++){
        const e = enemies[i];
        if (!e.alive) continue;
        const dx = e.x - px, dy = e.y - py;
        const d2 = dx*dx + dy*dy;
        if (d2 < bestD){ bestD = d2; best = e; }
      }
      return best;
    }

    function fireMagicBullet(){
      const s = magicStats();
      const target = findNearestEnemy(player.x, player.y, s.range);
      if (!target) return;
      sound.play("shoot");

      const dx = target.x - player.x;
      const dy = target.y - player.y;
      const baseAng = Math.atan2(dy, dx);
      const count = s.count;
      const spread = WEAPON_CONFIG.magic.projectile.spread;

      for (let i=0;i<count;i++){
        const b = bulletPool.get();
        b.alive = true;
        b.x = player.x; b.y = player.y;
        b.r = WEAPON_CONFIG.magic.projectile.radius;
        b.dmg = s.dmg;
        b.life = WEAPON_CONFIG.magic.projectile.life;
        b.critChance = s.critChance;
        b.critMult = s.critMult;

        const t = (count === 1) ? 0 : (i/(count-1) - 0.5);
        const ang = baseAng + t * spread;

        b.vx = Math.cos(ang) * s.speed;
        b.vy = Math.sin(ang) * s.speed;
        bullets.push(b);
      }
    }

    function fireRailShot(){
      const s = railStats();
      const target = findNearestEnemy(player.x, player.y, s.range);
      if (!target) return;
      sound.play("rail");

      const dx = target.x - player.x;
      const dy = target.y - player.y;
      const ang = Math.atan2(dy, dx);

      const r = railPool.get();
      r.alive = true;
      r.x = player.x;
      r.y = player.y;
      r.r = WEAPON_CONFIG.rail.projectile.radius;
      r.dmg = s.dmg;
      r.critChance = s.critChance;
      r.critMult = s.critMult;
      r.pierce = s.pierce;
      r.life = s.range / s.speed;
      r.vx = Math.cos(ang) * s.speed;
      r.vy = Math.sin(ang) * s.speed;
      r.trail.length = 0;
      r.trail.push({ x:r.x, y:r.y, life:WEAPON_CONFIG.rail.projectile.trailLife });
      rails.push(r);

      addParticles(player.x, player.y, COLORS.rail, 10, 520);
    }

    function throwAxe(){
      const s = axeStats();
      const throwCfg = WEAPON_CONFIG.axe.throw;
      const target = findNearestEnemy(player.x, player.y, throwCfg.range);
      if (!target) return;
      sound.play("axe");

      const count = s.count;
      for (let i=0;i<count;i++){
        const a = axePool.get();
        a.alive = true;
        a.x = player.x;
        a.y = player.y;
        a.r = throwCfg.radius;
        a.dmg = s.dmg;
        a.critChance = s.critChance;
        a.critMult = s.critMult;
        a.life = throwCfg.life;
        a.rot = rand(TAU, 0);
        a.spin = rand(throwCfg.spinMax, throwCfg.spinMin) * (Math.random() < throwCfg.spinInvertChance ? -1 : 1);

        const dx = target.x - player.x;
        const dy = target.y - player.y;
        const ang = Math.atan2(dy, dx) + rand(throwCfg.angleJitter, -throwCfg.angleJitter);

        const sp = s.speed * rand(throwCfg.speedJitterMax, throwCfg.speedJitterMin);
        a.vx = Math.cos(ang) * sp;
        a.vy = Math.sin(ang) * sp - rand(throwCfg.launchVyMax, throwCfg.launchVyMin);
        axes.push(a);
      }
    }

    function fireOrb(){
      const s = orbStats();
      const target = findNearestEnemy(player.x, player.y, s.range);
      if (!target) return;
      sound.play("shoot");

      const dx = target.x - player.x;
      const dy = target.y - player.y;
      const ang = Math.atan2(dy, dx);
      const o = orbPool.get();
      o.alive = true;
      o.state = "fly";
      o.x = player.x;
      o.y = player.y;
      o.r = 10;
      o.vx = Math.cos(ang) * s.speed;
      o.vy = Math.sin(ang) * s.speed;
      o.life = s.range / s.speed;
      o.park = s.park;
      o.tick = 0; // pulse immediately on park
      o.pull = s.pull;
      o.radius = s.radius;
      o.dmg = s.dmg;
      o.explosion = s.explosion;
      o.critChance = s.critChance;
      o.critMult = s.critMult;
      orbs.push(o);
    }

    function updateWeapons(dt){
      if (weapons.magic.unlocked){
        weapons.magic.t -= dt;
        if (weapons.magic.t <= 0){
          weapons.magic.t += magicStats().cd;
          fireMagicBullet();
        }
      }

      if (weapons.aura.unlocked){
        const s = auraStats();
        weapons.aura.tick -= dt;
        if (weapons.aura.tick <= 0){
          weapons.aura.tick += s.tick;
          const r = s.radius, r2 = r*r;
          for (let i=0;i<enemies.length;i++){
            const e = enemies[i];
            if (!e.alive) continue;
            const dx = e.x - player.x;
            const dy = e.y - player.y;
            const d2 = dx*dx + dy*dy;
            if (d2 <= (r2 + e.r*e.r)){
              const d = Math.sqrt(d2) || 1;
              const nx = dx / d, ny = dy / d;
              const hit = calcCrit(s.dmg, s.critChance, s.critMult);
              damageEnemy(e, hit.dmg, nx, ny, s.knock, true, hit.crit, "aura");
            }
          }
        }
      }

      if (weapons.rail.unlocked){
        weapons.rail.t -= dt;
        if (weapons.rail.t <= 0){
          weapons.rail.t += railStats().cd;
          fireRailShot();
        }
      }

      if (weapons.axe.unlocked){
        weapons.axe.t -= dt;
        if (weapons.axe.t <= 0){
          weapons.axe.t += axeStats().cd;
          throwAxe();
        }
      }

      if (weapons.orb.unlocked){
        weapons.orb.t -= dt;
        if (weapons.orb.t <= 0){
          weapons.orb.t += orbStats().cd;
          fireOrb();
        }
      }
    }

    /* ============================
       XP
       ============================ */
    function addXP(amount){
      const xpMul = buffs.xp > 0 ? XP_CONFIG.buffMultiplier : 1.0;
      player.xp += amount * xpMul;
      while (player.xp >= player.xpNeed){
        player.xp -= player.xpNeed;
        player.level++;
        player.xpNeed = xpNeedForLevel(player.level);
        openLevelUp();
        break;
      }
    }

    /* ============================
       Level up UI
       ============================ */
    let currentCards = [];

    function openLevelUp(){
      if (state !== STATE.PLAYING) return;
      state = STATE.LEVELUP;
      currentCards = pickUpgrades(XP_CONFIG.cardChoices);
      renderUpgradeCards(currentCards);
      ui.levelup.classList.add("on");
      ui.levelup.style.pointerEvents = "auto";
      sound.play("level");
    }
    function closeLevelUp(){
      ui.levelup.classList.remove("on");
      ui.levelup.style.pointerEvents = "none";
      state = STATE.PLAYING;
      focusCanvas();
    }
    function renderUpgradeCards(cards){
      ui.upgradeCards.innerHTML = "";
      for (const u of cards){
        const el = document.createElement("div");
        el.className = "card";
        el.innerHTML = `
          <h3>${u.title}</h3>
          <p>${u.desc}</p>
          <div class="pill">${u.tag()}</div>
        `;
        el.addEventListener("click", () => {
          if (state !== STATE.LEVELUP) return;
          u.apply();
          closeLevelUp();
        }, { passive:true });
        ui.upgradeCards.appendChild(el);
      }
    }

    /* ============================
       Main Menu / Pause
       ============================ */
    function openMenu(){
      if (state !== STATE.PLAYING) return;
      state = STATE.MENU;
      updateMenuStats();
      ui.menu.classList.add("on");
      ui.menu.style.pointerEvents = "auto";
    }

    function closeMenu(){
      if (state !== STATE.MENU) return;
      ui.menu.classList.remove("on");
      ui.menu.style.pointerEvents = "none";
      state = STATE.PLAYING;
      focusCanvas();
    }

    /* ============================
       Game Over
       ============================ */
    function fmtTime(sec){
      sec = Math.max(0, sec);
      const m = (sec/60) | 0;
      const s = (sec - m*60) | 0;
      return String(m).padStart(2,"0") + ":" + String(s).padStart(2,"0");
    }

    function openGameOver(){
      state = STATE.GAMEOVER;
      ui.gameover.classList.add("on");
      ui.gameover.style.pointerEvents = "auto";
      const t = fmtTime(player.time);
      ui.summary.textContent = `You survived ${t}.`;
      ui.goTime.textContent = t;
      ui.goLvl.textContent = String(player.level);
      ui.goKills.textContent = String(player.kills);
      ui.goUpgrades.textContent = listUpgradeSummary();
      ui.goDps.textContent = formatDpsSummary();
    }

    function restart(){
      for (let i=enemies.length-1;i>=0;i--) enemyPool.put(enemies.pop());
      for (let i=bullets.length-1;i>=0;i--) bulletPool.put(bullets.pop());
      for (let i=rails.length-1;i>=0;i--) railPool.put(rails.pop());
      for (let i=axes.length-1;i>=0;i--) axePool.put(axes.pop());
      for (let i=orbs.length-1;i>=0;i--) orbPool.put(orbs.pop());
      for (let i=enemyShots.length-1;i>=0;i--) shotPool.put(enemyShots.pop());
      for (let i=voidZones.length-1;i>=0;i--) voidPool.put(voidZones.pop());
      for (let i=gems.length-1;i>=0;i--) gemPool.put(gems.pop());
      for (let i=particles.length-1;i>=0;i--) partPool.put(particles.pop());
      for (let i=chests.length-1;i>=0;i--) chestPool.put(chests.pop());
      for (let i=dmgTexts.length-1;i>=0;i--) dmgPool.put(dmgTexts.pop());
      for (let i=floatTexts.length-1;i>=0;i--) textPool.put(floatTexts.pop());
      telegraphs.length = 0;

      spawn.acc = 0;
      spawn.squadT = SPAWN_CONFIG.squadInterval;
      spawn.eliteT = ELITE_CONFIG.interval;
      spawn.bossSpawned = false;
      spawn.bossAlive = false;
      spawn.bossRef = null;
      chestSpawn.t = CHEST_CONFIG.timerStart;

      resetPlayer();
      resetWeapons();
      resetUpgradeState();
      resetDps();

      ui.gameover.classList.remove("on");
      ui.gameover.style.pointerEvents = "none";
      ui.levelup.classList.remove("on");
      ui.levelup.style.pointerEvents = "none";
      ui.menu.classList.remove("on");
      ui.menu.style.pointerEvents = "none";
      clearDirectionalInput();

      state = STATE.PLAYING;
      last = performance.now();
      focusCanvas();
    }

    ui.btnRestart.addEventListener("click", restart, { passive:true });
    ui.btnResume.addEventListener("click", closeMenu, { passive:true });
    ui.btnMenuRestart.addEventListener("click", restart, { passive:true });
    if (ui.btnGod){
      ui.btnGod.addEventListener("click", () => {
        godMode = !godMode;
        updateGodButton();
        if (godMode){
          player.hp = player.maxHp;
          buffs.shield = 9999;
        } else {
          buffs.shield = 0;
        }
      }, { passive:true });
    }
    if (ui.btnMute){
      ui.btnMute.addEventListener("click", () => {
        sound.enabled = !sound.enabled;
        updateMuteButton();
        if (sound.enabled) sound.unlock();
      }, { passive:true });
    }

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

    function updatePlayer(dt){
      let mx = 0, my = 0;
      if (input.up) my -= 1;
      if (input.down) my += 1;
      if (input.left) mx -= 1;
      if (input.right) mx += 1;

      const len = hypot(mx,my);
      if (len > 0){ mx /= len; my /= len; }

      const moveSpeed = player.speed * (buffs.haste > 0 ? BUFF_EFFECTS.hasteMoveMult : 1.0);
      player.x += mx * moveSpeed * dt;
      player.y += my * moveSpeed * dt;

      if (player.iFrame > 0) player.iFrame -= dt;
    }

    function spawnEnemyShot(x,y,nx,ny,speed,dmg){
      const s = shotPool.get();
      s.alive = true;
      s.x = x; s.y = y;
      s.vx = nx * speed;
      s.vy = ny * speed;
      s.r = RANGED_SHOT_CONFIG.radius;
      s.dmg = dmg;
      s.life = RANGED_SHOT_CONFIG.life;
      s.color = RANGED_SHOT_CONFIG.color; // red projectile
      enemyShots.push(s);
    }

    function spawnVoidZone(x,y,radius,duration,dps,color,type,tick=0.25){
      const z = voidPool.get();
      z.alive = true;
      z.x = x; z.y = y;
      z.radius = radius;
      z.maxLife = duration;
      z.life = duration;
      z.dps = dps;
      z.tick = tick;
      z.tickT = tick;
      z.color = color;
      z.type = type || "poison";
      voidZones.push(z);
    }

    function updateEnemyShots(dt){
      for (let i=enemyShots.length-1;i>=0;i--){
        const s = enemyShots[i];
        if (!s.alive){ enemyShots[i] = enemyShots[enemyShots.length-1]; enemyShots.pop(); shotPool.put(s); continue; }

        s.life -= dt;
        s.x += s.vx * dt;
        s.y += s.vy * dt;

        const dx = player.x - s.x;
        const dy = player.y - s.y;
        const rr = player.r + s.r + RANGED_SHOT_CONFIG.hitPad;
        if (dx*dx + dy*dy <= rr*rr){
          s.alive = false;
          if (buffs.shield <= 0 && player.iFrame <= 0){
            player.hp -= s.dmg;
            player.iFrame = PLAYER_CONFIG.shotIFrame;
          spawnDmgText(player.x, player.y - player.r - 12, s.dmg, COLORS.warnHit);
          addParticles(player.x, player.y, COLORS.warnHitDim, 8, 360);
        } else {
          addParticles(s.x, s.y, COLORS.shieldBlock, 4, 260);
        }
        }

        if (s.life <= 0) s.alive = false;

        if (!s.alive){
          enemyShots[i] = enemyShots[enemyShots.length-1];
          enemyShots.pop();
          shotPool.put(s);
        }
      }
    }

    function updateVoidZones(dt){
      for (let i=voidZones.length-1;i>=0;i--){
        const z = voidZones[i];
        if (!z.alive){ voidZones[i] = voidZones[voidZones.length-1]; voidZones.pop(); voidPool.put(z); continue; }

        z.life -= dt;
        if (z.life <= 0) z.alive = false;

        if (z.alive){
          const dx = player.x - z.x;
          const dy = player.y - z.y;
          const rr = player.r + z.radius;
          if (dx*dx + dy*dy <= rr*rr && buffs.shield <= 0 && !godMode){
            z.tickT -= dt;
            while (z.tickT <= 0){
              z.tickT += z.tick;
              const dmg = z.dps * z.tick;
              player.hp -= dmg;
              spawnDmgText(player.x, player.y - player.r - 12, dmg, COLORS.warnHit, 14);
            }
          } else {
            z.tickT = z.tick;
          }
        }

        if (!z.alive){
          voidZones[i] = voidZones[voidZones.length-1];
          voidZones.pop();
          voidPool.put(z);
        }
      }
    }

    function updateEnemies(dt){
      const slowMul = (buffs.slow > 0) ? BUFF_EFFECTS.slowMoveMult : 1.0;

      for (let i=0;i<enemies.length;i++){
        const e = enemies[i];
        if (!e.alive) continue;

        // rider DoTs and timers
        if (e.burnT > 0){
          const dmg = e.burnDps * dt;
          e.burnT -= dt;
          if (dmg > 0) damageEnemy(e, dmg, 0, 0, 0, false, false, "rail");
          if (!e.alive) continue;
        }
        if (e.bleedT > 0){
          const dmg = e.bleedDps * dt;
          e.bleedT -= dt;
          if (dmg > 0) damageEnemy(e, dmg, 0, 0, 0, false, false, "axe");
          if (!e.alive) continue;
        }
        if (e.slowT > 0) e.slowT -= dt;

        // knockback velocity
        e.x += e.kx * dt;
        e.y += e.ky * dt;
        const kbDecay = Math.pow(ENEMY_BEHAVIOR.knockbackDecayBase, dt);
        e.kx *= kbDecay;
        e.ky *= kbDecay;

        const dx = player.x - e.x;
        const dy = player.y - e.y;
        const d = hypot(dx,dy) || 1;
        const nx = dx / d, ny = dy / d;

        const statusSpeedMul = (e.slowT > 0) ? e.slowMul : 1.0;
        if (e.ranged){
          const prefer = (e.spitter ? (e.spitRange || e.shotRange) : e.shotRange) || ENEMY_BEHAVIOR.rangedPreferredRange;

          if (d < prefer * ENEMY_BEHAVIOR.preferredClose){
            const flee = e.speed * ENEMY_BEHAVIOR.fleeMult;
            e.x += (-nx) * (flee * slowMul * statusSpeedMul) * dt;
            e.y += (-ny) * (flee * slowMul * statusSpeedMul) * dt;
          } else if (d > prefer * ENEMY_BEHAVIOR.preferredFar){
            const creep = e.speed * ENEMY_BEHAVIOR.creepMult;
            e.x += (nx) * (creep * slowMul * statusSpeedMul) * dt;
            e.y += (ny) * (creep * slowMul * statusSpeedMul) * dt;
          } else {
            const strafe = e.speed * ENEMY_BEHAVIOR.strafeMult;
            const px = -ny, py = nx;
            const dir = (i & 1) ? 1 : -1;
            e.x += (px * dir) * (strafe * slowMul * statusSpeedMul) * dt;
            e.y += (py * dir) * (strafe * slowMul * statusSpeedMul) * dt;
          }

          if (e.spitter){
            e.spitT -= dt;
            if (e.spitT <= 0 && d < (e.spitRange || ENEMY_BEHAVIOR.rangedPreferredRange)){
              const slowFireMul = (buffs.slow > 0) ? BUFF_EFFECTS.slowFireMult : 1.0;
              e.spitT += (e.spitCd || RANGED_SHOT_CONFIG.defaultCd) * slowFireMul;
              const tx = player.x, ty = player.y;
              const marker = ++e.shotSeq;
              addTelegraph({
                x: tx, y: ty, radius: e.spitRadius, color: e.spitColor, time: e.spitTelegraph,
                fire: () => {
                  if (e.alive && e.shotSeq === marker) spawnVoidZone(tx, ty, e.spitRadius, e.spitDuration, e.spitDps, e.spitColor, e.spitType, e.spitTick);
                }
              });
            }
          } else {
            e.shotT -= dt;
            if (e.shotT <= 0 && d < (e.shotRange || ENEMY_BEHAVIOR.rangedPreferredRange)){
              const slowFireMul = (buffs.slow > 0) ? BUFF_EFFECTS.slowFireMult : 1.0;
              e.shotT += (e.shotCd || RANGED_SHOT_CONFIG.defaultCd) * slowFireMul;
              const marker = ++e.shotSeq;
              addTelegraph({
                x: e.x, y: e.y, dx: nx, dy: ny, radius: RANGED_SHOT_CONFIG.telegraphRadius, color: COLORS.warn, time: RANGED_SHOT_CONFIG.telegraphTime,
                follow: (tg) => {
                  if (!e.alive) return;
                  tg.x = e.x;
                  tg.y = e.y;
                },
                fire: () => {
                  if (e.alive && e.shotSeq === marker) spawnEnemyShot(e.x, e.y, nx, ny, e.shotSpeed || RANGED_SHOT_CONFIG.defaultSpeed, e.shotDmg || RANGED_SHOT_CONFIG.defaultDmg);
                }
              });
            }
          }

          if (e.boss){
            e.novaT -= dt;
            if (e.novaT <= 0){
              const slowFireMul = (buffs.slow > 0) ? BUFF_EFFECTS.slowFireMult : 1.0;
              e.novaT += (e.novaCd || 6) * slowFireMul;
              const marker = ++e.novaSeq;
              addTelegraph({
                x: e.x, y: e.y, radius: e.novaRadius, color: BOSS_CONFIG.telegraph.color, time: e.novaTelegraph,
                follow: (tg) => {
                  if (!e.alive) return;
                  tg.x = e.x;
                  tg.y = e.y;
                },
                fire: () => {
                  if (!e.alive || e.novaSeq !== marker) return;
                  for (let k=0;k<e.novaShots;k++){
                    const ang = (TAU * k) / e.novaShots;
                    spawnEnemyShot(e.x, e.y, Math.cos(ang), Math.sin(ang), e.novaShotSpeed || RANGED_SHOT_CONFIG.defaultSpeed, e.novaShotDmg || RANGED_SHOT_CONFIG.defaultDmg);
                  }
                }
              });
            }
          }
        } else {
          e.x += nx * (e.speed * slowMul) * dt;
          e.y += ny * (e.speed * slowMul) * dt;
        }

        // contact damage
        const rr = player.r + e.r;
        if (d < rr){
          if (godMode){
            // ignore melee damage
          } else if (buffs.shield <= 0 && player.iFrame <= 0){
            const hurt = e.dmg;
            player.hp -= hurt;
            player.iFrame = PLAYER_CONFIG.meleeIFrame;
            sound.play("hurt");

            // floating damage numbers for melee hits too
            spawnDmgText(player.x, player.y - player.r - 12, hurt, COLORS.warnHit);
            addParticles(player.x, player.y, COLORS.warnHitDim, 6, 340);

            player.x += nx * PLAYER_CONFIG.hitPush;
            player.y += ny * PLAYER_CONFIG.hitPush;
          } else {
            e.kx -= nx * PLAYER_CONFIG.shieldPushback * dt;
            e.ky -= ny * PLAYER_CONFIG.shieldPushback * dt;
          }
        }
      }
    }

    function updateBullets(dt){
      const knock = magicStats().knock;
      for (let i=bullets.length-1;i>=0;i--){
        const b = bullets[i];
        if (!b.alive){ bullets[i] = bullets[bullets.length-1]; bullets.pop(); bulletPool.put(b); continue; }

        b.life -= dt;
        b.x += b.vx * dt;
        b.y += b.vy * dt;

        for (let j=0;j<enemies.length;j++){
          const e = enemies[j];
          if (!e.alive) continue;
          const dx = e.x - b.x;
          const dy = e.y - b.y;
          const rr = e.r + b.r;
          if (dx*dx + dy*dy <= rr*rr){
            const d = hypot(b.vx, b.vy) || 1;
            const px = b.vx / d, py = b.vy / d;
            const hit = calcCrit(b.dmg, b.critChance, b.critMult);
            damageEnemy(e, hit.dmg, px, py, knock, true, hit.crit, "magic");
            b.alive = false;
            break;
          }
        }

        if (b.life <= 0) b.alive = false;

        if (!b.alive){
          bullets[i] = bullets[bullets.length-1];
          bullets.pop();
          bulletPool.put(b);
        }
      }
    }

    function updateRailShots(dt){
      const s = railStats();
      const trailLife = WEAPON_CONFIG.rail.projectile.trailLife;
      const trailMax = WEAPON_CONFIG.rail.projectile.trailMax;
      for (let i=rails.length-1;i>=0;i--){
        const r = rails[i];
        if (!r.alive){ rails[i] = rails[rails.length-1]; rails.pop(); railPool.put(r); continue; }

        r.life -= dt;
        r.x += r.vx * dt;
        r.y += r.vy * dt;

        // fade existing trail nodes
        for (let t=r.trail.length-1;t>=0;t--){
          r.trail[t].life -= dt;
          if (r.trail[t].life <= 0) r.trail.splice(t,1);
        }
        // add a fresh node to keep the streak behind the projectile
        r.trail.unshift({ x:r.x, y:r.y, life:trailLife });
        if (r.trail.length > trailMax) r.trail.pop();

        for (let j=0;j<enemies.length;j++){
          const e = enemies[j];
          if (!e.alive) continue;
          const dx = e.x - r.x;
          const dy = e.y - r.y;
          const rr = e.r + r.r;
          if (dx*dx + dy*dy <= rr*rr){
            const d = hypot(r.vx, r.vy) || 1;
            const px = r.vx / d, py = r.vy / d;
            const hit = calcCrit(r.dmg, r.critChance, r.critMult);
            damageEnemy(e, hit.dmg, px, py, s.knock, true, hit.crit, "rail");
            r.pierce--;
            if (r.pierce <= 0){ r.alive = false; break; }
          }
        }

        if (r.life <= 0) r.alive = false;

        if (!r.alive){
          rails[i] = rails[rails.length-1];
          rails.pop();
          railPool.put(r);
        }
      }
    }

    function updateAxes(dt){
      const s = axeStats();
      for (let i=axes.length-1;i>=0;i--){
        const a = axes[i];
        if (!a.alive){ axes[i] = axes[axes.length-1]; axes.pop(); axePool.put(a); continue; }

        a.life -= dt;
        a.vy += s.gravity * dt;
        a.x += a.vx * dt;
        a.y += a.vy * dt;
        a.rot += a.spin * dt;

        for (let j=0;j<enemies.length;j++){
          const e = enemies[j];
          if (!e.alive) continue;
          const dx = e.x - a.x;
          const dy = e.y - a.y;
          const rr = e.r + a.r;
          if (dx*dx + dy*dy <= rr*rr){
            const d = hypot(a.vx, a.vy) || 1;
            const px = a.vx / d, py = a.vy / d;
            const hit = calcCrit(a.dmg, a.critChance, a.critMult);
            damageEnemy(e, hit.dmg, px, py, s.knock, true, hit.crit, "axe");
            a.life -= WEAPON_CONFIG.axe.throw.hitLifeLoss;
            if (a.life <= 0){ a.alive = false; break; }
          }
        }

        if (a.life <= 0) a.alive = false;

        if (!a.alive){
          axes[i] = axes[axes.length-1];
          axes.pop();
          axePool.put(a);
        }
      }
    }

    function updateOrbs(dt){
      for (let i=orbs.length-1;i>=0;i--){
        const o = orbs[i];
        if (!o.alive){ orbs[i] = orbs[orbs.length-1]; orbs.pop(); orbPool.put(o); continue; }

        if (o.state === "fly"){
          o.life -= dt;
          o.x += o.vx * dt;
          o.y += o.vy * dt;
          // pull while flying
          const r2 = o.radius * o.radius;
          for (let j=0;j<enemies.length;j++){
            const e = enemies[j];
            if (!e.alive) continue;
            const dx = e.x - o.x;
            const dy = e.y - o.y;
            const d2 = dx*dx + dy*dy;
            if (d2 <= r2){
              const d = Math.sqrt(d2) || 1;
              const nx = dx / d, ny = dy / d;
              const pull = o.pull * dt;
              e.kx -= nx * pull;
              e.ky -= ny * pull;
            }
          }
          if (o.life <= 0){
            o.state = "park";
            o.tick = 0;
          }
        } else if (o.state === "park"){
          o.park -= dt;
          o.tick -= dt;
          if (o.tick <= 0){
            o.tick += orbStats().tick;
            const r2 = o.radius * o.radius;
            for (let j=0;j<enemies.length;j++){
              const e = enemies[j];
              if (!e.alive) continue;
              const dx = e.x - o.x;
              const dy = e.y - o.y;
              const d2 = dx*dx + dy*dy;
              if (d2 <= r2){
                const d = Math.sqrt(d2) || 1;
                const nx = dx / d, ny = dy / d;
                const pull = o.pull * dt;
                e.kx -= nx * pull;
                e.ky -= ny * pull;
                const hit = calcCrit(o.dmg, o.critChance, o.critMult);
                damageEnemy(e, hit.dmg, 0, 0, 0, true, hit.crit, "orb");
              }
            }
          }

          if (o.park <= 0){
            const r2 = o.radius * o.radius;
            for (let j=0;j<enemies.length;j++){
              const e = enemies[j];
              if (!e.alive) continue;
              const dx = e.x - o.x;
              const dy = e.y - o.y;
              if (dx*dx + dy*dy <= r2){
                const hit = calcCrit(o.explosion, o.critChance, o.critMult);
                damageEnemy(e, hit.dmg, 0, 0, 0, true, hit.crit, "orb");
              }
            }
            addParticles(o.x, o.y, COLORS.bullet, 48, 720);
            o.alive = false;
          }
        }

        if (!o.alive){
          orbs[i] = orbs[orbs.length-1];
          orbs.pop();
          orbPool.put(o);
        }
      }
    }

    function updateGems(dt){
      const magnetMul = (buffs.magnet > 0) ? BUFF_EFFECTS.magnetRadiusMult : 1.0; // 3x radius while active
      const pickup = player.pickup * magnetMul;
      const pickupMult = player.pickup / BASE_STATS.pickup;
      const pr2 = pickup * pickup;

      for (let i=gems.length-1;i>=0;i--){
        const g = gems[i];
        if (!g.alive){ gems[i] = gems[gems.length-1]; gems.pop(); gemPool.put(g); continue; }

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

    function updateParticles(dt){
      for (let i=particles.length-1;i>=0;i--){
        const p = particles[i];
        if (!p.alive){ particles[i] = particles[particles.length-1]; particles.pop(); partPool.put(p); continue; }

        p.life -= dt;
        p.x += p.vx * dt;
        p.y += p.vy * dt;
        p.vx *= Math.pow(0.02, dt);
        p.vy *= Math.pow(0.02, dt);

        if (p.life <= 0) p.alive = false;

        if (!p.alive){
          particles[i] = particles[particles.length-1];
          particles.pop();
          partPool.put(p);
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

      for (let i=enemies.length-1;i>=0;i--){
        const e = enemies[i];
        if (!e.alive || e.x < minX || e.x > maxX || e.y < minY || e.y > maxY){
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
    function drawGrid(camX, camY){
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

    function neonCircle(x,y,r,fill,glow=16){
      ctx.save();
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

    function neonRing(x,y,r,stroke,glow=18,lw=2,alpha=1){
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

    function neonRect(x,y,w,h,fill,glow=16){
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

    function drawTextWorld(x,y,text,color,size,alpha){
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

    function drawChestIndicators(camX, camY){
      const cx = W * 0.5, cy = H * 0.5;
      const margin = CHEST_CONFIG.indicatorMargin;
      const minX = margin, maxX = W - margin;
      const minY = margin, maxY = H - margin;
      const size = CHEST_CONFIG.indicatorSize;
      const pulse = 0.65 + 0.35 * Math.sin(performance.now() * 0.008);

      ctx.save();
      ctx.lineWidth = 2;
      ctx.shadowColor = COLORS.chest;
      ctx.strokeStyle = COLORS.chest;
      ctx.fillStyle = UI_COLORS.chestFill;
      ctx.shadowBlur = 14;

      for (let i=0;i<chests.length;i++){
        const c = chests[i];
        if (!c.alive) continue;
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
        const s = size * pulse;

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

    function render(){
      const camX = player.x - W*0.5;
      const camY = player.y - H*0.5;

        ctx.fillStyle = COLORS.bg;
      ctx.fillRect(0,0,W,H);

      ctx.save();
      ctx.translate(-camX, -camY);

      drawGrid(camX, camY);


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
        const pulse = (Math.sin(c.pulse) * 0.15 + 0.85);
        const rr = c.r * (1.0 + 0.05 * Math.sin(c.pulse * 1.7));
        neonRect(c.x - rr, c.y - rr, rr*2, rr*2, COLORS.chest, 20);
        neonRing(c.x, c.y, rr*1.45, COLORS.gold, 22, 2, pulse);
      }

      for (let i=0;i<gems.length;i++){
        const g = gems[i];
        if (g.x < camX - WORLD.spawnPad || g.x > camX + W + WORLD.spawnPad || g.y < camY - WORLD.spawnPad || g.y > camY + H + WORLD.spawnPad) continue;
        neonCircle(g.x, g.y, g.r, COLORS.gem, 14);
      }

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

      // Enemies: batch by style to reduce save/restore churn
      const visMinX = camX - WORLD.spawnPad, visMaxX = camX + W + WORLD.spawnPad;
      const visMinY = camY - WORLD.spawnPad, visMaxY = camY + H + WORLD.spawnPad;

      ctx.save();
      ctx.shadowBlur = 16;
      ctx.lineWidth = 1;
      ctx.strokeStyle = UI_COLORS.strokeDim;
      for (let i=0;i<enemies.length;i++){
        const e = enemies[i];
        if (!e.alive) continue;
        if (e.x < visMinX || e.x > visMaxX || e.y < visMinY || e.y > visMaxY) continue;
        const size = e.r * 2;
        ctx.shadowColor = e.color;
        ctx.fillStyle = e.color;
        ctx.fillRect(e.x - e.r, e.y - e.r, size, size);
        ctx.strokeRect(e.x - e.r, e.y - e.r, size, size);
      }
      ctx.restore();

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
        const size = e.r * 2;
        ctx.strokeRect(e.x - e.r - 6, e.y - e.r - 6, size + 12, size + 12);
      }
      ctx.restore();

      // Enemy HP bars (background then fill)
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
      ctx.shadowBlur = 8;
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

      // draw telegraphs (no culling—they are sparse and usually on-screen)
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
        neonCircle(s.x, s.y, s.r, s.color, 18);
      }

      for (let i=0;i<rails.length;i++){
        const r = rails[i];
        const trailLife = WEAPON_CONFIG.rail.projectile.trailLife;
        ctx.save();
        // trailing streak behind the rail shot
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
        // head glow
        ctx.shadowColor = COLORS.rail;
        ctx.shadowBlur = 24;
        ctx.fillStyle = COLORS.rail;
        ctx.beginPath();
        ctx.ellipse(r.x, r.y, r.r * 1.6, r.r, Math.atan2(r.vy, r.vx), 0, TAU);
        ctx.fill();
        ctx.restore();
      }

      for (let i=0;i<bullets.length;i++){
        const b = bullets[i];
        neonCircle(b.x, b.y, b.r, COLORS.bullet, 18);
      }

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

      for (let i=0;i<orbs.length;i++){
        const o = orbs[i];
        const pulse = 0.75 + 0.25 * Math.sin(performance.now() * 0.006 + i);
        const r = o.state === "fly" ? o.r : o.radius * 0.4;
        neonCircle(o.x, o.y, r, COLORS.bullet, 18);
        neonRing(o.x, o.y, o.radius, UI_COLORS.orbRing, 24, 2, 0.35 * pulse);
        if (o.state === "park"){
          neonRing(o.x, o.y, o.radius, UI_COLORS.orbRing, 24, 2, 0.6 * pulse);
        }
      }

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
      }

      const flicker = player.iFrame > 0 ? (Math.sin(performance.now() * 0.03) * 0.25 + 0.75) : 1;
      ctx.save();
      ctx.globalAlpha = flicker;
      neonCircle(player.x, player.y, player.r, COLORS.player, 22);
      ctx.shadowColor = UI_COLORS.playerGlow;
      ctx.shadowBlur = 10;
      ctx.fillStyle = UI_COLORS.playerCore;
      ctx.beginPath();
      ctx.arc(player.x, player.y, 5.5, 0, TAU);
      ctx.fill();
      ctx.restore();

      if (buffs.shield > 0){
        const a = 0.55 + 0.25 * Math.sin(performance.now()*0.004);
        neonRing(player.x, player.y, player.r + 10, UI_COLORS.shieldRing, 26, 2.5, a);
      }
      if (buffs.magnet > 0){
        neonRing(player.x, player.y, player.r + 18, UI_COLORS.magnetRing, 22, 2, 0.55);
      }

      for (let i=0;i<dmgTexts.length;i++){
        const d = dmgTexts[i];
        const a = clamp(d.life / d.maxLife, 0, 1);
        drawTextWorld(d.x, d.y, d.text, d.color, d.size, a);
      }
      for (let i=0;i<floatTexts.length;i++){
        const t = floatTexts[i];
        const a = clamp(t.life / t.maxLife, 0, 1);
        drawTextWorld(t.x, t.y, t.text, t.color, t.size, a);
      }

      ctx.restore();

      drawChestIndicators(camX, camY);

      if (state === STATE.LEVELUP){
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

      const boss = getActiveBoss();
      const bossHp = boss ? clamp(boss.hp, 0, boss.maxHp) : 0;
      const bossHpT = boss && boss.maxHp > 0 ? clamp(bossHp / boss.maxHp, 0, 1) : 0;
      const bossName = boss ? (ENEMY_TYPES[boss.type]?.name || "Boss") : "";
      const bossOn = !!boss;
      if (ui.bossWrap) ui.bossWrap.classList.toggle("on", bossOn);
      if (ui.bossCard){
        ui.bossCard.classList.toggle("on", bossOn);
        if (bossOn){
          ui.bossName.textContent = bossName;
          ui.bossHp.textContent = `${Math.ceil(bossHp)} / ${Math.ceil(boss.maxHp)}`;
          ui.bossHpPct.textContent = `${Math.round(bossHpT * 100)}%`;
          ui.bossHpFill.style.width = `${(bossHpT*100).toFixed(2)}%`;
        }
      }
      if (ui.mBossBar){
        ui.mBossBar.classList.toggle("on", bossOn);
        if (bossOn){
          ui.mBossName.textContent = bossName || "Boss";
          ui.mBossPct.textContent = `${Math.round(bossHpT * 100)}%`;
          ui.mBossFill.style.width = `${(bossHpT*100).toFixed(2)}%`;
        } else {
          ui.mBossName.textContent = "Boss";
          ui.mBossPct.textContent = "0%";
          ui.mBossFill.style.width = "0%";
        }
      }

      ui.xp.textContent = fmtFloat(player.xp);
      ui.xpNeed.textContent = fmtFloat(player.xpNeed);
      const xpT = player.xpNeed > 0 ? clamp(player.xp / player.xpNeed, 0, 1) : 0;
      ui.xpFill.style.width = `${(xpT*100).toFixed(2)}%`;
      if (ui.mXp){
        ui.mXp.textContent = fmtFloat(player.xp);
        ui.mXpNeed.textContent = fmtFloat(player.xpNeed);
        ui.mXpFill.style.width = `${(xpT*100).toFixed(2)}%`;
      }

      // Loadout & bonuses
      const weaponPills = [];
      const pill = (label, w) => `${label} Lv ${w.level}${w.mastery ? ` (M${w.mastery})` : ""}`;
      if (weapons.magic.unlocked) weaponPills.push(pill("Magic", weapons.magic));
      if (weapons.aura.unlocked) weaponPills.push(pill("Aura", weapons.aura));
      if (weapons.rail.unlocked) weaponPills.push(pill("Rail", weapons.rail));
      if (weapons.axe.unlocked) weaponPills.push(pill("Axe", weapons.axe));
      if (weapons.orb.unlocked) weaponPills.push(pill("Orb", weapons.orb));
      const loadoutHtml = (weaponPills.length ? weaponPills : ["None"]).map(w => `<span class="pill">${w}</span>`).join(" ");
      ui.loadout.innerHTML = loadoutHtml;
      if (ui.mWeapons) ui.mWeapons.innerHTML = loadoutHtml;

      const fmtBonus = (label, mult, current) => {
        const pct = Math.round((mult - 1) * 100);
        if (Math.abs(pct) < 1) return `${label} Base (${current})`;
        const sign = pct > 0 ? "+" : "";
        return `${label} ${sign}${pct}% (${current})`;
      };
      const bonusPills = [];
      const speedMult = player.speed / BASE_STATS.speed;
      const pickupMult = player.pickup / BASE_STATS.pickup;
      const hpMult = player.maxHp / BASE_STATS.hp;
      bonusPills.push(fmtBonus("Move", speedMult, Math.round(player.speed)));
      bonusPills.push(fmtBonus("Pickup", pickupMult, Math.round(player.pickup)));
      bonusPills.push(fmtBonus("Max HP", hpMult, Math.round(player.maxHp)));
      ui.bonuses.innerHTML = bonusPills.map(b => `<span class="pill">${b}</span>`).join(" ");

      const parts = [];
      if (buffs.magnet > 0) parts.push(`Magnet ${fmtFloat(buffs.magnet)}s`);
      if (buffs.shield > 0) parts.push(`Shield ${fmtFloat(buffs.shield)}s`);
      if (buffs.slow > 0) parts.push(`Freeze ${fmtFloat(buffs.slow)}s`);
      if (buffs.power > 0) parts.push(`Power ${fmtFloat(buffs.power)}s`);
      if (buffs.haste > 0) parts.push(`Sprint ${fmtFloat(buffs.haste)}s`);
      if (buffs.xp > 0) parts.push(`XP ${fmtFloat(buffs.xp)}s`);
      ui.buffs.innerHTML = `<b>Buffs:</b> ${parts.length ? parts.join(" | ") : "-"}`;
    }

    /* ============================
       Main Loop
       ============================ */
    let last = performance.now();

    function update(dt){
      player.time += dt;

      updateBuffs(dt);
      updatePlayer(dt);

      const camX = player.x - W*0.5;
      const camY = player.y - H*0.5;

      spawnController(dt, camX, camY);
      updateChests(dt, camX, camY);

      updateWeapons(dt);
      updateEnemies(dt);
      updateTelegraphs(dt);
      updateVoidZones(dt);
      updateEnemyShots(dt);
      updateBullets(dt);
      updateRailShots(dt);
      updateAxes(dt);
      updateOrbs(dt);
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
      for (let i=0;i<30;i++) addParticles(rand(200,-200), rand(120,-120), COLORS.player, 1, 220);
      requestAnimationFrame(frame);
    }

    boot();

    ui.levelup.addEventListener("click", (e)=> e.stopPropagation(), { passive:true });
    ui.gameover.addEventListener("click", (e)=> e.stopPropagation(), { passive:true });
    ui.menu.addEventListener("click", (e)=> e.stopPropagation(), { passive:true });

  })();
  
