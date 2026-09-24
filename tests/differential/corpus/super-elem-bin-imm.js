function run(a, i) {
  var l = a[i] & 0x3fff;
  var h = a[i] >> 14;
  for (var n = 0; n < 3; n++) {
    l = a[i] & 0x3fff;
    h = a[i] >> 14;
  }
  return [l, h];
}

function alias(a, i) {
  a[i] = a[i] & 0x3fff;
  return a[i] >> 14;
}

function unsupported(a, i, x) {
  return a[i] & x;
}

var getter = { get 0() { throw new Error("getter"); } };
var results = [];
results.push(JSON.stringify(run([65535], 0)));
results.push(JSON.stringify(alias([65535], 0)));
results.push(JSON.stringify(unsupported([65535], 0, 3)));
try { run(getter, 0); } catch (e) { results.push(e.message); }
try { run([1n], 0); } catch (e) { results.push(e.name); }
print(results.join("|"));
