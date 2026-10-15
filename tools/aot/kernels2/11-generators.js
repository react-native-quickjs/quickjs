// Generators: function*, yield, for..of, spread of a generator.
function* gen(n) {
  var i;
  for (i = 0; i < n; i++) yield i * i;
}
function k(n) {
  var s = 0, it = gen(n), r;
  while (!(r = it.next()).done) s = (s + r.value) | 0;
  var t = 0, it2 = gen(5), x;
  for (x of it2) t = (t + x) | 0;
  return [s, t, [...gen(4)].join(",")];
}
print(k(2000));
