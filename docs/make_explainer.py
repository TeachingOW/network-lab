#!/usr/bin/env python3
"""Renders the explanation diagram and video for COMPARISON.md.

    python3 docs/make_explainer.py                 # docs/request-flow.png + docs/explainer.mp4 (silent)
    python3 docs/make_explainer.py --audio DIR     # same, with narration from DIR/<key>.wav
    python3 docs/make_explainer.py --narration     # prints {key: text} as JSON, to feed a TTS engine
    python3 docs/make_explainer.py --still 12.5 out.png   # one video frame, for checking

Narration: synthesise one mono WAV per key from --narration (all at the same sample rate), then pass
the directory with --audio. Scene lengths stretch to fit each clip. The committed video was voiced
with Piper (voice en_US-lessac-medium):

    python3 docs/make_explainer.py --narration > narration.json
    # for each key: echo "$text" | python3 -m piper -m en_US-lessac-medium.onnx -f DIR/$key.wav

Needs Pillow, numpy (for audio only), the DejaVu fonts and ffmpeg. All numbers come from COMPARISON.md and
bulkapi/RESULTS.md (measured on a 4-core laptop; the reference figures are from its README).
"""
import json
import multiprocessing
import os
import subprocess
import sys
import tempfile
import wave

from PIL import Image, ImageDraw, ImageFont

W, H, S, FPS = 1280, 720, 2, 30
FONT_DIR = "/usr/share/fonts/truetype/dejavu/"
HERE = os.path.dirname(os.path.abspath(__file__))

BG = (15, 20, 26)
PANEL = (26, 34, 46)
LINE = (58, 70, 88)
TEXT = (230, 237, 243)
MUTED = (139, 152, 168)
OURS = (76, 194, 255)
REF = (255, 180, 84)
GOOD = (127, 217, 98)
KERNEL = (110, 122, 140)

_fonts = {}


def font(size, bold=False):
    key = (size, bold)
    if key not in _fonts:
        name = "DejaVuSans-Bold.ttf" if bold else "DejaVuSans.ttf"
        _fonts[key] = ImageFont.truetype(FONT_DIR + name, int(round(size * S)))
    return _fonts[key]


def mix(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))


def ease(t):
    t = max(0.0, min(1.0, t))
    return t * t * (3 - 2 * t)


class Canvas:
    """Draws in logical pixels; the image is S times larger and is scaled down at the end."""

    def __init__(self, w=W, h=H):
        self.w, self.h = w, h
        self.img = Image.new("RGB", (w * S, h * S), BG)
        self.d = ImageDraw.Draw(self.img)

    def rect(self, x, y, w, h, fill=None, outline=None, r=10, width=1.5):
        self.d.rounded_rectangle([x * S, y * S, (x + w) * S, (y + h) * S], radius=r * S, fill=fill,
                                 outline=outline, width=int(round(width * S)))

    def text(self, x, y, s, size=16, fill=TEXT, bold=False, anchor="la"):
        self.d.text((x * S, y * S), s, font=font(size, bold), fill=fill, anchor=anchor)

    def width(self, s, size, bold=False):
        return self.d.textlength(s, font=font(size, bold)) / S

    def wrapped(self, x, y, s, size, max_w, fill=TEXT, anchor="ma", gap=1.35):
        words, lines, cur = s.split(), [], ""
        for word in words:
            trial = (cur + " " + word).strip()
            if self.width(trial, size) > max_w and cur:
                lines.append(cur)
                cur = word
            else:
                cur = trial
        lines.append(cur)
        for i, ln in enumerate(lines):
            self.text(x, y + i * size * gap, ln, size, fill, anchor=anchor)

    def line(self, x0, y0, x1, y1, fill=LINE, width=2):
        self.d.line([x0 * S, y0 * S, x1 * S, y1 * S], fill=fill, width=int(round(width * S)))

    def arrow(self, x0, y0, x1, y1, fill=LINE, width=2, head=7):
        self.line(x0, y0, x1, y1, fill, width)
        dx, dy = x1 - x0, y1 - y0
        n = max((dx * dx + dy * dy) ** 0.5, 1e-6)
        ux, uy = dx / n, dy / n
        px, py = -uy, ux
        pts = [(x1, y1), (x1 - ux * head * 1.6 + px * head, y1 - uy * head * 1.6 + py * head),
               (x1 - ux * head * 1.6 - px * head, y1 - uy * head * 1.6 - py * head)]
        self.d.polygon([(a * S, b * S) for a, b in pts], fill=fill)

    def dot(self, x, y, r, fill):
        self.d.ellipse([(x - r) * S, (y - r) * S, (x + r) * S, (y + r) * S], fill=fill)

    def final(self):
        return self.img.resize((self.w, self.h), Image.LANCZOS)


# ---------------------------------------------------------------- the two request flows

BW, BH, GX = 200, 84, 40
X0 = (W - (5 * BW + 4 * GX)) // 2

OURS_FLOW = dict(
    key="ours", name="bulkapi (ours)", color=OURS,
    tag="thread per core  ·  one ring per thread  ·  built for requests/s and bulk items/s",
    nodes=[
        ("Client", "sends a request", ""),
        ("Kernel socket", "bytes arrive", ""),
        ("Connection buffer", "its own 16 KB buffer,", "RECV always armed"),
        ("Parse + route", "in place; all pipelined", "requests in one pass"),
        ("Handler + store", "256-shard hash map,", "spinlock per shard"),
        ("Res.body", "handler builds", "the body here"),
        ("c.out", "serialize() copies", "headers + body"),
        ("SEND", "one per connection,", "sent immediately"),
    ],
    # (caption, +submissions, +completions, +body copies) on arriving at node i+1
    steps=[
        ("The request's bytes arrive on the connection's socket.", 0, 0, 0),
        ("A single-shot RECV is always armed into this connection's own buffer. "
         "It completes with the data: one submission, one completion.", 1, 1, 0),
        ("The request is parsed in place. Every complete pipelined request in the buffer is "
         "handled in this one pass. The route comes from an if-chain generated from api.spec.", 0, 0, 0),
        ("The handler runs against the sharded in-memory map, taking one spinlock per item.", 0, 0, 0),
        ("The handler builds the response body in Res.body. That is the first copy of the body.", 0, 0, 1),
        ("serialize() then copies headers and body into c.out: a second copy.", 0, 0, 1),
        ("One SEND goes out as soon as the request is processed, and the kernel copies the bytes "
         "a third time. Then the RECV is armed again.", 1, 1, 1),
    ],
    idle="about 4 KB",
)

REF_FLOW = dict(
    key="ref", name="c-iouring-pavlyhalim (reference)", color=REF,
    tag="one thread  ·  up to 8 rings  ·  built to hold ~300,000 connections on 1 vCPU / 2 GB",
    nodes=[
        ("Client", "sends a request", ""),
        ("Kernel socket", "bytes arrive", ""),
        ("Multishot POLL", "idle connection holds", "no buffer: 40 bytes"),
        ("Read queue", "FIFO, bounded by", "free buffers"),
        ("Shared buffer ring", "RECV, MSG_DONTWAIT,", "4096 × 1 KB per ring"),
        ("Parse + route", "in place; one request", "per connection per batch"),
        ("Batch arena", "body written once,", "headers put in front"),
        ("Group commit", "one SQLite transaction", "per batch"),
        ("SEND", "after the commit,", "straight from the arena"),
    ],
    steps=[
        ("The request's bytes arrive on the connection's socket.", 0, 0, 0),
        ("An idle connection holds only a multishot poll: no buffer, a 40-byte record. "
         "The poll fires: one completion, no new submission.", 0, 1, 0),
        ("The connection joins a FIFO read queue. Reads are issued only while shared buffers "
         "are free, so a burst of any size waits here instead of failing.", 0, 0, 0),
        ("One non-blocking RECV reads into a 1 KB buffer the kernel picks from a shared ring: "
         "one submission, one completion.", 1, 1, 0),
        ("The request is parsed in place in the shared buffer. A connection gets one answer "
         "per batch; pipelined bytes wait in a side buffer.", 0, 0, 0),
        ("The body is written once into the batch arena and the headers are placed in front "
         "of it. There is no second copy.", 0, 0, 1),
        ("All of the batch's reads and writes commit in one SQLite transaction.", 0, 0, 0),
        ("Only after the commit, one SEND per reply goes out straight from the arena.", 1, 1, 1),
    ],
    idle="40 bytes",
)


def node_pos(n, y0, gy):
    """Serpentine layout: five boxes left to right, then the rest right to left below."""
    pos = []
    for i in range(n):
        row, col = divmod(i, 5)
        if row == 1:
            col = 4 - col
        pos.append((X0 + col * (BW + GX), y0 + row * (BH + gy)))
    return pos


def draw_flow(c, flow, y0, gy, active=None, reached=None):
    nodes, color = flow["nodes"], flow["color"]
    n = len(nodes)
    reached = n - 1 if reached is None else reached
    pos = node_pos(n, y0, gy)
    for i in range(n - 1):
        (xa, ya), (xb, yb) = pos[i], pos[i + 1]
        col = mix(BG, color, 0.75) if i < reached else LINE
        if ya == yb and xb > xa:
            c.arrow(xa + BW + 3, ya + BH / 2, xb - 4, yb + BH / 2, col)
        elif ya == yb:
            c.arrow(xa - 3, ya + BH / 2, xb + BW + 4, yb + BH / 2, col)
        else:
            c.arrow(xa + BW / 2, ya + BH + 3, xb + BW / 2, yb - 4, col)
    for i, (label, s1, s2) in enumerate(nodes):
        x, y = pos[i]
        on, seen = i == active, i <= reached
        fill = mix(PANEL, color, 0.30) if on else PANEL
        outline = color if on else (mix(BG, color, 0.6) if seen else LINE)
        c.rect(x, y, BW, BH, fill=fill, outline=outline, width=2.5 if on else 1.5)
        c.text(x + BW / 2, y + 13, label, 15, TEXT if seen else MUTED, bold=True, anchor="ma")
        c.text(x + BW / 2, y + 41, s1, 11.5, MUTED, anchor="ma")
        c.text(x + BW / 2, y + 57, s2, 11.5, MUTED, anchor="ma")
    return pos


def draw_tiles(c, flow, y, h, counts, flash=(0, 0, 0)):
    labels = ["Submissions per request", "Completions per request", "Copies of the response body",
              "Memory per idle connection"]
    values = [str(counts[0]), str(counts[1]), str(counts[2]), flow["idle"]]
    tw, gap = 275, 20
    x0 = (W - (4 * tw + 3 * gap)) // 2
    for i in range(4):
        x = x0 + i * (tw + gap)
        f = flash[i] if i < 3 else 0
        c.rect(x, y, tw, h, fill=mix(PANEL, flow["color"], 0.35 * f), outline=LINE)
        c.text(x + 16, y + 11, labels[i], 12, MUTED)
        c.text(x + 16, y + h - 14, values[i], 26 if h >= 80 else 21, TEXT, bold=True, anchor="ls")


def header(c, title, sub, color=TEXT, y=26):
    c.text(X0, y, title, 27, color, bold=True)
    c.text(X0, y + 40, sub, 14, MUTED)


# ---------------------------------------------------------------- narration and timing

# Spoken text per clip. Flow steps default to their on-screen caption (see narration()).
NARR = {
    "title": "How one request flows through two I O U ring servers: bulk A P I, which is ours, and the "
             "reference implementation. They were built for different goals.",
    "ours.intro": "First, bulk A P I. It runs one thread per core, each with its own ring, and is built "
                  "for requests per second and bulk items per second.",
    "ours.outro": "In total, per request: two submissions, two completions, three copies of the response body, "
                  "and about four kilobytes of memory held for every idle connection.",
    "ref.intro": "Now the reference. It runs a single thread, and is built to hold about three hundred thousand "
                 "connections on one virtual C P U with two gigabytes of memory.",
    "ref.outro": "In total, per request: two submissions, three completions, two copies of the response body, "
                 "and only forty bytes held for every idle connection.",
    "table": "Side by side. Both make two submissions per request. Ours needs one completion fewer, and handles "
             "every pipelined request in a single pass. The reference copies the response body one time less, "
             "allocates nothing on the request path, and holds only forty bytes per idle connection.",
    "idle": "Idle connections are where the two designs differ most. For one hundred thousand idle connections, "
            "ours holds about four hundred and thirty megabytes of receive buffers. The reference needs about four megabytes.",
    "time": "Where does our C P U time go? For single requests, up to ninety one percent is spent in the kernel, "
            "so tuning our parser or router can gain little. Bulk requests spend most of their time in our own "
            "code. That is where optimising pays.",
    "opt.intro": "So, what should we optimise? In order of expected gain.",
    "opt.0": "One. Write each response once, straight into the output buffer.",
    "opt.1": "Two. Replace the unordered map in the store with an open addressing table.",
    "opt.2": "Three. Lock once per shard, not once per item.",
    "opt.3": "Four. Faster jason output, scanning eight bytes at a time.",
    "opt.4": "Five. One system call per request at low load, by linking the send to the next receive.",
    "opt.5": "Six. A timed, batched wait, instead of waking for every completion.",
    "opt.bug": "And before any of that, fix the unicode escape bug in the jason reader. It silently corrupts "
               "stored text.",
    "keep.intro": "Three things we should not copy from the reference.",
    "keep.0": "One answer per connection per batch. It would cost us the five times speed up we get from pipelining.",
    "keep.1": "Holding replies for a group commit. We have no durable store to commit to.",
    "keep.2": "And the single thread, which is right for one virtual C P U, but wrong on a multi core machine.",
    "keep.end": "The details and the numbers are in the comparison document.",
}

SPOKEN = [("RECV", "receive"), ("SEND", "send"), ("serialize()", "serialize"), ("c.out", "the output buffer"),
          ("Res.body", "the response body buffer"), ("api.spec", "the A P I spec"), ("1 KB", "one kilobyte"),
          ("FIFO", "first in, first out"), ("SQLite", "S Q lite"), ("if-chain", "if chain")]


def narration():
    out = dict(NARR)
    for flow in (OURS_FLOW, REF_FLOW):
        for i, step in enumerate(flow["steps"]):
            text = step[0]
            for a, b in SPOKEN:
                text = text.replace(a, b)
            out["%s.%d" % (flow["key"], i)] = text
    return out


CLIPS = {}  # key -> seconds of narration; empty for a silent video
PAD = 0.55  # silence left after each clip


def plan(parts):
    """parts: [(key, minimum seconds)]. Returns (durations, start times, audio cues)."""
    durs = [max(m, CLIPS.get(k, 0.0) + PAD) for k, m in parts]
    starts = [sum(durs[:i]) for i in range(len(durs))]
    cues = [(k, st + 0.2) for (k, _), st in zip(parts, starts) if k in CLIPS]
    return durs, starts, cues


def segment(starts, t):
    idx = 0
    for i, st in enumerate(starts):
        if t >= st:
            idx = i
    return idx, t - starts[idx]


# ---------------------------------------------------------------- video scenes

def scene_flow(flow):
    key, steps = flow["key"], flow["steps"]
    parts = [(key + ".intro", 1.6)] + [("%s.%d" % (key, i), 2.4) for i in range(len(steps))] + [(key + ".outro", 3.0)]
    durs, starts, cues = plan(parts)

    def render(t):
        c = Canvas()
        header(c, flow["name"], flow["tag"], flow["color"])
        idx, local = segment(starts, t)
        k = idx - 1  # -1 intro, 0..n-1 steps, n outro
        in_step = 0 <= k < len(steps)
        move = ease(local / 0.75) if in_step else 0.0
        arrived = k + 1 if (in_step and move >= 1.0) else max(k, 0)
        arrived = min(arrived, len(steps))
        pos = draw_flow(c, flow, 118, 60, active=arrived, reached=arrived)
        counts, flash = [0, 0, 0], [0, 0, 0]
        for i in range(arrived):
            for j in range(3):
                counts[j] += steps[i][1 + j]
        if in_step and move >= 1.0:
            glow = max(0.0, 1.0 - (local - 0.75) / 0.9)
            flash = [glow if steps[k][1 + j] else 0 for j in range(3)]
        draw_tiles(c, flow, 412, 84, counts, flash)
        if in_step:
            (xa, ya), (xb, yb) = pos[k], pos[k + 1]
            if move < 1.0:
                px = xa + BW / 2 + (xb - xa) * move
                py = ya + BH / 2 + (yb - ya) * move
                c.dot(px, py, 13, mix(BG, flow["color"], 0.35))
                c.dot(px, py, 8, flow["color"])
            c.text(W / 2, 528, "Step %d of %d" % (k + 1, len(steps)), 12.5, flow["color"], bold=True, anchor="ma")
            c.wrapped(W / 2, 556, steps[k][0], 19, 1080)
        elif k < 0:
            c.wrapped(W / 2, 556, "Follow one request from the client's socket to the reply.", 19, 1080, MUTED)
        else:
            c.text(W / 2, 528, "Per request", 12.5, flow["color"], bold=True, anchor="ma")
            c.wrapped(W / 2, 556, "%d submissions, %d completions, %d copies of the response body, "
                      "and %s held per idle connection." % (counts[0], counts[1], counts[2], flow["idle"]), 19, 1080)
        return c
    return render, sum(durs), cues


def scene_title():
    durs, _, cues = plan([("title", 4.0)])

    def render(t):
        c = Canvas()
        c.text(W / 2, 236, "How one request flows", 46, TEXT, bold=True, anchor="ma")
        c.text(W / 2, 318, "bulkapi (ours)", 24, OURS, bold=True, anchor="ma")
        c.text(W / 2, 356, "compared with", 16, MUTED, anchor="ma")
        c.text(W / 2, 386, "c-iouring-pavlyhalim (reference)", 24, REF, bold=True, anchor="ma")
        c.text(W / 2, 470, "Two io_uring HTTP servers, two different goals", 17, MUTED, anchor="ma")
        return c
    return render, durs[0], cues


def scene_table():
    durs, _, cues = plan([("table", 9.0)])
    rows = [
        ("Submissions per request", "2", "2", None),
        ("Completions per request", "2", "3", 0),
        ("Copies of the response body", "3", "2", 1),
        ("Heap allocations on the request path", "yes", "none", 1),
        ("Memory per idle connection", "about 4 KB", "40 bytes", 1),
        ("Pipelined requests per pass", "all of them", "one", 0),
    ]

    def render(t):
        c = Canvas()
        header(c, "Side by side", "Cost of one request, and of one idle connection")
        x_lab, x_a, x_b, y0, rh = X0, 720, 990, 150, 66
        c.text(x_a, y0 - 34, "bulkapi", 15, OURS, bold=True, anchor="ma")
        c.text(x_b, y0 - 34, "reference", 15, REF, bold=True, anchor="ma")
        for i, (lab, a, b, win) in enumerate(rows):
            if t < 0.5 + i * 0.6:
                break
            y = y0 + i * rh
            c.rect(X0 - 16, y, W - 2 * X0 + 32, rh - 12, fill=PANEL, outline=None)
            c.text(x_lab, y + 27, lab, 17, TEXT, anchor="lm")
            c.text(x_a, y + 27, a, 20, GOOD if win == 0 else TEXT, bold=True, anchor="mm")
            c.text(x_b, y + 27, b, 20, GOOD if win == 1 else TEXT, bold=True, anchor="mm")
        c.wrapped(W / 2, 588, "Ours does less work per request. The reference spends an extra completion "
                  "to make an idle connection almost free. Green marks the cheaper side.", 17, 1080, MUTED)
        return c
    return render, durs[0], cues


def scene_idle():
    durs, _, cues = plan([("idle", 8.0)])

    def render(t):
        c = Canvas()
        header(c, "What idle connections cost", "Server memory for 100,000 idle connections (measured: 4.3 KB each after one request)")
        grow = ease((t - 0.6) / 1.8)
        x0, full = 330, 760
        for i, (name, col, val, frac) in enumerate([("bulkapi", OURS, "about 430 MB", 1.0), ("reference", REF, "4 MB", 0.0093)]):
            y = 200 + i * 130
            c.text(x0 - 24, y + 32, name, 19, col, bold=True, anchor="rm")
            c.rect(x0, y, full, 64, fill=PANEL, outline=None, r=6)
            wbar = max(3, full * frac * grow)
            c.rect(x0, y, wbar, 64, fill=col, outline=None, r=6 if wbar > 14 else 1)
            if grow > 0.95:
                inside = frac > 0.5
                c.text(x0 + (wbar - 16 if inside else wbar + 14), y + 32, val, 22, BG if inside else TEXT, bold=True,
                       anchor="rm" if inside else "lm")
        c.wrapped(W / 2, 480, "Ours gives every connection its own 16 KB receive buffer, of which about 4 KB stays in memory once "
                  "used. The reference keeps a 40-byte record and borrows a shared buffer only while reading.", 19, 1080)
        c.wrapped(W / 2, 580, "The kernel's own socket memory, about 4 KB per connection, comes on top for both.",
                  15, 1080, MUTED)
        return c
    return render, durs[0], cues


def scene_time():
    durs, _, cues = plan([("time", 10.0)])
    rows = [
        ("GET, no pipelining", 0.62, 6.31),
        ("GET, pipeline of 8", 0.38, 1.02),
        ("Bulk insert, 100 items", 13.9, 12.9),
        ("Bulk get, 100 items", 39.4, 15.9),
    ]

    def render(t):
        c = Canvas()
        header(c, "Where bulkapi's CPU time goes", "io_uring backend, one worker, CPU time per request")
        x0, full, y0, rh = 330, 560, 168, 84
        c.rect(x0, y0 - 44, 16, 16, fill=OURS, outline=None, r=3)
        c.text(x0 + 24, y0 - 36, "our code", 14, TEXT, anchor="lm")
        c.rect(x0 + 130, y0 - 44, 16, 16, fill=KERNEL, outline=None, r=3)
        c.text(x0 + 154, y0 - 36, "kernel", 14, TEXT, anchor="lm")
        for i, (lab, user, kern) in enumerate(rows):
            y = y0 + i * rh
            grow = ease((t - 0.5 - i * 0.5) / 1.0)
            share = user / (user + kern)
            c.text(x0 - 24, y + 26, lab, 16, TEXT, anchor="rm")
            wu = full * share * grow
            wk = full * (1 - share) * grow
            if wu > 2:
                c.rect(x0, y, wu, 52, fill=OURS, outline=None, r=4)
            if wk > 2:
                c.rect(x0 + wu + 2, y, wk, 52, fill=KERNEL, outline=None, r=4)
            if grow > 0.95:
                if full * share > 60:
                    c.text(x0 + 12, y + 26, "%d%%" % round(share * 100), 16, BG, bold=True, anchor="lm")
                fmt = "%.2f + %.2f µs" if user < 10 else "%.1f + %.1f µs"
                c.text(x0 + full + 20, y + 26, fmt % (user, kern), 15, MUTED, anchor="lm")
        c.wrapped(W / 2, 540, "Single requests are bound by the kernel: tuning our parser or router can gain little. "
                  "Bulk requests spend most of their time in our own code, so that is where optimising pays.", 19, 1080)
        return c
    return render, durs[0], cues


def scene_optimise():
    items = [
        ("Write each response once", "handlers append straight into c.out, no Res.body copy"),
        ("Replace the store's unordered_map", "open addressing: no allocation per item, no pointer chase"),
        ("Lock once per shard, not once per item", "group each bulk batch by shard"),
        ("Faster JSON output", "scan 8 bytes at a time; reserve the response size up front"),
        ("One syscall per request at low load", "link SEND to the next RECV and skip the send completion"),
        ("Timed batched wait", "wake for several completions or about 1 ms, not for every one"),
    ]
    parts = [("opt.intro", 0.5)] + [("opt.%d" % i, 0.9) for i in range(len(items))] + [("opt.bug", 5.0)]
    durs, starts, cues = plan(parts)

    def render(t):
        c = Canvas()
        header(c, "What to optimise in bulkapi", "In order of expected gain for a bulk-throughput server")
        idx, _ = segment(starts, t)
        for i, (a, b) in enumerate(items):
            if idx < i + 1:
                break
            y = 132 + i * 70
            now = idx == i + 1
            c.rect(X0 - 16, y, W - 2 * X0 + 32, 58, fill=mix(PANEL, OURS, 0.16) if now else PANEL, outline=None)
            c.text(X0 + 18, y + 29, str(i + 1), 22, OURS, bold=True, anchor="mm")
            c.text(X0 + 52, y + 19, a, 17, TEXT, bold=True, anchor="lm")
            c.text(X0 + 52, y + 41, b, 13.5, MUTED, anchor="lm")
        if idx == len(items) + 1 or not CLIPS:
            c.wrapped(W / 2, 580, "First of all, fix the \\u escape bug in the JSON reader: "
                      "it silently corrupts stored text.", 17, 1080, GOOD)
        return c
    return render, sum(durs), cues


def scene_keep():
    items = [
        ("One answer per connection per batch", "it would cost the 5x we get from pipelining"),
        ("Holding replies for a group commit", "we have no durable store to commit to"),
        ("A single thread", "right for 1 vCPU, wrong on a multi-core machine"),
    ]
    parts = [("keep.intro", 0.4)] + [("keep.%d" % i, 0.8) for i in range(len(items))] + [("keep.end", 4.2)]
    durs, starts, cues = plan(parts)

    def render(t):
        c = Canvas()
        header(c, "What not to copy from the reference", "Right for its goal, wrong for ours")
        idx, _ = segment(starts, t)
        for i, (a, b) in enumerate(items):
            if idx < i + 1:
                break
            y = 150 + i * 84
            c.rect(X0 - 16, y, W - 2 * X0 + 32, 68, fill=PANEL, outline=None)
            c.text(X0 + 8, y + 23, a, 18, TEXT, bold=True, anchor="lm")
            c.text(X0 + 8, y + 48, b, 14.5, MUTED, anchor="lm")
        c.text(W / 2, 500, "Details and numbers: COMPARISON.md and bulkapi/RESULTS.md", 17, MUTED, anchor="ma")
        return c
    return render, sum(durs), cues


FADE = 0.35
_BLANK = Image.new("RGB", (W, H), BG)
SCENES = []  # (render, seconds, cues); built by build() once CLIPS is known


def build():
    SCENES[:] = [scene_title(), scene_flow(OURS_FLOW), scene_flow(REF_FLOW), scene_table(), scene_idle(),
                 scene_time(), scene_optimise(), scene_keep()]


def frame_at(t):
    for fn, dur, _ in SCENES:
        if t < dur:
            img = fn(t).final()
            a = min(1.0, t / FADE, (dur - t) / FADE)
            return img if a >= 1.0 else Image.blend(_BLANK, img, max(0.0, a))
        t -= dur
    return _BLANK


def total_seconds():
    return sum(d for _, d, _ in SCENES)


# ---------------------------------------------------------------- audio

def load_clips(directory):
    """Reads the length of every narration clip. Returns the common sample rate."""
    rate = None
    for key in narration():
        path = os.path.join(directory, key + ".wav")
        if not os.path.exists(path):
            sys.exit("missing narration clip: " + path)
        with wave.open(path) as w:
            if w.getnchannels() != 1 or w.getsampwidth() != 2 or (rate and w.getframerate() != rate):
                sys.exit("narration clips must all be 16-bit mono at one sample rate: " + path)
            rate = w.getframerate()
            CLIPS[key] = w.getnframes() / rate
    return rate


def mix_narration(directory, rate, path):
    import numpy as np
    track = np.zeros(int(total_seconds() * rate) + rate, dtype=np.int32)
    base = 0.0
    for _, dur, cues in SCENES:
        for key, off in cues:
            with wave.open(os.path.join(directory, key + ".wav")) as w:
                data = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16)
            at = int((base + off) * rate)
            track[at:at + len(data)] += data
        base += dur
    track = np.clip(track, -32768, 32767).astype(np.int16)[:int(total_seconds() * rate)]
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(track.tobytes())


# ---------------------------------------------------------------- static diagram

def make_diagram(path):
    h = 930
    c = Canvas(W, h)
    c.text(X0, 24, "How one request flows", 28, TEXT, bold=True)
    c.text(X0, 64, "bulkapi (ours) compared with c-iouring-pavlyhalim (reference). Counts are per request.", 14, MUTED)
    for flow, y in ((OURS_FLOW, 108), (REF_FLOW, 520)):
        c.text(X0, y, flow["name"], 20, flow["color"], bold=True)
        c.text(X0, y + 30, flow["tag"], 12.5, MUTED)
        draw_flow(c, flow, y + 62, 46)
        totals = [sum(s[1 + j] for s in flow["steps"]) for j in range(3)]
        draw_tiles(c, flow, y + 62 + 2 * BH + 46 + 22, 62, totals)
    c.line(X0, 498, W - X0, 498, LINE, 1)
    c.final().save(path)


def _frame_bytes(i):
    return frame_at(i / FPS).tobytes()


def make_video(path, audio=None):
    n = int(total_seconds() * FPS)
    cmd = ["ffmpeg", "-y", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", "%dx%d" % (W, H),
           "-r", str(FPS), "-i", "-"]
    if audio:
        cmd += ["-i", audio, "-c:a", "aac", "-b:a", "96k"]
    cmd += ["-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "20", "-preset", "medium", "-movflags", "+faststart", path]
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE)
    # Frames are independent, so render them on every core (workers inherit SCENES via fork).
    with multiprocessing.Pool() as pool:
        for i, raw in enumerate(pool.imap(_frame_bytes, range(n), chunksize=8)):
            proc.stdin.write(raw)
            if i % (FPS * 20) == 0:
                print("  %3d / %d s" % (i // FPS, n // FPS), flush=True)
    proc.stdin.close()
    if proc.wait() != 0:
        sys.exit("ffmpeg failed")


if __name__ == "__main__":
    args = sys.argv[1:]
    if args == ["--narration"]:
        json.dump(narration(), sys.stdout, indent=1)
        sys.exit(0)
    clip_dir = rate = None
    if len(args) >= 2 and args[0] == "--audio":
        clip_dir, args = args[1], args[2:]
        rate = load_clips(clip_dir)
    build()
    if len(args) == 3 and args[0] == "--still":
        frame_at(float(args[1])).save(args[2])
        sys.exit(0)
    make_diagram(os.path.join(HERE, "request-flow.png"))
    print("wrote docs/request-flow.png")
    out = os.path.join(HERE, "explainer.mp4")
    if clip_dir:
        with tempfile.TemporaryDirectory() as tmp:
            track = os.path.join(tmp, "narration.wav")
            mix_narration(clip_dir, rate, track)
            make_video(out, track)
    else:
        make_video(out)
    print("wrote docs/explainer.mp4 (%.0f s%s)" % (total_seconds(), ", with narration" if clip_dir else ", silent"))
