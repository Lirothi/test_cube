import { rand } from "./math.js";
import { floatTexts } from "./state.js";
import { textPool } from "./pools.js";

export function popFloatText(
  x,
  y,
  text,
  color = "#d7f6ff",
  size = 16,
  life = 0.9,
  vx = 16,
  vyMin = 34,
  vyMax = 54
) {
  if (text == null) return;
  const t = textPool.get();
  t.alive = true;
  t.x = x;
  t.y = y;
  t.vx = rand(vx, -vx);
  t.vy = -rand(vyMax, vyMin);
  t.maxLife = life;
  t.life = t.maxLife;
  t.text = text;
  t.color = color;
  t.size = size;
  floatTexts.push(t);
}
