#!/usr/bin/env python3
# <wtl> project: legioncpupower | file: tools/cpuload.py | author: watchthelight </wtl>
"""cpuload.py <label> [threads] [seconds] [outdir]: runs spin on N threads and reads LegionCpuProbe while it runs.

Prints and saves (outdir/<label>.json): throughput, package power from MSR 0x611 over the middle of the run, per-core
actual ratio (0x198) and HWP request (0x774) split into P- and E-cores (0x771 highest >= 80 = P), limit reasons (0x64F),
core and package temperatures (0x19C / 0x1B1 against TjMax in 0x1A2), and the MCHBAR power limit when the probe has it.
The probe refreshes every 2 s, so the run should be at least 12 s."""
import json, os, re, subprocess, sys, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))


def probe():
    out = subprocess.run(["ioreg", "-r", "-c", "LegionCpuProbe", "-w0"], capture_output=True, text=True).stdout
    d = {}
    for k, v in re.findall(r'"(\w+)" = (\d+)', out):
        d[k] = int(v)
    for k, v in re.findall(r'"(\w+)" = "([^"]*)"', out):
        d.setdefault(k, v)
    return d


def wait_tick(after):
    t0 = time.time()
    while time.time() - t0 < 5:
        p = probe()
        if p.get("Ticks", 0) > after:
            return p, time.time()
        time.sleep(0.1)
    return probe(), time.time()


def fields(p):
    n = p.get("CpuCount", 0)
    tj = (p.get("Pkg_Msr1A2", 0) >> 16) & 0xFF or 100
    cores = []
    for c in range(n):
        g = lambda m: p.get(f"Cpu{c:02d}_Msr{m}", 0)
        cap = g("771")
        cores.append({
            "cpu": c, "type": "P" if (cap & 0xFF) >= 80 else "E",
            "ratio": (g("198") >> 8) & 0xFF,
            "req_min": g("774") & 0xFF, "req_max": (g("774") >> 8) & 0xFF, "req_epp": (g("774") >> 24) & 0xFF,
            "highest": cap & 0xFF, "temp_c": tj - ((g("19C") >> 16) & 0x7F),
            "limit_reasons": g("64F"),
        })
    return cores, tj


def summarize(cores):
    s = {}
    for t in ("P", "E"):
        cs = [c for c in cores if c["type"] == t]
        if cs:
            r = [c["ratio"] for c in cs]
            s[t] = {"n": len(cs), "ratio_avg": round(sum(r) / len(r), 1), "ratio_max": max(r),
                    "req_max": sorted({c["req_max"] for c in cs}), "highest": sorted({c["highest"] for c in cs}),
                    "temp_max_c": max(c["temp_c"] for c in cs)}
    return s


def main():
    label = sys.argv[1]
    threads = int(sys.argv[2]) if len(sys.argv) > 2 else os.cpu_count()
    secs = float(sys.argv[3]) if len(sys.argv) > 3 else 30
    outdir = sys.argv[4] if len(sys.argv) > 4 else os.path.join(HERE, "../results")
    os.makedirs(outdir, exist_ok=True)
    spin = os.path.join(HERE, "spin")
    res = {}
    th = threading.Thread(target=lambda: res.update(out=subprocess.run([spin, str(threads), str(secs)], capture_output=True,
                                                                        text=True).stdout))
    th.start()
    time.sleep(min(6, secs / 4))
    p0, t0 = wait_tick(probe().get("Ticks", 0))
    time.sleep(max(2, secs - 10))
    p1, t1 = wait_tick(p0.get("Ticks", 0))
    th.join()
    unit = 0.5 ** ((p1.get("Pkg_Msr606", 0) >> 8) & 0x1F)
    e0, e1 = p0.get("Pkg_Msr611", 0) & 0xFFFFFFFF, p1.get("Pkg_Msr611", 0) & 0xFFFFFFFF
    joules = ((e1 - e0) % (1 << 32)) * unit
    # the probe reads 0x611 once per tick, so the energy spans the tick interval, not our wall clock
    ticks = max(1, p1.get("Ticks", 0) - p0.get("Ticks", 0))
    watts = joules / (ticks * 2.0)
    cores, tj = fields(p1)
    pl = p1.get("Pkg_Msr610", 0)
    m = re.search(r"total_mops ([\d.]+)", res.get("out", ""))
    rep = {
        "label": label, "threads": threads, "seconds": secs, "time": time.strftime("%F %T"),
        "throughput_mops": float(m.group(1)) if m else None,
        "pkg_watts": round(watts, 1), "energy_window_ticks": ticks,
        "pkg_temp_c": tj - ((p1.get("Pkg_Msr1B1", 0) >> 16) & 0x7F),
        "msr610_pl1_w": round((pl & 0x7FFF) * (0.5 ** (p1.get("Pkg_Msr606", 0) & 0xF)), 1),
        "msr610_pl2_w": round(((pl >> 32) & 0x7FFF) * (0.5 ** (p1.get("Pkg_Msr606", 0) & 0xF)), 1),
        "mchbar": p1.get("Mchbar", ""), "mchbar_59a0": p1.get("Mchbar_PkgPowerLimit_59A0"),
        "limit_reasons_any": hex(max((c["limit_reasons"] for c in cores), default=0)),
        "cores": summarize(cores), "per_core": cores,
    }
    if isinstance(rep["mchbar_59a0"], int):
        v = rep["mchbar_59a0"]; u = 0.5 ** (p1.get("Pkg_Msr606", 0) & 0xF)
        rep["mchbar_pl1_w"] = round((v & 0x7FFF) * u, 1); rep["mchbar_pl2_w"] = round(((v >> 32) & 0x7FFF) * u, 1)
        rep["mchbar_locked"] = bool(v >> 63)
    json.dump(rep, open(os.path.join(outdir, f"{label}.json"), "w"), indent=1)
    c = rep["cores"]
    print(f"{label}: {threads} thr, {rep['throughput_mops']} Mops/s, pkg {rep['pkg_watts']} W, pkg {rep['pkg_temp_c']} C, "
          f"P ratio avg {c.get('P', {}).get('ratio_avg')} max {c.get('P', {}).get('ratio_max')} req {c.get('P', {}).get('req_max')}, "
          f"E ratio avg {c.get('E', {}).get('ratio_avg')} max {c.get('E', {}).get('ratio_max')} req {c.get('E', {}).get('req_max')}, "
          f"reasons {rep['limit_reasons_any']}, MSR PL {rep['msr610_pl1_w']}/{rep['msr610_pl2_w']} W, MCHBAR {rep.get('mchbar_pl1_w', rep['mchbar'])}")


if __name__ == "__main__":
    main()
