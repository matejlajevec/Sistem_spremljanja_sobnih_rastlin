"""Izrise vse stiri casovne poteke na eno sliko (mreza 2x2) iz meritve_24h.csv.

Zagon:
    python3 analysis/izdelaj_grafe.py

Skripta samo BERE CSV in shrani porocilo/slike/graf_skupno.png.
Na osi X je dejanska ura meritve: oznacene ure na vsake 3 h, med njimi
neoznacene crtice na vsako uro.
"""

import csv
from datetime import datetime, timedelta
from pathlib import Path

import matplotlib
matplotlib.use("Agg")              # brez graficnega vmesnika
import matplotlib.pyplot as plt
import matplotlib.dates as mdates

KOREN = Path(__file__).resolve().parent.parent
CSV_POT = KOREN / "measurements" / "meritve_24h.csv"
IZHOD = KOREN / "porocilo" / "slike"

# Zacetek 24-urne meritve; popravi, ce se datum ali ura zacetka spremenita.
ZACETEK = datetime(2026, 8, 16, 10, 50)

# Barve v slogu Excela
MODRA, ORANZNA, ZELENA, RDECA = "#4472C4", "#ED7D31", "#70AD47", "#C00000"
MREZA, OKVIR = "#D9D9D9", "#BFBFBF"

# (oznaka osi Y, stolpec v CSV, stolpec veljavnosti, spodnja meja, zgornja meja)
# Vlaga substrata je brez mej, ker profil spatifila zanjo nima stevilcnega praga.
PLOSCE = [
    ("Temperatura [°C]", "temperatura_C", "temp_ok", 18.0, 27.0),
    ("Rel. vlažnost zraka [% RH]", "zracna_vlaga_pct", "vlaga_ok", 40.0, 60.0),
    ("Osvetljenost [lx]", "svetloba_lux", "svetloba_ok", 270.0, 1080.0),
    ("Rel. vlažnost substrata [%]", "zemlja_pct", "zemlja_ok", None, None),
]


def preberi():
    if not CSV_POT.exists():
        raise SystemExit(f"Datoteke z meritvami ni: {CSV_POT}")
    with open(CSV_POT, newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))


def serija(zapisi, stolpec, stolpec_ok):
    """Vrne (casi, vrednosti) samo za veljavne meritve; cas je dejanski datum in ura."""
    casi, vrednosti = [], []
    for v in zapisi:
        if v[stolpec_ok] == "1":
            casi.append(ZACETEK + timedelta(minutes=float(v["cas_min"])))
            vrednosti.append(float(v[stolpec]))
    return casi, vrednosti


def main():
    IZHOD.mkdir(parents=True, exist_ok=True)
    zapisi = preberi()
    print(f"Prebranih zapisov: {len(zapisi)}")

    fig, osi = plt.subplots(2, 2, figsize=(12, 6.7))

    for ax, (y_oznaka, stolpec, stolpec_ok, sp, zg) in zip(osi.flat, PLOSCE):
        casi, vrednosti = serija(zapisi, stolpec, stolpec_ok)
        povprecje = sum(vrednosti) / len(vrednosti)

        ax.plot(casi, vrednosti, color=MODRA, linewidth=1.2, label="Meritev", zorder=3)
        ax.axhline(povprecje, color=ORANZNA, linewidth=1.2, label="Povprečje", zorder=2)
        if sp is not None:
            ax.axhline(sp, color=ZELENA, linestyle="--", linewidth=1.0,
                       label="Spodnja meja", zorder=2)
        if zg is not None:
            ax.axhline(zg, color=RDECA, linestyle="--", linewidth=1.0,
                       label="Zgornja meja", zorder=2)

        # rob, da crta meje ne lezi na okvirju; os ne gre pod nic, ce so vsi podatki nenegativni
        kand = vrednosti + [povprecje] + [v for v in (sp, zg) if v is not None]
        lo, hi = min(kand), max(kand)
        rob = (hi - lo) * 0.08 or 1.0
        ax.set_ylim(max(0.0, lo - rob) if lo >= 0 else lo - rob, hi + rob)

        ax.set_ylabel(y_oznaka, fontsize=8)
        ax.set_xlabel("Čas meritve", fontsize=9)

        ax.xaxis.set_major_locator(mdates.HourLocator(interval=3))
        ax.xaxis.set_major_formatter(mdates.DateFormatter("%H:%M"))
        ax.xaxis.set_minor_locator(mdates.HourLocator(interval=1))
        ax.tick_params(axis="x", which="major", length=6, labelsize=8)
        ax.tick_params(axis="x", which="minor", length=3)
        ax.tick_params(axis="y", labelsize=8)

        ax.grid(axis="y", color=MREZA, linewidth=0.7, zorder=0)
        ax.set_axisbelow(True)
        for rob_osi in ax.spines.values():
            rob_osi.set_color(OKVIR)
        ax.legend(loc="lower right", frameon=True, fontsize=7)

    fig.tight_layout()
    fig.savefig(IZHOD / "graf_skupno.png", dpi=200)
    plt.close(fig)
    print("Shranjeno:", IZHOD / "graf_skupno.png")


if __name__ == "__main__":
    main()
