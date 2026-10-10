"""plot_fast_path.py — avant/apres du chemin rapide (fast_gear) sur captures reelles.

Rejoue des captures CDC existantes a travers le firmware corrige
(Firmware/App/test/replay_fast_path.c, compile a la volee avec gcc) et trace,
pour une selection d'evenements, la position du levier et les boutons HID
recus par le jeu :
  - avant correctif : chemin lent seul (firmware precedent), rejoue ;
  - apres correctif : chemin lent + chemin rapide, rejoue sur les memes
    valeurs Hall (boucle 1ms, Hall interpole lineairement entre les
    echantillons 100ms de la capture).
Les deux voies sortent du meme rejeu : certaines captures ont ete
enregistrees avec un firmware encore plus ancien (ex. test_Seq : shifts
sequentiels maintenus jamais envoyes, bug corrige le 2026-09-13), les
comparer a l'enregistrement surestimerait le gain. Sur les captures denses
(sync_assetto2/3), le rejeu "avant" reproduit le HID enregistre a 99.7-99.9%.

Les rejeux sont ecrits dans captures/fast_path_replay/ ; le graphique
dans captures/fast_path_before_after.png (copie publiee :
docs/images/). La plupart des captures utilisees sont dans l'archive de
developpement, pas dans ce depot.

Usage (depuis la racine du depot) : .venv/Scripts/python.exe tools/plot_fast_path.py
"""
import csv
import math
import re
import subprocess
import tempfile
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
from matplotlib.patches import Rectangle  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
CAPTURES = ROOT / "captures"
REPLAY_DIR = CAPTURES / "fast_path_replay"
FW_APP = ROOT / "Firmware" / "App"
OUT_PNG = CAPTURES / "fast_path_before_after.png"

# Captures dont l'ancien chemin rejoue reproduit l'enregistrement HID (>=95%,
# meme calibration que le device au moment de la capture) — seules
# utilisees pour les statistiques. test_H_pattern4/6/7, test_H_pattern et
# test1 ont ete enregistrees avec une autre calibration/firmware : exclues.
STATS_CAPTURES = [
    "sync_assetto1", "sync_assetto2", "sync_assetto3", "rapid_shift_test2",
    "test_Benrapid_shift_test", "test_Seq", "test_H_pattern2", "test_H_pattern3",
    "test_H_pattern5", "test_H_pattern8", "test_H_pattern9",
]

# (capture, t_ms approx. de l'arrivee du levier (0 cale sur le debut de bande), fenetre avant/apres en ms, titre, sous-titre)
EXAMPLES = [
    ("sync_assetto2", 300310, (350, 1000), "H-pattern · shift arrives late",
     "Assetto Corsa session"),
    ("rapid_shift_test2", 284600, (500, 900), "H-pattern · 6th → 5th never sent",
     "Direct 6→5 without a neutral pause"),
    ("test_Benrapid_shift_test", 142170, (300, 900), "H-pattern · 3rd never sent",
     "4 → 3 → 4, 3rd held ~300 ms"),
    ("test_Seq", 91740, (300, 1700), "Sequential · shift − sent on release",
     "Three quick downshifts"),
    ("test_Seq", 96090, (300, 1900), "Sequential · lever held at the stop",
     "Downshift, lever kept pushed"),
    ("test_H_pattern2", 85960, (300, 900), "Sequential · deep shift − never sent",
     "Previous firmware read it as 3rd and dropped it"),
]

ENGAGE = 0.35  # = FAST_GEAR_ENGAGE_DIST (fast_gear.h)

# Couleurs (palette de reference dataviz, validee light : bleu/orange).
C_NEW = "#2a78d6"
C_OLD = "#eb6834"
INK = "#0b0b0b"
INK2 = "#52514e"
MUTED = "#8a8983"
GRID = "#e4e2dc"
SURFACE = "#fcfcfb"
BAND = "#ecebe6"

POS_NAMES = ["neutral", "gear1", "gear2", "gear3", "gear4", "gear5", "gear6", "seq_up", "seq_down"]
POS_LABEL = {"gear1": "1st", "gear2": "2nd", "gear3": "3rd", "gear4": "4th", "gear5": "5th",
             "gear6": "6th", "seq_up": "seq +", "seq_down": "seq −"}
BUTTON_LABEL = {"1": "1st", "2": "2nd", "3": "3rd", "4": "4th", "5": "5th", "6": "6th",
                "7": "shift +", "8": "shift −", "11": "R"}


# ---------------------------------------------------------------- calibration
def load_default_calibration():
    """Lit DEFAULT_CALIBRATION dans calibration_store.c (source unique)."""
    src = (FW_APP / "Src" / "calibration_store.c").read_text(encoding="utf-8")
    pos = {}
    for name, vals in re.findall(r"\[GEAR_(\w+)\]\s*=\s*\{\{([\d,\s]+)\}\}", src):
        pos[name.lower()] = [int(v) for v in vals.split(",")]
    order = {"neutral": "neutral", "1": "gear1", "2": "gear2", "3": "gear3", "4": "gear4",
             "5": "gear5", "6": "gear6", "seq_up": "seq_up", "seq_down": "seq_down"}
    points = {order[k]: v for k, v in pos.items()}
    neutral = points["neutral"]
    scale = [max(abs(points[p][i] - neutral[i]) for p in points if p != "neutral") or 1 for i in range(6)]
    norm_points = {p: [(v[i] - neutral[i]) / scale[i] for i in range(6)] for p, v in points.items()}
    return neutral, scale, norm_points


NEUTRAL, SCALE, NORM_POINTS = load_default_calibration()


def distances(hall):
    n = [(hall[i] - NEUTRAL[i]) / SCALE[i] for i in range(6)]
    return {p: math.sqrt(sum((n[i] - ref[i]) ** 2 for i in range(6))) for p, ref in NORM_POINTS.items()}


# ---------------------------------------------------------------- replay
def build_and_replay(names):
    REPLAY_DIR.mkdir(exist_ok=True)
    test = FW_APP / "test"
    with tempfile.TemporaryDirectory() as tmp:
        exe = Path(tmp) / "replay_fast_path.exe"
        srcs = ["replay_fast_path.c", "../Src/fast_gear.c", "../Src/gear_position.c", "../Src/calibration_store.c",
                "../Src/mode_state_machine.c", "../Src/hid_report.c", "../Src/calibration_flash.c",
                "hal_stub/hal_flash_stub.c"]
        subprocess.run(["gcc", "-std=c99", "-O2", "-DCALIBRATION_FLASH_READ_OVERRIDE", "-I", "../Inc", "-I",
                        "hal_stub", *srcs, "-lm", "-o", str(exe)], cwd=test, check=True)
        for name in names:
            subprocess.run([str(exe), str(CAPTURES / f"{name}_cdc.csv"), str(REPLAY_DIR / f"{name}_replay.csv")],
                           check=True)


def read_csv(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def gear_buttons(s):
    return [b for b in (s or "").split() if int(b) <= 8 or b == "11"]


def segments(times, labels):
    """[(t_start, t_end, label)] des intervalles ou label est non vide."""
    out, cur, start = [], None, None
    for t, lab in zip(times, labels):
        if lab != cur:
            if cur:
                out.append((start, t, cur))
            cur, start = lab, t
    if cur:
        out.append((start, times[-1], cur))
    return out


# ---------------------------------------------------------------- stats
def press_onsets(rows, col):
    out, prev = [], ""
    for r in rows:
        b = gear_buttons(r[col])
        u = b[0] if b else ""
        if u and u != prev:
            out.append((int(r["t_ms"]), u))
        prev = u
    return out


def compute_stats():
    gains, dropped, total_old = [], 0, 0
    for name in STATS_CAPTURES:
        rows = read_csv(REPLAY_DIR / f"{name}_replay.csv")
        old, new = press_onsets(rows, "slow_buttons"), press_onsets(rows, "new_buttons")
        total_old += len(old)
        used = set()
        for t, u in new:
            m = [i for i, (to, uo) in enumerate(old) if uo == u and i not in used and -150 <= to - t <= 1500]
            if m:
                used.add(m[0])
                gains.append(old[m[0]][0] - t)
            else:
                dropped += 1
    gains.sort()
    return {"matched": len(gains), "old": total_old, "median": gains[len(gains) // 2],
            "p90": gains[int(len(gains) * 0.9)], "dropped": dropped}


# ---------------------------------------------------------------- plot
def plot_example(ax_lever, ax_btn, name, t_approx, window, title, subtitle, first_col):
    cdc = read_csv(CAPTURES / f"{name}_cdc.csv")
    rep = read_csv(REPLAY_DIR / f"{name}_replay.csv")
    t_lo, t_hi = t_approx - window[0] - 400, t_approx + window[1] + 400
    rep_w = [r for r in rep if t_lo <= int(r["t_ms"]) <= t_hi]
    cdc_w = [r for r in cdc if t_lo <= int(r["t_ms"]) <= t_hi]

    # Levier : distance au neutre et position engagee, sur le Hall interpole
    # 1ms (exactement ce que voit le chemin rapide dans le rejeu).
    samples = [(int(r["t_ms"]), [int(r[k]) for k in ("u2", "u3", "u4", "u5", "u6", "u7")], r["mode"]) for r in cdc_w]
    mode = samples[0][2]
    allowed = [p for p in POS_NAMES[1:] if p.startswith("seq") == (mode == "SEQ")]
    t_line, d_line, pos_line = [], [], []
    for (ta, ha, _), (tb, hb, _) in zip(samples, samples[1:]):
        for t in range(ta, tb):
            f = (t - ta) / (tb - ta)
            d = distances([ha[i] + (hb[i] - ha[i]) * f for i in range(6)])
            best = min(allowed, key=d.get)
            t_line.append(t)
            d_line.append(d["neutral"])
            pos_line.append(best if d[best] < ENGAGE and d["neutral"] > ENGAGE else "")
    bands_abs = [(s, e, p) for s, e, p in segments(t_line, pos_line) if e - s > 30]

    # 0 = arrivee du levier dans la premiere bande qui suit t_approx
    t_ref = min((s for s, _, _ in bands_abs if s >= t_approx - 250), default=t_approx)
    x_lo, x_hi = -window[0], window[1]
    rel = [t - t_ref for t in t_line]
    bands = [(s - t_ref, e - t_ref, p) for s, e, p in bands_abs if e - t_ref > x_lo and s - t_ref < x_hi]

    for s, e, p in bands:
        ax_lever.axvspan(s, e, color=BAND, lw=0, zorder=0)
        vs, ve = max(s, x_lo), min(e, x_hi)
        if ve - vs > 0.12 * (x_hi - x_lo):
            ax_lever.text((vs + ve) / 2, 1.33, f"lever in {POS_LABEL[p]}", ha="center", va="top", fontsize=8.5,
                          color=INK2)
    ax_lever.plot(rel, d_line, color=INK, lw=1.4, zorder=2)
    ax_lever.plot([t - t_ref for t, _, _ in samples], [distances(h)["neutral"] for _, h, _ in samples],
                  "o", ms=3.2, color=INK, zorder=3)

    # Voies boutons (meme rejeu pour les deux)
    times = [int(r["t_ms"]) - t_ref for r in rep_w]
    old_segs = segments(times, [" ".join(gear_buttons(r["slow_buttons"])) for r in rep_w])
    new_segs = segments(times, [" ".join(gear_buttons(r["new_buttons"])) for r in rep_w])
    y_old, y_new = 1.0, 0.0

    def draw(segs, y, color):
        for s, e, lab in segs:
            s, e = max(s, x_lo), min(e, x_hi)
            if e <= s:
                continue
            ax_btn.add_patch(Rectangle((s, y - 0.32), e - s, 0.64, color=color, lw=0, zorder=2))
            if e - s > 0.09 * (x_hi - x_lo):
                ax_btn.text((s + e) / 2, y, BUTTON_LABEL.get(lab.split()[0], lab), ha="center", va="center",
                            fontsize=8, color="white", fontweight="bold", zorder=3)

    draw(old_segs, y_old, C_OLD)
    draw(new_segs, y_new, C_NEW)

    # Pour chaque arrivee du levier dans la fenetre : retard avant/apres
    want_btn = {"gear1": "1", "gear2": "2", "gear3": "3", "gear4": "4", "gear5": "5", "gear6": "6",
                "seq_up": "7", "seq_down": "8"}
    for k, (s, e, p) in enumerate(bands):
        if s <= x_lo:
            continue  # levier deja en place au debut de la fenetre
        nxt = bands[k + 1][0] if k + 1 < len(bands) else s + 1500
        want = want_btn[p]
        old_on = [os_ for os_, _, lab in old_segs if want in lab.split() and s - 50 <= os_ < nxt]
        new_on = [ns for ns, _, lab in new_segs if want in lab.split() and s - 50 <= ns <= e + 50]
        if new_on:
            ax_btn.annotate(f"+{new_on[0] - s:.0f} ms", (new_on[0], y_new - 0.42), fontsize=7.5, color=C_NEW,
                            ha="left", va="top", xytext=(1, 0), textcoords="offset points")
        if old_on:
            ax_btn.add_patch(Rectangle((s, y_old - 0.32), old_on[0] - s, 0.64, facecolor="none", edgecolor=C_OLD,
                                       hatch="////", lw=0, zorder=1, alpha=0.6))
            near_edge = old_on[0] > x_hi - 0.1 * (x_hi - x_lo)
            ax_btn.annotate(f"+{old_on[0] - s:.0f} ms", (old_on[0], y_old + 0.42), fontsize=7.5, color=INK2,
                            ha="right" if near_edge else "left", va="bottom", xytext=(-2 if near_edge else 1, 0),
                            textcoords="offset points")
        elif new_on:
            ve = min(e, x_hi)
            # Bouton d'un autre rapport encore affiche par le chemin lent ?
            stale = [lab for os_, oe, lab in old_segs if os_ <= s and oe >= ve and lab]
            never = f"still {BUTTON_LABEL[stale[0].split()[0]]}: {POS_LABEL[p]} never sent" if stale else "never sent"
            ax_btn.add_patch(Rectangle((s, y_old - 0.32), ve - s, 0.64, facecolor="none", edgecolor=C_OLD,
                                       hatch="////", lw=0, zorder=1, alpha=0.6))
            ax_btn.text(s + (ve - s) / 2, y_old + (0.55 if stale else 0), never, ha="center", va="center", fontsize=8.5,
                        color=C_OLD, fontweight="bold", fontstyle="italic", zorder=3,
                        bbox=dict(facecolor=SURFACE, edgecolor="none", pad=1.5))

    # Axes
    for ax in (ax_lever, ax_btn):
        ax.set_xlim(x_lo, x_hi)
        ax.set_facecolor(SURFACE)
        for side in ("top", "right"):
            ax.spines[side].set_visible(False)
        ax.spines["left"].set_color(GRID)
        ax.spines["bottom"].set_color(GRID)
        ax.tick_params(colors=MUTED, labelsize=8, length=0)
    ax_lever.set_ylim(-0.02, 1.38)
    ax_lever.set_yticks([0, 0.5, 1.0])
    ax_lever.grid(axis="y", color=GRID, lw=0.8)
    ax_lever.axhline(0.25, color=MUTED, lw=0.8, ls=(0, (2, 3)), zorder=1)
    ax_lever.tick_params(labelbottom=False)
    ax_lever.set_title(title, loc="left", fontsize=11, fontweight="bold", color=INK, pad=16)
    ax_lever.text(0, 1.02, subtitle, transform=ax_lever.transAxes, fontsize=8.5, color=INK2, va="bottom")
    ax_btn.set_ylim(-0.75, 1.75)
    ax_btn.set_yticks([y_old, y_new])
    ax_btn.set_xlabel("ms (0 = lever reaches the position)", fontsize=8, color=MUTED)
    if first_col:
        ax_btn.set_yticklabels(["before fix", "after fix"], fontsize=8.5, fontweight="bold")
        ax_btn.get_yticklabels()[0].set_color(C_OLD)
        ax_btn.get_yticklabels()[1].set_color(C_NEW)
        ax_lever.set_ylabel("lever distance\nfrom neutral", fontsize=8, color=INK2)
        ax_lever.text(x_lo + 8, 0.27, "neutral zone", fontsize=7, color=MUTED, va="bottom")
    else:
        ax_btn.set_yticklabels([])


def main():
    names = sorted(set(STATS_CAPTURES) | {e[0] for e in EXAMPLES})
    build_and_replay(names)
    stats = compute_stats()

    fig = plt.figure(figsize=(18, 11.6), facecolor=SURFACE)
    outer = fig.add_gridspec(2, 3, hspace=0.3, wspace=0.08, left=0.055, right=0.99, top=0.85, bottom=0.05)
    for k, (name, t_approx, window, title, subtitle) in enumerate(EXAMPLES):
        col = k % 3
        inner = outer[k // 3, col].subgridspec(2, 1, height_ratios=[2.2, 1.0], hspace=0.06)
        ax_l = fig.add_subplot(inner[0])
        ax_b = fig.add_subplot(inner[1], sharex=ax_l)
        plot_example(ax_l, ax_b, name, t_approx, window, title, subtitle, col == 0)

    fig.text(0.055, 0.965, "GX100 shifter: when the game gets the shift, before vs after the fix",
             fontsize=16, fontweight="bold", color=INK)
    fig.text(0.055, 0.935,
             f"Across {len(STATS_CAPTURES)} real captures: all {stats['old']} shifts the previous firmware sent now "
             f"arrive {stats['median']} ms earlier (median; 90th pct {stats['p90']} ms), and {stats['dropped']} "
             "shifts it never sent now reach the game.",
             fontsize=10.5, color=INK2)
    fig.text(0.055, 0.912,
             "Grey band = lever sitting in a gear / at the sequential stop.  Hatched = lever already there, game "
             "still has no button.  Both lanes replay the same captured Hall data (dots = 100 ms samples); the "
             "after-fix path runs every 1 ms on Hall interpolated between samples.",
             fontsize=9, color=MUTED)
    fig.savefig(OUT_PNG, dpi=120, facecolor=SURFACE)
    print(f"ecrit : {OUT_PNG}")
    print(stats)


if __name__ == "__main__":
    main()
