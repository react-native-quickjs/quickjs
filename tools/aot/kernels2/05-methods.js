// Methods: this reads, prototype reads, method calls, instanceof.
function P(x) { this.x = x; }
P.prototype.get = function () { return this.x; };
P.prototype.add = function (d) { return this.x + d; };
function k(n) {
  var p = new P(3), s = 0, i;
  for (i = 0; i < n; i++) {
    s = (s + p.get()) | 0;
    s = (s + p.add(i & 3)) | 0;
  }
  var q = { v: 10, f: function () { return this.v; } };
  return [s, p.get(), p.add(4), q.f(), q instanceof Object, p instanceof P];
}
print(k(10000));
