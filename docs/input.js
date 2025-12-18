import { input } from "./state.js";

export const clearDirectionalInput = () => { input.up = input.down = input.left = input.right = false; };

export function setupInput({ canvas, ui, sound, isMobile, STATE, getState, openMenu, closeMenu }) {
  const CODE_MAP = new Map([
    ["KeyW", "up"], ["ArrowUp", "up"],
    ["KeyS", "down"], ["ArrowDown", "down"],
    ["KeyA", "left"], ["ArrowLeft", "left"],
    ["KeyD", "right"], ["ArrowRight", "right"],
  ]);

  addEventListener("keydown", (e) => {
    if (e.code === "Escape") {
      const st = getState();
      if (st === STATE.PLAYING) {
        openMenu();
      } else if (st === STATE.MENU) {
        closeMenu();
      }
      e.preventDefault();
      return;
    }

    const m = CODE_MAP.get(e.code);
    if (m) { input[m] = true; e.preventDefault(); }
  }, { passive: false });

  addEventListener("keyup", (e) => {
    const m = CODE_MAP.get(e.code);
    if (m) { input[m] = false; e.preventDefault(); }
  }, { passive: false });
  addEventListener("keydown", () => sound.unlock(), { passive: true });

  // Touch drag controls for mobile (simple virtual stick)
  if (isMobile) {
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
    const updateTouchDir = (x, y) => {
      const dx = x - startX;
      const dy = y - startY;
      clearDirectionalInput();
      if (Math.abs(dx) > DEAD) {
        if (dx > 0) input.right = true; else input.left = true;
      }
      if (Math.abs(dy) > DEAD) {
        if (dy > 0) input.down = true; else input.up = true;
      }
      if (stickInner) {
        const clampLen = 48;
        const len = Math.min(clampLen, Math.hypot(dx, dy));
        const ang = Math.atan2(dy, dx);
        stickInner.style.transform = `translate(${Math.cos(ang) * len}px, ${Math.sin(ang) * len}px)`;
      }
    };
    const endTouch = () => { touchId = null; clearDirectionalInput(); resetStick(); };
    canvas.addEventListener("touchstart", (e) => {
      if (touchId !== null) return;
      const t = e.changedTouches[0];
      touchId = t.identifier;
      startX = t.clientX;
      startY = t.clientY;
      if (stick) {
        stick.classList.add("on");
        stick.style.left = `${startX - 60}px`;
        stick.style.top = `${startY - 60}px`;
      }
      updateTouchDir(startX, startY);
    }, { passive: true });
    canvas.addEventListener("touchmove", (e) => {
      if (touchId === null) return;
      for (let i = 0; i < e.changedTouches.length; i++) {
        const t = e.changedTouches[i];
        if (t.identifier === touchId) {
          updateTouchDir(t.clientX, t.clientY);
          e.preventDefault();
          break;
        }
      }
    }, { passive: false });
    const touchEndHandler = (e) => {
      if (touchId === null) return;
      for (let i = 0; i < e.changedTouches.length; i++) {
        if (e.changedTouches[i].identifier === touchId) {
          endTouch();
          break;
        }
      }
    };
    canvas.addEventListener("touchend", touchEndHandler, { passive: true });
    canvas.addEventListener("touchcancel", touchEndHandler, { passive: true });

    if (stickOuter) stickOuter.style.display = "block";
  }
}
