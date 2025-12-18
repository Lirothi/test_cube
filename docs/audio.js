export const sound = {
  ctx: null,
  unlocked: false,
  enabled: true,
  master: null,
  bank: {
    shoot: { freq: 720, type: "square", dur: 0.06, vol: 0.05 },
    rail: { freq: 220, type: "sawtooth", dur: 0.18, vol: 0.08, glide: 140 },
    axe: { freq: 320, type: "triangle", dur: 0.12, vol: 0.06 },
    pickup: { freq: 960, type: "square", dur: 0.05, vol: 0.04 },
    level: { freq: 560, type: "triangle", dur: 0.25, vol: 0.06, glide: 140 },
    hurt: { freq: 160, type: "sawtooth", dur: 0.12, vol: 0.08 },
    boss: { freq: 90, type: "square", dur: 0.4, vol: 0.10, glide: -60 },
  },
  unlock() {
    if (this.unlocked) return;
    try {
      this.ctx = this.ctx || new (window.AudioContext || window.webkitAudioContext)();
      this.ctx.resume();
      if (!this.master) {
        this.master = this.ctx.createGain();
        this.master.gain.value = 0.35;
        this.master.connect(this.ctx.destination);
      }
      this.unlocked = true;
    } catch {}
  },
  play(name) {
    if (!this.enabled || !this.unlocked || !this.ctx || !this.master) return;
    if (this.ctx.state === "suspended") return;
    const cfg = this.bank[name];
    if (!cfg) return;
    const ctx = this.ctx;
    const now = ctx.currentTime;
    const osc = ctx.createOscillator();
    const gain = ctx.createGain();
    osc.type = cfg.type || "sine";
    osc.frequency.value = cfg.freq || 440;
    if (cfg.glide) {
      osc.frequency.linearRampToValueAtTime(
        Math.max(20, (cfg.freq || 440) + cfg.glide),
        now + Math.max(0.01, cfg.dur || 0.1)
      );
    }
    const vol = cfg.vol || 0.05;
    gain.gain.setValueAtTime(vol, now);
    gain.gain.exponentialRampToValueAtTime(0.0001, now + (cfg.dur || 0.1));
    osc.connect(gain);
    gain.connect(this.master);
    osc.start(now);
    osc.stop(now + (cfg.dur || 0.1));
  },
};
