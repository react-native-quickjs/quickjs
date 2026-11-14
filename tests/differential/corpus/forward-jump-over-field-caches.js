// A forward jump over a short run of property accesses. The compiler picks a jump's 8-bit form from the distance to its
// label before the final code is laid out; field accesses grow by an inline-cache index on the way, so a jump over four
// `this.p.x = this.c.x` stores looked close enough for 8 bits but landed over 127 bytes away, and the function failed to
// compile ("inconsistent stack size"). Shape taken from JetStream 3 babylonjs (a viewport cache check).
"use strict";
function Ctx() {
  this.c = { x: 1, y: 2, z: 3, w: 4 };
  this.p = { x: 0, y: 0, w: 0, h: 0 };
}
Ctx.prototype.sync = function () {
  var e = this.c.x, t = this.c.y, n = this.c.z, r = this.c.w,
      i = this.p.x !== e || this.p.y !== t || this.p.w !== n || this.p.h !== r;
  return i && (this.p.x = this.c.x, this.p.y = this.c.y, this.p.w = this.c.z, this.p.h = this.c.w), i;
};
Ctx.prototype.copy = function (i) {
  i && (this.p.x = this.c.x, this.p.y = this.c.y, this.p.w = this.c.z, this.p.h = this.c.w);
  return this.p.x + this.p.y + this.p.w + this.p.h;
};
var c = new Ctx(), out = [];
out.push(c.sync(), c.sync(), JSON.stringify(c.p));
c.c.y = 7;
out.push(c.sync(), c.copy(0), c.copy(1));
print("forward-jump-over-field-caches " + out.join(" "));
