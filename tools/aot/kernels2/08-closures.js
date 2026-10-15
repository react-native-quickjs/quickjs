// Closures: captured variables, cells, counters, loops creating closures.
function make(n) {
  var c = 0;
  return function () { c = (c + n) | 0; return c; };
}
function k(n) {
  var f = make(3), g = make(5), s = 0, i;
  for (i = 0; i < n; i++) s = (s + f()) | 0;
  for (i = 0; i < n; i++) s = (s + g()) | 0;
  var fns = [], j;
  for (j = 0; j < 3; j++) fns.push(function () { return j; });
  return [s, f(), g(), fns[0](), fns[1](), fns[2]()];
}
print(k(1000));
