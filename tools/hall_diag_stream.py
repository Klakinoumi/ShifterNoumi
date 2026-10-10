import serial
import sys
import time
import re
from pathlib import Path

import matplotlib.pyplot as plt

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM9"
LOG_NAME = sys.argv[2] if len(sys.argv) > 2 else "hall_diag_log.txt"
BAUD = 115200  # ignore avec USB CDC, mais requis par pyserial

CAPTURES_DIR = Path(__file__).parent.parent / "captures"
LOG_PATH = CAPTURES_DIR / LOG_NAME
PLOT_PATH = LOG_PATH.with_suffix(".png")

LINE_RE = re.compile(
    r"T\s+(\d+)\s+HALL\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+COLLAR\s+(\d)"
)


def capture(port):
    try:
        ser = serial.Serial(port, BAUD, timeout=1)
    except serial.SerialException as e:
        print(f"Impossible d'ouvrir {port}: {e}")
        sys.exit(1)

    print(f"Connecte a {port}. Lecture en continu (Ctrl+C pour arreter).")
    print("Format attendu: T <ms> HALL <U2> <U3> <U4> <U5> <U6> <U7> COLLAR <0|1>")
    print(f"Log ecrit dans: {LOG_PATH}")
    print()

    rows = []
    t0_ms = None
    try:
        with open(LOG_PATH, "w", encoding="utf-8") as logf:
            while True:
                line = ser.readline().decode("ascii", errors="replace").strip()
                if not line:
                    continue
                ts_wall = time.strftime("%H:%M:%S")
                print(f"[{ts_wall}] {line}")
                logf.write(f"[{ts_wall}] {line}\n")
                logf.flush()

                m = LINE_RE.search(line)
                if m:
                    ms, *values = (int(x) for x in m.groups())
                    if t0_ms is None:
                        t0_ms = ms
                    t = (ms - t0_ms) / 1000.0
                    rows.append((t, *values))
    except KeyboardInterrupt:
        print("\nArret demande.")
    finally:
        ser.close()

    return rows


def load_from_log(path):
    raw = path.read_bytes()
    # gere UTF-16 (avec BOM) ou UTF-8/ascii
    if raw.startswith(b"\xff\xfe") or raw.startswith(b"\xfe\xff"):
        text = raw.decode("utf-16")
    else:
        text = raw.decode("utf-8", errors="replace")

    rows = []
    t0_ms = None
    for line in text.splitlines():
        m = LINE_RE.search(line)
        if not m:
            continue
        ms, *values = (int(x) for x in m.groups())
        if t0_ms is None:
            t0_ms = ms
        t = (ms - t0_ms) / 1000.0
        rows.append((t, *values))
    return rows


def plot(rows, out_path):
    if not rows:
        print("Aucune donnee exploitable a tracer.")
        return

    times = [r[0] for r in rows]
    labels = ["U2 (PA0)", "U3 (PA1)", "U4 (PA2)", "U5 (PA3)", "U6 (PA4)", "U7 (PA5)"]

    fig, (ax_hall, ax_collar) = plt.subplots(
        2, 1, figsize=(12, 7), sharex=True, height_ratios=[3, 1]
    )

    for i, label in enumerate(labels):
        series = [r[1 + i] for r in rows]
        ax_hall.plot(times, series, label=label, linewidth=1)

    ax_hall.set_ylabel("Valeur ADC (0-4095)")
    ax_hall.set_title("Capteurs Hall en fonction du temps")
    ax_hall.legend(loc="upper right", fontsize=8)
    ax_hall.grid(True, alpha=0.3)

    collar = [r[7] for r in rows]
    ax_collar.step(times, collar, where="post", color="black")
    ax_collar.set_ylabel("Collerette")
    ax_collar.set_xlabel("Temps ecoule (s)")
    ax_collar.set_yticks([0, 1])
    ax_collar.grid(True, alpha=0.3)

    fig.tight_layout()
    fig.savefig(out_path, dpi=150)
    print(f"Graphique enregistre: {out_path}")


def main():
    rows = capture(PORT)
    if not rows:
        print("Pas de lignes capturees en direct, tentative de relecture du log ecrit.")
        rows = load_from_log(LOG_PATH)
    plot(rows, PLOT_PATH)


if __name__ == "__main__":
    main()
