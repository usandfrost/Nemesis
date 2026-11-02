"use strict";
// What the pages share: the status line at the top, the pixel sprites, and the labels on the real screens. Nothing here animates: the start page's title screen is demo.js, the rest stays still.
(() => {
  const PAGES = [["index.html", "Start"], ["online.html", "Online Nemesis"], ["tutorial.html", "Tutorial"], ["install.html", "Install"], ["manual.html", "Manual"], ["librarian.html", "Preset librarian"]];
  const stem = (name) => name.replace(/\.(html|php)$/, "");   // (some hosts drop the .html, and the PHP version of the site has .php)
  const here = stem(location.pathname.split("/").pop() || "index");
  const bar = document.getElementById("bar");
  if (bar) bar.innerHTML = `<a class="logo chrome" href="index.html">NEMESIS</a><span class="beta">Beta 2</span><nav aria-label="Pages">` +
    PAGES.map(([href, label]) => `<a href="${href}"${stem(href) === here ? ' aria-current="page"' : ""}>${label}</a>`).join("") + `</nav><span class="aside">firmware for the Drumboy Pro</span>`;

  // The sprites: 12 x 12, one letter a pixel. Drawn here for this site.
  const INK = { k: "#000000", n: "#1a1058", v: "#6a1e9c", s: "#4f78b0", i: "#b6cce4", w: "#fffaf0", o: "#ff8a30", r: "#c23a24", c: "#30c0e0" };
  const SPRITES = {
    steps: ["............", ".oo.ss.oo.ss", ".oo.ss.oo.ss", "............", ".ss.cc.ss.cc", ".ss.cc.ss.cc", "............", ".rr.ss.rr.rr", ".rr.ss.rr.rr", "............", ".ss.oo.ss.oo", ".ss.oo.ss.oo"],
    filter: ["............", "cccccc......", "ccccccccc...", "wwwwwwwwwww.", "ccccccccccc.", "cccccccccccw", "ccccccccccsw", "ccccccccss.w", "ccccccsss..w", "sssssss.....", "............", "............"],
    blade: ["...i........", "...ii.......", "...iii......", "...iiii.....", "...iiiii....", "...iwwii....", "...iiiii....", "...oo.......", "...oo.......", "...rr.......", "...rr.......", "............"],
    fader: ["..o..c..o..c", "..o..c..o..c", "..o..c.www.c", ".www.c..o..c", "..o..c..o.ww", "..o.www.o..c", "..o..c..o..c", "..o..c..o..c", "..o..c.www.c", "..o..c..o..c", ".www.c..o..c", "..o..c..o..c"],
    chip: ["..i..i..i...", "..i..i..i...", ".ssssssssss.", "issssssssssi", ".ssnnnnnnss.", "issnccccnssi", ".ssncwwcnss.", "issnccccnssi", ".ssnnnnnnss.", ".ssssssssss.", "..i..i..i...", "..i..i..i..."],
    echo: ["....o.......", "...oo.....v.", "..ooo..v..v.", ".oooo..vv.v.", "ooooo.vvv.v.", "ooooo.vvv.v.", "ooooo.vvv.v.", ".oooo..vv.v.", "..ooo..v..v.", "...oo.....v.", "....o.......", "............"],
    star: ["............", ".....oo.....", ".....oo.....", "....owwo....", "oooowwwwoooo", ".oowwwwwwoo.", "..oowwwwoo..", "..oowwwwoo..", ".oowo..owoo.", ".oo......oo.", ".o........o.", "............"],
    note: ["......www...", "......w.ww..", "......w..ww.", "......w...w.", "......w.....", "......w.....", "......w.....", "..ooooo.....", ".oooooo.....", ".oooooo.....", "..oooo......", "............"],
    wave: ["............", "..cc........", ".c..c.......", "c....c......", "c....c.....c", "......c...c.", "......c...c.", ".......c.c..", "........c...", "............", "............", "............"],
    bolt: ["......oo....", ".....oo.....", "....oo......", "...oo.......", "..oooooo....", ".....oo.....", "....oo......", "...oo.......", "..oo........", "..o.........", "............", "............"],
    lock: ["............", "...iiiiii...", "..ii....ii..", "..ii....ii..", "..ii....ii..", ".ssssssssss.", ".ssssssssss.", ".sssskksss..", ".sssskksss..", ".ssssssssss.", ".ssssssssss.", "............"],
    disk: ["ssssssssss..", "ssiiiiiissw.", "ssiiiiiisss.", "ssiiiiiisss.", "ssssssssssss", "ssssssssssss", "sswwwwwwwwss", "sswwwwwwwwss", "sswwkkwwwwss", "sswwwwwwwwss", "ssssssssssss", "............"],
  };
  const cache = {};
  const sprite = (name) => {
    if (cache[name]) return cache[name];
    const c = document.createElement("canvas");
    c.width = c.height = 12;
    const g = c.getContext("2d");
    SPRITES[name].forEach((row, y) => [...row].forEach((ch, x) => { if (INK[ch]) { g.fillStyle = INK[ch]; g.fillRect(x, y, 1, 1); } }));
    return (cache[name] = c.toDataURL());
  };
  document.querySelectorAll(".sprite[data-s]").forEach((e) => (e.style.backgroundImage = `url(${sprite(e.dataset.s)})`));

  // a stage: a note and the label it belongs to light up together
  document.querySelectorAll(".tour").forEach((t) => {
    const marks = t.querySelectorAll(".mark");
    t.querySelectorAll(".notes li").forEach((li, i) => {
      const lit = (on) => { li.classList.toggle("on", on); marks[i].classList.toggle("on", on); };
      li.addEventListener("pointerenter", () => lit(true));
      li.addEventListener("pointerleave", () => lit(false));
    });
  });

  // a few stars behind the page, drawn once
  const sky = document.getElementById("sky");
  if (!sky) return;
  const c = sky.getContext("2d");
  const rnd = (seed) => () => (seed = (seed * 1664525 + 1013904223) >>> 0) / 4294967296;
  const COL = ["#4f78b0", "#b6cce4", "#fffaf0", "#6a1e9c", "#30c0e0"];
  function draw() {
    const W = 320, H = Math.max(120, Math.round((W * innerHeight) / innerWidth));
    sky.width = W; sky.height = H;
    const r = rnd(7);
    c.fillStyle = "#000"; c.fillRect(0, 0, W, H);
    for (let i = 0; i < 70; ++i) { c.fillStyle = COL[Math.floor(r() * COL.length)]; c.fillRect(Math.round(r() * W), Math.round(r() * H), 1, 1); }
  }
  draw();
  addEventListener("resize", draw);
})();
