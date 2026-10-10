"""
capture_session.py — capture combinee HID (inputs controleur, comme un vrai
gamepad) + CDC (donnees Hall brutes / etat interne, cf Firmware/App/Src/
debug_trace.c) + telemetrie Assetto Corsa (memoire partagee, rapport de
vitesse reellement percu par le jeu) du firmware GX100 composite HID+CDC.

Ajoute en Phase 6 (session debug "shift rapide 3<->4") pour repondre a la
question : un mouvement de levier rapide est-il perdu par le firmware, ou
bien le firmware envoie bien le bouton mais AC ne l'enregistre pas ? Les
sessions bancs precedentes (rapid_shift_test/2) ont montre 0 perte cote
firmware->HID a la cadence la plus rapide qu'une main peut produire — la
question restante ne peut se trancher qu'en comparant avec ce que le jeu
voit reellement, d'ou l'ajout de ce troisieme flux plutot que de construire
un outil separe (evite tout risque de desynchronisation d'horloge entre
deux processus, cf PLAN.md).

Usage :
    python capture_session.py <PORT_COM> [nom_session]

    <PORT_COM> : port serie du GX100 (ex. COM7), voir Gestionnaire de
    peripheriques > Ports (COM & LPT) > "GX100 Debug" une fois le firmware
    composite flashe. Le device HID gamepad est trouve automatiquement par
    VID/PID (pas besoin de le preciser).

    La telemetrie AC est optionnelle : si Assetto Corsa (original, pas EVO)
    n'est pas lance ou n'a pas encore cree son segment de memoire partagee,
    la capture continue normalement en HID+CDC seul (nouvelle tentative
    d'ouverture a chaque pas de boucle, donc lancer AC apres le script reste
    possible).

Sortie (dans captures/, meme dossier que les logs Phase 4) :
    <nom_session>_hid.csv   : chaque changement de rapport HID, avec
                              timestamp hote (perf_counter, secondes depuis
                              le debut de la capture) + boutons actifs.
    <nom_session>_cdc.csv   : chaque ligne CSV recue du firmware (Hall bruts
                              + etat interne, cf debug_trace.h pour les
                              colonnes), timestamp hote en plus du t_ms
                              firmware.
    <nom_session>_cal.csv   : calibration active du boitier (ligne CAL du
                              firmware, toutes les 2 s et a chaque point
                              calibre) — replay_fast_path la reprend
                              automatiquement.
    <nom_session>_ac.csv    : chaque changement de rapport engage tel que lu
                              par AC (memoire partagee acpmf_physics), avec
                              le meme timestamp hote que les deux autres —
                              permet de voir si une impulsion HID a bien un
                              changement de rapport AC correspondant.
    <nom_session>_pedals.csv : gas/brake/clutch AC (memoire partagee), a
                              chaque pas de boucle (pas seulement sur
                              changement — on veut la trace continue de la
                              pedale d'embrayage pour la correler avec les
                              latences de rapport, cf session "j'ai du
                              re-embrayer plusieurs fois", PLAN.md).
    <nom_session>.png       : graphique combine (valeurs Hall + etat tracker
                              en fond, marqueurs verticaux aux boutons HID
                              actives, rapport AC en escalier, pedales AC)
                              pour visualiser en un coup d'oeil pourquoi un
                              bouton ne se declenche pas, pourquoi la
                              bascule de mode n'a pas lieu, ou si AC a
                              ignore une impulsion pourtant envoyee — et si
                              la pedale d'embrayage explique la latence.

Ctrl+C pour arreter la capture et generer le graphique.
"""
import csv
import ctypes
import ctypes.wintypes
import mmap
import re
import struct
import sys
import time
from pathlib import Path

import hid
import serial
import matplotlib.pyplot as plt

VID = 0x0483
PID = 0x572C  # GX100, cf Firmware/USB_DEVICE/App/usbd_desc.c (USBD_PID_FS)

BAUD = 115200  # ignore avec USB CDC, requis par pyserial

CAPTURES_DIR = Path(__file__).parent.parent / "captures"

CDC_HEADER = [
    "t_ms", "u2", "u3", "u4", "u5", "u6", "u7", "collar",
    "tracker_state", "mode", "report_debug",
    "emit_pos", "emit_kind", "switch_target",
    "gear6_active", "gear6_blips", "seqdown_window", "seqdown_count", "seqdown_blips",
    "calib_active", "calib_step", "calib_target",
    "fast_pos", "buttons",
]
# Firmware anterieur a l'ajout de fast_pos/buttons (sept. 2026) : lignes
# acceptees, colonnes manquantes laissees vides.
CDC_HEADER_LEGACY_LEN = len(CDC_HEADER) - 2

# Ligne "CAL,t_ms,<54 valeurs>" (debug_trace_calibration_poll) : calibration
# active, 9 positions x U2..U7 dans l'ordre de gear_position_t.
CAL_POSITIONS = ["neutral", "gear1", "gear2", "gear3", "gear4", "gear5", "gear6", "seq_up", "seq_down"]
CAL_HEADER = ["t_ms"] + [f"{p}_u{i}" for p in CAL_POSITIONS for i in range(2, 8)]

# --- Telemetrie Assetto Corsa (memoire partagee, AC original/AC1 pas EVO) ---
# Struct SPageFilePhysics : packetId u32 @0, gas f32 @4, brake f32 @8,
# fuel f32 @12, gear u32 @16. Convention AC1 du champ gear : 0=marche
# arriere, 1=neutre, 2=1ere, 3=2eme, etc (decale de +2 par rapport au
# rapport reellement engage, sauf neutre/marche arriere) — cf
# gear_to_label() ci-dessous pour la conversion.
#
# GX100 (session "j'ai du re-embrayer plusieurs fois") : gear seul ne
# suffit plus a expliquer la latence AC observee (jusqu'a 1.8s alors que
# le bouton HID est tenu net et continu, cf sync_assetto2 + PLAN.md) — il
# faut correler avec ce que fait la pedale d'embrayage au moment du
# changement de rapport signale. Le champ clutch n'existe pas dans le
# prefixe ci-dessus : ajoute a AC 1.10, bien plus loin dans la struct
# (verifie via Physics.cs du projet mdjarv/assettocorsasharedmemory,
# cf PLAN.md pour le detail des offsets recalcules). 1.0 = pedale
# relachee/embrayage ferme, 0.0 = pedale au plancher/embrayage ouvert.
AC_SHM_NAME = "acpmf_physics"
AC_GEAR_OFFSET = 16
AC_GEAR_SIZE = 4  # uint32
AC_GAS_OFFSET = 4
AC_BRAKE_OFFSET = 8
AC_CLUTCH_OFFSET = 364
AC_PEDAL_SIZE = 4  # float32
AC_SHM_MAP_SIZE = AC_CLUTCH_OFFSET + AC_PEDAL_SIZE  # couvre tous les champs lus

# GX100 : mmap.mmap(-1, size, tagname=...) sur Windows utilise
# CreateFileMapping en interne, qui CREE un nouveau segment vierge (rempli
# de zeros) si le tag n'existe pas encore au lieu d'echouer — verifie
# concretement dans cet environnement (AC non lance -> read_ac_gear()
# renvoyait silencieusement (0, "R") en continu, indiscernable d'une vraie
# session AC dont la voiture serait en marche arriere). Il faut donc
# d'abord verifier l'EXISTENCE du mapping via OpenFileMappingW (semantique
# "ouvre seulement, echoue si absent"), puis seulement ensuite utiliser
# mmap.mmap pour la lecture — cf commentaire Win32 OpenFileMapping vs
# CreateFileMapping (l'un cree si absent, l'autre echoue si absent).
_FILE_MAP_READ = 0x0004
_kernel32 = ctypes.windll.kernel32
_OpenFileMappingW = _kernel32.OpenFileMappingW
_OpenFileMappingW.argtypes = [ctypes.wintypes.DWORD, ctypes.wintypes.BOOL, ctypes.wintypes.LPCWSTR]
_OpenFileMappingW.restype = ctypes.wintypes.HANDLE
_CloseHandle = _kernel32.CloseHandle
_CloseHandle.argtypes = [ctypes.wintypes.HANDLE]
_CloseHandle.restype = ctypes.wintypes.BOOL


def _ac_shared_memory_exists():
    handle = _OpenFileMappingW(_FILE_MAP_READ, False, AC_SHM_NAME)
    if not handle:
        return False
    _CloseHandle(handle)
    return True


def open_ac_shared_memory():
    """Tente d'ouvrir le segment de memoire partagee AC. Retourne None si AC
    n'est pas lance (segment inexistant) — pas une erreur fatale, la capture
    HID+CDC reste valide sans telemetrie AC."""
    if not _ac_shared_memory_exists():
        return None
    try:
        return mmap.mmap(-1, AC_SHM_MAP_SIZE, tagname=AC_SHM_NAME, access=mmap.ACCESS_READ)
    except OSError:
        return None


def gear_to_label(raw_gear):
    """Convertit le champ gear brut AC1 (0=R, 1=N, 2=1ere, ...) en etiquette
    lisible coherente avec le mapping boutons GX100 (Phase 3) : "R", "N",
    "1".."6"."""
    if raw_gear == 0:
        return "R"
    if raw_gear == 1:
        return "N"
    return str(raw_gear - 1)


def read_ac_gear(ac_shm):
    ac_shm.seek(AC_GEAR_OFFSET)
    raw = ac_shm.read(AC_GEAR_SIZE)
    if len(raw) < AC_GEAR_SIZE:
        return None
    raw_gear = int.from_bytes(raw, byteorder="little", signed=False)
    return raw_gear, gear_to_label(raw_gear)


def read_ac_pedals(ac_shm):
    """Lit gas/brake/clutch (float32) en un seul passage. Retourne None si
    le segment est trop court (ne devrait pas arriver, AC_SHM_MAP_SIZE
    couvre deja le champ le plus eloigne)."""
    try:
        ac_shm.seek(AC_GAS_OFFSET)
        gas = struct.unpack("<f", ac_shm.read(AC_PEDAL_SIZE))[0]
        ac_shm.seek(AC_BRAKE_OFFSET)
        brake = struct.unpack("<f", ac_shm.read(AC_PEDAL_SIZE))[0]
        ac_shm.seek(AC_CLUTCH_OFFSET)
        clutch = struct.unpack("<f", ac_shm.read(AC_PEDAL_SIZE))[0]
    except struct.error:
        return None
    return gas, brake, clutch


def open_hid():
    devices = hid.enumerate(VID, PID)
    if not devices:
        print(f"Shifter GX100 (HID) non trouve (VID={VID:#06x} PID={PID:#06x}). "
              "Verifiez qu'il est branche et que le firmware composite est flashe.")
        sys.exit(1)
    dev = hid.device()
    dev.open_path(devices[0]["path"])
    dev.set_nonblocking(True)
    print(f"HID connecte : {dev.get_product_string()} ({dev.get_manufacturer_string()})")
    return dev


def open_cdc(port):
    try:
        ser = serial.Serial(port, BAUD, timeout=0)
        ser.timeout = 0  # non bloquant, on poll nous-memes
    except serial.SerialException as e:
        print(f"Impossible d'ouvrir le port CDC {port}: {e}")
        sys.exit(1)
    print(f"CDC connecte sur {port}.")
    return ser


def decode_hid_report(data):
    buttons_raw = data[0] | (data[1] << 8)
    pressed = [i + 1 for i in range(16) if buttons_raw & (1 << i)]
    axes = list(data[2:16])
    return pressed, axes


def main():
    if len(sys.argv) < 2:
        print("Usage: python capture_session.py <PORT_COM> [nom_session]")
        sys.exit(1)

    port = sys.argv[1]
    session_name = sys.argv[2] if len(sys.argv) > 2 else "capture_session"

    CAPTURES_DIR.mkdir(parents=True, exist_ok=True)
    hid_csv_path = CAPTURES_DIR / f"{session_name}_hid.csv"
    cdc_csv_path = CAPTURES_DIR / f"{session_name}_cdc.csv"
    cal_csv_path = CAPTURES_DIR / f"{session_name}_cal.csv"
    ac_csv_path = CAPTURES_DIR / f"{session_name}_ac.csv"
    pedals_csv_path = CAPTURES_DIR / f"{session_name}_pedals.csv"
    plot_path = CAPTURES_DIR / f"{session_name}.png"

    hid_dev = open_hid()
    cdc_ser = open_cdc(port)
    ac_shm = open_ac_shared_memory()
    if ac_shm is not None:
        print("Telemetrie AC connectee (memoire partagee acpmf_physics).")
    else:
        print("Telemetrie AC non disponible pour l'instant (AC pas lance ?) "
              "— nouvelle tentative en continu, la capture HID+CDC n'est pas affectee.")

    print("Capture en cours (Ctrl+C pour arreter et generer le graphique).")
    print("Actionnez les boutons / tentez le geste de bascule maintenant.")
    print()

    t_start = time.perf_counter()
    hid_rows = []  # (t_host, pressed_list, axes_list)
    cdc_rows = []  # dict par colonne CDC_HEADER + t_host
    ac_rows = []  # (t_host, raw_gear, label)
    pedal_rows = []  # (t_host, gas, brake, clutch)

    cdc_buf = ""
    last_hid = None
    last_ac_gear = None

    try:
        with open(hid_csv_path, "w", newline="", encoding="utf-8") as hid_f, \
             open(cdc_csv_path, "w", newline="", encoding="utf-8") as cdc_f, \
             open(cal_csv_path, "w", newline="", encoding="utf-8") as cal_f, \
             open(ac_csv_path, "w", newline="", encoding="utf-8") as ac_f, \
             open(pedals_csv_path, "w", newline="", encoding="utf-8") as pedals_f:

            hid_writer = csv.writer(hid_f)
            hid_writer.writerow(["t_host_s", "buttons_pressed", "axes_bytes"])

            cdc_writer = csv.writer(cdc_f)
            cdc_writer.writerow(["t_host_s"] + CDC_HEADER)

            cal_writer = csv.writer(cal_f)
            cal_writer.writerow(["t_host_s"] + CAL_HEADER)
            last_cal = None

            ac_writer = csv.writer(ac_f)
            ac_writer.writerow(["t_host_s", "gear_raw", "gear"])

            pedals_writer = csv.writer(pedals_f)
            pedals_writer.writerow(["t_host_s", "gas", "brake", "clutch"])

            while True:
                # --- HID : non bloquant, poll rapide ---
                data = hid_dev.read(64, timeout_ms=1)
                if data:
                    data = bytes(data)
                    if data != last_hid:
                        t_host = time.perf_counter() - t_start
                        pressed, axes = decode_hid_report(data)
                        hid_writer.writerow([f"{t_host:.4f}", " ".join(map(str, pressed)), " ".join(map(str, axes))])
                        hid_f.flush()
                        hid_rows.append((t_host, pressed, axes))
                        print(f"[HID  {t_host:7.3f}s] boutons={pressed if pressed else '-'}")
                        last_hid = data

                # --- CDC : accumulation ligne par ligne ---
                n = cdc_ser.in_waiting
                if n:
                    chunk = cdc_ser.read(n).decode("ascii", errors="replace")
                    cdc_buf += chunk
                    while "\n" in cdc_buf:
                        line, cdc_buf = cdc_buf.split("\n", 1)
                        line = line.strip()
                        if not line or line.startswith("t_ms"):
                            continue  # ligne d'entete CSV emise par debug_trace_init()
                        fields = line.split(",")
                        if fields[0] == "CAL":
                            values = fields[2:]
                            if len(fields) == len(CAL_HEADER) + 1 and values != last_cal:
                                t_host = time.perf_counter() - t_start
                                cal_writer.writerow([f"{t_host:.4f}"] + fields[1:])
                                cal_f.flush()
                                if last_cal is not None:
                                    print(f"[CAL  {t_host:7.3f}s] calibration modifiee")
                                last_cal = values
                            continue
                        if len(fields) == CDC_HEADER_LEGACY_LEN:
                            fields += [""] * (len(CDC_HEADER) - CDC_HEADER_LEGACY_LEN)
                        if len(fields) != len(CDC_HEADER):
                            continue
                        t_host = time.perf_counter() - t_start
                        cdc_writer.writerow([f"{t_host:.4f}"] + fields)
                        cdc_f.flush()
                        row = dict(zip(CDC_HEADER, fields))
                        row["t_host_s"] = t_host
                        cdc_rows.append(row)
                        if row["emit_pos"] or row["switch_target"]:
                            print(f"[CDC  {t_host:7.3f}s] emit={row['emit_pos'] or '-'} "
                                  f"kind={row['emit_kind'] or '-'} mode={row['mode']} "
                                  f"switch={row['switch_target'] or '-'}")

                # --- AC : rapport engage (log seulement sur changement) +
                # pedales gas/brake/clutch (log a chaque pas, cf entete du
                # module — on veut la trace continue de l'embrayage pour la
                # correler avec les latences de rapport observees). ---
                if ac_shm is None:
                    ac_shm = open_ac_shared_memory()  # AC a pu etre lance depuis le debut de la capture
                else:
                    result = read_ac_gear(ac_shm)
                    if result is not None:
                        raw_gear, label = result
                        if raw_gear != last_ac_gear:
                            t_host = time.perf_counter() - t_start
                            ac_writer.writerow([f"{t_host:.4f}", raw_gear, label])
                            ac_f.flush()
                            ac_rows.append((t_host, raw_gear, label))
                            print(f"[AC   {t_host:7.3f}s] rapport={label}")
                            last_ac_gear = raw_gear

                    pedals = read_ac_pedals(ac_shm)
                    if pedals is not None:
                        gas, brake, clutch = pedals
                        t_host = time.perf_counter() - t_start
                        pedals_writer.writerow([f"{t_host:.4f}", f"{gas:.4f}", f"{brake:.4f}", f"{clutch:.4f}"])
                        pedals_f.flush()
                        pedal_rows.append((t_host, gas, brake, clutch))

    except KeyboardInterrupt:
        print("\nArret demande.")
    finally:
        hid_dev.close()
        cdc_ser.close()
        if ac_shm is not None:
            ac_shm.close()

    plot(hid_rows, cdc_rows, ac_rows, pedal_rows, plot_path)


def plot(hid_rows, cdc_rows, ac_rows, pedal_rows, out_path):
    if not cdc_rows:
        print("Aucune donnee CDC capturee, pas de graphique genere.")
        return

    times = [float(r["t_host_s"]) for r in cdc_rows]
    labels = ["U2", "U3", "U4", "U5", "U6", "U7"]

    fig, (ax_hall, ax_collar, ax_buttons, ax_ac, ax_pedals) = plt.subplots(
        5, 1, figsize=(14, 13), sharex=True, height_ratios=[3, 1, 1.5, 1.5, 1.5]
    )

    for i, label in enumerate(labels):
        series = [int(r[f"u{i + 2}"]) for r in cdc_rows]
        ax_hall.plot(times, series, label=label, linewidth=1)
    ax_hall.set_ylabel("Valeur ADC (0-4095)")
    ax_hall.set_title("Capteurs Hall + boutons HID vs temps (session hote)")
    ax_hall.legend(loc="upper right", fontsize=8, ncol=6)
    ax_hall.grid(True, alpha=0.3)

    collar = [int(r["collar"]) for r in cdc_rows]
    ax_collar.step(times, collar, where="post", color="black", label="collerette")
    mode_seq = [1 if r["mode"] == "SEQ" else 0 for r in cdc_rows]
    ax_collar.step(times, mode_seq, where="post", color="tab:red", label="mode=SEQ", alpha=0.7)
    ax_collar.set_ylabel("Etat")
    ax_collar.set_yticks([0, 1])
    ax_collar.legend(loc="upper right", fontsize=8)
    ax_collar.grid(True, alpha=0.3)

    ax_buttons.set_ylabel("Bouton HID actif")
    ax_buttons.set_xlabel("Temps ecoule (s)")
    ax_buttons.set_ylim(0, 17)
    ax_buttons.grid(True, alpha=0.3)
    for t_host, pressed, _axes in hid_rows:
        for b in pressed:
            ax_buttons.plot(t_host, b, "o", color="tab:blue", markersize=3)
    if not hid_rows:
        ax_buttons.text(0.5, 0.5, "Aucun input HID capture", transform=ax_buttons.transAxes,
                         ha="center", va="center", color="tab:red")

    ax_ac.set_ylabel("Rapport AC (jeu)")
    ax_ac.set_xlabel("Temps ecoule (s)")
    ax_ac.grid(True, alpha=0.3)
    if ac_rows:
        ac_times = [t for t, _raw, _label in ac_rows]
        ac_labels = [label for _t, _raw, label in ac_rows]
        # Ordre d'affichage fixe (pas alphabetique) pour rester lisible :
        # R, N, 1..6, plus tout ce qui serait mesure au-dela (rapports
        # additionnels sur un autre vehicule) ajoute a la fin.
        order = ["R", "N"] + [str(i) for i in range(1, 7)]
        for label in ac_labels:
            if label not in order:
                order.append(label)
        y_positions = [order.index(label) for label in ac_labels]
        ax_ac.step(ac_times, y_positions, where="post", color="tab:green", linewidth=1.5)
        ax_ac.set_yticks(range(len(order)))
        ax_ac.set_yticklabels(order)
    else:
        ax_ac.text(0.5, 0.5, "Aucune telemetrie AC capturee (AC non lance pendant la session ?)",
                    transform=ax_ac.transAxes, ha="center", va="center", color="tab:red")

    ax_pedals.set_ylabel("Pedale AC (0-1)")
    ax_pedals.set_xlabel("Temps ecoule (s)")
    ax_pedals.set_ylim(-0.05, 1.05)
    ax_pedals.grid(True, alpha=0.3)
    if pedal_rows:
        p_times = [t for t, _g, _b, _c in pedal_rows]
        p_gas = [g for _t, g, _b, _c in pedal_rows]
        p_brake = [b for _t, _g, b, _c in pedal_rows]
        p_clutch = [c for _t, _g, _b, c in pedal_rows]
        ax_pedals.plot(p_times, p_clutch, color="tab:purple", linewidth=1.3, label="embrayage")
        ax_pedals.plot(p_times, p_brake, color="tab:orange", linewidth=0.8, alpha=0.6, label="frein")
        ax_pedals.plot(p_times, p_gas, color="tab:green", linewidth=0.8, alpha=0.6, label="acc.")
        ax_pedals.legend(loc="upper right", fontsize=8)
    else:
        ax_pedals.text(0.5, 0.5, "Aucune donnee pedale capturee",
                        transform=ax_pedals.transAxes, ha="center", va="center", color="tab:red")

    fig.tight_layout()
    fig.savefig(out_path, dpi=150)
    print(f"Graphique enregistre : {out_path}")


if __name__ == "__main__":
    main()
