"use strict";
// The start page's title screen: one canvas of a few hundred pixels across, scaled up by a whole number: stars, copper bars, the grid, a chrome logo that bounces (web/tools/gen_logo.py, drawn 1:1) and a
// sine scroller in the site's bitmap font. It only runs while it can be seen, at most 30 times a second, and it holds one still frame for people who ask for less motion.
(() => {
  const cv = document.getElementById("demo");
  if (!cv) return;
  const g = cv.getContext("2d", { alpha: false });
  const still = matchMedia("(prefers-reduced-motion:reduce)").matches;
  const COPPER = ["#1a1058", "#6a1e9c", "#c23a24", "#ff8a30", "#fffaf0", "#ff8a30", "#c23a24", "#6a1e9c", "#1a1058"];
  const STAR = ["#4f78b0", "#b6cce4", "#fffaf0", "#ff8a30", "#30c0e0"];
  const load = (src) => new Promise((res) => { const i = new Image(); i.onload = () => res(i); i.onerror = () => res(null); i.src = src; });
  const rnd = (seed) => () => (seed = (seed * 1664525 + 1013904223) >>> 0) / 4294967296;
  let W = 480, H = 270, hor = 170, stars = [], floor = null, logo = null, logos = [], strip = null, stripW = 0;

  function size() {
    const r = cv.parentElement.getBoundingClientRect(), zoom = Math.max(1, Math.round(r.width / 470));   // a whole number, so every pixel of the art is the same size
    W = Math.ceil(r.width / zoom); H = Math.ceil(r.height / zoom);
    cv.width = W; cv.height = H;
    cv.style.width = W * zoom + "px"; cv.style.height = H * zoom + "px";
    g.imageSmoothingEnabled = false;
    logo = logos.find((l) => l.width <= W - 12) || logos[logos.length - 1] || null;
    hor = Math.round(H * 0.68);
    const n = rnd(11);
    stars = Array.from({ length: 80 }, (_, i) => ({ x: n() * W, y: n() * hor, v: 2 + (i % 3) * 5, c: STAR[Math.floor(n() * STAR.length)] }));
    // the lines to the vanishing point never move, so they are plotted once. Pixel by pixel: a canvas line would be smoothed
    floor = document.createElement("canvas");
    floor.width = W; floor.height = H - hor;
    const f = floor.getContext("2d");
    f.fillStyle = "#30c0e0";
    for (let k = -14; k <= 14; ++k) for (let y = 2; y < H - hor; y += 2) f.fillRect(Math.round(W / 2 + k * 12 * (y / 12)), y, 1, 1);
  }

  // The scroller's text as one long strip, thresholded to hard pixels and coloured by rows: the copper again
  function makeStrip(text) {
    const m = document.createElement("canvas").getContext("2d");
    const font = '18.667px "Jersey 10", monospace';
    m.font = font;
    const w = Math.ceil(m.measureText(text).width) + 16, h = 20;
    const s = document.createElement("canvas");
    s.width = w; s.height = h;
    const c = s.getContext("2d", { willReadFrequently: true });
    c.font = font; c.textBaseline = "top"; c.fillStyle = "#fff";
    c.fillText(text, 0, 1);
    const im = c.getImageData(0, 0, w, h), d = im.data;
    const rows = [[255, 250, 240], [255, 250, 240], [182, 204, 228], [182, 204, 228], [48, 192, 224], [48, 192, 224], [255, 250, 240], [255, 138, 48], [255, 138, 48], [255, 138, 48], [194, 58, 36], [194, 58, 36], [194, 58, 36], [106, 30, 156], [106, 30, 156], [106, 30, 156], [106, 30, 156], [106, 30, 156], [106, 30, 156], [106, 30, 156]];
    for (let y = 0; y < h; ++y) for (let x = 0; x < w; ++x) {
      const i = (y * w + x) * 4;
      const on = d[i + 3] > 110;
      d[i] = rows[y][0]; d[i + 1] = rows[y][1]; d[i + 2] = rows[y][2]; d[i + 3] = on ? 255 : 0;
    }
    c.putImageData(im, 0, 0);
    strip = s; stripW = w;
  }

  function frame(t) {
    g.fillStyle = "#000"; g.fillRect(0, 0, W, H);
    for (const s of stars) {
      g.fillStyle = s.c;
      g.fillRect(Math.round(((s.x - t * s.v) % W + W) % W), Math.round(s.y), s.v > 8 ? 2 : 1, 1);
    }
    // copper bars, on sines of their own
    for (let b = 0; b < 3; ++b) {
      const y = Math.round(hor * 0.46 + Math.sin(t * 1.1 + b * 2.1) * hor * 0.36);
      COPPER.forEach((col, i) => { g.fillStyle = col; g.fillRect(0, y + i * 2, W, 2); });
    }
    // the horizon and the grid. Its rows close up towards the horizon and run towards you
    g.fillStyle = "#6a1e9c"; g.fillRect(0, hor - 3, W, 1); g.fillRect(0, hor + 2, W, 1);
    g.fillStyle = "#ff8a30"; g.fillRect(0, hor - 1, W, 1);
    g.fillStyle = "#fffaf0"; g.fillRect(0, hor, W, 1);
    g.fillStyle = "#c23a24"; g.fillRect(0, hor + 1, W, 1);
    if (floor) g.drawImage(floor, 0, hor);
    g.fillStyle = "#4f78b0";
    const fl = H - hor;
    for (let k = 0; k < 7; ++k) g.fillRect(0, hor + 3 + Math.round((((k + ((t * 0.7) % 1)) / 7) ** 2.4) * fl), W, 1);
    // the logo: a bounce, and a ripple through its rows
    if (logo) {
      const lw = logo.width, lh = logo.height;
      // it bounces in the space above PRESS START, and its spikes never leave the canvas
      const room = Math.max(4, Math.round(H * 0.53) - lh - 8), amp = Math.min(room, 22), top = Math.round(4 + (room - amp) / 2);
      const x0 = Math.round((W - lw) / 2), y0 = top + amp - Math.round(Math.abs(Math.sin(t * 1.9)) * amp);
      for (let r = 0; r < lh; ++r) g.drawImage(logo, 0, r, lw, 1, x0 + Math.round(Math.sin(t * 2.6 + r * 0.09) * 3), y0 + r, lw, 1);
    }
    // the sine scroller
    if (strip) {
      const base = H - 34, off = Math.round(t * 42) % stripW;
      for (let x = 0; x < W; x += 2) {
        const sx = (x + off) % stripW;
        g.drawImage(strip, sx, 0, 2, 20, x, Math.round(base + Math.sin(t * 2.2 + x * 0.018) * 8), 2, 20);
      }
    }
    cv.dataset.frames = String(+(cv.dataset.frames || 0) + 1);
  }

  let seen = true, running = false, last = 0;
  function tick(ms) {
    if (!seen || document.hidden) { running = false; return; }
    requestAnimationFrame(tick);
    if (ms - last < 32) return;
    last = ms;
    frame(ms / 1000);
  }
  const go = () => { if (still) { frame(0.62); return; } if (!running && seen && !document.hidden) { running = true; requestAnimationFrame(tick); } };
  new IntersectionObserver((e) => { seen = e[e.length - 1].isIntersecting; go(); }).observe(cv);
  document.addEventListener("visibilitychange", go);
  let resizing = 0;
  addEventListener("resize", () => { clearTimeout(resizing); resizing = setTimeout(() => { size(); if (still) frame(0.62); }, 120); });

  size();
  const words = document.querySelector(".scroll-text").textContent.trim().replace(/\s+/g, " ");
  Promise.all([load("img/logo-440.png"), load("img/logo-340.png"), load("img/logo-260.png"), document.fonts.load('18.667px "Jersey 10"').catch(() => null)]).then(([a, b, c]) => {
    logos = [a, b, c].filter(Boolean);
    logo = logos.find((l) => l.width <= W - 12) || logos[logos.length - 1] || null;
    makeStrip(words.toUpperCase() + "        ");
    go();
  });
  // Enter, as on a title screen, while nothing else has the keyboard
  addEventListener("keydown", (e) => { if (e.key === "Enter" && seen && document.activeElement === document.body) document.getElementById("stages").scrollIntoView(); });
})();
