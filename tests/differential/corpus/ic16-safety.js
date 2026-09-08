/* 0016 safety stress: weak megamorphic entries and prototype invalidation. */
function fail(s) { throw new Error(s); }
function check(v, s) { if (!v) fail(s); }

function read(o) { return o.value; }

/* Promote the site, then retire all of the shapes which populated the weak
   fallback table.  The following allocation wave exercises stale-address
   protection and ordinary lookup after shape-free epochs advance. */
var retired = [];
for (var round = 0; round < 12; round++) {
  var a = { value: round };
  for (var j = 0; j < round; j++) a['p' + j] = j;
  retired.push(a);
  check(read(a) === round, 'warm');
}
retired = null;
var checksum = 0;
for (var wave = 0; wave < 80; wave++) {
  var objs = [];
  for (var i = 0; i < 250; i++) {
    var o = { value: wave * 250 + i };
    for (var k = 0; k < ((i + wave) % 9); k++) o['q' + k] = k;
    checksum += read(o);
    if ((i & 15) === 0) objs.push(o);
  }
}
check(checksum > 0, 'shape reuse checksum');

/* Depth-two and final-holder mutation matrix, including failed/cyclic and
   exotic traversals. */
function depth(o) { return o.x; }
var final = { x: 'final' }, middle = Object.create(final), receiver = Object.create(middle);
check(depth(receiver) === 'final', 'depth2 warm');
delete final.x;
final.x = 'replacement';
check(depth(receiver) === 'replacement', 'final replacement');
Object.defineProperty(final, 'x', { get: function () { return 'accessor'; }, configurable: true });
check(depth(receiver) === 'accessor', 'accessor conversion');
Object.defineProperty(final, 'x', { value: 'readonly', writable: false, configurable: true });
check(depth(receiver) === 'readonly', 'writable conversion');
var other = { x: 'other' };
Object.setPrototypeOf(middle, other);
check(depth(receiver) === 'other', 'intermediate replacement');
Object.setPrototypeOf(receiver, Object.create({ x: 'chain' }));
check(depth(receiver) === 'chain', 'chain replacement');
try { Object.setPrototypeOf(receiver, receiver); fail('cyclic mutation accepted'); } catch (e) {}
var p = new Proxy({ x: 'proxy' }, { get: function (t, k) { return k === 'x' ? 'proxy-trap' : t[k]; } });
check(depth(p) === 'proxy-trap', 'proxy');
print('ic16-safety ok checksum=' + checksum);
