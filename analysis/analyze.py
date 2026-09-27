#!/usr/bin/env python3
"""
analyze.py - Hafta 01 buton yanit suresi analizi

Girdi (arayuzun "CSV Kaydet" ciktilari, measurements/ altinda):
  S<n>.csv           scenario,event_id,t0_us,t1_us,t2_us,t3_us,t4_us,status
  S<n>_counters.csv  name,value   (pencere sayaclari; yoksa sayac kolonlari bos kalir)

Cikti:
  measurements/summary.csv
  analysis/plots/r_vs_event.png        olay numarasi -> R, 20 ms deadline cizgisi
  analysis/plots/stages_by_scenario.png senaryo -> asamalarin ortalama sureleri (yigilmis)

Tum sureler ayni kart sayacindan (TIM2, 1 us) gelir; farklar mod 2^32
hesaplanir (sayac ~71,6 dakikada doner). R ve asama istatistikleri yalnizca
status == "ok" satirlarindan hesaplanir; digerleri (drop, tx_error, timeout)
ayrica sayilir ve "deadline karsilandi" sayilmaz. drop'un nerede oldugu bos
zamanlardan okunur: buton kuyrugu -> yalnizca t0, TX kuyrugu -> t0..t2.

Kullanim:
    python analyze.py
    python analyze.py --measurements ../measurements --plots ./plots

Bagimliliklar: pandas, matplotlib (pip install pandas matplotlib)
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

import matplotlib.pyplot as plt
import pandas as pd

DEADLINE_US = 20_000  # R <= 20 ms deney butcesi
MOD = 2**32

# ID -> (nominal telemetri Hz, nominal ek CPU isi ms) - README ile birebir.
SCENARIOS = {
    "S0": (0, 0),
    "S1": (10, 0),
    "S2": (50, 0),
    "S3": (100, 0),
    "S4": (100, 2),
    "S5": (100, 5),
}
STATUSES = ["ok", "drop", "tx_error", "timeout"]  # sartname degerleri
LEGACY_STATUS = {"tx_drop": "drop", "btn_drop": "drop"}  # eski arayuz ciktilari
STAGES = [  # (kolon, etiket, bas, son)
    ("wait", "t1-t0 gorev bekleme", "t0_us", "t1_us"),
    ("prep", "t2-t1 hazirlama", "t1_us", "t2_us"),
    ("txq", "t3-t2 TX oncesi", "t2_us", "t3_us"),
    ("uart", "t4-t3 UART + TC", "t3_us", "t4_us"),
]
STAGE_COLORS = ["#8f7fe0", "#e0a030", "#4c9aff", "#2fbf9a"]
COLUMNS = ["scenario", "event_id", "t0_us", "t1_us", "t2_us", "t3_us", "t4_us", "status"]
COUNTERS = ["accepted", "repeat", "btn_queue_drop", "tx_queue_drop", "tx_queue_max",
            "pool_overflow", "droplog_overflow", "tx_timeout", "tx_start_fail",
            "spurious_tc", "tel_sent", "tel_period_avg_us",
            "btn_direct", "btn_latched", "btn_fallback", "btn_result_lost"]


def load_events(path: Path) -> pd.DataFrame:
    df = pd.read_csv(path, dtype={"scenario": str, "status": str})
    missing = set(COLUMNS) - set(df.columns)
    if missing:
        raise ValueError(f"{path.name}: eksik kolon(lar): {sorted(missing)}")
    for c in ["t0_us", "t1_us", "t2_us", "t3_us", "t4_us"]:
        df[c] = pd.to_numeric(df[c], errors="coerce")  # bos -> NaN (0 degil)
    df["status"] = df["status"].replace(LEGACY_STATUS)
    df = df.sort_values("event_id").reset_index(drop=True)
    ok = df["status"] == "ok"
    df["r_us"] = ((df["t4_us"] - df["t0_us"]) % MOD).where(ok)
    for key, _, a, b in STAGES:
        df[key] = ((df[b] - df[a]) % MOD).where(ok)
    return df


def load_counters(path: Path) -> dict[str, int]:
    if not path.exists():
        return {}
    c = pd.read_csv(path)
    return {str(k): v for k, v in zip(c["name"], c["value"]) if pd.notna(v)}


def summarize(sid: str, df: pd.DataFrame, counters: dict[str, int]) -> dict:
    hz, load_ms = SCENARIOS[sid]
    row: dict = {"scenario": sid, "telemetry_hz_nominal": hz, "extra_load_ms_nominal": load_ms,
                 "n_events": len(df)}
    for s in STATUSES:
        row[f"n_{s}"] = int((df["status"] == s).sum())
    drop = df["status"] == "drop"
    row["drop_btn_queue"] = int((drop & df["t1_us"].isna()).sum())
    row["drop_tx_queue"] = int((drop & df["t1_us"].notna()).sum())
    row["n_other"] = len(df) - sum(row[f"n_{s}"] for s in STATUSES)

    r = df["r_us"].dropna() / 1000.0
    if len(r):
        row.update(r_min_ms=round(r.min(), 3), r_mean_ms=round(r.mean(), 3),
                   r_p95_ms=round(r.quantile(0.95), 3), r_max_ms=round(r.max(), 3),
                   deadline_miss=int((r > DEADLINE_US / 1000).sum()))
        for key, *_ in STAGES:
            row[f"{key}_mean_ms"] = round(df[key].dropna().mean() / 1000.0, 3)
    else:
        row.update(r_min_ms="", r_mean_ms="", r_p95_ms="", r_max_ms="", deadline_miss="")
        for key, *_ in STAGES:
            row[f"{key}_mean_ms"] = ""

    for k in COUNTERS:
        row[k] = counters.get(k, "")
    avg = counters.get("tel_period_avg_us")
    row["tel_rate_hz_measured"] = round(1e6 / avg, 2) if avg else ""

    notes = []
    if len(df) < 30:
        notes.append(f"{len(df)} olay (<30)")
    if counters and counters.get("accepted") not in (None, len(df)):
        notes.append(f"kabul {counters['accepted']} != kayit {len(df)}")
    row["notes"] = "; ".join(notes)
    return row


def plot_r_vs_event(data: dict[str, pd.DataFrame], out: Path) -> bool:
    fig, ax = plt.subplots(figsize=(9, 5))
    drawn = False
    for sid, df in data.items():
        ok = df.dropna(subset=["r_us"])
        if ok.empty:
            continue
        excluded = len(df) - len(ok)
        ax.plot(range(1, len(ok) + 1), ok["r_us"] / 1000.0, marker="o", markersize=3,
                linewidth=0.8, label=f"{sid} (n={len(ok)} ok, {excluded} hariç)")
        drawn = True
    if not drawn:
        plt.close(fig)
        return False
    ax.axhline(DEADLINE_US / 1000, color="red", linestyle="--", linewidth=1, label="deadline 20 ms")
    ax.set_xlabel("Olay numarası (senaryo içinde, yalnızca status=ok)")
    ax.set_ylabel("R = t4 − t0 (ms)")
    ax.set_title("Olay başına yanıt süresi")
    ax.legend(fontsize=8)
    ax.grid(alpha=0.3)
    fig.tight_layout()
    fig.savefig(out / "r_vs_event.png", dpi=150)
    plt.close(fig)
    return True


def plot_stages(summary: pd.DataFrame, out: Path) -> bool:
    s = summary[summary["r_mean_ms"] != ""].copy()
    if s.empty:
        return False
    fig, ax = plt.subplots(figsize=(8, 5))
    bottom = [0.0] * len(s)
    for (key, label, *_), color in zip(STAGES, STAGE_COLORS):
        vals = s[f"{key}_mean_ms"].astype(float).tolist()
        ax.bar(s["scenario"], vals, bottom=bottom, color=color, label=label)
        bottom = [b + v for b, v in zip(bottom, vals)]
    for x, total, n in zip(s["scenario"], bottom, s["n_ok"]):
        ax.text(x, total, f"{total:.2f} ms\nn={n}", ha="center", va="bottom", fontsize=8)
    ax.axhline(DEADLINE_US / 1000, color="red", linestyle="--", linewidth=1, label="deadline 20 ms")
    ax.set_ylim(0, max(max(bottom), DEADLINE_US / 1000) * 1.18)  # sutun etiketlerine yer
    ax.set_ylabel("Ortalama süre (ms)")
    ax.set_title("Senaryoya göre aşamaların ortalama süreleri (status=ok)")
    ax.legend(fontsize=8)
    fig.tight_layout()
    fig.savefig(out / "stages_by_scenario.png", dpi=150)
    plt.close(fig)
    return True


def main() -> int:
    here = Path(__file__).resolve().parent
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--measurements", type=Path, default=here.parent / "measurements")
    ap.add_argument("--plots", type=Path, default=here / "plots")
    args = ap.parse_args()
    args.plots.mkdir(parents=True, exist_ok=True)

    data: dict[str, pd.DataFrame] = {}
    rows = []
    for sid in SCENARIOS:
        path = args.measurements / f"{sid}.csv"
        if not path.exists():
            print(f"uyari: {path} yok, atlaniyor", file=sys.stderr)
            continue
        df = load_events(path)
        data[sid] = df
        rows.append(summarize(sid, df, load_counters(args.measurements / f"{sid}_counters.csv")))

    if not rows:
        print("hic olcum dosyasi yok")
        return 1

    summary = pd.DataFrame(rows)
    summary.to_csv(args.measurements / "summary.csv", index=False)
    print(summary.to_string(index=False))

    made = []
    if plot_r_vs_event(data, args.plots):
        made.append("r_vs_event.png")
    if plot_stages(summary, args.plots):
        made.append("stages_by_scenario.png")
    print("grafikler:", ", ".join(made) if made else "cizilecek 'ok' olay yok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
