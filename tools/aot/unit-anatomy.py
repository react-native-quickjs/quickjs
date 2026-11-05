#!/usr/bin/env python3
"""Size anatomy of a generated AOT unit: lines and bytes by kind of C function (brace-depth count), the largest functions, exit-stub count.

  unit-anatomy.py u.c [--top N] [--tsv]

Kinds are the generator's name prefixes: aot_s_/aot_f_ (speculative body), aot_g_ (generic body), aot_n_/aot_nb_/aot_nx_/aot_rx_ (entries, trampolines), e_*/q2_*/rg_* (helpers
re-emitted in every unit), everything else at file scope (prelude: macros, tables, declarations) is counted as 'other'.
"""
import re, sys, collections

def kind(name):
    for p, k in (("aot_s_", "spec"), ("aot_f_", "spec"), ("aot_g_", "generic"), ("aot_nx_", "tramp"), ("aot_nb_", "tramp"),
                 ("aot_rx_", "tramp"), ("aot_n_", "entry"), ("ei_", "inline"), ("e_", "helper"), ("q2_", "helper"), ("rg_", "helper"), ("qjs_aot_", "helper")):
        if name.startswith(p):
            return k
    return "other-fn"

def main():
    path = sys.argv[1]
    top = int(sys.argv[sys.argv.index("--top") + 1]) if "--top" in sys.argv else 10
    src = open(path, errors="replace").read().split("\n")
    funcs = []          # (name, start line, lines, bytes)
    depth = 0
    cur = None
    kinds = collections.Counter(); kbytes = collections.Counter()
    total_lines = len(src)
    in_fn_lines = 0
    for i, line in enumerate(src):
        if depth == 0 and cur is None:
            m = re.match(r'^(?:E_TEXT\s+)?(?:static\s+)?(?:inline\s+)?(?:__attribute__\(\([^)]*\)\)\s+)*[A-Za-z_][\w\s\*]*?\b([A-Za-z_]\w*)\s*\([^;]*$', line)
            if m and "{" not in line and not line.strip().startswith("#"):
                # a definition starts on a following line with '{'; remember the candidate name
                cand = (m.group(1), i)
            else:
                cand = None
            if line.rstrip().endswith("{") and cand is None:
                m2 = re.match(r'^(?:E_TEXT\s+)?(?:static\s+)?(?:inline\s+)?[\w\s\*]*?\b([A-Za-z_]\w*)\s*\(.*\)\s*\{\s*$', line)
                if m2:
                    cur = [m2.group(1), i, 0, 0]
            elif cand is not None:
                pending = cand
        # brace counting (ignore braces inside strings/comments crudely: strip string literals)
        s = re.sub(r'"(\\.|[^"\\])*"', '""', line)
        s = re.sub(r"'(\\.|[^'\\])'", "''", s)
        s = re.sub(r'/\*.*?\*/', '', s)
        s = s.split("//")[0]
        opens = s.count("{"); closes = s.count("}")
        if cur is None and depth == 0 and opens > 0:
            # find name from preceding lines (up to 4 lines back)
            back = " ".join(src[max(0, i - 4):i + 1])
            m3 = re.findall(r'\b([A-Za-z_]\w*)\s*\([^()]*(?:\([^()]*\)[^()]*)*\)\s*(?:__attribute__\(\([^)]*\)\))?\s*\{?', back)
            nm = m3[-1] if m3 else "?"
            cur = [nm, i, 0, 0]
        depth += opens - closes
        if cur is not None:
            cur[3] += len(line) + 1
            if depth == 0:
                cur[2] = i - cur[1] + 1
                funcs.append(tuple(cur))
                kinds[kind(cur[0])] += cur[2]; kbytes[kind(cur[0])] += cur[3]
                in_fn_lines += cur[2]
                cur = None
    kinds["other-file-scope"] = total_lines - in_fn_lines
    kbytes["other-file-scope"] = sum(len(l) + 1 for l in src) - sum(kbytes.values())
    stubs = sum(1 for l in src if re.match(r'^\s*(vx|dx|ox|hx|rx|rp)_\d+:;?\s*$', l))
    if "--tsv" in sys.argv:
        print("\t".join([path, str(total_lines), str(sum(kbytes.values()))] + ["%s=%d" % (k, kinds[k]) for k in sorted(kinds)] + ["stubs=%d" % stubs, "maxfn=%d" % (max([f[2] for f in funcs]) if funcs else 0)]))
        return
    print("%s: %d lines, %.1f MB, %d C functions, %d exit-stub labels" % (path, total_lines, sum(kbytes.values()) / 1e6, len(funcs), stubs))
    print("%-18s %10s %6s %10s" % ("kind", "lines", "%", "MB"))
    for k, v in sorted(kinds.items(), key=lambda x: -x[1]):
        print("%-18s %10d %5.1f%% %10.2f" % (k, v, 100.0 * v / total_lines, kbytes[k] / 1e6))
    print("largest functions:")
    for f in sorted(funcs, key=lambda f: -f[2])[:top]:
        print("  %7d lines  %-60s (line %d)" % (f[2], f[0][:60], f[1] + 1))
    over = sum(1 for f in funcs if f[2] > 3000)
    print("functions over 3000 lines: %d" % over)

main()
