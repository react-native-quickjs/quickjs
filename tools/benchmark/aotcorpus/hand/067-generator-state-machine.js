var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 38092;
function* machine() {
  var state = 0, total = 0;
  for (;;) {
    var inp = yield state;
    switch (state) {
      case 0: state = inp > 0 ? 1 : 2; break;
      case 1: total += inp; state = total > 50 ? 3 : 0; break;
      case 2: state = 0; total = 0; break;
      case 3: total = 0; state = 0; break;
    }
  }
}
var g = machine(); g.next();
var t = 0;
for (var i = 0; i < N * 10; i++) t = (t * 3 + g.next((i * 7) % 13 - 4).value) | 0;
mix(t);
print('generator-state-machine ' + h);
