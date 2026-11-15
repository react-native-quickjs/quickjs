/*
 * Mixed program: objects of one shape read and written from both sides, so a
 * property site warmed by one kind of function sees objects the other made;
 * getters, setters and conversions defined on one side run from the other, and
 * a throw inside them keeps the frames of both sides.
 */
function frames(e) {
  var out = [];
  var ls = String(e.stack).split("\n");
  for (var i = 0; i < ls.length && out.length < 3; i++) {
    var m = /^\s*at\s+([\w$.]+)/.exec(ls[i]);
    if (!m) continue;
    /* the test's own functions only: engines name module and eval frames differently */
    var name = m[1].replace(/^(Object|Array)\./, "");
    if (/^(host|ev|get|valueOf)/.test(name)) out.push(name);
  }
  return out.join(">");
}
function HostRec(a, b) { this.a = a; this.b = b; }
function hostRead(o) { return o.a * 10 + o.b; }
function hostWrite(o, v) { o.b = v; return o; }
/* Payloads compiled at run time see the host through the global object. */
globalThis.hostRead = hostRead;
globalThis.hostWrite = hostWrite;
globalThis.HostRec = HostRec;
var ev = (0, eval)([
  "function EvRec(a, b) { this.a = a; this.b = b; }",
  "function evRead(o) { return o.a * 10 + o.b; }",
  "function evWrite(o, v) { o.a = v; return o; }",
  "var acc = { get v() { return hostRead(this.inner); }, set v(x) { hostWrite(this.inner, x); },",
  "            inner: new HostRec(1, 2) };",
  "var thrower = { get bad() { throw new Error('bad'); }, valueOf: function () { throw new Error('vo'); } };",
  "({ EvRec: EvRec, evRead: evRead, evWrite: evWrite, acc: acc, thrower: thrower })"
].join("\n"));
var sum = 0;
for (var i = 0; i < 200; i++) {
  var h = new HostRec(i, 1), e = new ev.EvRec(i, 2);
  sum += hostRead(e) + ev.evRead(h);
  ev.evWrite(h, i); hostWrite(e, i);
  sum += hostRead(h) + ev.evRead(e);
}
print(sum);
ev.acc.v = 7;
print(ev.acc.v, ev.acc.inner.b);
function hostGetBad(o) { return o.bad; }
function hostAddOne(o) { return o + 1; }
try { hostGetBad(ev.thrower); } catch (err) { print(err.message, frames(err)); }
try { hostAddOne(ev.thrower); } catch (err) { print(err.message, frames(err)); }
var evGetBad = (0, eval)("(function evGetBad(o) { return o.bad; })");
var hostThrower = { get bad() { throw new Error("hostbad"); } };
try { evGetBad(hostThrower); } catch (err) { print(err.message, frames(err)); }
