"""
analyze_shift_latency.py — latence de bout en bout d'une session
capture_session.py (HID 1 ms avec valeurs Hall brutes + telemetrie AC).

Pour chaque changement de rapport :
  levier en grille (distance normalisee < ENGAGE au point calibre, mesuree
  sur les octets vendor du rapport HID, 1 ms) -> bouton HID -> nouveau
  rapport dans AC (AC passe par N a chaque changement : ignore).
Signale aussi :
  - levier tenu en grille sans bouton (rapport rate par le firmware) ;
  - bouton que le jeu n'a jamais pris (aucun changement AC correspondant).

Usage :
    python analyze_shift_latency.py <session> [<session> ...]
    (<session> = nom passe a capture_session.py, fichiers dans captures/)
Normalisation identique au firmware (gear_position_compute_normalization) ;
calibration lue dans <session>_cal.csv, sinon defauts.
"""
import csv
import math
import statistics
import sys
from pathlib import Path

CAPTURES = Path(__file__).resolve().parents[1] / "captures"
POSITIONS = ["neutral", "1", "2", "3", "4", "5", "6", "shift+", "shift-"]
DEFAULT_CAL = [
    2977, 3093, 2917, 2979, 2772, 2902, 3792, 3771, 2946, 3017, 2826, 2990,
    3012, 3109, 2980, 3771, 3375, 2938, 3183, 4095, 3146, 3024, 2800, 2949,
    3020, 3078, 2973, 3188, 4046, 3067, 3008, 3747, 3727, 3045, 2823, 2942,
    3062, 3103, 2942, 3008, 3538, 3664, 2970, 3016, 2923, 3082, 3321, 2940,
    3037, 3942, 2998, 2991, 2789, 2924,
]
ENGAGE = 0.35          # FAST_GEAR_ENGAGE_DIST
HELD_MS = 40           # levier tenu au moins ce temps en grille = intention de passer le rapport
GAME_WINDOW_S = 3.0    # bouton -> rapport AC au plus tard


def load_cal(session):
    path = CAPTURES / f"{session}_cal.csv"
    values = DEFAULT_CAL
    if path.exists():
        rows = list(csv.reader(path.open(encoding="utf-8")))
        if len(rows) > 1:
            values = [int(v) for v in rows[-1][2:]]
    pts = [values[i * 6:i * 6 + 6] for i in range(9)]
    neutral = pts[0]
    scale = [max(abs(p[i] - neutral[i]) for p in pts[1:]) or 1 for i in range(6)]
    return pts, neutral, scale


def norm_dist(h, p, neutral, scale):
    return math.sqrt(sum(((h[i] - neutral[i]) / scale[i] - (p[i] - neutral[i]) / scale[i]) ** 2
                         for i in range(6)))


def load_hid(session):
    rows = []
    for r in csv.DictReader((CAPTURES / f"{session}_hid.csv").open(encoding="utf-8")):
        b = r["axes_bytes"].split()
        if len(b) < 13:
            continue
        hall = [int(b[2 * i]) | (int(b[2 * i + 1]) << 8) for i in range(6)]
        buttons = set(r["buttons_pressed"].split()) if r["buttons_pressed"] else set()
        mode = "SEQ" if int(b[12]) & 1 else "H"
        rows.append((float(r["t_host_s"]), buttons, hall, mode))
    return rows


def load_ac(session):
    changes, last = [], None
    for r in csv.DictReader((CAPTURES / f"{session}_ac.csv").open(encoding="utf-8")):
        if r["gear"] != last:
            changes.append((float(r["t_host_s"]), r["gear"]))
            last = r["gear"]
    return changes


BUTTON_TO_POS = {"1": "1", "2": "2", "3": "3", "4": "4", "5": "5", "6": "6", "7": "shift+", "8": "shift-"}


def analyse(session):
    pts, neutral, scale = load_cal(session)
    hid = load_hid(session)
    ac = load_ac(session)
    print(f"\n===== {session}: {hid[-1][0] - hid[0][0]:.0f} s, {len(hid)} HID reports, "
          f"{len(ac) - 1} gear changes in AC")

    # Distances par echantillon aux points utiles
    dist = [{pos: norm_dist(h, pts[k], neutral, scale) for k, pos in enumerate(POSITIONS) if k}
            for _, _, h, _ in hid]

    # Fronts montants des boutons de rapport
    onsets, prev = [], set()
    for idx, (t, buttons, _, mode) in enumerate(hid):
        for b in sorted(buttons - prev):
            if b in BUTTON_TO_POS:
                onsets.append((idx, t, b, mode))
        prev = buttons

    # 1) Firmware : entree en grille -> bouton
    fw_lat = []
    for idx, t, b, mode in onsets:
        pos = BUTTON_TO_POS[b]
        k = idx
        while k > 0 and dist[k - 1][pos] < ENGAGE and t - hid[k - 1][0] < 2.0:
            k -= 1
        if dist[k][pos] < ENGAGE:
            fw_lat.append((t - hid[k][0]) * 1000)

    # 2) Jeu : bouton -> changement AC (rapport attendu pour H ; changement suivant pour SEQ)
    game_lat, not_taken, used_ac = [], [], set()
    for idx, t, b, mode in onsets:
        want = b if b in "123456" else None
        for j, (ta, g) in enumerate(ac[1:], 1):
            if ta < t - 0.05 or j in used_ac:
                continue
            if ta - t > GAME_WINDOW_S:
                break
            if g == "N":
                continue  # AC passe par N a chaque changement (temps de passage de la voiture)
            if want is None or g == want:
                game_lat.append(((ta - t) * 1000, t, b))
                used_ac.add(j)
                break
            if want is not None:
                break  # le jeu est passe a un autre rapport
        else:
            not_taken.append((t, b))
            continue
        if not game_lat or game_lat[-1][1] != t:
            not_taken.append((t, b))

    # 3) Rates firmware : levier tenu en grille sans bouton
    missed = []
    for pos_btn, pos in BUTTON_TO_POS.items():
        start = None
        for idx, (t, buttons, _, mode) in enumerate(hid):
            ok_mode = (mode == "H") == (pos_btn in "123456")
            inside = ok_mode and dist[idx][pos] < ENGAGE * 0.7
            if inside and start is None:
                start, seen = idx, False
            if start is not None:
                seen = seen or pos_btn in buttons or (pos_btn == "1" and "11" in buttons)
                if not inside:
                    if (t - hid[start][0]) * 1000 >= HELD_MS and not seen:
                        missed.append((hid[start][0], pos, (t - hid[start][0]) * 1000))
                    start = None

    # 4) Changements AC sans bouton juste avant (autre source)
    orphan = [ (ta, g) for j, (ta, g) in enumerate(ac[1:], 1) if j not in used_ac and g != "N"]

    def stats(xs):
        xs = sorted(xs)
        if not xs:
            return "none"
        p90 = xs[min(len(xs) - 1, int(0.9 * len(xs)))]
        return f"n={len(xs)} median {statistics.median(xs):.0f} ms, p90 {p90:.0f} ms, max {xs[-1]:.0f} ms"

    print(f"buttons sent: {len(onsets)} "
          f"({sum(1 for o in onsets if o[2] in '123456')} gears, "
          f"{sum(1 for o in onsets if o[2] in '78')} shift pulses)")
    print(f"lever in gate -> button (firmware): {stats(fw_lat)}")
    print(f"button -> gear in AC (game):        {stats([g[0] for g in game_lat])}")
    end_to_end = []
    for (lag, t, b) in game_lat:
        # retrouve la latence firmware de ce meme bouton
        for idx, t2, b2, _ in onsets:
            if t2 == t and b2 == b:
                k = idx
                pos = BUTTON_TO_POS[b]
                while k > 0 and dist[k - 1][pos] < ENGAGE and t - hid[k - 1][0] < 2.0:
                    k -= 1
                end_to_end.append(lag + (t - hid[k][0]) * 1000)
    print(f"lever in gate -> gear in AC (felt): {stats(end_to_end)}")
    slow = sorted(game_lat, reverse=True)[:5]
    print("slowest button -> AC:", ", ".join(f"{b}@{t:.1f}s {lag:.0f}ms" for lag, t, b in slow))
    print(f"buttons the game did not take: {len(not_taken)}",
          " ".join(f"{b}@{t:.1f}s" for t, b in not_taken[:10]))
    print(f"lever held in a gate >= {HELD_MS} ms with no button (firmware misses): {len(missed)}",
          " ".join(f"{p}@{t:.1f}s({d:.0f}ms)" for t, p, d in missed[:10]))
    print(f"AC gear changes not explained by a button: {len(orphan)}",
          " ".join(f"{g}@{t:.1f}s" for t, g in orphan[:10]))


if __name__ == "__main__":
    for s in sys.argv[1:] or ["sync_assetto4", "sync_assetto5"]:
        analyse(s)
