export const TAU = Math.PI * 2;

export const rand = (a = 1, b = 0) => (Math.random() * (a - b) + b);
export const randi = (a, b = 0) => (Math.random() * (a - b) + b) | 0;
export const clamp = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);
export const hypot = Math.hypot;

export const fmtFloat = (n, digits = 2) => {
  if (Math.abs(n - Math.round(n)) < 1e-6) return String(Math.round(n));
  const s = n.toFixed(digits);
  return s === "-0" ? "0" : s;
};
