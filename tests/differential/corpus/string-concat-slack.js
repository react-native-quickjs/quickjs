// Differential corpus for the concatenation-result slack change (round-5 item
// C): JS_ConcatString1 asks the allocator for a few spare bytes and resets
// p->len, so the spare capacity is reachable only through
// js_malloc_usable_size() -- which is exactly the test JS_ConcatString2's
// in-place append makes.
//
// The change is meant to be semantically invisible, which is precisely why it
// needs a corpus: the failure modes are all "the extra capacity leaked into
// something that reads length from the allocation".  The cases below make each
// of those observable:
//
//   * length, indexing and iteration of a string built by concatenation
//   * the trailing NUL: a narrow string is C-string-compatible, so a stale byte
//     past p->len shows up through anything that goes via JS_ToCString
//   * a concatenation result used as a PROPERTY KEY, i.e. interned as an atom
//     whose hash and comparison are taken over p->len
//   * repeated in-place appends to the SAME accumulator, which is the path the
//     change turns on and where an off-by-one would corrupt the tail
//   * narrow -> wide promotion, where the slack is measured in characters
//   * a rope (> 8192 chars) that is later flattened

function chk(label, s) {
  var h = 0;
  for (var i = 0; i < s.length; i++) h = (h * 31 + s.charCodeAt(i)) | 0;
  print(label + " len=" + s.length + " hash=" + h +
        " head=" + s.slice(0, 12) + " tail=" + s.slice(-12));
}

// 1. the splay shape: fresh intermediate, then a second append onto it.
function key(tag) { return "String for key " + tag + " in leaf node"; }
for (var i = 0; i < 40; i++) print(key(i * 37));
chk("key big", key(1234567890123));

// 2. an accumulator appended to many times.  Every append after the first is
// the in-place path the change enables.
var acc = "";
for (var i = 0; i < 200; i++) acc = acc + (i % 10);
chk("acc", acc);
var acc2 = "x";
for (var i = 0; i < 64; i++) acc2 = acc2 + acc2.length;
chk("acc2", acc2);

// 3. lengths straddling every 8-byte allocation bucket boundary from 1 to 96,
// each built by concatenation and then appended to.
for (var n = 1; n <= 96; n++) {
  var a = "";
  for (var i = 0; i < n; i++) a = a + "a";
  var b = a + "Z";
  print("bucket " + n + " " + a.length + " " + b.length + " " +
        b.charAt(b.length - 1) + b.charAt(b.length - 2) + " " +
        (b === a + "Z") + " " + (b.indexOf("Z") === n));
}

// 4. the trailing NUL and C-string paths.  A stale byte past p->len would show
// up in anything that hands the buffer to C.
var s = "abc" + "def";
print("json " + JSON.stringify(s) + " " + JSON.stringify({ k: s }));
print("num " + Number("12" + "34") + " " + parseInt("56" + "78", 10) +
      " " + parseFloat("1" + ".5"));
print("uri " + encodeURIComponent("a b" + "c d"));
print("re " + /^abcdef$/.test(s) + " " + s.match(/c(d)e/)[1]);

// 5. concatenation result as a property key -- the string is interned, and the
// atom table hashes and compares over p->len.
var obj = {};
for (var i = 0; i < 30; i++) obj["pre" + i + "post"] = i;
var ks = Object.keys(obj);
print("keys " + ks.length + " " + ks.join(","));
print("lookup " + obj["pre" + 7 + "post"] + " " + ("pre7post" in obj) + " " +
      (obj["pre7post"] === 7));
var dyn = "a" + "b";
var o2 = {}; o2[dyn] = 1;
print("dyn key " + JSON.stringify(o2) + " " + (o2.ab === 1) + " " +
      (Object.keys(o2)[0].length === 2));

// 6. narrow -> wide.  The slack is a byte count, so a wide result gets half as
// many characters of it; an append that miscounts the width corrupts the tail.
var wide = "é€中";
var mix = "abc" + wide;
chk("mix", mix);
var wacc = "";
for (var i = 0; i < 100; i++) wacc = wacc + "€";
chk("wacc", wacc);
var wacc2 = "ascii-";
for (var i = 0; i < 50; i++) wacc2 = wacc2 + "中" + i;
chk("wacc2", wacc2);
print("wide cmp " + (mix === "abc" + "é€中") + " " +
      (wacc.charCodeAt(99) === 0x20ac) + " " + (wacc.length === 100));

// 7. a real rope (JS_STRING_ROPE_SHORT2_LEN is 8192) and its flattening.
var rope = "a".repeat(9000) + "b";
var flat = rope.split("").join("");
print("rope " + rope.length + " " + (rope === flat) + " " +
      (rope.charAt(9000) === "b") + " " + rope.slice(8995, 9001) + " " +
      (rope.indexOf("b") === 9000));
var rope2 = rope + "tail";
print("rope2 " + rope2.length + " " + rope2.slice(-5) + " " +
      (rope2.charAt(9000) === "b"));

// 8. surrogate pairs across a concatenation boundary must not be merged or
// split by the copy.
var hi = "\ud83d", lo = "\ude00";
var emoji = hi + lo;
print("emoji " + emoji.length + " " + emoji.codePointAt(0) + " " +
      (emoji === "😀") + " " + emoji.charCodeAt(1));
var e2 = "x" + emoji + "y" + emoji;
print("emoji2 " + e2.length + " " + Array.from(e2).length + " " +
      JSON.stringify(e2));

// 9. empty operands on both sides, which take the early-out branches.
var e = "";
print("empty " + JSON.stringify(e + "a") + JSON.stringify("a" + e) +
      JSON.stringify(e + e) + (("" + "") === ""));

// 10. a shared (refcount > 1) left operand must never be extended in place.
var shared = "shared-base-";
var one = shared + "one";
var two = shared + "two";
print("shared " + shared + " | " + one + " | " + two + " " +
      (shared.length === 12) + " " + (one !== two));
var arrOfRefs = [];
var base = "b";
for (var i = 0; i < 20; i++) { base = base + i; arrOfRefs.push(base); }
print("refs " + arrOfRefs.join("|"));

// ---------------------------------------------------------------------------
// SLACK-1 additions, 2026-08-15.  The section above was written for the
// "does the extra capacity leak into a length" failure mode.  These cases are
// for the OTHER failure mode, which is the dangerous one: the in-place append
// MUTATES a string that a second holder can still observe.  Every case below
// creates a second holder by a DIFFERENT mechanism and then appends to the
// same JSString, so that a build with the owner-count guard deleted prints
// different text rather than crashing.
//
// The aliasing surface is the one enumerated in docs/concat-1-inplace-append.md
// §4a: another variable, an object property, an array element, a closure
// capture, a Map/Set member, a property KEY (interned as an atom), the
// interpreter's own operand stack, a callee's argument frame, and a native
// that holds the string across a call that appends to it.

function show(l, v) { print("A" + l + " " + JSON.stringify(v)); }

// A1. second holder = another local variable.
(function () {
  var a = "alias-" + "base";           // fresh, uniquely owned
  var b = a;                            // b is now a second holder
  a = a + "XYZ";
  show("1", [a, b, a.length, b.length]);
})();

// A2. second holder = an object property.
(function () {
  var o = {};
  var s = "prop-" + "base";
  o.k = s;
  s = s + "APPENDED";
  show("2", [s, o.k, o.k.length]);
})();

// A3. second holder = an array element.
(function () {
  var arr = [];
  var s = "elem-" + "base";
  arr.push(s);
  s = s + "APPENDED";
  show("3", [s, arr[0], arr[0].length]);
})();

// A4. second holder = a closure capture (a var_ref, not a stack slot).
(function () {
  var s = "clos-" + "base";
  var get = function () { return s; };
  var t = s;                            // the closure's variable is `s`
  s = s + "APPENDED";                   // rebinds `s`; `t` still holds the old
  show("4", [s, t, get()]);
  // and the other direction: append to the value the closure still holds
  var u = t + "MORE";
  show("4b", [t, u, get()]);
})();

// A5. second holder = a Map key, a Map value and a Set member.
(function () {
  var m = new Map(), st = new Set();
  var s = "map-" + "base";
  m.set(s, 1); m.set("v", s); st.add(s);
  var s2 = s + "APPENDED";
  show("5", [s, s2, m.get("map-base"), m.get("v"), st.has("map-base"),
             m.get("v").length]);
})();

// A6. second holder = the ATOM TABLE.  js_dup deliberately does not count the
// atom table's hold, so this case is covered by the atom_type guard and NOT by
// the owner count -- it is the case that catches a build with that guard gone.
(function () {
  var o = {};
  var k = "atom-" + "key";              // fresh, uniquely owned
  o[k] = 1;                             // now interned as an atom
  var k2 = k + "SUFFIX";                // appending onto an interned string
  var keys = Object.keys(o);
  show("6", [k, k2, keys, o["atom-key"], o[k], keys[0].length]);
  // a second round: intern, append, intern again
  var j = "atom2-" + "key";
  o[j] = 2;
  j = j + "TAIL";
  o[j] = 3;
  show("6b", [Object.keys(o).sort(), o["atom2-key"], o["atom2-keyTAIL"]]);
})();

// A7. second holder = the interpreter's own operand stack, plus a callee's
// argument frame.  `f(s, s + "X")` evaluates `s` onto the stack and only then
// runs the concatenation, so the left operand is live in a stack slot at the
// moment of the append.
(function () {
  function f(x, y, z) { return [x, y, z]; }
  var s = "stack-" + "base";
  show("7", f(s, s + "X", s.length));
  var t = "stack2-" + "base";
  var pair = [t, t + "Y"];
  show("7b", [pair, t]);
})();

// A8. second holder = a native.  String.prototype.replace holds the subject
// string across the callback, and the callback appends to that same string.
(function () {
  var s = "native-" + "base";
  var out = s.replace(/base/g, function (m) { s = s + "MUTATED"; return m; });
  show("8", [s, out, out.length]);
  var u = "native2-" + "base";
  var joined = [u, u].join("|");
  u = u + "TAIL";
  show("8b", [u, joined]);
  var v = "native3-" + "base";
  var enc = encodeURIComponent(v);
  v = v + "TAIL";
  show("8c", [v, enc]);
})();

// A9. the SLICE guard.  A slice is only created for a substring longer than
// JS_STRING_SLICE_LEN_MAX (1024 bytes), and a slice's own block is
// sizeof(JSString) + sizeof(JSStringSlice) = 40 bytes, so the in-place
// append's size test (needs 24 + len + 1 >= 1049 bytes) can never pass on one.
// This case therefore CANNOT discriminate the kind == NORMAL guard -- it is
// here to record that fact and to catch any future change to either constant
// that would make it discriminable.  See docs/slack-1-string-slack.md.
(function () {
  var big = "";
  for (var i = 0; i < 300; i++) big = big + "0123456789";  // 3000 chars
  var sl = big.substring(5, 2005);      // > 1024 -> a real slice
  var r = big.substring(7, 2007) + "SLICETAIL";
  show("9", [big.length, big.slice(0, 12), big.slice(-12),
             sl.length, sl.slice(0, 6), sl.slice(-6),
             r.length, r.slice(-12), big.charAt(2007), big.charAt(2008)]);
})();

// A10. repeated appends to a string that is ALSO reachable from a growing
// array -- the shape that makes an off-by-one in the owner count visible as a
// divergence that accumulates rather than as a single wrong character.
(function () {
  var keep = [], s = "acc-";
  for (var i = 0; i < 40; i++) { s = s + (i % 10); keep.push(s); }
  show("10", [s, keep.length, keep[0], keep[19], keep[39], keep.join("").length]);
})();
