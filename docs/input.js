import { input } from "./state.js";

export const clearDirectionalInput = () => { input.up = input.down = input.left = input.right = false; };

const clampToRange = (value, min, max) => Math.max(min, Math.min(max, value));

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

  // Pointer Events cover touch, pen and the QA-forced mobile mode with one path.
  if (isMobile) {
    let pointerId = null;
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
    const endPointer = () => { pointerId = null; clearDirectionalInput(); resetStick(); };
    canvas.addEventListener("pointerdown", (e) => {
      if (pointerId !== null || (e.pointerType === "mouse" && e.button !== 0)) return;
      pointerId = e.pointerId;
      startX = e.clientX;
      startY = e.clientY;
      try { canvas.setPointerCapture(pointerId); } catch {}
      if (stick) {
        const viewport = window.visualViewport;
        const viewLeft = viewport?.offsetLeft || 0;
        const viewTop = viewport?.offsetTop || 0;
        const viewWidth = viewport?.width || window.innerWidth;
        const viewHeight = viewport?.height || window.innerHeight;
        const radius = 60;
        const margin = 8;
        const landscape = viewWidth > viewHeight;
        const hudClearance = landscape ? 82 : 132;
        const minX = viewLeft + radius + margin;
        const maxX = Math.max(minX, viewLeft + Math.min(viewWidth - radius - margin, viewWidth * 0.64));
        const minY = viewTop + Math.max(radius + margin, hudClearance);
        const maxY = Math.max(minY, viewTop + viewHeight - radius - margin);
        const visualX = clampToRange(startX, minX, maxX);
        const visualY = clampToRange(startY, minY, maxY);
        stick.classList.add("on");
        stick.style.left = `${visualX - radius}px`;
        stick.style.top = `${visualY - radius}px`;
      }
      updateTouchDir(startX, startY);
      e.preventDefault();
    }, { passive: false });
    canvas.addEventListener("pointermove", (e) => {
      if (pointerId !== e.pointerId) return;
      updateTouchDir(e.clientX, e.clientY);
      e.preventDefault();
    }, { passive: false });
    const pointerEndHandler = (e) => {
      if (pointerId !== e.pointerId) return;
      try { canvas.releasePointerCapture(pointerId); } catch {}
      endPointer();
    };
    canvas.addEventListener("pointerup", pointerEndHandler, { passive: true });
    canvas.addEventListener("pointercancel", pointerEndHandler, { passive: true });
    addEventListener("blur", endPointer, { passive: true });

    if (stickOuter) stickOuter.style.display = "block";
  }
}
