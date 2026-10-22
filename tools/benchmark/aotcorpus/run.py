#!/usr/bin/env python3
"""Drive the AOT differential corpus.

    run.py gen OUTDIR                  write the templated programs (calibrated) and
                                       copy the hand-written ones
    run.py calibrate [--plain BIN]     scale every program's loop count so the
                                       interpreter takes 60-250 ms; writes calib.json
    run.py run [--m1 DIR] [--out DIR] [--jobs N] [--filter RE] [--time-jobs N]
                                       profile, compile and run every program in four
                                       modes (interpreted, speculation on, off, forced
                                       misses), compare the outputs with the
                                       interpreter's, time them, and report

`run` needs the generator harness (`m1.sh e FILE` builds and checks one program: its
profile, its unit, and the three compiled modes) and the interpreter binary
`run-plain` in the harness directory. Nothing in the report is a pass unless the
output of every compiled mode equals the interpreter's.
"""
import json
import math
import os
import re
import shutil
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
CALIB = os.path.join(HERE, "calib.json")
TARGET_LO, TARGET_HI = 60.0, 250.0  # milliseconds of interpreter time


def sh(cmd, env=None, timeout=300, cwd=None):
    e = dict(os.environ)
    if env:
        e.update(env)
    try:
        r = subprocess.run(cmd, env=e, timeout=timeout, cwd=cwd, capture_output=True, text=True)
        return r.returncode, r.stdout, r.stderr
    except subprocess.TimeoutExpired:
        return 124, "", "timeout"


def timed(cmd, env=None, timeout=120):
    t0 = time.perf_counter()
    rc, out, err = sh(cmd, env, timeout)
    return (time.perf_counter() - t0) * 1000.0, rc, out.strip()


def gen_into(outdir, calib=None):
    args = [sys.executable, os.path.join(HERE, "gen.py"), outdir]
    if calib:
        args += ["--calib", calib]
    rc, out, err = sh(args)
    if rc != 0:
        sys.exit("gen.py failed: " + err[-400:])
    hand = os.path.join(HERE, "hand")
    if os.path.isdir(hand):
        for f in sorted(os.listdir(hand)):
            if f.endswith(".js"):
                shutil.copy(os.path.join(hand, f), os.path.join(outdir, "hand_" + f))


def cmd_gen(args):
    out = args[0]
    calib = CALIB if os.path.exists(CALIB) else None
    gen_into(out, calib)
    print("wrote", len([f for f in os.listdir(out) if f.endswith(".js")]), "files to", out)


def cmd_calibrate(args):
    plain = os.path.join(ROOT, ".local", "m1", "run-plain")
    if "--plain" in args:
        plain = args[args.index("--plain") + 1]
    jobs = max(2, (os.cpu_count() or 4) // 2)
    tmp = os.path.join(ROOT, ".local", "aotcorpus-calib")
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(tmp)
    empty = os.path.join(tmp, "empty.js")
    open(empty, "w").write("")
    start = min(timed([plain, empty])[0] for _ in range(5))
    calib = json.load(open(CALIB)) if os.path.exists(CALIB) else {}
    base = {}
    rc, out, err = sh([sys.executable, os.path.join(HERE, "gen.py"), "--list"])
    names = out.split()
    # The base counts come from a plain generation.
    gen_into(tmp)
    for n in names:
        m = re.search(r"var N = (\d+);", open(os.path.join(tmp, n + ".js")).read())
        base[n] = int(m.group(1))
    cur = {n: calib.get(n, base[n]) for n in names}

    def settle(n):
        count = cur[n]
        for _ in range(4):
            d = os.path.join(tmp, "p_" + n)
            os.makedirs(d, exist_ok=True)
            src = open(os.path.join(tmp, n + ".js")).read()
            src = re.sub(r"var N = \d+;", "var N = %d;" % count, src, count=1)
            path = os.path.join(d, n + ".js")
            open(path, "w").write(src)
            t, rc, out = timed([plain, path], timeout=60)
            if rc != 0:
                return n, count, -1.0
            work = max(t - start, 1.0)
            if TARGET_LO <= work <= TARGET_HI:
                break
            want = 110.0 / work
            count = max(2, int(count * want))
        return n, count, t - start

    with ThreadPoolExecutor(jobs) as ex:
        res = list(ex.map(settle, names))
    bad = [r for r in res if r[2] < 0]
    for n, count, t in res:
        cur[n] = count
    json.dump(cur, open(CALIB, "w"), indent=0, sort_keys=True)
    ts = sorted(r[2] for r in res if r[2] >= 0)
    print("calibrated %d programs (startup %.0f ms); median %.0f ms, min %.0f, max %.0f, failed %d" % (
        len(res), start, ts[len(ts) // 2], ts[0], ts[-1], len(bad)))
    for n, count, t in bad[:20]:
        print("  failed:", n)


def parse_report(path):
    funcs, bytes_total = 0, 0
    try:
        for line in open(path):
            if line.startswith("func "):
                funcs += 1
            m = re.match(r"code hex=\S+ lines=\S+ bytes=(\d+)", line)
            if m:
                bytes_total += int(m.group(1))
    except OSError:
        pass
    return funcs, bytes_total


def cmd_run(args):
    m1 = os.path.join(ROOT, ".local", "m1")
    out = os.path.join(ROOT, ".local", "aotcorpus-run")
    jobs = os.cpu_count() or 4
    time_jobs = 4
    filt = None
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--m1":
            m1 = os.path.abspath(args[i + 1]); i += 2
        elif a == "--out":
            out = os.path.abspath(args[i + 1]); i += 2
        elif a == "--jobs":
            jobs = int(args[i + 1]); i += 2
        elif a == "--time-jobs":
            time_jobs = int(args[i + 1]); i += 2
        elif a == "--filter":
            filt = re.compile(args[i + 1]); i += 2
        else:
            sys.exit("unknown argument " + a)
    progs = os.path.join(out, "progs")
    shutil.rmtree(progs, ignore_errors=True)
    os.makedirs(progs)
    gen_into(progs, CALIB if os.path.exists(CALIB) else None)
    names = sorted(f[:-3] for f in os.listdir(progs) if f.endswith(".js") and not f.endswith(".prof.js"))
    if filt:
        names = [n for n in names if filt.search(n)]
    plain = os.path.join(m1, "run-plain")
    scratch = os.path.join(out, "scratch")
    os.makedirs(scratch, exist_ok=True)
    results = {}
    t0 = time.time()

    def build(n):
        path = os.path.join(progs, n + ".js")
        rc, so, se = sh(["./m1.sh", "e", path], env={"E_OPT": "-O2", "E_SCRATCH": scratch}, cwd=m1, timeout=900)
        tail = (so + se).strip().splitlines()
        return n, rc, (tail[-1] if tail else "")

    with ThreadPoolExecutor(jobs) as ex:
        built = list(ex.map(build, names))
    print("built %d programs in %.0f s" % (len(built), time.time() - t0), flush=True)

    def measure(item):
        n, rc, last = item
        r = {"name": n, "build_rc": rc, "note": last}
        d = os.path.join(scratch, n)
        path = os.path.join(progs, n + ".js")
        funcs, nbytes = parse_report(os.path.join(d, "report.txt"))
        r["funcs"], r["bytes"] = funcs, nbytes
        if rc != 0:
            r["status"] = "FAIL"
            return r
        run = os.path.join(d, "run")
        modes = {
            "interp": ([plain, path], {}),
            "spec_on": ([run, path], {}),
            "spec_off": ([run, path], {"QJS_AOT_SPEC": "0"}),
            "force_miss": ([run, path], {"QJS_AOT_FORCE_MISS": "1"}),
        }
        ref = None
        for mode, (cmd, env) in modes.items():
            best, outp = None, None
            for _ in range(2 if mode in ("interp", "spec_on") else 1):
                t, mrc, o = timed(cmd, env)
                if best is None or t < best:
                    best = t
                outp = o if mrc == 0 else "rc=%d" % mrc
            r[mode] = best
            if mode == "interp":
                ref = outp
            elif outp != ref:
                r["status"] = "WRONG:" + mode
        # The exit sites the compiled run took: a site that fires thousands of times is a
        # speculation the data does not match, which shows up as a slow program, not a wrong one.
        if r.get("status", "ok") == "ok":
            rc, so, se = sh([run, path], {"QJS_AOT_UNIT_STATS": "1"})
            sites = []
            for ln in se.splitlines():
                if ln.startswith("unit-exit-site"):
                    kv = dict(p.split("=", 1) for p in ln.split()[1:])
                    if int(kv["count"]) >= 100:
                        sites.append([kv["fn"], kv["kind"], int(kv["pc"]), int(kv["count"])])
            r["exit_sites"] = sites
        r.setdefault("status", "ok")
        return r

    with ThreadPoolExecutor(time_jobs) as ex:
        rows = list(ex.map(measure, built))
    for r in rows:
        results[r["name"]] = r
    summarise(rows, out)


def summarise(rows, out):
    ok = [r for r in rows if r["status"] == "ok"]
    bad = [r for r in rows if r["status"] != "ok"]
    ratios = [(r["spec_on"] / r["interp"], r) for r in ok if r["interp"] > 0 and r["spec_on"] > 0]
    gm = math.exp(sum(math.log(x) for x, _ in ratios) / len(ratios)) if ratios else 0.0
    ratios_off = [r["spec_off"] / r["interp"] for r in ok if r["interp"] > 0 and r["spec_off"] > 0]
    gm_off = math.exp(sum(math.log(x) for x in ratios_off) / len(ratios_off)) if ratios_off else 0.0
    ratios_fm = [r["force_miss"] / r["interp"] for r in ok if r["interp"] > 0 and r["force_miss"] > 0]
    gm_fm = math.exp(sum(math.log(x) for x in ratios_fm) / len(ratios_fm)) if ratios_fm else 0.0
    per = [r["bytes"] / r["funcs"] for r in ok if r["funcs"]]
    lines = []
    lines.append("programs %d  ok %d  failed %d" % (len(rows), len(ok), len(bad)))
    lines.append("time ratio (compiled / interpreted, lower is faster): geomean spec on %.3f  spec off %.3f  forced misses %.3f" % (gm, gm_off, gm_fm))
    if per:
        per.sort()
        lines.append("compiled bytes per function: median %.0f  p90 %.0f  max %.0f" % (per[len(per) // 2], per[int(len(per) * 0.9)], per[-1]))
    faster = sum(1 for x, _ in ratios if x < 1.0)
    lines.append("programs faster compiled than interpreted: %d of %d (%.1f%%)" % (
        faster, len(ratios), 100.0 * faster / max(1, len(ratios))))
    fam = {}
    for x, r in ratios:
        parts = r["name"].split("_")
        key = "hand" if parts[0] == "hand" else ("_".join(parts[:2]) if len(parts) > 1 else parts[0])
        fam.setdefault(key, []).append(x)
    rows_f = sorted(
        ((math.exp(sum(math.log(v) for v in vs) / len(vs)), k, len(vs),
          sum(1 for v in vs if v >= 1.0)) for k, vs in fam.items()), reverse=True)
    lines.append("")
    lines.append("families (first two name parts) by geomean, slowest first; slower = programs at >= 1.0x:")
    for g, k, n, slow in rows_f:
        lines.append("  %-28s geomean %5.2fx  programs %4d  slower %4d%s" % (k, g, n, slow, "   <-- below 1.0x" if g >= 1.0 else ""))
    lines.append("")
    lines.append("failures:")
    for r in bad[:100]:
        lines.append("  %-44s %s %s" % (r["name"], r["status"], r["note"][:90]))
    lines.append("")
    lines.append("worst 50 by spec-on / interpreted:")
    for x, r in sorted(ratios, key=lambda p: -p[0])[:50]:
        lines.append("  %-44s %6.2fx  interp %7.1f ms  on %7.1f  off %7.1f  miss %7.1f  bytes/fn %6.0f" % (
            r["name"], x, r["interp"], r["spec_on"], r["spec_off"], r["force_miss"],
            r["bytes"] / r["funcs"] if r["funcs"] else 0))
    hot = []
    for r in ok:
        for fn, kind, pc, cnt in r.get("exit_sites", []):
            if cnt >= 1000:
                hot.append((cnt, r["name"], fn[:8], kind, pc))
    hot.sort(reverse=True)
    lines.append("")
    lines.append("exit sites taken 1000 times or more in a run (count, program, function, guard, pc): %d" % len(hot))
    for cnt, n, fn, kind, pc in hot[:60]:
        lines.append("  %9d  %-40s %s %s@%d" % (cnt, n, fn, kind, pc))
    text = "\n".join(lines) + "\n"
    os.makedirs(out, exist_ok=True)
    open(os.path.join(out, "report.txt"), "w").write(text)
    json.dump(rows, open(os.path.join(out, "results.json"), "w"))
    print(text)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    cmd, args = sys.argv[1], sys.argv[2:]
    if cmd == "gen":
        cmd_gen(args)
    elif cmd == "calibrate":
        cmd_calibrate(args)
    elif cmd == "run":
        cmd_run(args)
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
