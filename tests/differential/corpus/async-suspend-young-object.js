// A suspended async function's registers hold objects that nothing else references. Register VM: suspension made
// those registers counted references without noting nursery objects as referenced from the heap, so a young
// collection while the function waited freed them, and resuming parked the freed slot (a later reconcile freed it a
// second time: JetStream 3 doxbee-async crashed). Shape of that workload: helpers reached through a module object,
// many concurrent calls, an object literal built after an await and read after more awaits.
"use strict";
var F = (function () {
  async function none() { }
  async function take(a) { return a; }
  function Query(arg) { this.arg = arg; }
  Query.prototype.within = take;
  Query.prototype.get = none;
  function query(arg) { return new Query(arg); }
  return { q: query };
})();
var sum = 0;
async function job(i) {
  var tx = {};
  var file = await F.q().get();
  var version = { a: 1, b: i, c: 2, d: file ? 1 : null };
  var r1 = await F.q({ fileId: i, versionId: version.a }).within(tx);
  var r2 = await F.q({ id: i }).within(tx);
  sum += version.a + version.b + version.c + (r1 === tx ? 1 : 0) + (r2 === tx ? 1 : 0);
}
var rounds = 0;
function round() {
  if (rounds++ >= 6) { print("async-suspend-young-object " + sum); return; }
  var ps = new Array(10000);
  for (var i = 0; i < 10000; i++) ps[i] = job(i);
  return Promise.all(ps).then(round);
}
round();
