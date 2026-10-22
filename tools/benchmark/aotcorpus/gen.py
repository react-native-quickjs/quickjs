#!/usr/bin/env python3
"""Generate the templated half of the AOT corpus.

    gen.py OUTDIR [--calib calib.json] [--list]

Every program is a small, deterministic JS file that prints one checksum line.
The templates cross an operation (or a call form, or a shape) with a type and a
context, so a lowering that is only wrong for one combination has a program that
reaches it. Iteration counts come from `calib.json` (written by `run.py
--calibrate`) so that each program runs at least 50 ms in the interpreter.

Nothing here is copied from another project's tests: the families below were
written from the list of language features and op shapes the compiler lowers.
"""
import json
import os
import re as _re
import sys

PRE = """var N = %(n)d;
var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) %% 1000003); }
function mixs(s) { for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
"""

POST = "print(%(name)r + ' ' + h);\n"

PROGRAMS = []  # (name, base_n, body)


def add(name, n, body):
    PROGRAMS.append((name, n, body))


# ---- operand expressions ---------------------------------------------------

def operand(ty, i, k):
    """An expression of type `ty` computed from the loop index `i`, salted by `k`."""
    if ty == "int":
        return "(((%s * %d) & 1023) + 1)" % (i, 3 + 2 * k)
    if ty == "dbl":
        return "(((%s * %d) & 1023) * 0.5 + 0.25)" % (i, 3 + 2 * k)
    if ty == "mix":
        return "((%s & 1) ? (((%s * %d) & 1023) + 1) : (((%s * %d) & 1023) * 0.5 + 0.25))" % (
            i, i, 3 + 2 * k, i, 3 + 2 * k)
    if ty == "big":
        return "(((%s * %d) & 1023) + 1073741824)" % (i, 3 + 2 * k)
    if ty == "neg":
        return "(-(((%s * %d) & 1023) + 1))" % (i, 3 + 2 * k)
    raise ValueError(ty)


def sink(ty):
    return "mixd(s)" if ty in ("dbl", "mix", "big") else "mix(s)"


BINOPS = ["+", "-", "*", "/", "%", "&", "|", "^", "<<", ">>", ">>>"]
CMPOPS = ["<", "<=", ">", ">=", "==", "===", "!=", "!=="]
TYPES = ["int", "dbl", "mix", "big", "neg"]


# ---- family 1: arithmetic, op x type x context ------------------------------

def fam_arith():
    for op in BINOPS:
        for ty in TYPES:
            for ctx in ["local", "elem", "field", "closure", "arg", "global"]:
                x = operand(ty, "i", 0)
                y = operand(ty, "i", 1)
                name = "arith_%s_%s_%s" % (OPNAME[op], ty, ctx)
                if ctx == "local":
                    body = ("function work(n) { var s = 0, i;\n"
                            "  for (i = 0; i < n; i++) { var x = %s, y = %s; s = x %s y; %s; }\n"
                            "  return s; }\nwork(N);\n") % (x, y, op, sink(ty))
                elif ctx == "elem":
                    body = ("var A = [], B = [];\nfor (var j = 0; j < 256; j++) { var i = j; A.push(%s); B.push(%s); }\n"
                            "function work(n) { var s = 0, i;\n"
                            "  for (i = 0; i < n; i++) { s = A[i & 255] %s B[(i + 3) & 255]; %s; }\n"
                            "  return s; }\nwork(N);\n") % (x, y, op, sink(ty))
                elif ctx == "field":
                    body = ("function P(a, b) { this.x = a; this.y = b; }\nvar ps = [];\n"
                            "for (var j = 0; j < 64; j++) { var i = j; ps.push(new P(%s, %s)); }\n"
                            "function work(n) { var s = 0, i;\n"
                            "  for (i = 0; i < n; i++) { var p = ps[i & 63]; s = p.x %s p.y; %s; }\n"
                            "  return s; }\nwork(N);\n") % (x, y, op, sink(ty))
                elif ctx == "closure":
                    body = ("function mk() { var a = 0, b = 0;\n"
                            "  return function (i) { a = %s; b = %s; return a %s b; }; }\n"
                            "var f = mk();\nfunction work(n) { var s = 0, i;\n"
                            "  for (i = 0; i < n; i++) { s = f(i); %s; }\n  return s; }\nwork(N);\n") % (
                                x, y, op, sink(ty))
                elif ctx == "arg":
                    body = ("function f(a, b) { return a %s b; }\n"
                            "function work(n) { var s = 0, i;\n"
                            "  for (i = 0; i < n; i++) { s = f(%s, %s); %s; }\n  return s; }\nwork(N);\n") % (
                                op, x, y, sink(ty))
                else:
                    body = ("var ga = 0, gb = 0;\nfunction work(n) { var s = 0, i;\n"
                            "  for (i = 0; i < n; i++) { ga = %s; gb = %s; s = ga %s gb; %s; }\n"
                            "  return s; }\nwork(N);\n") % (x, y, op, sink(ty))
                add(name, 1500000, body)


OPNAME = {"+": "add", "-": "sub", "*": "mul", "/": "div", "%": "mod", "&": "and", "|": "or",
          "^": "xor", "<<": "shl", ">>": "sar", ">>>": "shr", "<": "lt", "<=": "le", ">": "gt",
          ">=": "ge", "==": "eq", "===": "seq", "!=": "ne", "!==": "sne"}


# ---- family 2: comparisons and branches --------------------------------------

def fam_compare():
    pairs = {
        "ii": ("(i & 63)", "((i >> 2) & 63)"),
        "dd": ("((i & 63) * 0.5)", "(((i >> 2) & 63) * 0.5)"),
        "id": ("(i & 63)", "(((i >> 2) & 63) * 0.5)"),
        "ss": ("('k' + (i & 15))", "('k' + ((i >> 1) & 15))"),
        "nu": ("((i & 3) === 0 ? null : (i & 3))", "((i & 4) ? undefined : (i & 3))"),
        "oo": ("objs[i & 7]", "objs[(i >> 1) & 7]"),
    }
    forms = ["if", "ternary", "while", "andor", "count"]
    for op in CMPOPS:
        for pk, (a, b) in pairs.items():
            if pk == "oo" and op not in ("==", "===", "!=", "!=="):
                continue
            if pk == "ss" and op in ("<", "<=", ">", ">="):
                pass
            for form in forms:
                name = "cmp_%s_%s_%s" % (OPNAME[op], pk, form)
                pre = "var objs = []; for (var j = 0; j < 8; j++) objs.push({ v: j });\n"
                if form == "if":
                    loop = "if (%s %s %s) s += 3; else s -= 1;" % (a, op, b)
                elif form == "ternary":
                    loop = "s += (%s %s %s) ? i & 7 : 2;" % (a, op, b)
                elif form == "while":
                    loop = "var k = 0; while (k < 3 && (%s %s %s)) { k++; s += k; } s += k;" % (a, op, b)
                elif form == "andor":
                    loop = "if ((%s %s %s) && (i & 1)) s += 5; else if ((%s %s %s) || (i & 2)) s += 7;" % (
                        a, op, b, b, op, a)
                else:
                    loop = "s += +((%s) %s (%s));" % (a, op, b)
                body = pre + ("function work(n) { var s = 0, i;\n  for (i = 0; i < n; i++) { %s mix(s); }\n  return s; }\nwork(N);\n" % loop)
                add(name, 1200000, body)


# ---- family 3: unary and miscellaneous ops -----------------------------------

def fam_unary():
    exprs = {
        "neg": "-x", "plus": "+x", "bnot": "~x", "not": "!x", "typeof": "typeof x",
        "inc": "x + 1", "dec": "x - 1", "void": "void x", "pow": "x ** 2", "pow3": "Math.pow(x, 3)",
        "tonum": "x * 1", "toint": "x | 0", "abs": "x < 0 ? -x : x", "sign": "x > 0 ? 1 : x < 0 ? -1 : 0",
        "comma": "(x, x + 2)", "nullish": "(x == null ? 1 : x)", "in": "('a' in obj) ? x : 0",
        "instanceof": "(obj instanceof Object) ? x : 0",
    }
    for key, e in exprs.items():
        for ty in ["int", "dbl", "mix", "neg", "big"]:
            body = ("var obj = { a: 1 };\nfunction work(n) { var s = 0, i;\n"
                    "  for (i = 0; i < n; i++) { var x = %s; var r = %s; "
                    "if (typeof r === 'string') mixs(r); else if (typeof r === 'boolean') mix(r ? 1 : 0); "
                    "else if (r === undefined) mix(9); else mixd(r); }\n  return s; }\nwork(N);\n") % (
                        operand(ty, "i", 0), e)
            add("unary_%s_%s" % (key, ty), 1200000, body)


# ---- family 4: cross of two ops (expression chains) ---------------------------

def fam_chain():
    ops = ["+", "-", "*", "&", "|", "^", "<<", ">>"]
    for a in ops:
        for b in ops:
            for ty in ["int", "dbl", "mix"]:
                x, y, z = operand(ty, "i", 0), operand(ty, "i", 1), operand(ty, "i", 2)
                body = ("function work(n) { var s = 0, i;\n"
                        "  for (i = 0; i < n; i++) { var x = %s, y = %s, z = %s; s = (x %s y) %s z; %s; }\n"
                        "  return s; }\nwork(N);\n") % (x, y, z, a, b, sink(ty))
                add("chain_%s_%s_%s" % (OPNAME[a], OPNAME[b], ty), 1200000, body)


# ---- family 5: property access -------------------------------------------------

def fam_props():
    for nf in [1, 2, 3, 4, 6, 8, 12, 16]:
        fields = ["f%d" % k for k in range(nf)]
        ctor = "function C(v) { %s }\n" % " ".join("this.%s = v + %d;" % (f, k) for k, f in enumerate(fields))
        lit = "function mkLit(v) { return { %s }; }\n" % ", ".join("%s: v + %d" % (f, k) for k, f in enumerate(fields))
        cls = "class K { constructor(v) { %s } }\n" % " ".join("this.%s = v + %d;" % (f, k) for k, f in enumerate(fields))
        first, last = fields[0], fields[-1]
        for recv, mk in [("ctor", "new C(i & 15)"), ("lit", "mkLit(i & 15)"), ("class", "new K(i & 15)")]:
            decl = {"ctor": ctor, "lit": lit, "class": cls}[recv]
            for op in ["read", "write", "rmw", "readall", "chain"]:
                if op == "read":
                    stmt = "s += o.%s + o.%s;" % (first, last)
                elif op == "write":
                    stmt = "o.%s = i; o.%s = i + 1; s += o.%s;" % (first, last, last)
                elif op == "rmw":
                    stmt = "o.%s += 2; o.%s++; s += o.%s;" % (first, last, first)
                elif op == "readall":
                    stmt = "s += %s;" % " + ".join("o.%s" % f for f in fields)
                else:
                    stmt = "var p = { q: o }; s += p.q.%s + p.q.%s;" % (first, last)
                body = (decl + "var pool = []; for (var j = 0; j < 32; j++) { var i = j; pool.push(%s); }\n"
                        "function work(n) { var s = 0, i;\n  for (i = 0; i < n; i++) { var o = pool[i & 31]; %s mix(s); }\n  return s; }\nwork(N);\n") % (mk, stmt)
                add("prop_%dfields_%s_%s" % (nf, recv, op), 1500000, body)
    # prototype chains of depth 0..3, method and data
    for depth in range(0, 4):
        for kind in ["data", "method", "accessor"]:
            chain = "function B0() { this.own = 1; }\nB0.prototype.val = 5;\nB0.prototype.meth = function () { return this.own + 2; };\n"
            chain += "Object.defineProperty(B0.prototype, 'acc', { get: function () { return this.own + 3; } });\n"
            for d in range(1, depth + 1):
                chain += "function B%d() { B%d.call(this); }\nB%d.prototype = Object.create(B%d.prototype);\n" % (d, d - 1, d, d - 1)
            top = "B%d" % depth
            stmt = {"data": "s += o.val + o.own;", "method": "s += o.meth() + o.own;", "accessor": "s += o.acc;"}[kind]
            body = chain + ("var o = new %s();\nfunction work(n) { var s = 0, i;\n  for (i = 0; i < n; i++) { %s mix(s); }\n  return s; }\nwork(N);\n") % (top, stmt)
            add("prop_proto%d_%s" % (depth, kind), 1500000, body)
    # polymorphic receivers: k distinct shapes at one site
    for k in [1, 2, 3, 4, 5, 8, 12]:
        for op in ["read", "write", "call"]:
            makers = []
            for s in range(k):
                pad = "".join("pad%d_%d: 0, " % (s, t) for t in range(s % 3))
                makers.append("{ %sx: %d, y: %d, get: function () { return this.x + %d; } }" % (pad, s, s + 1, s))
            stmt = {"read": "s += o.x + o.y;", "write": "o.x = i & 7; s += o.x;", "call": "s += o.get();"}[op]
            body = ("var objs = [%s];\nfunction work(n) { var s = 0, i;\n  for (i = 0; i < n; i++) { var o = objs[i %% %d]; %s mix(s); }\n  return s; }\nwork(N);\n") % (
                ", ".join(makers), k, stmt)
            add("prop_poly%d_%s" % (k, op), 1500000, body)
    # getters / setters / defineProperty / delete / in / hasOwnProperty
    forms = {
        "getter": ("var o = { v: 3, get g() { return this.v * 2; } };", "s += o.g;"),
        "setter": ("var o = { v: 3, set g(x) { this.v = x + 1; } };", "o.g = i & 15; s += o.v;"),
        "defprop": ("var o = {}; Object.defineProperty(o, 'p', { value: 11, writable: false });", "s += o.p;"),
        "frozen": ("var o = Object.freeze({ a: 4, b: 5 });", "s += o.a + o.b;"),
        "delete": ("var o = { a: 1, b: 2, c: 3 };", "var t = { a: i, b: 2 }; delete t.a; s += (t.a === undefined ? 1 : 0) + t.b;"),
        "in": ("var o = { a: 1, b: 2 };", "s += ('a' in o ? 1 : 0) + ('z' in o ? 1 : 0);"),
        "hasown": ("var o = { a: 1, b: 2 };", "s += (o.hasOwnProperty('a') ? 1 : 0) + (o.hasOwnProperty('q') ? 1 : 0);"),
        "dynkey": ("var o = { k0: 1, k1: 2, k2: 3, k3: 4 }; var keys = ['k0', 'k1', 'k2', 'k3'];", "s += o[keys[i & 3]];"),
        "computed": ("var o = {};", "o['a' + (i & 7)] = i; s += o['a' + ((i + 1) & 7)] | 0;"),
        "symbol": ("var sy = Symbol('s'); var o = {}; o[sy] = 5;", "s += o[sy];"),
        "protoset": ("function F() {} F.prototype.z = 1; var o = new F();", "o.z = i & 3; s += o.z;"),
        "undefprop": ("var o = { a: 1 };", "s += o.nothing === undefined ? 1 : 0;"),
    }
    for key, (decl, stmt) in forms.items():
        body = decl + "\nfunction work(n) { var s = 0, i;\n  for (i = 0; i < n; i++) { %s mix(s); }\n  return s; }\nwork(N);\n" % stmt
        add("prop_special_%s" % key, 1000000, body)


# ---- family 6: element access ----------------------------------------------------

def fam_elems():
    kinds = {
        "packed_int": "var a = []; for (var j = 0; j < 256; j++) a.push(j * 3 + 1);",
        "packed_dbl": "var a = []; for (var j = 0; j < 256; j++) a.push(j * 0.5 + 0.25);",
        "mixed": "var a = []; for (var j = 0; j < 256; j++) a.push(j & 1 ? j : 'e' + j);",
        "holey": "var a = new Array(256); for (var j = 0; j < 256; j += 2) a[j] = j + 1;",
        "sparse": "var a = []; a[0] = 1; a[100000] = 2; for (var j = 1; j < 256; j++) a[j] = j;",
        "objects": "var a = []; for (var j = 0; j < 256; j++) a.push({ v: j });",
        "string": "var a = 'abcdefghijklmnopqrstuvwxyz0123456789'.repeat(8);",
        "arguments": "function ag() { return arguments; } var a = ag.apply(null, [1, 2, 3, 4, 5, 6, 7, 8]);",
        "twod": "var a = []; for (var j = 0; j < 16; j++) { var r = []; for (var k = 0; k < 16; k++) r.push(j * k); a.push(r); }",
    }
    ops = {
        "read": "s += a[i & 255] | 0;",
        "write": "a[i & 255] = i;",
        "rmw": "a[i & 127] += 3;",
        "oob": "s += (a[i & 511] === undefined) ? 1 : 0;",
        "length": "s += a.length;",
        "negidx": "s += (a[-(i & 3)] === undefined ? 1 : 0);",
        "strkey": "s += (a['' + (i & 63)] | 0);",
        "dblkey": "s += (a[(i & 63) + 0.0] | 0);",
    }
    for kk, decl in kinds.items():
        for ok, stmt in ops.items():
            if kk == "string" and ok in ("write", "rmw"):
                continue
            if kk == "twod":
                stmt = {"read": "s += a[i & 15][(i >> 4) & 15];", "write": "a[i & 15][(i >> 4) & 15] = i;",
                        "rmw": "a[i & 15][3] += 1;", "oob": "s += a[(i & 31)] === undefined ? 1 : 0;",
                        "length": "s += a[0].length;", "negidx": "s += a[-1] === undefined ? 1 : 0;",
                        "strkey": "s += a['3'][2];", "dblkey": "s += a[(i & 7) + 0.0][1];"}[ok]
            if kk == "string":
                stmt = stmt.replace("a[i & 255] | 0", "a.charCodeAt(i & 255)").replace("a[i & 255]", "a.charCodeAt(i & 255)")
            body = decl + "\nfunction work(n) { var s = 0, i;\n  for (i = 0; i < n; i++) { %s mix(s); }\n  return s; }\nwork(N);\n" % stmt
            add("elem_%s_%s" % (kk, ok), 1500000, body)
    for ty in ["Int8", "Uint8", "Uint8Clamped", "Int16", "Uint16", "Int32", "Uint32", "Float32", "Float64"]:
        for ok, stmt in {
            "read": "s += a[i & 255];", "write": "a[i & 255] = i * 3;", "rmw": "a[i & 63] += 7;",
            "fill": "a[(i * 5) & 255] = (i & 127) - 5;", "len": "s += a.length;", "oob": "a[300 + (i & 3)] = 1; s += a[400] === undefined ? 1 : 0;",
        }.items():
            body = ("var a = new %sArray(256);\nfunction work(n) { var s = 0, i;\n  for (i = 0; i < n; i++) { %s mixd(s); }\n  return s; }\nwork(N);\n") % (ty, stmt)
            add("typed_%s_%s" % (ty.lower(), ok), 1500000, body)
    # push / pop / shift patterns
    for key, stmt in {
        "push": "a.push(i); if (a.length > 64) a.length = 0; s += a.length;",
        "pushpop": "a.push(i); s += a.pop();",
        "unshift": "a.unshift(i); if (a.length > 16) a.length = 4; s += a[0];",
        "shift": "a.push(i, i + 1); s += a.shift() + a.shift();",
        "splice": "a.splice(1, 0, i); if (a.length > 32) a.splice(0, 16); s += a.length;",
        "slice": "s += a.slice(2, 6).length + a.length;",
        "concat": "s += a.concat([i]).length;",
        "indexof": "s += a.indexOf(i & 15);",
        "reverse": "a.reverse(); s += a[0];",
        "join": "s += a.join(',').length;",
        "spread": "s += [...a, i].length;",
    }.items():
        body = ("var a = [1, 2, 3, 4, 5, 6, 7, 8];\nfunction work(n) { var s = 0, i;\n  for (i = 0; i < n; i++) { %s mix(s); }\n  return s; }\nwork(N);\n") % stmt
        add("array_method_%s" % key, 500000, body)


# ---- family 7: calls ---------------------------------------------------------------

def fam_calls():
    for arity in range(0, 7):
        params = ", ".join("p%d" % k for k in range(arity))
        args = ", ".join("(i + %d)" % k for k in range(arity))
        use = " + ".join("p%d" % k for k in range(arity)) if arity else "7"
        decl = "function f(%s) { return %s; }\n" % (params, use)
        forms = {
            "direct": "s += f(%s);" % args,
            "method": "s += o.m(%s);" % args,
            "call": "s += f.call(null%s);" % ("".join(", (i + %d)" % k for k in range(arity))),
            "apply": "s += f.apply(null, [%s]);" % args,
            "bind": "s += g(%s);" % args,
            "closure": "s += c(%s);" % args,
            "new": "s += new K(%s).v;" % args,
            "spread": "s += f(...[%s]);" % args,
            "arrow": "s += af(%s);" % args,
            "nested": "s += f(%s) + f(%s);" % (args, args),
            "tail": "s += tail(%s);" % args,
            "cond": "s += (i & 1 ? f : f2)(%s);" % args,
            "default": "s += d(%s);" % args,
            "rest": "s += r(%s);" % args,
            "argsobj": "s += ao(%s);" % args,
        }
        extra = ("var o = { m: f };\nvar g = f.bind(null);\nvar c = (function () { var t = 1; return function (%s) { return f(%s) + t - 1; }; })();\n"
                 "function K(%s) { this.v = %s; }\nvar af = (%s) => %s;\nfunction f2(%s) { return %s + 1; }\n"
                 "function tail(%s) { return f(%s); }\nfunction d(%s) { return f(%s); }\n"
                 "function r(...xs) { var t = 0; for (var q = 0; q < xs.length; q++) t += xs[q]; return t; }\n"
                 "function ao() { var t = 0; for (var q = 0; q < arguments.length; q++) t += arguments[q]; return t; }\n") % (
                     params, params, params, use, params, use, params, use, params, params, params, params)
        for fk, stmt in forms.items():
            body = decl + extra + "function work(n) { var s = 0, i;\n  for (i = 0; i < n; i++) { %s mix(s); }\n  return s; }\nwork(N);\n" % stmt
            add("call_%s_a%d" % (fk, arity), 1000000, body)
    # recursion
    for kind, decl, call, nn in [
        ("fib", "function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }", "fib(%d)", [20, 22, 24]),
        ("tak", "function tak(x, y, z) { return y >= x ? z : tak(tak(x - 1, y, z), tak(y - 1, z, x), tak(z - 1, x, y)); }", "tak(%d, %d, %d)", [(14, 8, 3), (16, 10, 4)]),
        ("ack", "function ack(m, n) { return m === 0 ? n + 1 : n === 0 ? ack(m - 1, 1) : ack(m - 1, ack(m, n - 1)); }", "ack(2, %d)", [60, 120]),
        ("mutual", "function ev(n) { return n === 0 ? 1 : od(n - 1); } function od(n) { return n === 0 ? 0 : ev(n - 1); }", "ev(%d)", [200, 400]),
        ("sum", "function sum(n) { return n === 0 ? 0 : n + sum(n - 1); }", "sum(%d)", [200, 500]),
        ("hanoi", "function hanoi(n, a, b, c) { return n === 0 ? 0 : hanoi(n - 1, a, c, b) + 1 + hanoi(n - 1, c, b, a); }", "hanoi(%d, 1, 2, 3)", [10, 12]),
        ("depth", "function dp(n, acc) { return n === 0 ? acc : dp(n - 1, acc + (n & 3)); }", "dp(%d, 0)", [300, 600]),
    ]:
        for k, a in enumerate(nn):
            c = call % (a if isinstance(a, tuple) else (a,))
            reps = 40 if kind in ("fib", "tak", "hanoi") else 400
            body = decl + "\nfunction work(n) { var s = 0, i;\n  for (i = 0; i < n; i++) { s += %s; mix(s); }\n  return s; }\nwork(N);\n" % c
            add("rec_%s_%d" % (kind, k), reps, body)
    # callbacks through builtins
    for meth, stmt in {
        "map": "s += a.map(function (x) { return x + i; }).length;",
        "forEach": "a.forEach(function (x) { s += x & 7; });",
        "reduce": "s += a.reduce(function (p, x) { return p + x; }, i);",
        "filter": "s += a.filter(function (x) { return (x + i) & 1; }).length;",
        "some": "s += a.some(function (x) { return x > 30 + (i & 7); }) ? 1 : 0;",
        "every": "s += a.every(function (x) { return x < 100; }) ? 1 : 0;",
        "find": "s += a.find(function (x) { return x > 20 + (i & 7); });",
        "sort": "s += a.slice().sort(function (p, q) { return q - p; })[0];",
        "arrow": "s += a.map((x) => x * 2).reduce((p, x) => p + x, 0);",
        "flatmap": "s += a.flatMap(function (x) { return [x, x]; }).length;",
        "from": "s += Array.from({ length: 8 }, function (v, k) { return k + i; }).length;",
    }.items():
        body = ("var a = [3, 9, 27, 4, 1, 77, 5, 8, 13, 2, 21, 34];\nfunction work(n) { var s = 0, i;\n  for (i = 0; i < n; i++) { %s mix(s); }\n  return s; }\nwork(N);\n") % stmt
        add("callback_%s" % meth, 100000, body)


# ---- family 8: closures ------------------------------------------------------------

def fam_closures():
    for depth in range(1, 5):
        for shared in ["counter", "readonly", "writeback", "arrayfill"]:
            outer = ""
            inner_open = ""
            for d in range(depth):
                outer += "function L%d() { var v%d = %d;\n" % (d, d, d + 1)
            body_stmt = {
                "counter": "v0 += 1; return v0 + %s;" % "+".join("v%d" % d for d in range(depth)),
                "readonly": "return %s;" % "+".join("v%d" % d for d in range(depth)),
                "writeback": "".join("v%d = (v%d + x) & 1023; " % (d, d) for d in range(depth)) + "return v0;",
                "arrayfill": "arr[x & 7] = v0 + x; return arr[(x + 1) & 7];",
            }[shared]
            code = ""
            for d in range(depth):
                code += "function L%d(x%s) { var v%d = x + %d;\n" % (d, "" if d == depth - 1 else "", d, d + 1)
            code += "var arr = [0, 0, 0, 0, 0, 0, 0, 0];\n"
            # build nested definitions
            def build(d):
                if d == depth:
                    return "return function (x) { %s };" % body_stmt
                return "return (function () { var v%d = %d; %s })();" % (d, d + 2, build(d + 1))
            prog = "function mk() { var v0 = 1; var arr = [0,0,0,0,0,0,0,0]; " + build(1) + " }\n" if depth > 1 else \
                   "function mk() { var v0 = 1; var arr = [0,0,0,0,0,0,0,0]; return function (x) { %s }; }\n" % body_stmt
            body = prog + "var f = mk();\nfunction work(n) { var s = 0, i;\n  for (i = 0; i < n; i++) { s += f(i); mix(s); }\n  return s; }\nwork(N);\n"
            add("closure_d%d_%s" % (depth, shared), 1500000, body)
    for key, body in {
        "loopvar_let": "var fs = []; for (let k = 0; k < 8; k++) fs.push(function () { return k * 2; });\nfunction work(n) { var s = 0, i; for (i = 0; i < n; i++) { s += fs[i & 7](); mix(s); } return s; }\nwork(N);\n",
        "loopvar_var": "var fs = []; for (var k = 0; k < 8; k++) fs.push((function (q) { return function () { return q * 2; }; })(k));\nfunction work(n) { var s = 0, i; for (i = 0; i < n; i++) { s += fs[i & 7](); mix(s); } return s; }\nwork(N);\n",
        "iife": "function work(n) { var s = 0, i; for (i = 0; i < n; i++) { s += (function (a) { return a * 3; })(i); mix(s); } return s; }\nwork(N);\n",
        "counter_obj": "function mk() { var c = 0; return { inc: function () { return ++c; }, get: function () { return c; } }; }\nvar o = mk();\nfunction work(n) { var s = 0, i; for (i = 0; i < n; i++) { o.inc(); s += o.get() & 15; mix(s); } return s; }\nwork(N);\n",
        "compose": "function compose(f, g) { return function (x) { return f(g(x)); }; }\nvar inc = function (x) { return x + 1; }, dbl = function (x) { return x * 2; };\nvar h2 = compose(inc, compose(dbl, inc));\nfunction work(n) { var s = 0, i; for (i = 0; i < n; i++) { s += h2(i & 255); mix(s); } return s; }\nwork(N);\n",
        "curry": "function add(a) { return function (b) { return function (c) { return a + b + c; }; }; }\nfunction work(n) { var s = 0, i; for (i = 0; i < n; i++) { s += add(i)(2)(3); mix(s); } return s; }\nwork(N);\n",
        "memo": "function memo(f) { var c = {}; return function (x) { return x in c ? c[x] : (c[x] = f(x)); }; }\nvar sq = memo(function (x) { return x * x; });\nfunction work(n) { var s = 0, i; for (i = 0; i < n; i++) { s += sq(i & 255); mix(s); } return s; }\nwork(N);\n",
        "thisarg": "var o = { v: 4, get: function () { var self = this; return function () { return self.v; }; } };\nvar g = o.get();\nfunction work(n) { var s = 0, i; for (i = 0; i < n; i++) { s += g(); mix(s); } return s; }\nwork(N);\n",
        "generator_closure": "function mk() { var c = 0; return function () { return ++c; }; }\nvar nx = mk();\nfunction work(n) { var s = 0, i; for (i = 0; i < n; i++) { s += nx(); mix(s); } return s; }\nwork(N);\n",
    }.items():
        add("closure_%s" % key, 1500000, body)


# ---- family 9: allocation ---------------------------------------------------------

def fam_alloc():
    for nf in list(range(1, 17)) + [20, 24, 32]:
        fields = ", ".join("p%d: i + %d" % (k, k) for k in range(nf))
        body = ("function work(n) { var s = 0, i;\n  for (i = 0; i < n; i++) { var o = { %s }; s += o.p0 + o.p%d; mix(s); }\n  return s; }\nwork(N);\n") % (fields, nf - 1)
        add("alloc_literal_%d" % nf, 600000, body)
        ctor_body = " ".join("this.p%d = a + %d;" % (k, k) for k in range(nf))
        body = ("function C(a) { %s }\nfunction work(n) { var s = 0, i;\n  for (i = 0; i < n; i++) { var o = new C(i); s += o.p0 + o.p%d; mix(s); }\n  return s; }\nwork(N);\n") % (ctor_body, nf - 1)
        add("alloc_ctor_%d" % nf, 600000, body)
        body = ("class K { constructor(a) { %s } get sum() { return this.p0 + this.p%d; } }\nfunction work(n) { var s = 0, i;\n  for (i = 0; i < n; i++) { var o = new K(i); s += o.sum; mix(s); }\n  return s; }\nwork(N);\n") % (ctor_body, nf - 1)
        add("alloc_class_%d" % nf, 600000, body)
    for key, stmt in {
        "array_lit": "var a = [i, i + 1, i + 2, i + 3]; s += a[3];",
        "array_nested": "var a = [[i], [i + 1, i + 2], [[i]]]; s += a[1][1] + a[2][0][0];",
        "array_new": "var a = new Array(8); a[7] = i; s += a[7];",
        "array_fill": "var a = new Array(16).fill(i & 3); s += a[15];",
        "obj_create": "var o = Object.create({ z: 1 }); o.y = i; s += o.z + o.y;",
        "obj_assign": "var o = Object.assign({}, { a: i, b: 2 }); s += o.a + o.b;",
        "obj_spread": "var b = { a: i, b: 2 }; var o = { ...b, c: 3 }; s += o.a + o.c;",
        "arr_spread": "var b = [i, 2, 3]; var o = [...b, 4]; s += o[0] + o[3];",
        "string_build": "var t = 'a' + i + 'b'; s += t.length;",
        "closure_alloc": "var f = function () { return i; }; s += f();",
        "template": "var t = `v${i}-${i + 1}`; s += t.length;",
        "map_new": "var m = new Map(); m.set(i & 7, i); s += m.get(i & 7);",
        "set_new": "var m = new Set(); m.add(i & 7); m.add(3); s += m.size;",
        "date_free": "var o = { d: [i, i], e: { f: i } }; s += o.d[0] + o.e.f;",
        "json_small": "var o = JSON.parse('{\"a\":' + (i & 63) + ',\"b\":[1,2]}'); s += o.a + o.b[1];",
        "array_from": "var a = Array.from([i, 2, 3]); s += a.length + a[0];",
        "array_of": "var a = Array.of(i, 2); s += a[0] + a[1];",
        "bigarray": "var a = new Array(64); for (var q = 0; q < 64; q++) a[q] = q; s += a[63];",
    }.items():
        add("alloc_%s" % key, 400000, "function work(n) { var s = 0, i;\n  for (i = 0; i < n; i++) { %s mix(s); }\n  return s; }\nwork(N);\n" % stmt)
    for depth in [1, 2, 3, 4, 6]:
        for form in ["list", "tree"]:
            if form == "list":
                code = ("function mk(d, v) { var h = null; for (var q = 0; q < d; q++) h = { v: v + q, next: h }; return h; }\n"
                        "function sum(h) { var t = 0; while (h) { t += h.v; h = h.next; } return t; }\n"
                        "function work(n) { var s = 0, i;\n  for (i = 0; i < n; i++) { s += sum(mk(%d * 4, i)); mix(s); }\n  return s; }\nwork(N);\n") % depth
            else:
                code = ("function mk(d, v) { return d === 0 ? { v: v, l: null, r: null } : { v: v, l: mk(d - 1, v + 1), r: mk(d - 1, v + 2) }; }\n"
                        "function sum(t) { return t === null ? 0 : t.v + sum(t.l) + sum(t.r); }\n"
                        "function work(n) { var s = 0, i;\n  for (i = 0; i < n; i++) { s += sum(mk(%d, i)); mix(s); }\n  return s; }\nwork(N);\n") % (depth + 2)
            add("alloc_%s_%d" % (form, depth), max(1000, 40000 >> max(0, depth - 3)) if form == "tree" else 60000, code)


# ---- family 10: strings -----------------------------------------------------------

def fam_strings():
    ops = {
        "concat": "var t = ''; for (var q = 0; q < 8; q++) t += 'x' + (i + q); s += t.length;",
        "charcode": "var t = 'hello world ' + (i & 255); for (var q = 0; q < t.length; q++) s += t.charCodeAt(q);",
        "charat": "var t = 'abcdef' + (i & 15); s += t.charAt(i % t.length).length;",
        "indexof": "var t = 'the quick brown fox ' + (i & 7); s += t.indexOf('fox') + t.lastIndexOf('q');",
        "slice": "var t = 'abcdefghij' + (i & 15); s += t.slice(2, 6).length + t.substring(1, 3).length;",
        "split": "var a = ('a,b,c,' + (i & 63)).split(','); s += a.length + a[3].length;",
        "join": "s += [i, 2, 3, 4].join('-').length;",
        "replace": "s += ('aXbXc' + (i & 7)).replace('X', 'yy').length;",
        "replaceall": "s += ('aXbXc' + (i & 7)).replace(/X/g, 'y').length;",
        "upper": "s += ('MiXeD' + (i & 7)).toUpperCase().length + ('MiXeD').toLowerCase().length;",
        "trim": "s += ('  pad ' + (i & 7) + '  ').trim().length;",
        "padstart": "s += String(i & 255).padStart(6, '0').length;",
        "repeat": "s += 'ab'.repeat(1 + (i & 7)).length;",
        "compare": "s += ('k' + (i & 15)) < ('k' + ((i >> 1) & 15)) ? 1 : 0;",
        "equal": "s += ('k' + (i & 15)) === ('k' + (i & 7)) ? 1 : 0;",
        "template": "s += `a${i & 7}b${i & 3}`.length;",
        "tostring": "s += String(i).length + (i * 0.5).toString().length;",
        "parseint": "s += parseInt('12' + (i & 63), 10) + parseFloat('1.5' + (i & 3));",
        "number": "s += Number('3' + (i & 15)) + +('2.5');",
        "startswith": "s += ('prefix' + (i & 7)).startsWith('pre') ? 1 : 0;",
        "includes": "s += ('some text here ' + (i & 7)).includes('text') ? 1 : 0;",
        "codepoint": "s += String.fromCharCode(65 + (i & 15)).charCodeAt(0);",
        "localecmp": "s += ('a' + (i & 3)).localeCompare('a' + (i & 1));",
        "regex_test": "s += /^k\\d+$/.test('k' + (i & 63)) ? 1 : 0;",
        "regex_exec": "var m = /(\\d+)-(\\d+)/.exec('x' + (i & 63) + '-' + (i & 7)); s += m ? +m[1] + +m[2] : 0;",
        "regex_replace": "s += ('a1b22c' + (i & 7)).replace(/\\d+/g, '#').length;",
        "json_str": "s += JSON.stringify({ a: i & 15, b: [1, 2] }).length;",
        "escape": "s += encodeURIComponent('a b&c' + (i & 7)).length;",
        "switchstr": "switch ('k' + (i & 3)) { case 'k0': s += 1; break; case 'k1': s += 2; break; case 'k2': s += 3; break; default: s += 4; }",
        "objkey": "var o = {}; o['key' + (i & 7)] = i; s += Object.keys(o).length;",
    }
    for key, stmt in ops.items():
        add("string_%s" % key, 300000, "function work(n) { var s = 0, i;\n  for (i = 0; i < n; i++) { %s mix(s); }\n  return s; }\nwork(N);\n" % stmt)
    for size in [16, 64, 256, 1024]:
        for key, stmt in {
            "buildjoin": "var a = []; for (var q = 0; q < %d; q++) a.push(q & 15); s += a.join('').length;" % size,
            "concatloop": "var t = ''; for (var q = 0; q < %d; q++) t += 'a'; s += t.length;" % size,
            "scan": "var t = 'abcd'.repeat(%d); for (var q = 0; q < t.length; q++) s += t.charCodeAt(q) & 3;" % (size // 4),
        }.items():
            add("string_%s_%d" % (key, size), max(2000, 400000 // size), "function work(n) { var s = 0, i;\n  for (i = 0; i < n; i++) { %s mix(s); }\n  return s; }\nwork(N);\n" % stmt)


# ---- family 11: control flow ---------------------------------------------------------

def fam_control():
    for ncase in [2, 3, 5, 8, 16]:
        for form in ["dense", "sparse", "str"]:
            cases = []
            for c in range(ncase):
                label = {"dense": str(c), "sparse": str(c * 37 + 5), "str": "'k%d'" % c}[form]
                cases.append("case %s: s += %d; break;" % (label, c + 1))
            sel = {"dense": "(i %% %d)" % (ncase + 1), "sparse": "(((i %% %d) * 37) + 5)" % (ncase + 1),
                   "str": "('k' + (i %% %d))" % (ncase + 1)}[form]
            body = ("function work(n) { var s = 0, i;\n  for (i = 0; i < n; i++) { switch (%s) { %s default: s -= 1; } mix(s); }\n  return s; }\nwork(N);\n") % (sel, " ".join(cases))
            add("switch_%s_%d" % (form, ncase), 1500000, body)
    for key, body in {
        "try_nothrow": "function f(x) { try { return x + 1; } catch (e) { return -1; } finally { h = h; } }\nfunction work(n) { var s = 0, i; for (i = 0; i < n; i++) { s += f(i); mix(s); } return s; }\nwork(N);\n",
        "try_throw": "function f(x) { try { if (x % 7 === 0) throw new Error('e' + x); return x; } catch (e) { return e.message.length; } }\nfunction work(n) { var s = 0, i; for (i = 0; i < n; i++) { s += f(i); mix(s); } return s; }\nwork(N);\n",
        "try_finally_order": "var log = 0;\nfunction f(x) { try { try { if (x & 1) throw 1; log += 1; } finally { log += 2; } } catch (e) { log += 3; } return log & 15; }\nfunction work(n) { var s = 0, i; for (i = 0; i < n; i++) { s += f(i); mix(s); } return s; }\nwork(N);\n",
        "throw_in_callee": "function g(x) { if (x % 5 === 0) throw { code: x }; return x; }\nfunction f(x) { try { return g(x) + 1; } catch (e) { return e.code; } }\nfunction work(n) { var s = 0, i; for (i = 0; i < n; i++) { s += f(i); mix(s); } return s; }\nwork(N);\n",
        "rethrow": "function f(x) { try { try { if (x & 1) throw new Error('a'); } catch (e) { throw e; } return 1; } catch (e2) { return 2; } }\nfunction work(n) { var s = 0, i; for (i = 0; i < n; i++) { s += f(i); mix(s); } return s; }\nwork(N);\n",
        "label_break": "function work(n) { var s = 0, i; outer: for (i = 0; i < n; i++) { for (var j = 0; j < 8; j++) { if (j === (i & 7)) continue outer; s += j; } mix(s); } return s; }\nwork(N);\n",
        "while_do": "function work(n) { var s = 0, i = 0; while (i < n) { var k = 0; do { k++; s += k; } while (k < 3); i++; mix(s); } return s; }\nwork(N);\n",
        "forin": "var o = { a: 1, b: 2, c: 3, d: 4 };\nfunction work(n) { var s = 0, i; for (i = 0; i < n; i++) { for (var k in o) s += o[k]; mix(s); } return s; }\nwork(N);\n",
        "forof_array": "var a = [1, 2, 3, 4, 5, 6];\nfunction work(n) { var s = 0, i; for (i = 0; i < n; i++) { for (var v of a) s += v; mix(s); } return s; }\nwork(N);\n",
        "forof_string": "function work(n) { var s = 0, i; for (i = 0; i < n; i++) { for (var c of 'abcdef') s += c.charCodeAt(0); mix(s); } return s; }\nwork(N);\n",
        "forof_map": "var m = new Map([[1, 2], [3, 4], [5, 6]]);\nfunction work(n) { var s = 0, i; for (i = 0; i < n; i++) { for (var [k, v] of m) s += k + v; mix(s); } return s; }\nwork(N);\n",
        "forof_break": "var a = [1, 2, 3, 4, 5, 6];\nfunction work(n) { var s = 0, i; for (i = 0; i < n; i++) { for (var v of a) { if (v > (i & 3) + 2) break; s += v; } mix(s); } return s; }\nwork(N);\n",
        "generator": "function* gen(k) { for (var q = 0; q < k; q++) yield q * 2; }\nfunction work(n) { var s = 0, i; for (i = 0; i < n; i++) { for (var v of gen(5)) s += v; mix(s); } return s; }\nwork(N);\n",
        "generator_send": "function* acc() { var t = 0; while (true) { t += yield t; } }\nfunction work(n) { var s = 0, i, g = acc(); g.next(); for (i = 0; i < n; i++) { s += g.next(i & 7).value; mix(s); } return s; }\nwork(N);\n",
        "destruct_obj": "function work(n) { var s = 0, i; for (i = 0; i < n; i++) { var { a, b, c = 3 } = { a: i, b: 2 }; s += a + b + c; mix(s); } return s; }\nwork(N);\n",
        "destruct_arr": "function work(n) { var s = 0, i; for (i = 0; i < n; i++) { var [a, b, ...r] = [i, 2, 3, 4]; s += a + b + r.length; mix(s); } return s; }\nwork(N);\n",
        "destruct_args": "function f({ x, y }, [p, q]) { return x + y + p + q; }\nfunction work(n) { var s = 0, i; for (i = 0; i < n; i++) { s += f({ x: i, y: 1 }, [2, 3]); mix(s); } return s; }\nwork(N);\n",
        "optional_chain": "var o = { a: { b: 5 } };\nfunction work(n) { var s = 0, i; for (i = 0; i < n; i++) { s += (o?.a?.b ?? 0) + (o.z?.y ?? 1); mix(s); } return s; }\nwork(N);\n",
        "nullish": "function work(n) { var s = 0, i; for (i = 0; i < n; i++) { var v = (i & 3) === 0 ? null : i; s += v ?? 9; s += (v || 4); mix(s); } return s; }\nwork(N);\n",
        "short_circuit": "function work(n) { var s = 0, i; for (i = 0; i < n; i++) { s += ((i & 1) && (i & 2)) ? 1 : ((i & 4) || (i & 8)) ? 2 : 3; mix(s); } return s; }\nwork(N);\n",
        "ternary_chain": "function work(n) { var s = 0, i; for (i = 0; i < n; i++) { s += i < 10 ? 1 : i < 100 ? 2 : i < 1000 ? 3 : 4; mix(s); } return s; }\nwork(N);\n",
        "with_arguments": "function f() { return arguments.length + arguments[0]; }\nfunction work(n) { var s = 0, i; for (i = 0; i < n; i++) { s += f(i, 2, 3); mix(s); } return s; }\nwork(N);\n",
        "getter_loop": "var o = { _v: 1, get v() { return this._v; }, set v(x) { this._v = x & 255; } };\nfunction work(n) { var s = 0, i; for (i = 0; i < n; i++) { o.v = i; s += o.v; mix(s); } return s; }\nwork(N);\n",
        "class_static": "class A { static n = 0; static inc() { return ++A.n; } }\nfunction work(n) { var s = 0, i; for (i = 0; i < n; i++) { s += A.inc() & 7; mix(s); } return s; }\nwork(N);\n",
        "class_private": "class A { #v = 1; inc() { this.#v += 1; return this.#v; } }\nvar a = new A();\nfunction work(n) { var s = 0, i; for (i = 0; i < n; i++) { s += a.inc() & 7; mix(s); } return s; }\nwork(N);\n",
        "inherit_super": "class A { f(x) { return x + 1; } }\nclass B extends A { f(x) { return super.f(x) * 2; } }\nvar b = new B();\nfunction work(n) { var s = 0, i; for (i = 0; i < n; i++) { s += b.f(i & 255); mix(s); } return s; }\nwork(N);\n",
        "inherit_ctor": "class A { constructor(v) { this.v = v; } }\nclass B extends A { constructor(v) { super(v + 1); this.w = v; } }\nfunction work(n) { var s = 0, i; for (i = 0; i < n; i++) { var b = new B(i); s += b.v + b.w; mix(s); } return s; }\nwork(N);\n",
        "symbol_iter": "var it = { [Symbol.iterator]() { var q = 0; return { next() { return q < 4 ? { value: q++, done: false } : { value: undefined, done: true }; } }; } };\nfunction work(n) { var s = 0, i; for (i = 0; i < n; i++) { for (var v of it) s += v; mix(s); } return s; }\nwork(N);\n",
        "tagged": "function tag(strs, a) { return strs.length + a; }\nfunction work(n) { var s = 0, i; for (i = 0; i < n; i++) { s += tag`a${i}b`; mix(s); } return s; }\nwork(N);\n",
        "date_math": "function work(n) { var s = 0, i; for (i = 0; i < n; i++) { s += new Date(2020, 0, 1 + (i & 63)).getDate(); mix(s); } return s; }\nwork(N);\n",
    }.items():
        add("control_%s" % key, 300000, body)


# ---- family 12: builtins -------------------------------------------------------------

def fam_builtins():
    fns1 = ["abs", "floor", "ceil", "round", "trunc", "sqrt", "sin", "cos", "tan", "atan", "exp", "log", "log2", "sign", "cbrt", "fround", "clz32"]
    for fn in fns1:
        for ty in ["int", "dbl", "mix"]:
            x = operand(ty, "i", 0)
            add("math_%s_%s" % (fn, ty), 1200000,
                "function work(n) { var s = 0, i;\n  for (i = 0; i < n; i++) { var x = %s; s = Math.%s(x); mixd(s); }\n  return s; }\nwork(N);\n" % (x, fn))
    for fn in ["min", "max", "pow", "atan2", "hypot", "imul"]:
        for ty in ["int", "dbl", "mix"]:
            x, y = operand(ty, "i", 0), operand(ty, "i", 1)
            add("math2_%s_%s" % (fn, ty), 1200000,
                "function work(n) { var s = 0, i;\n  for (i = 0; i < n; i++) { var x = %s, y = %s; s = Math.%s(x, y); mixd(s); }\n  return s; }\nwork(N);\n" % (x, y, fn))
    add("math_random_lcg", 1500000, "var seed = 12345;\nfunction rnd() { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed / 0x7fffffff; }\nfunction work(n) { var s = 0, i; for (i = 0; i < n; i++) { s += rnd(); mixd(s); } return s; }\nwork(N);\n")
    for key, stmt in {
        "keys": "s += Object.keys(o).length;", "values": "s += Object.values(o).length;", "entries": "s += Object.entries(o).length;",
        "assign": "s += Object.assign({}, o).a;", "freeze": "s += Object.isFrozen(Object.freeze({ a: i })) ? 1 : 0;",
        "getproto": "s += Object.getPrototypeOf(o) === Object.prototype ? 1 : 0;", "hasown": "s += Object.prototype.hasOwnProperty.call(o, 'a') ? 1 : 0;",
        "defprops": "var t = {}; Object.defineProperty(t, 'x', { value: i, enumerable: true }); s += t.x;",
        "fromentries": "s += Object.fromEntries([['a', i], ['b', 2]]).a;", "tostring": "s += Object.prototype.toString.call(o).length;",
        "json_parse": "s += JSON.parse('[1,2,{\"a\":' + (i & 15) + '}]')[2].a;", "json_stringify": "s += JSON.stringify(o).length;",
        "isarray": "s += Array.isArray(a) ? 1 : 0;", "arr_keys": "s += Object.keys(a).length;",
    }.items():
        add("builtin_obj_%s" % key, 200000, "var o = { a: 1, b: 2, c: 3 }; var a = [1, 2, 3];\nfunction work(n) { var s = 0, i;\n  for (i = 0; i < n; i++) { %s mix(s); }\n  return s; }\nwork(N);\n" % stmt)
    for key, stmt in {
        "map_set_get": "m.set(i & 63, i); s += m.get((i + 1) & 63) | 0;", "map_has_delete": "m.set(i & 31, 1); s += m.has((i + 3) & 31) ? 1 : 0; m.delete((i + 5) & 31);",
        "map_string": "m.set('k' + (i & 31), i); s += m.get('k' + ((i + 1) & 31)) | 0;", "set_add_has": "st.add(i & 63); s += st.has((i + 7) & 63) ? 1 : 0;",
        "set_size": "st.add(i & 127); s += st.size;", "weakmap": "wm.set(objs[i & 7], i); s += wm.get(objs[(i + 1) & 7]) | 0;",
        "map_iter": "var c = 0; for (var [k, v] of m) { c += v; if (c > 100) break; } s += c;", "map_foreach": "m.forEach(function (v) { s += v & 3; });",
        "set_iter": "for (var v of st) { s += v & 3; if (s > 1000000) break; }",
    }.items():
        add("builtin_coll_%s" % key, 200000, "var m = new Map(), st = new Set(), wm = new WeakMap(); var objs = []; for (var j = 0; j < 8; j++) objs.push({}); for (var j = 0; j < 16; j++) { m.set(j, j); st.add(j); }\nfunction work(n) { var s = 0, i;\n  for (i = 0; i < n; i++) { %s mix(s); }\n  return s; }\nwork(N);\n" % stmt)
    for key, stmt in {
        "tofixed": "s += (i * 0.37).toFixed(2).length;", "tostring_radix": "s += i.toString(16).length + i.toString(2).length;",
        "isnan": "s += isNaN(i * 0.5) ? 1 : 0; s += Number.isInteger(i * 0.5) ? 1 : 0;", "parse": "s += parseInt(String(i), 10) + parseFloat(String(i * 0.5));",
        "isfinite": "s += isFinite(i / (i & 3)) ? 1 : 0;", "maxsafe": "s += Number.MAX_SAFE_INTEGER > i ? 1 : 0;",
        "bitcount": "var x = i; var c = 0; while (x) { c += x & 1; x >>>= 1; } s += c;", "int_div": "s += (i / 7) | 0; s += (i % 7);",
        "double_round": "s += Math.round((i * 0.37) * 100) / 100;", "exp_form": "s += (i / 1e3 + 1e-3) * 1e3;",
    }.items():
        add("builtin_num_%s" % key, 300000, "function work(n) { var s = 0, i;\n  for (i = 0; i < n; i++) { %s mixd(s); }\n  return s; }\nwork(N);\n" % stmt)
    for key, stmt in {
        "isarray_proto": "s += Array.prototype.slice.call(arguments0, 1).length;", "fn_toString": "s += (function () { return 1; }).toString().length;",
        "arguments_len": "s += (function () { return arguments.length; })(1, 2, 3);", "typeof_fn": "s += typeof work === 'function' ? 1 : 0;",
        "instanceof": "s += [] instanceof Array ? 1 : 0; s += ({}) instanceof Object ? 1 : 0;", "new_target": "function Q() { return new.target === Q; } s += new Q() ? 1 : 0;",
        "symbol_desc": "s += Symbol('abc').toString().length;", "bigint": "s += Number(BigInt(i) * 3n);",
        "date_now_free": "s += new Date(0).getFullYear() > 1969 ? 1 : 0;", "regexp_new": "s += new RegExp('a' + (i & 3)).test('xa1') ? 1 : 0;",
        "error_new": "s += new Error('x' + (i & 3)).message.length;", "array_ctor": "s += new Array(3).length + Array(2).length;",
        "string_ctor": "s += new String('abc').length + String(i).length;", "number_ctor": "s += new Number(i).valueOf();",
        "boolean_ctor": "s += Boolean(i & 1) ? 1 : 0;", "proxy_get": "s += px.a;", "reflect_get": "s += Reflect.get(o2, 'a') + Reflect.has(o2, 'b');",
    }.items():
        add("builtin_misc_%s" % key, 100000, "var arguments0 = [1, 2, 3]; var o2 = { a: 1, b: 2 }; var px = new Proxy({ a: 5 }, {});\nfunction work(n) { var s = 0, i;\n  for (i = 0; i < n; i++) { %s mixd(s); }\n  return s; }\nwork(N);\n" % stmt)


# ---- family 13: polymorphic dispatch and class hierarchies ------------------------------

def fam_dispatch():
    for nclass in [1, 2, 3, 4, 6, 8]:
        for form in ["method", "field", "super", "tostring", "valueof"]:
            classes = ""
            for c in range(nclass):
                base = "A" if c else ""
                classes += "class A%d%s { constructor(v) { %sthis.v = v + %d; %s } m(x) { return x + this.v + %d; } t() { return 'c%d'; } valueOf() { return this.v + %d; } }\n" % (
                    c, (" extends A%d" % (c - 1)) if c else "", "super(v); " if c else "", c, "this.w%d = 1;" % c, c, c, c)
            use = {"method": "s += o.m(i & 15);", "field": "s += o.v + (o.w0 | 0);",
                   "super": "s += o.m(i & 7) + o.t().length;", "tostring": "s += o.t().length;", "valueof": "s += +o;"}[form]
            body = classes + ("var os = []; for (var j = 0; j < 16; j++) os.push(new A%d(j));\n".replace("A%d", "A%d")) % 0
            # distribute classes round-robin
            mk = "".join("os.push(new A%d(j));" % (k % nclass) for k in range(1))
            body = classes + "var os = []; for (var j = 0; j < 16; j++) { var c = j %% %d; %s }\n" % (
                nclass, " ".join("if (c === %d) os.push(new A%d(j));" % (k, k) for k in range(nclass)))
            body += "function work(n) { var s = 0, i;\n  for (i = 0; i < n; i++) { var o = os[i & 15]; %s mix(s); }\n  return s; }\nwork(N);\n" % use
            add("dispatch_%dclass_%s" % (nclass, form), 1200000, body)
    for nsite in [1, 2, 4, 8]:
        fns = ", ".join("function (x) { return x + %d; }" % k for k in range(nsite))
        body = ("var fns = [%s];\nfunction work(n) { var s = 0, i;\n  for (i = 0; i < n; i++) { s += fns[i %% %d](i & 255); mix(s); }\n  return s; }\nwork(N);\n") % (fns, nsite)
        add("dispatch_fnarray_%d" % nsite, 1500000, body)
    for key, body in {
        "visitor": "var nodes = []; for (var j = 0; j < 12; j++) nodes.push(j % 3 === 0 ? { t: 'a', v: j } : j % 3 === 1 ? { t: 'b', v: j, w: 1 } : { t: 'c', v: j });\nfunction visit(nd) { switch (nd.t) { case 'a': return nd.v; case 'b': return nd.v + nd.w; default: return nd.v * 2; } }\nfunction work(n) { var s = 0, i; for (i = 0; i < n; i++) { s += visit(nodes[i % 12]); mix(s); } return s; }\nwork(N);\n",
        "observer": "function Em() { this.l = []; }\nEm.prototype.on = function (f) { this.l.push(f); };\nEm.prototype.emit = function (x) { var t = 0; for (var q = 0; q < this.l.length; q++) t += this.l[q](x); return t; };\nvar e = new Em(); e.on(function (x) { return x + 1; }); e.on(function (x) { return x * 2; }); e.on(function (x) { return x & 7; });\nfunction work(n) { var s = 0, i; for (i = 0; i < n; i++) { s += e.emit(i); mix(s); } return s; }\nwork(N);\n",
        "mixin": "var M = { hello: function () { return this.n + 1; } };\nfunction K(n) { this.n = n; }\nObject.assign(K.prototype, M);\nvar k = new K(4);\nfunction work(n) { var s = 0, i; for (i = 0; i < n; i++) { s += k.hello(); mix(s); } return s; }\nwork(N);\n",
        "proto_swap": "var a = { v: 1 }, b = { v: 2 };\nvar o = Object.create(a);\nfunction work(n) { var s = 0, i; for (i = 0; i < n; i++) { if ((i & 1023) === 0) Object.setPrototypeOf(o, (i & 1024) ? b : a); s += o.v; mix(s); } return s; }\nwork(N);\n",
        "shape_transition": "function work(n) { var s = 0, i; for (i = 0; i < n; i++) { var o = {}; if (i & 1) o.a = 1; if (i & 2) o.b = 2; o.c = 3; s += (o.a | 0) + (o.b | 0) + o.c; mix(s); } return s; }\nwork(N);\n",
        "delete_shape": "function work(n) { var s = 0, i; for (i = 0; i < n; i++) { var o = { a: 1, b: 2, c: 3 }; if (i & 1) delete o.b; s += o.a + (o.b | 0) + o.c; mix(s); } return s; }\nwork(N);\n",
    }.items():
        add("dispatch_%s" % key, 400000, body)


# ---- family 14: kernels, templated by size and element type ---------------------------

def fam_kernels():
    for size in [64, 256, 1024]:
        for ty in ["int", "dbl"]:
            init = "(j * 7 + 3) & 255" if ty == "int" else "((j * 7 + 3) & 255) * 0.5"
            kern = {
                "sum": "var t = 0; for (var q = 0; q < %d; q++) t += a[q]; s += t;" % size,
                "dot": "var t = 0; for (var q = 0; q < %d; q++) t += a[q] * b[q]; s += t;" % size,
                "max": "var t = a[0]; for (var q = 1; q < %d; q++) if (a[q] > t) t = a[q]; s += t;" % size,
                "prefix": "for (var q = 1; q < %d; q++) c[q] = c[q - 1] + a[q]; s += c[%d];" % (size, size - 1),
                "copy": "for (var q = 0; q < %d; q++) c[q] = a[q]; s += c[%d];" % (size, size - 1),
                "saxpy": "for (var q = 0; q < %d; q++) c[q] = 3 * a[q] + b[q]; s += c[%d];" % (size, size - 1),
                "bubble_pass": "for (var q = 0; q < %d - 1; q++) if (a[q] > a[q + 1]) { var t = a[q]; a[q] = a[q + 1]; a[q + 1] = t; } s += a[0];" % size,
                "count": "var t = 0; for (var q = 0; q < %d; q++) if (a[q] > 64) t++; s += t;" % size,
                "reverse": "for (var q = 0, r = %d; q < r; q++, r--) { var t = a[q]; a[q] = a[r]; a[r] = t; } s += a[0];" % (size - 1),
                "stencil": "for (var q = 1; q < %d; q++) c[q] = (a[q - 1] + a[q] + a[q + 1]) / 3; s += c[%d];" % (size - 1, size // 2),
            }
            for kn, ks in kern.items():
                body = ("var a = [], b = [], c = [];\nfor (var j = 0; j < %d; j++) { a.push(%s); b.push(%s + 1); c.push(0); }\n"
                        "function work(n) { var s = 0, i;\n  for (i = 0; i < n; i++) { %s mixd(s); }\n  return s; }\nwork(N);\n") % (
                            size, init, init, ks)
                add("kernel_%s_%s_%d" % (kn, ty, size), max(3000, 2000000 // size), body)
    for size in [8, 16, 32]:
        body = ("function mm(a, b, c, n) { for (var i = 0; i < n; i++) for (var j = 0; j < n; j++) { var t = 0; for (var k = 0; k < n; k++) t += a[i * n + k] * b[k * n + j]; c[i * n + j] = t; } }\n"
                "var a = [], b = [], c = [];\nfor (var j = 0; j < %d; j++) { a.push(j & 7); b.push((j * 3) & 7); c.push(0); }\n"
                "function work(n) { var s = 0, i;\n  for (i = 0; i < n; i++) { mm(a, b, c, %d); s += c[%d]; mix(s); }\n  return s; }\nwork(N);\n") % (
                    size * size, size, size * size - 1)
        add("kernel_matmul_%d" % size, max(30, 60000 // (size * size)), body)
    for size in [100, 1000, 5000]:
        body = ("function sieve(n) { var f = new Uint8Array(n + 1), c = 0; for (var i = 2; i <= n; i++) { if (!f[i]) { c++; for (var j = i * 2; j <= n; j += i) f[j] = 1; } } return c; }\n"
                "function work(n) { var s = 0, i;\n  for (i = 0; i < n; i++) { s += sieve(%d); mix(s); }\n  return s; }\nwork(N);\n") % size
        add("kernel_sieve_%d" % size, max(20, 600000 // size), body)
    for size in [10, 100, 1000]:
        body = ("function collatz(n) { var c = 0; while (n !== 1) { n = n & 1 ? 3 * n + 1 : n >> 1; c++; } return c; }\n"
                "function work(n) { var s = 0, i;\n  for (i = 1; i < n; i++) { s += collatz((i %% %d) + 1); mix(s); }\n  return s; }\nwork(N);\n") % size
        add("kernel_collatz_%d" % size, 100000, body)


# ---- family 15: loop forms around an arithmetic op --------------------------------

def fam_forms():
    for op in BINOPS:
        for ty in TYPES:
            x, y = operand(ty, "j", 0), operand(ty, "j", 1)
            pre = "var A = [], B = [];\nfor (var j = 0; j < 128; j++) { A.push(%s); B.push(%s); }\n" % (x, y)
            forms = {
                "while": "function work(n) { var s = 0, i = 0; while (i < n) { s = A[i & 127] %s B[(i + 1) & 127]; %s; i++; } return s; }\nwork(N);\n",
                "dowhile": "function work(n) { var s = 0, i = 0; do { s = A[i & 127] %s B[(i + 1) & 127]; %s; i++; } while (i < n); return s; }\nwork(N);\n",
                "forof": "function work(n) { var s = 0; for (var r = 0; r < n / 128; r++) for (var v of A) { s = v %s B[r & 127]; %s; } return s; }\nwork(N);\n",
                "foreach": "function work(n) { var s = 0; for (var r = 0; r < n / 128; r++) A.forEach(function (v, k) { s = v %s B[k]; %s; }); return s; }\nwork(N);\n",
                "reduce": "function work(n) { var s = 0; for (var r = 0; r < n / 128; r++) s = A.reduce(function (p, v, k) { var t = v %s B[k]; %s; return t; }, 0); return s; }\nwork(N);\n",
                "nested": "function work(n) { var s = 0; for (var r = 0; r < n / 16; r++) for (var k = 0; k < 16; k++) { s = A[(r + k) & 127] %s B[k]; %s; } return s; }\nwork(N);\n",
            }
            for fk, tpl in forms.items():
                add("form_%s_%s_%s" % (OPNAME[op], ty, fk), 1200000, pre + tpl % (op, sink(ty)))


# ---- family 16: type flips (the profile sees the early phase only) ----------------

def fam_flip():
    flips = {
        "int_dbl": ("(i & 255) + 1", "((i & 255) + 1) * 0.5"),
        "int_big": ("(i & 255) + 1", "(i & 255) + 1073741824"),
        "int_str": ("(i & 255) + 1", "'s' + (i & 15)"),
        "int_null": ("(i & 255) + 1", "(i & 1) ? null : (i & 7)"),
        "dbl_int": ("((i & 255) + 1) * 0.5", "(i & 255) + 1"),
        "int_bool": ("(i & 255) + 1", "(i & 1) === 0"),
        "int_obj": ("(i & 255) + 1", "{ valueOf: function () { return 3; } }"),
        "int_undef": ("(i & 255) + 1", "(i & 3) === 0 ? undefined : i"),
    }
    for op in ["+", "-", "*", "&", "<", "===", "/"]:
        for fk, (a, b) in flips.items():
            body = ("function f(x, y) { return x %s y; }\nfunction work(n) { var s = 0, i;\n"
                    "  for (i = 0; i < n; i++) { var x = i < T ? %s : %s; var r = f(x, 3); "
                    "if (typeof r === 'number') mixd(r); else if (typeof r === 'boolean') mix(r ? 1 : 0); else mixs(String(r)); }\n  return s; }\nwork(N);\n") % (
                        op, a, b)
            add("flip_%s_%s" % (OPNAME[op], fk), 1000000, "var T = N / 2;\n" + body)
    for fk, (decl, early, late) in {
        "shape": ("function P(v) { this.a = v; }\nfunction Q(v) { this.b = 0; this.a = v; }\n", "new P(i)", "new Q(i)"),
        "proto": ("function P(v) { this.a = v; }\nP.prototype.m = function () { return this.a; };\nfunction Q(v) { this.a = v; }\nQ.prototype.m = function () { return this.a + 1; };\n", "new P(i)", "new Q(i)"),
        "array": ("", "[i, i + 1, i + 2]", "[i * 0.5, 'x', i]"),
        "callee": ("function g1(x) { return x + 1; }\nfunction g2(x) { return x * 2; }\n", "g1", "g2"),
    }.items():
        use = {"shape": "s += o.a;", "proto": "s += o.m();", "array": "s += typeof o[1] === 'string' ? 7 : o[1] | 0;", "callee": "s += o(i & 15);"}[fk]
        body = "var T = N / 2;\n" + decl + ("function work(n) { var s = 0, i;\n  for (i = 0; i < n; i++) { var o = i < T ? %s : %s; %s mix(s); }\n  return s; }\nwork(N);\n") % (early, late, use)
        add("flip_site_%s" % fk, 1000000, body)


# ---- family 17: more compare pairs ------------------------------------------------

def fam_compare2():
    pairs = {
        "bb": ("((i & 1) === 0)", "((i & 2) === 0)"),
        "bi": ("((i & 1) === 0)", "(i & 1)"),
        "sn": ("('' + (i & 7))", "(i & 7)"),
        "nn": ("((i & 3) === 0 ? NaN : i & 7)", "((i & 5) === 0 ? NaN : i & 3)"),
        "zero": ("((i & 1) ? 0 : -0)", "0"),
        "ov": ("(i & 1 ? 2147483647 : -2147483648)", "(i & 2 ? 2147483647 : 1)"),
    }
    for op in CMPOPS:
        for pk, (a, b) in pairs.items():
            for form in ["if", "ternary", "count", "while"]:
                if form == "if":
                    loop = "if (%s %s %s) s += 3; else s -= 1;" % (a, op, b)
                elif form == "ternary":
                    loop = "s += (%s %s %s) ? i & 7 : 2;" % (a, op, b)
                elif form == "while":
                    loop = "var k = 0; while (k < 3 && (%s %s %s)) { k++; s += k; } s += k;" % (a, op, b)
                else:
                    loop = "s += +((%s) %s (%s));" % (a, op, b)
                body = "function work(n) { var s = 0, i;\n  for (i = 0; i < n; i++) { %s mix(s); }\n  return s; }\nwork(N);\n" % loop
                add("cmp2_%s_%s_%s" % (OPNAME[op], pk, form), 1200000, body)


# ---- family 18: argument types through call forms ----------------------------------

def fam_argtypes():
    argexprs = {
        "int": "(i & 255)", "dbl": "((i & 255) * 0.5)", "str": "('k' + (i & 15))", "obj": "objs[i & 3]",
        "bool": "((i & 1) === 0)", "nullish": "((i & 1) ? null : undefined)", "mixed": "(i & 1 ? (i & 255) : (i & 255) * 0.5)",
        "arr": "arrs[i & 3]", "fn": "fns[i & 1]",
    }
    forms = {
        "direct": "s += use(f(%s));", "method": "s += use(o.m(%s));", "ctor": "s += use(new K(%s).v);",
        "closure": "s += use(c(%s));", "callback": "s += use([%s].map(function (z) { return z; })[0]);",
        "rest": "s += use(rest(%s, 1));", "default": "s += use(dflt(%s));", "spread": "s += use(f(...[%s]));",
    }
    pre = ("var objs = [{ v: 1 }, { v: 2 }, { v: 3 }, { v: 4 }], arrs = [[1], [2, 3], [], [4]], fns = [function () { return 1; }, function () { return 2; }];\n"
           "function use(v) { return typeof v === 'number' ? v | 0 : typeof v === 'string' ? v.length : typeof v === 'boolean' ? (v ? 1 : 0) : v === null ? 2 : v === undefined ? 3 : 5; }\n"
           "function f(x) { return x; }\nvar o = { m: function (x) { return x; } };\nfunction K(x) { this.v = x; }\n"
           "var c = (function () { var t = 0; return function (x) { return x; }; })();\n"
           "function rest(x, ...r) { return x; }\nfunction dflt(x = 1) { return x; }\n")
    for ak, ae in argexprs.items():
        for fk, tpl in forms.items():
            body = pre + "function work(n) { var s = 0, i;\n  for (i = 0; i < n; i++) { %s mix(s); }\n  return s; }\nwork(N);\n" % (tpl % ae)
            add("argty_%s_%s" % (ak, fk), 800000, body)


# ---- family 19: element loops by loop form ------------------------------------------

def fam_elemforms():
    kinds = {
        "int": "var a = []; for (var j = 0; j < 128; j++) a.push(j * 3 + 1);",
        "dbl": "var a = []; for (var j = 0; j < 128; j++) a.push(j * 0.5 + 0.25);",
        "mixed": "var a = []; for (var j = 0; j < 128; j++) a.push(j & 1 ? j : j + 0.5);",
        "obj": "var a = []; for (var j = 0; j < 128; j++) a.push({ v: j });",
        "str": "var a = []; for (var j = 0; j < 128; j++) a.push('s' + j);",
        "f64": "var a = new Float64Array(128); for (var j = 0; j < 128; j++) a[j] = j * 0.5;",
        "i32": "var a = new Int32Array(128); for (var j = 0; j < 128; j++) a[j] = j * 3;",
        "u8": "var a = new Uint8Array(128); for (var j = 0; j < 128; j++) a[j] = j;",
    }
    for kk, decl in kinds.items():
        val = "v.v" if kk == "obj" else "v.length" if kk == "str" else "v"
        forms = {
            "for": "for (var q = 0; q < a.length; q++) { var v = a[q]; s += %s; }" % val,
            "forof": "for (var v of a) s += %s;" % val,
            "foreach": "a.forEach(function (v) { s += %s; });" % val,
            "reduce": "s += a.reduce(function (p, v) { return p + (%s); }, 0);" % val,
            "map": "s += a.map(function (v) { return %s; }).length;" % val,
            "while": "var q = 0; while (q < a.length) { var v = a[q++]; s += %s; }" % val,
            "rev": "for (var q = a.length - 1; q >= 0; q--) { var v = a[q]; s += %s; }" % val,
            "step": "for (var q = 0; q < a.length; q += 3) { var v = a[q]; s += %s; }" % val,
        }
        for fk, stmt in forms.items():
            if kk in ("f64", "i32", "u8") and fk in ("map",):
                continue
            body = decl + "\nfunction work(n) { var s = 0, i;\n  for (i = 0; i < n; i++) { %s mixd(s); }\n  return s; }\nwork(N);\n" % stmt
            add("elemform_%s_%s" % (kk, fk), max(2000, 400000 // 128), body)


# ---- family 20: small algorithms over input ranges ----------------------------------

def fam_algos():
    algos = {
        "gcd": "function algo(a, b) { while (b) { var t = a %% b; a = b; b = t; } return a; }\nfunction run(x) { return algo(x + 1000, (x %% %d) + 7); }",
        "isprime": "function algo(n) { if (n < 2) return 0; for (var d = 2; d * d <= n; d++) if (n %% d === 0) return 0; return 1; }\nfunction run(x) { return algo(x %% %d + 2); }",
        "digits": "function algo(n) { var s = 0; while (n > 0) { s += n %% 10; n = (n / 10) | 0; } return s; }\nfunction run(x) { return algo(x %% %d); }",
        "popcount": "function algo(n) { var c = 0; while (n) { n &= n - 1; c++; } return c; }\nfunction run(x) { return algo((x * 2654435761) >>> 0 %% %d); }",
        "reverse_bits": "function algo(n) { var r = 0; for (var k = 0; k < 16; k++) { r = (r << 1) | (n & 1); n >>= 1; } return r; }\nfunction run(x) { return algo(x %% %d); }",
        "fibiter": "function algo(n) { var a = 0, b = 1; for (var k = 0; k < n; k++) { var t = (a + b) | 0; a = b; b = t; } return a; }\nfunction run(x) { return algo(x %% %d); }",
        "binsearch": "var arr = []; for (var j = 0; j < 1024; j++) arr.push(j * 3);\nfunction algo(t) { var lo = 0, hi = 1023; while (lo <= hi) { var m = (lo + hi) >> 1; if (arr[m] === t) return m; if (arr[m] < t) lo = m + 1; else hi = m - 1; } return -1; }\nfunction run(x) { return algo(x %% %d); }",
        "hash": "function algo(n) { var h = 2166136261; for (var k = 0; k < 8; k++) { h ^= (n >> (k * 3)) & 255; h = Math.imul(h, 16777619); } return h | 0; }\nfunction run(x) { return algo(x %% %d); }",
        "lcs": "function algo(a, b) { var m = a.length, n = b.length, d = []; for (var i = 0; i <= m; i++) { d.push([]); for (var j = 0; j <= n; j++) d[i].push(0); } for (var i = 1; i <= m; i++) for (var j = 1; j <= n; j++) d[i][j] = a[i - 1] === b[j - 1] ? d[i - 1][j - 1] + 1 : Math.max(d[i - 1][j], d[i][j - 1]); return d[m][n]; }\nfunction run(x) { return algo('abcbdab' + (x %% %d), 'bdcaba'); }",
        "editdist": "function algo(a, b) { var p = []; for (var j = 0; j <= b.length; j++) p.push(j); for (var i = 1; i <= a.length; i++) { var c = [i]; for (var j = 1; j <= b.length; j++) c.push(Math.min(p[j] + 1, c[j - 1] + 1, p[j - 1] + (a[i - 1] === b[j - 1] ? 0 : 1))); p = c; } return p[b.length]; }\nfunction run(x) { return algo('kitten' + (x %% %d), 'sitting'); }",
        "bubble": "function algo(n) { var a = []; for (var k = 0; k < 16; k++) a.push((k * 7 + n) %% 31); for (var i = 0; i < 16; i++) for (var j = 0; j < 15 - i; j++) if (a[j] > a[j + 1]) { var t = a[j]; a[j] = a[j + 1]; a[j + 1] = t; } return a[0] + a[15]; }\nfunction run(x) { return algo(x %% %d); }",
        "base": "function algo(n) { var s = ''; do { s = '0123456789abcdef'[n & 15] + s; n >>>= 4; } while (n); return s.length; }\nfunction run(x) { return algo(x %% %d); }",
    }
    for key, tpl in algos.items():
        for rng in [4, 64, 1024, 100000]:
            code = tpl % rng
            add("algo_%s_%d" % (key, rng), 2000 if (key == "fibiter" and rng >= 100000) else 15000 if key in ("lcs", "editdist") else 150000, code + "\nfunction work(n) { var s = 0, i;\n  for (i = 0; i < n; i++) { s += run(i); mix(s); }\n  return s; }\nwork(N);\n")


# ---- family 21: objects with typed fields ----------------------------------------------

def fam_fieldtypes():
    vals = {"int": "i & 255", "dbl": "(i & 255) * 0.5", "str": "'s' + (i & 7)", "obj": "{ k: i & 3 }", "bool": "(i & 1) === 0",
            "mixed": "(i & 1) ? i & 255 : 'm'", "arr": "[i & 7]", "nullish": "(i & 1) ? null : undefined"}
    for nf in [1, 2, 4, 8, 12]:
        for vk, ve in vals.items():
            for form in ["literal", "ctor", "assign"]:
                names = ["p%d" % k for k in range(nf)]
                if form == "literal":
                    mk = "{ %s }" % ", ".join("%s: %s" % (nm, ve) for nm in names)
                elif form == "ctor":
                    mk = "new C(%s)" % ve
                else:
                    mk = "(function () { var o = {}; %s return o; })()" % " ".join("o.%s = %s;" % (nm, ve) for nm in names)
                decl = "function C(v) { %s }\n" % " ".join("this.%s = v;" % nm for nm in names) if form == "ctor" else ""
                use = "var t = o.p0; s += typeof t === 'number' ? t | 0 : typeof t === 'string' ? t.length : t === null ? 2 : t === undefined ? 3 : typeof t === 'boolean' ? (t ? 1 : 0) : 5;"
                body = decl + "function work(n) { var s = 0, i;\n  for (i = 0; i < n; i++) { var o = %s; %s mix(s); }\n  return s; }\nwork(N);\n" % (mk, use)
                add("fieldty_%d_%s_%s" % (nf, vk, form), 400000, body)


# ---- family 22: more shapes, sizes and trip counts ---------------------------------

def fam_more():
    vals = {"int": "i & 255", "dbl": "(i & 255) * 0.5", "str": "'s' + (i & 7)", "obj": "{ k: 1 }", "bool": "(i & 1) === 0",
            "mixed": "(i & 1) ? i & 255 : 'm'", "arr": "[1]", "nullish": "(i & 1) ? null : undefined"}
    for nf in [1, 3, 6]:
        for vk, ve in vals.items():
            for op in ["read", "write", "rmw"]:
                names = ["f%d" % k for k in range(nf)]
                lit = ", ".join("%s: %s" % (nm, _re.sub(r"\bi\b", "j", ve)) for nm in names)
                stmt = {"read": "var t = o.f0; s += typeof t === 'number' ? t | 0 : typeof t === 'string' ? t.length : 1;",
                        "write": "o.f%d = %s; s += 1;" % (nf - 1, ve),
                        "rmw": "var t = o.f0; o.f0 = typeof t === 'number' ? t + 1 : t; s += 1;"}[op]
                body = ("var pool = []; for (var j = 0; j < 32; j++) pool.push({ %s });\n"
                        "function work(n) { var s = 0, i;\n  for (i = 0; i < n; i++) { var o = pool[i & 31]; %s mix(s); }\n  return s; }\nwork(N);\n") % (lit, stmt)
                add("prop2_%d_%s_%s" % (nf, vk, op), 1200000, body)
    for size in [32, 128, 512, 2048]:
        for ty in ["int", "dbl"]:
            init = "(j * 7 + 3) & 255" if ty == "int" else "((j * 7 + 3) & 255) * 0.5"
            for kn, ks in {
                "sum": "var t = 0; for (var q = 0; q < %d; q++) t += a[q]; s += t;" % size,
                "dot": "var t = 0; for (var q = 0; q < %d; q++) t += a[q] * b[q]; s += t;" % size,
                "min": "var t = a[0]; for (var q = 1; q < %d; q++) if (a[q] < t) t = a[q]; s += t;" % size,
                "scale": "for (var q = 0; q < %d; q++) c[q] = a[q] * 2 + 1; s += c[%d];" % (size, size - 1),
                "swap": "for (var q = 0; q + 1 < %d; q += 2) { var t = a[q]; a[q] = a[q + 1]; a[q + 1] = t; } s += a[0];" % size,
                "hist": "var hh = [0, 0, 0, 0]; for (var q = 0; q < %d; q++) hh[a[q] & 3]++; s += hh[1];" % size,
                "while": "var q = 0, t = 0; while (q < %d) { t += a[q]; q += 2; } s += t;" % size,
                "nested": "var t = 0; for (var q = 0; q < %d; q += 16) for (var r = 0; r < 16 && q + r < %d; r++) t += a[q + r]; s += t;" % (size, size),
            }.items():
                body = ("var a = [], b = [], c = [];\nfor (var j = 0; j < %d; j++) { a.push(%s); b.push(%s + 1); c.push(0); }\n"
                        "function work(n) { var s = 0, i;\n  for (i = 0; i < n; i++) { %s mixd(s); }\n  return s; }\nwork(N);\n") % (size, init, init, ks)
                add("kernel2_%s_%s_%d" % (kn, ty, size), max(3000, 2000000 // size), body)
    rets = {"int": "x + 1", "dbl": "x * 0.5", "str": "'r' + x", "obj": "{ r: x }", "bool": "x > 3", "null": "null", "undef": "undefined", "arr": "[x]"}
    for rk, re in rets.items():
        for fk, call in {"direct": "f(i & 15)", "method": "o.m(i & 15)", "closure": "c(i & 15)", "ctor": "new K(i & 15).r",
                         "arrow": "af(i & 15)", "callback": "[i & 15].map(f)[0]", "apply": "f.apply(null, [i & 15])", "bind": "g(i & 15)"}.items():
            decl = ("function f(x) { return %s; }\nvar o = { m: f };\nvar c = (function () { return function (x) { return %s; }; })();\n"
                    "function K(x) { this.r = %s; }\nvar af = (x) => %s;\nvar g = f.bind(null);\n"
                    "function use(v) { return typeof v === 'number' ? v | 0 : typeof v === 'string' ? v.length : typeof v === 'boolean' ? (v ? 1 : 0) : v === null ? 2 : v === undefined ? 3 : Array.isArray(v) ? 4 : 5; }\n") % (re, re, re, re)
            add("ret_%s_%s" % (rk, fk), 800000, decl + "function work(n) { var s = 0, i;\n  for (i = 0; i < n; i++) { s += use(%s); mix(s); }\n  return s; }\nwork(N);\n" % call)
    for sel, val in {"int": "(i & 7)", "dbl": "((i & 7) * 0.5)", "str": "('s' + (i & 7))", "mixed": "(i & 1 ? (i & 7) : 's' + (i & 7))"}.items():
        for ncase in [2, 4, 8]:
            for dpos in ["last", "first", "none"]:
                cs = []
                for c in range(ncase):
                    lab = {"int": str(c), "dbl": "%s" % (c * 0.5), "str": "'s%d'" % c, "mixed": ("%d" % c) if c % 2 else "'s%d'" % c}[sel]
                    cs.append("case %s: s += %d; break;" % (lab, c + 1))
                d = "default: s += 9; break;"
                arms = (d + " " + " ".join(cs)) if dpos == "first" else (" ".join(cs) + (" " + d if dpos == "last" else ""))
                body = "function work(n) { var s = 0, i;\n  for (i = 0; i < n; i++) { switch (%s) { %s } mix(s); }\n  return s; }\nwork(N);\n" % (val, arms)
                add("switch2_%s_%d_%s" % (sel, ncase, dpos), 1200000, body)
    for trips in [1, 2, 3, 4, 8, 16, 100]:
        for form in ["for", "while", "dowhile", "forin", "forof", "foreach", "reduce"]:
            inner = {
                "for": "for (var q = 0; q < %d; q++) s += q & 3;" % trips,
                "while": "var q = 0; while (q < %d) { s += q & 3; q++; }" % trips,
                "dowhile": "var q = 0; do { s += q & 3; q++; } while (q < %d);" % trips,
                "forin": "for (var k in arr) s += k.length;",
                "forof": "for (var v of arr) s += v & 3;",
                "foreach": "arr.forEach(function (v) { s += v & 3; });",
                "reduce": "s += arr.reduce(function (p, v) { return p + (v & 3); }, 0);",
            }[form]
            body = "var arr = []; for (var j = 0; j < %d; j++) arr.push(j);\nfunction work(n) { var s = 0, i;\n  for (i = 0; i < n; i++) { %s mix(s); }\n  return s; }\nwork(N);\n" % (trips, inner)
            add("trips_%s_%d" % (form, trips), max(2000, 800000 // trips), body)
    for freq in [0, 1000, 10, 1]:
        for depth in range(0, 4):
            for fin in [0, 1]:
                call = "g%d(x)" % depth
                chain = "function g0(x) { %s return x; }\n" % ("if (x %% %d === 0) throw new Error('t');" % freq if freq else "")
                for d in range(1, depth + 1):
                    chain += "function g%d(x) { return g%d(x) + 1; }\n" % (d, d - 1)
                stmt = ("try { s += %s; } catch (e) { s += 7; }" if not fin else "try { s += %s; } catch (e) { s += 7; } finally { s += 1; }") % call
                body = chain + "function work(n) { var s = 0, i;\n  for (i = 1; i <= n; i++) { var x = i; %s mix(s); }\n  return s; }\nwork(N);\n" % stmt
                add("exc_f%d_d%d_fin%d" % (freq, depth, fin), 400000, body)
    for depth in range(1, 5):
        for pat in ["override", "super", "static", "getter"]:
            cls = "class A0 { constructor() { this.v = 1; } f(x) { return x + this.v; } static s() { return 1; } get g() { return this.v; } }\n"
            for d in range(1, depth + 1):
                body_m = {"override": "f(x) { return x * 2 + %d; }" % d, "super": "f(x) { return super.f(x) + %d; }" % d,
                          "static": "static s() { return super.s() + %d; }" % d, "getter": "get g() { return super.g + %d; }" % d}[pat]
                cls += "class A%d extends A%d { %s }\n" % (d, d - 1, body_m)
            use = {"override": "o.f(i & 15)", "super": "o.f(i & 15)", "static": "A%d.s()" % depth, "getter": "o.g"}[pat]
            add("inherit_d%d_%s" % (depth, pat), 800000, cls + "var o = new A%d();\nfunction work(n) { var s = 0, i;\n  for (i = 0; i < n; i++) { s += %s; mix(s); }\n  return s; }\nwork(N);\n" % (depth, use))


def build():
    fam_arith()
    fam_compare()
    fam_unary()
    fam_chain()
    fam_props()
    fam_elems()
    fam_calls()
    fam_closures()
    fam_alloc()
    fam_strings()
    fam_control()
    fam_builtins()
    fam_dispatch()
    fam_kernels()
    fam_forms()
    fam_flip()
    fam_compare2()
    fam_argtypes()
    fam_elemforms()
    fam_algos()
    fam_fieldtypes()
    fam_more()


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 2
    calib = {}
    if "--calib" in args:
        i = args.index("--calib")
        with open(args[i + 1]) as f:
            calib = json.load(f)
        del args[i:i + 2]
    listonly = "--list" in args
    if listonly:
        args.remove("--list")
    build()
    seen = set()
    for name, n, body in PROGRAMS:
        assert name not in seen, name
        seen.add(name)
    if listonly:
        for name, n, body in PROGRAMS:
            print(name)
        return 0
    out = args[0]
    os.makedirs(out, exist_ok=True)
    for name, n, body in PROGRAMS:
        n = calib.get(name, n)
        with open(os.path.join(out, name + ".js"), "w") as f:
            f.write("// generated: do not edit; tools/benchmark/aotcorpus/gen.py\n")
            f.write(PRE % {"n": n})
            f.write(body)
            f.write(POST % {"name": name})
        if "var T = N / 2;" in body:
            # The profiled run sees only the early phase; the checked run flips.
            with open(os.path.join(out, name + ".prof.js"), "w") as f:
                f.write("// generated: do not edit; tools/benchmark/aotcorpus/gen.py\n")
                f.write((PRE % {"n": n}) + body.replace("var T = N / 2;", "var T = Infinity;"))
                f.write(POST % {"name": name})
    print("generated %d programs" % len(PROGRAMS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
