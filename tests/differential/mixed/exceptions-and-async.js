/*
 * Mixed program: exceptions thrown through frames of both kinds and caught on
 * either side, finally blocks across the boundary, generators delegating to
 * each other, and async functions awaiting each other.
 */
function names(e) {
  var out = [];
  var ls = String(e.stack).split("\n");
  for (var i = 0; i < ls.length && out.length < 4; i++) {
    var m = /^\s*at\s+([\w$.]+)/.exec(ls[i]);
    if (!m) continue;
    /* the test's own functions only: engines name module and eval frames differently */
    var name = m[1].replace(/^Object\./, "");
    if (/^(host|ev)/.test(name)) out.push(name);
  }
  return out.join(">");
}
var log = [];
function hostThrow(msg) { throw new Error(msg); }
function hostCatch(f) { try { return f(); } catch (e) { return "caught " + e.message; } finally { log.push("host-finally"); } }
/* Payloads compiled at run time see the host through the global object. */
globalThis.hostThrow = hostThrow;
globalThis.hostGen = hostGen;
globalThis.hostAsync = hostAsync;
globalThis.log = log;
var ev = (0, eval)([
  "function evThrowVia() { hostThrow('from host'); }",
  "function evThrow() { throw new TypeError('from ev'); }",
  "function evCatch(f) { try { f(); } catch (e) { return 'ev caught ' + e.message; } finally { log.push('ev-finally'); } }",
  "function* evGen(n) { yield 'e0'; yield* hostGen(n); yield 'e1'; }",
  "async function evAsync(x) { var a = await hostAsync(x); return a + 1; }",
  "({ evThrowVia: evThrowVia, evThrow: evThrow, evCatch: evCatch, evGen: evGen, evAsync: evAsync })"
].join("\n"));
function* hostGen(n) { for (var i = 0; i < n; i++) yield "h" + i; }
function hostSquare(x) { return x * x; }
globalThis.hostSquare = hostSquare;
print((0, eval)("(function () { var s = 0; for (var i = 0; i < 100; i++) s += hostSquare(i); return s; })")());
async function hostAsync(x) { await null; return x * 2; }
print(hostCatch(ev.evThrowVia), hostCatch(ev.evThrow));
print(ev.evCatch(function () { hostThrow("cb"); }));
try { ev.evThrowVia(); } catch (e) { print(names(e)); }
print(log.join(","));
print(Array.from(ev.evGen(2)).join(","));
function* hostOuter() { yield* ev.evGen(1); yield "end"; }
print(Array.from(hostOuter()).join(","));
var it = ev.evGen(3); it.next();
try { it.throw(new Error("into gen")); } catch (e) { print("gen", e.message); }
ev.evAsync(20).then(function (v) { print("async", v); });
(async function () {
  try { await new Function("return (async function () { throw new Error('async ev'); })();")(); }
  catch (e) { print("await caught", e.message); }
})();
