#!/usr/bin/env python3
"""Builds presentation.html, a standalone viewer for the slide deck, from deck.json and slides/.

    python3 docs/presentation/build.py
    google-chrome --headless --no-pdf-header-footer --print-to-pdf=docs/presentation/presentation.pdf \
        docs/presentation/presentation.html          # optional: one slide per PDF page

deck.json and slides/*.html are the deck's source: one 1920x1080 <section> per slide with inline
styles, and speaker notes in an <aside>. The viewer approximates how the hosted deck renders them.
Keys: arrows / space / click to move, N to show the speaker notes, F for full screen.
"""
import html
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
deck = json.load(open(os.path.join(HERE, "deck.json"), encoding="utf-8"))
slides = [open(os.path.join(HERE, "slides", sid + ".html"), encoding="utf-8").read().strip() for sid in deck["order"]]
fonts = sorted({f["href"] for f in deck["faces"].values() if "href" in f})

PAGE = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>%(title)s</title>
%(fonts)s
<style>
html, body { margin: 0; height: 100%%; background: #05080c; overflow: hidden; }
#stage { position: absolute; left: 0; top: 0; width: 1920px; height: 1080px; transform-origin: 0 0; }
section { position: absolute; left: 0; top: 0; width: 1920px; height: 1080px; box-sizing: border-box; overflow: hidden; }
section:not(.on) { display: none !important; }
section * { margin: 0; box-sizing: border-box; }
section aside { display: none; }
section p { font-variant-ligatures: none; }  /* show code as typed: -> and != stay two characters */
section ol, section ul { padding-left: 1.3em; }
section table { border-collapse: collapse; }
section table:not([style*="width"]) { width: 100%%; }
section th, section td { padding: 0.35em 0.6em; text-align: left; vertical-align: top; border-bottom: 1px solid rgba(128, 128, 128, 0.28); }
section tr:last-child td { border-bottom: none; }
section th { font-weight: 600; }
#notes { position: fixed; left: 0; right: 0; bottom: 0; max-height: 38%%; overflow: auto; display: none; padding: 16px 24px;
         background: rgba(5, 8, 12, 0.94); color: #e6edf3; font: 18px/1.5 'IBM Plex Sans', Arial, sans-serif; }
#notes.on { display: block; }
@page { size: 1920px 1080px; margin: 0; }
@media print {
  html, body { height: auto; overflow: visible; background: none; }
  #stage { position: static; transform: none !important; width: 1920px; height: auto; }
  section, section:not(.on) { position: relative; display: flex !important; break-after: page; }
  section[style*="display:grid"] { display: grid !important; }
  #notes { display: none !important; }
}
</style>
</head>
<body>
<div id="stage">
%(slides)s
</div>
<div id="notes"></div>
<script>
(function () {
  var stage = document.getElementById('stage'), notes = document.getElementById('notes');
  var slides = Array.prototype.slice.call(stage.querySelectorAll('section')), at = 0;
  // A header cell's width and alignment apply to its whole column.
  document.querySelectorAll('section table').forEach(function (t) {
    var head = t.rows[0] ? t.rows[0].cells : [];
    for (var r = 1; r < t.rows.length; r++)
      for (var c = 0; c < t.rows[r].cells.length && c < head.length; c++)
        if (head[c].style.textAlign && !t.rows[r].cells[c].style.textAlign) t.rows[r].cells[c].style.textAlign = head[c].style.textAlign;
  });
  function fit() {
    var k = Math.min(innerWidth / 1920, innerHeight / 1080);
    stage.style.transform = 'translate(' + (innerWidth - 1920 * k) / 2 + 'px,' + (innerHeight - 1080 * k) / 2 + 'px) scale(' + k + ')';
  }
  function show(i) {
    at = Math.max(0, Math.min(slides.length - 1, i));
    slides.forEach(function (s, j) { s.classList.toggle('on', j === at); });
    var a = slides[at].querySelector('aside');
    notes.textContent = a ? a.textContent : '';
    history.replaceState(null, '', '#slide-' + (at + 1));
  }
  addEventListener('resize', fit);
  addEventListener('keydown', function (e) {
    if (e.key === 'ArrowRight' || e.key === 'ArrowDown' || e.key === ' ' || e.key === 'PageDown') show(at + 1);
    else if (e.key === 'ArrowLeft' || e.key === 'ArrowUp' || e.key === 'PageUp') show(at - 1);
    else if (e.key === 'Home') show(0);
    else if (e.key === 'End') show(slides.length - 1);
    else if (e.key === 'n' || e.key === 'N') notes.classList.toggle('on');
    else if (e.key === 'f' || e.key === 'F') document.documentElement.requestFullscreen();
  });
  stage.addEventListener('click', function (e) { show(at + (e.clientX < innerWidth / 3 ? -1 : 1)); });
  var m = /^#slide-(\\d+)$/.exec(location.hash);
  fit();
  show(m ? +m[1] - 1 : 0);
})();
</script>
</body>
</html>
"""

out = PAGE % {
    "title": html.escape(deck["title"]),
    "fonts": "\n".join('<link rel="stylesheet" href="%s">' % html.escape(h) for h in fonts),
    "slides": "\n".join(slides),
}
with open(os.path.join(HERE, "presentation.html"), "w", encoding="utf-8") as f:
    f.write(out)
print("wrote presentation.html (%d slides)" % len(slides))
