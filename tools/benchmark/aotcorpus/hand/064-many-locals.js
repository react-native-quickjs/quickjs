var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 203055;
function f(a, b) {
  var v0 = a, v1 = b, v2 = a + 1, v3 = b + 2, v4 = a ^ b, v5 = a | b, v6 = a & b, v7 = a - b;
  var v8 = v0 + v1, v9 = v2 * 3, v10 = v3 - v4, v11 = v5 + v6, v12 = v7 ^ v8, v13 = v9 | v10, v14 = v11 & 255, v15 = v12 + v13;
  var v16 = v14 * v15, v17 = v0 ^ v16, v18 = v1 + v17, v19 = v2 - v18, v20 = v3 ^ v19, v21 = v4 + v20, v22 = v5 - v21, v23 = v6 ^ v22;
  for (var i = 0; i < 3; i++) {
    v0 = (v0 + v23) | 0; v1 = (v1 ^ v22) | 0; v2 = (v2 + v21) | 0; v3 = (v3 ^ v20) | 0; v4 = (v4 + v19) | 0; v5 = (v5 ^ v18) | 0;
    v6 = (v6 + v17) | 0; v7 = (v7 ^ v16) | 0; v8 = (v8 + v15) | 0; v9 = (v9 ^ v14) | 0; v10 = (v10 + v13) | 0; v11 = (v11 ^ v12) | 0;
    v23 = v0 + v11; v22 = v1 + v10; v21 = v2 + v9; v20 = v3 + v8; v19 = v4 + v7; v18 = v5 + v6;
  }
  return (v0 + v1 + v2 + v3 + v4 + v5 + v6 + v7 + v8 + v9 + v10 + v11 + v12 + v13 + v14 + v15 + v16 + v17 + v18 + v19 + v20 + v21 + v22 + v23) | 0;
}
var t = 0;
for (var i = 0; i < N; i++) t = (t + f(i, t & 1023)) | 0;
mix(t);
print('many-locals ' + h);
