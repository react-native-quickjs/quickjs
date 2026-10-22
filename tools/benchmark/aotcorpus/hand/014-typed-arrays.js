var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 13161;
var tot = 0;
for (var i = 0; i < N; i++) {
  var i8 = new Int8Array(8), u8 = new Uint8Array(8), c8 = new Uint8ClampedArray(8);
  var i16 = new Int16Array(4), u16 = new Uint16Array(4), i32 = new Int32Array(4), u32 = new Uint32Array(4);
  var f32 = new Float32Array(4), f64 = new Float64Array(4);
  for (var j = 0; j < 8; j++) { var v = i * 37 + j * 1001 - 500; i8[j] = v; u8[j] = v; c8[j] = v; }
  for (var j = 0; j < 4; j++) { var v = i * 99991 + j * 77777; i16[j] = v; u16[j] = v; i32[j] = v * 40000; u32[j] = v * 40000; f32[j] = v / 7; f64[j] = v / 7; }
  tot += i8[3] + u8[3] + c8[3] + i16[2] + u16[2] + i32[1] + (u32[1] >>> 0) % 1000;
  mixd(f32[1]); mixd(f64[1]);
  var sub = u8.subarray(2, 6);
  sub[0] = 200;
  var cp = new Uint8Array(10);
  cp.set(sub, 3);
  cp.set([1, 2, 3], 7);
  tot += u8[2] + cp[3] + cp[9] + sub.length + sub.byteOffset;
}
mix(tot);
print('typed-arrays ' + h);
