import { COLORS } from "./config.js";

function makePool(createFn, initial = 256) {
  const pool = [];
  for (let i = 0; i < initial; i++) pool.push(createFn());
  return {
    get() {
      return pool.pop() || createFn();
    },
    put(obj) {
      pool.push(obj);
    },
  };
}

export const enemyPool = makePool(
  () => ({
    alive: false,
    type: "A",
    x: 0,
    y: 0,
    r: 10,
    hp: 1,
    maxHp: 1,
    speed: 1,
    dmg: 1,
    color: "#fff",
    kx: 0,
    ky: 0,
    ranged: false,
    shotCd: 0,
    shotDmg: 0,
    shotSpeed: 0,
    shotRange: 0,
    shotT: 0,
    shotSeq: 0,
    spitter: false,
    spitCd: 0,
    spitRange: 0,
    spitRadius: 0,
    spitDuration: 0,
    spitDps: 0,
    spitColor: "#fff",
    spitTelegraph: 0,
    spitType: "",
    spitT: 0,
    boss: false,
    novaCd: 0,
    novaT: 0,
    novaShots: 0,
    novaShotSpeed: 0,
    novaShotDmg: 0,
    novaRadius: 0,
    novaTelegraph: 0,
    novaSeq: 0,
    voidSeq: 0,
    voidCd: 0,
    voidT: 0,
    voidCount: 0,
    voidRadius: 0,
    voidDuration: 0,
    voidDps: 0,
    voidTick: 0.25,
    voidColor: "#fff",
    voidTelegraph: 0,
    barrageCd: 0,
    barrageT: 0,
    barrageShots: 0,
    barrageWaves: 0,
    barrageWaveDelay: 0,
    barrageShotSpeed: 0,
    barrageShotDmg: 0,
    slamCd: 0,
    slamT: 0,
    slamRadius: 0,
    slamDmg: 0,
    slamTelegraph: 0,
    slamColor: "#fff",
    slamSeq: 0,
    rockCd: 0,
    rockT: 0,
    rockCount: 0,
    rockRadius: 0,
    rockDmg: 0,
    rockTelegraph: 0,
    rockOffsetMin: 0,
    rockOffsetMax: 0,
    rockColor: "#fff",
    rockSeq: 0,
    rayCd: 0,
    rayT: 0,
    rayCount: 0,
    raySpeed: 0,
    rayDmg: 0,
    rayTelegraph: 0,
    raySeq: 0,
    mineCd: 0,
    mineT: 0,
    mineCount: 0,
    mineRadius: 0,
    mineDmg: 0,
    mineTelegraph: 0,
    mineOffsetMin: 0,
    mineOffsetMax: 0,
    mineColor: "#fff",
    mineSeq: 0,
    slowT: 0,
    slowMul: 1,
    burnT: 0,
    burnDps: 0,
    bleedT: 0,
    bleedDps: 0,
    elite: false,
    knockResist: 0,
    gemBonus: 0,
  }),
  260
);

export const bulletPool = makePool(
  () => ({
    alive: false,
    x: 0,
    y: 0,
    vx: 0,
    vy: 0,
    r: 3,
    dmg: 8,
    life: 0,
    maxLife: 0,
    critChance: 0,
    critMult: 1,
    color: null,
    isExplosion: false,
  }),
  260
);
export const missilePool = makePool(
  () => ({
    alive: false,
    x: 0,
    y: 0,
    vx: 0,
    vy: 0,
    r: 6,
    dmg: 0,
    life: 0,
    speed: 0,
    maxSpeed: 0,
    accel: 0,
    turnRate: 0,
    explosion: 0,
    critChance: 0,
    critMult: 1,
  }),
  140
);
export const railPool = makePool(
  () => ({
    alive: false,
    x: 0,
    y: 0,
    vx: 0,
    vy: 0,
    r: 4.4,
    dmg: 60,
    life: 0,
    pierce: 0,
    trail: [],
    critChance: 0,
    critMult: 1,
  }),
  160
);
export const axePool = makePool(
  () => ({
    alive: false,
    x: 0,
    y: 0,
    vx: 0,
    vy: 0,
    r: 6,
    dmg: 18,
    life: 0,
    rot: 0,
    spin: 0,
    critChance: 0,
    critMult: 1,
  }),
  120
);
export const shotPool = makePool(
  () => ({
    alive: false,
    x: 0,
    y: 0,
    vx: 0,
    vy: 0,
    r: 3.6,
    dmg: 8,
    life: 0,
    color: COLORS.gem,
  }),
  240
);
export const voidPool = makePool(
  () => ({
    alive: false,
    x: 0,
    y: 0,
    radius: 0,
    life: 0,
    maxLife: 0,
    dps: 0,
    tick: 0.25,
    tickT: 0,
    color: "#fff",
    type: "poison",
  }),
  120
);
export const orbPool = makePool(
  () => ({
    alive: false,
    x: 0,
    y: 0,
    vx: 0,
    vy: 0,
    r: 10,
    dmg: 0,
    critChance: 0,
    critMult: 1,
    state: "fly",
    life: 0,
    park: 0,
    tick: 0,
    pull: 0,
    radius: 0,
    explosion: 0,
  }),
  80
);

export const gemPool = makePool(
  () => ({
    alive: false,
    x: 0,
    y: 0,
    vx: 0,
    vy: 0,
    v: 1,
    r: 5,
    life: 0,
    maxLife: 0,
  }),
  260
);
export const partPool = makePool(
  () => ({
    alive: false,
    x: 0,
    y: 0,
    vx: 0,
    vy: 0,
    life: 0,
    maxLife: 0,
    r: 2,
    color: COLORS.gem,
  }),
  520
);
export const chestPool = makePool(
  () => ({
    alive: false,
    x: 0,
    y: 0,
    r: 12,
    pulse: 0,
  }),
  24
);
export const dmgPool = makePool(
  () => ({
    alive: false,
    x: 0,
    y: 0,
    vx: 0,
    vy: 0,
    life: 0,
    maxLife: 0,
    text: "",
    color: "#fff",
    size: 14,
  }),
  240
);
export const textPool = makePool(
  () => ({
    alive: false,
    x: 0,
    y: 0,
    vx: 0,
    vy: 0,
    life: 0,
    maxLife: 0,
    text: "",
    color: "#fff",
    size: 14,
  }),
  120
);

export { makePool };
