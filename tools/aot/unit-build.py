#!/usr/bin/env python3
"""Build a generated AOT unit (u.c) as several translation units compiled in parallel, with an object cache.

  unit-build.py u.c --out DIR --cc "clang -O2 ... -I..." [--budget 3000] [--jobs N] [--cache DIR]
                [--hot-cc "..."] [--hot FILE] [--stats] [--split-only]

The unit text is cut at the boundaries of its top-level items, not re-generated: every part is the whole unit with the
bodies of the functions it does not own removed, so the directives that scope macros around a body (define/undef pairs,
conditionals) keep their order. Per function (named by the 32-hex digest in its symbols):
  - the aot_* functions lose `static` (hidden visibility) so parts can call each other; their prototypes stay everywhere;
  - the rg_/rk_/e_gfs_ statics and bodies of one function stay in the part that owns it;
  - ei_<hex> inline bodies go into the parts whose functions (transitively) use them.
The mutable file-scope state of the prelude (counters, registries, caches) becomes `extern`, defined once in rt.c, as do the
non-static definitions (tables, q2_cc_fill) and destructors, so no part has a private copy.
Functions are grouped into parts of about --budget lines: a function bigger than half the budget is a part of its own, the rest
are assigned by a hash of the digest, so a function keeps its part when other functions change. A part's object is cached under
sha256(part text, compiler command, compiler version); an unchanged part is not compiled again.
"""
import argparse, hashlib, os, re, subprocess, sys, time, concurrent.futures as cf

HEX = re.compile(r'[0-9a-f]{32}')
ATTR = re.compile(r'__attribute__\s*\(\((?:[^()]|\([^()]*\))*\)\)')


def strip_code(line, st):
    """Remove strings, chars and comments from one line (st = [in_block_comment]); keeps brace structure."""
    out = []
    i = 0
    n = len(line)
    while i < n:
        c = line[i]
        if st[0]:
            j = line.find('*/', i)
            if j < 0:
                return ''.join(out)
            st[0] = False
            i = j + 2
            continue
        if c == '/' and i + 1 < n and line[i + 1] == '*':
            st[0] = True
            i += 2
            continue
        if c == '/' and i + 1 < n and line[i + 1] == '/':
            break
        if c == '"' or c == "'":
            q = c
            i += 1
            while i < n and line[i] != q:
                i += 2 if line[i] == '\\' else 1
            i += 1
            out.append('""' if q == '"' else "''")
            continue
        out.append(c)
        i += 1
    return ''.join(out)


class Item:
    def __init__(self, lines, kind):
        self.lines = lines
        self.kind = kind
        self.nlines = len(lines)
        self.name = None
        self.owner = None
        self.static = False
        self.head = ''
        self.text = '\n'.join(lines)


def parse_items(src):
    """Split the unit into top-level items: directives, blank lines and code (declarations/definitions)."""
    items = []
    st = [False]
    depth = 0
    cur = []
    cont = False
    cstack, depth_end = [], []
    for line in src:
        if not cur and depth == 0 and not st[0]:
            if cont or line.startswith('#'):
                items.append(Item([line], 'dir'))
                cont = line.rstrip().endswith('\\')
                continue
            if line.strip() == '':
                items.append(Item([line], 'blank'))
                continue
        st_was = st[0]
        if cur and line.startswith('#') and not st_was:
            # a directive inside an item: each conditional branch counts from the depth of its #if, so braces that
            # appear in both branches do not add up; the end of the conditional keeps the depth of the last branch
            d = line[1:].lstrip()
            if d.startswith('if'):
                cstack.append(depth)
            elif (d.startswith('else') or d.startswith('elif')) and cstack:
                depth_end.append(depth)
                depth = cstack[-1]
            elif d.startswith('endif') and cstack:
                cstack.pop()
                if depth_end:
                    depth_end.pop()
            cur.append(line)
            continue
        code = strip_code(line, st)
        if not cur and depth == 0 and not code.strip():
            items.append(Item([line], 'blank'))   # comment-only line between items
            continue
        cur.append(line)
        depth += code.count('{') - code.count('}')
        s = code.strip()
        if depth == 0 and not st[0] and s and (s.endswith(';') or s.endswith('}')):
            items.append(Item(cur, 'code'))
            cur = []
    if cur:
        items.append(Item(cur, 'code'))
    return items


def classify(it):
    st = [False]
    stripped = '\n'.join(strip_code(l, st) for l in it.lines)
    m = re.search(r'[{=;]', stripped)
    head = stripped[:m.start()] if m else stripped
    term = stripped[m.start()] if m else ';'
    it.head = head
    it.static = bool(re.search(r'\bstatic\b', ATTR.sub(' ', head)))
    if re.match(r'\s*typedef\b', head) or re.match(r'\s*(struct|union|enum)\b[^()]*$', head):
        it.kind = 'type'
        return
    h = ATTR.sub(' ', head)
    is_fn = '(' in h and term in '{;'
    if is_fn:
        mm = re.search(r'([A-Za-z_]\w*)\s*\(', h)
        it.name = mm.group(1) if mm else None
        it.kind = 'fndef' if term == '{' else 'proto'
    else:
        if term == '{' and re.search(r'\b(?:struct|union|enum)(?:\s+\w+)?\s*$', h):
            # "static struct { ... } name[N];": the declarators follow the closing brace
            d, k = 0, stripped.index('{')
            for k2 in range(k, len(stripped)):
                d += (stripped[k2] == '{') - (stripped[k2] == '}')
                if d == 0:
                    break
            h = stripped[k2 + 1:].split(';')[0]
        names = re.findall(r'([A-Za-z_]\w*)\s*(?:\[[^\]]*\])*\s*(?:,|$)', h.strip())
        it.name = names[0] if names else None
        it.kind = 'data'
    hx = HEX.search(it.name or '')
    it.owner = hx.group(0) if hx else None


def split_top(text, sep):
    parts, depth, cur = [], 0, ''
    for c in text:
        if c in '([{':
            depth += 1
        elif c in ')]}':
            depth -= 1
        if c == sep and depth == 0:
            parts.append(cur)
            cur = ''
        else:
            cur += c
    parts.append(cur)
    return parts


def externalize_data(it):
    """static mutable data in the prelude -> 'extern decl; #ifdef DEFS def #endif' (initializers only on the definition)."""
    if not it.static or re.search(r'\bstatic\s+(?:_Thread_local\s+)?const\b', it.head):
        return None
    stmts = split_top(it.text, ';')
    if stmts[-1].strip():
        return None
    out_h, out_d = [], []
    for s in stmts[:-1]:
        s = s.strip()
        mm = re.match(r'static\s+(.*)$', s, re.S)
        if not mm:
            return None
        body = mm.group(1)
        decl = []
        for sg in split_top(body, ','):
            cut = split_top(sg, '=')
            decl.append(cut[0].rstrip())
        out_h.append('extern ' + ','.join(decl) + ';')
        out_d.append(body + ';')
    return '\n'.join(out_h) + '\n#ifdef QJS_AOT_STATE_DEFS\n' + '\n'.join(out_d) + '\n#endif'


def prototype_of(it):
    return it.text[:it.text.index('{')].rstrip() + ';'


def hide(txt):
    return re.sub(r'\bstatic\b', '__attribute__((visibility("hidden")))', txt, count=1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('unit')
    ap.add_argument('--out', required=True)
    ap.add_argument('--cc', required=True, help='compile command up to the file arguments (no -c/-o)')
    ap.add_argument('--hot-cc', default=None, help='compile command for the hot parts (default: --cc)')
    ap.add_argument('--hot', default=None, help='file with the digests (one per line) of hot functions')
    ap.add_argument('--report', default=None, help='the generator report: functions whose region holds at least --hot-min %% of the profile are hot')
    ap.add_argument('--hot-min', type=float, default=1.0)
    ap.add_argument('--budget', type=int, default=3000)
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    ap.add_argument('--cache', default=None)
    ap.add_argument('--stats', action='store_true')
    ap.add_argument('--split-only', action='store_true')
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    t0 = time.time()
    items = parse_items(open(args.unit, errors='replace').read().split('\n'))
    for it in items:
        if it.kind == 'code':
            classify(it)
    owners = {}
    ei = {}
    for it in items:
        if it.kind in ('fndef', 'data') and it.owner:
            if it.name.startswith('ei_'):
                ei[it.name] = it
            else:
                owners[it.owner] = owners.get(it.owner, 0) + it.nlines
    ei_refs = {n: set(re.findall(r'\bei_[0-9a-f]{32}\w*', it.text)) - {n} for n, it in ei.items()}
    total = sum(owners.values())
    ngroups = max(1, -(-total // args.budget))
    big = args.budget // 2
    hot = set(l.strip() for l in open(args.hot)) if args.hot else set()
    if args.report:
        cur_hex = None
        for l in open(args.report, errors='replace'):
            if l.startswith('root-hex '):
                cur_hex = l.split()[1]
            elif l.startswith('root ') and cur_hex:
                m = re.search(r'region ([\d.]+)%', l)
                if m and float(m.group(1)) >= args.hot_min:
                    hot.add(cur_hex)
                cur_hex = None
    parts = {}
    for h in sorted(owners):
        key = ('big', h, h in hot) if owners[h] > big else ('grp', int(h[:8], 16) % ngroups, h in hot)
        parts.setdefault(key, []).append(h)
    names = {k: (('b_' + k[1][:12]) if k[0] == 'big' else ('g%03d' % k[1])) + ('h' if k[2] else 'c') for k in parts}
    ei_use = {}
    for k, hs in parts.items():
        hs = set(hs)
        use = set()
        for it in items:
            if it.kind in ('fndef', 'data') and it.owner in hs and not it.name.startswith('ei_'):
                use |= set(re.findall(r'\bei_[0-9a-f]{32}\w*', it.text))
        todo = list(use)
        while todo:
            n = todo.pop()
            for m in ei_refs.get(n, ()):
                if m not in use:
                    use.add(m)
                    todo.append(m)
        ei_use[k] = use

    def emit_for(key):
        hs = set(parts[key]) if key else set()
        buf = []
        for it in items:
            if it.kind in ('dir', 'blank', 'type'):
                buf.append(it.text)
                continue
            nm = it.name or ''
            if it.kind == 'proto':
                if nm.startswith('aot_'):
                    buf.append(hide(it.text) if it.static else it.text)
                elif it.owner and it.owner not in hs:
                    continue
                else:
                    buf.append(it.text)
                continue
            if it.owner and nm.startswith('ei_'):
                if key is None or nm in ei_use[key]:
                    buf.append(it.text)
                continue
            if it.owner:
                if it.owner not in hs:
                    continue
                buf.append(hide(it.text) if it.kind == 'fndef' and nm.startswith('aot_') and it.static else it.text)
                continue
            if it.kind == 'data':
                ex = externalize_data(it)
                if ex is not None:
                    buf.append(ex)
                elif not it.static:
                    d = split_top(it.text, '=')[0].rstrip().rstrip(';').strip()
                    buf.append('extern ' + d + ';\n#ifdef QJS_AOT_STATE_DEFS\n' + it.text + '\n#endif')
                else:
                    buf.append(it.text)
                continue
            if it.kind == 'fndef':
                if re.search(r'\b(?:constructor|destructor)\b', ''.join(ATTR.findall(it.head))):
                    buf.append('#ifdef QJS_AOT_STATE_DEFS\n' + it.text + '\n#endif')
                elif not it.static and 'inline' not in it.head:
                    buf.append(prototype_of(it) + '\n#ifdef QJS_AOT_STATE_DEFS\n' + it.text + '\n#endif')
                else:
                    buf.append(it.text)
                continue
            buf.append(it.text)
        return '\n'.join(buf) + '\n'

    files = []
    rt_text = '#define QJS_AOT_STATE_DEFS 1\n' + emit_for(None)
    open(os.path.join(args.out, 'rt.c'), 'w').write(rt_text)
    files.append(('rt.c', False, rt_text.count('\n')))
    for k in sorted(parts):
        txt = emit_for(k)
        fn = names[k] + '.c'
        open(os.path.join(args.out, fn), 'w').write(txt)
        files.append((fn, k[2], txt.count('\n')))
    t_split = time.time() - t0
    if args.stats or args.split_only:
        print('split: %d parts + rt.c in %.1fs; owned lines %d, %d functions; largest part %d lines' % (
            len(parts), t_split, total, len(owners), max(f[2] for f in files)))
    if args.split_only:
        return 0
    cache = args.cache
    if cache:
        os.makedirs(cache, exist_ok=True)
    # A generated unit needs no stack canary (the frame records on its stacks made clang add one to every native entry: ~9 instructions a
    # call) and no per-entry statistics counter. QJS_UNIT_DEBUG=1 keeps both (the unit-stats report then counts entries).
    if not os.environ.get('QJS_UNIT_DEBUG'):
        extra = ' -fno-stack-protector -DQJS_AOT_NOENTNOTE=1'
        args.cc += extra
        if args.hot_cc:
            args.hot_cc += extra
    cc_ver = subprocess.run(args.cc.split()[0] + ' --version', shell=True, capture_output=True, text=True).stdout
    hot_cc = args.hot_cc or args.cc

    def build(fn, ishot):
        cc = hot_cc if ishot else args.cc
        src = os.path.join(args.out, fn)
        obj = src[:-2] + '.o'
        key = hashlib.sha256(open(src, 'rb').read() + b'\0' + cc.encode() + b'\0' + cc_ver.encode()).hexdigest()
        if cache:
            cobj = os.path.join(cache, key + '.o')
            if os.path.exists(cobj):
                subprocess.run(['cp', cobj, obj])
                return fn, 'hit', 0.0, obj
        t = time.time()
        r = subprocess.run('/usr/bin/time -l ' + cc + ' -c ' + src + ' -o ' + obj, shell=True, capture_output=True, text=True)
        dt = time.time() - t
        if r.returncode != 0:
            return fn, 'FAIL ' + '\n'.join(l for l in r.stderr.split('\n') if 'error' in l)[:800], dt, obj
        m = re.search(r'(\d+)\s+maximum resident set size', r.stderr)
        rss = int(m.group(1)) / 1048576 if m else 0
        if cache:
            subprocess.run(['cp', obj, cobj])
        return fn, 'built %.0fMB' % rss, dt, obj

    t1 = time.time()
    with cf.ThreadPoolExecutor(args.jobs) as ex:
        futs = [ex.submit(build, fn, ishot) for fn, ishot, _ in sorted(files, key=lambda f: -f[2])]
        res = [f.result() for f in futs]
    wall = time.time() - t1
    bad = [r for r in res if r[1].startswith('FAIL')]
    for r in bad[:6]:
        print(r[0], r[1])
    hits = sum(1 for r in res if r[1] == 'hit')
    peak = max([float(re.search(r'([\d.]+)MB', r[1]).group(1)) for r in res if 'MB' in r[1]] or [0])
    print('build: %d parts (%d cached) wall %.1fs, split %.1fs, largest part rss %.0fMB, %s' % (
        len(res), hits, wall, t_split, peak, 'FAILED' if bad else 'ok'))
    open(os.path.join(args.out, 'objs.txt'), 'w').write('\n'.join(r[3] for r in res) + '\n')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
