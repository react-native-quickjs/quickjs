// Differential corpus for the inline %TypedArray%.prototype.length getter on
// OP_get_length (round-7 item R7-L).
//
// The arm does not guess.  It lets the ordinary prototype walk run and then
// checks the IDENTITY of the accessor it landed on, so each guard below is
// made observable by a case the walk must still resolve the slow way:
//
//   * the exotic-class test  -- only a TYPED ARRAY may continue the walk; a
//                               Proxy, a String object, `arguments` and a
//                               module namespace must keep their [[Get]]
//   * the getter-identity    -- a subclass or a patched prototype that puts a
//     test                      DIFFERENT accessor on `length` must call it
//   * the receiver test      -- an object that merely INHERITS from a typed
//                               array is not a typed array and must throw
//
// Guards are proven live by rebuilding the engine with each one deleted and
// checking that THIS file fails.

function show(label, f) {
  var r;
  // Only the error TYPE: the message text is engine wording, not semantics.
  try { r = String(f()); } catch (e) { r = "threw " + e.constructor.name; }
  print(label + " = " + r);
}

var CTORS = [Int8Array, Uint8Array, Uint8ClampedArray, Int16Array, Uint16Array,
             Int32Array, Uint32Array, Float32Array, Float64Array,
             BigInt64Array, BigUint64Array];

// --- 1. every view class, plain and with a byte offset / explicit length.
for (var c = 0; c < CTORS.length; c++) {
  var C = CTORS[c];
  var bpe = C.BYTES_PER_ELEMENT;
  var a = new C(5);
  var buf = new ArrayBuffer(bpe * 8);
  var b = new C(buf);
  var d = new C(buf, bpe * 2);
  var e = new C(buf, bpe * 2, 3);
  print(C.name + " lengths: " + a.length + "," + b.length + "," + d.length +
        "," + e.length);
}

// --- 2. an empty view and a zero-length view over a non-empty buffer.
print("empty: " + new Uint8Array(0).length + "," +
      new Uint8Array(new ArrayBuffer(8), 8).length);

// --- 3. detached buffer.  The getter returns count, which detach sets to 0.
var db = new ArrayBuffer(8);
var dv = new Uint8Array(db);
print("before detach: " + dv.length);
if (typeof structuredClone === "function") {
  try { structuredClone(db, { transfer: [db] }); } catch (err) {}
}
if (typeof db.transfer === "function" && db.byteLength !== 0) db.transfer();
print("after detach: " + dv.length + " (detached=" + (db.byteLength === 0) + ")");
show("detached element read", function () { return dv[0]; });

// --- 4. resizable ArrayBuffer: a tracking view and a fixed-length view.
var rab = new ArrayBuffer(8, { maxByteLength: 64 });
var track = new Uint8Array(rab);
var fixed = new Uint8Array(rab, 0, 4);
var off = new Uint16Array(rab, 4);
print("rab 8: " + track.length + "," + fixed.length + "," + off.length);
rab.resize(32);
print("rab 32: " + track.length + "," + fixed.length + "," + off.length);
rab.resize(4);
print("rab 4: " + track.length + "," + fixed.length + "," + off.length);
rab.resize(0);
print("rab 0: " + track.length + "," + fixed.length + "," + off.length);
rab.resize(16);
print("rab 16: " + track.length + "," + fixed.length + "," + off.length);

// --- 5. own `length` on the instance shadows the intrinsic getter.
var own = new Uint8Array(7);
Object.defineProperty(own, "length", { value: 4242, configurable: true });
print("own data property: " + own.length);
Object.defineProperty(own, "length",
                      { get: function () { return 1; }, configurable: true });
print("own accessor: " + own.length);
delete own.length;
print("after delete: " + own.length);

// --- 6. subclasses.  GUARD: getter identity.
var Shadow = function () {};
Shadow.prototype = Object.create(Uint8Array.prototype);
var shadowInst = new Uint8Array(7);
Object.setPrototypeOf(shadowInst, Shadow.prototype);
Object.defineProperty(Shadow.prototype, "length", { value: 99 });
print("subclass data property: " + shadowInst.length);

var Acc = function () {};
Acc.prototype = Object.create(Uint8Array.prototype);
var accInst = new Uint8Array(7);
Object.setPrototypeOf(accInst, Acc.prototype);
var accCalls = 0;
Object.defineProperty(Acc.prototype, "length",
    { get: function () { accCalls++; return 55; }, configurable: true });
print("subclass accessor: " + accInst.length + " calls=" + accCalls);
print("subclass accessor this: " +
      (function () {
         var seen;
         Object.defineProperty(Acc.prototype, "length",
             { get: function () { seen = this === accInst; return 0; },
               configurable: true });
         accInst.length;
         return seen;
       })());

// --- 7. a getter that is a NATIVE accessor but not the length one.  The view
// is Float64Array(7) so byteLength (56) and length (7) CANNOT be confused: a
// build that stops checking the getter's identity prints 7 here.
var TAPROTO = Object.getPrototypeOf(Uint8Array.prototype);
var Other = new Float64Array(7);
Object.setPrototypeOf(Other,
    Object.create(Float64Array.prototype, {
        length: Object.getOwnPropertyDescriptor(TAPROTO, "byteLength")
    }));
print("foreign native getter (byteLength): " + Other.length);
var Other2 = new Float64Array(7);
Object.setPrototypeOf(Other2,
    Object.create(Float64Array.prototype, {
        length: Object.getOwnPropertyDescriptor(TAPROTO, "byteOffset")
    }));
print("foreign native getter (byteOffset): " + Other2.length);
var Other3 = new Float64Array(new ArrayBuffer(64), 8, 3);
Object.setPrototypeOf(Other3,
    Object.create(Float64Array.prototype, {
        length: Object.getOwnPropertyDescriptor(TAPROTO, "byteOffset")
    }));
print("foreign native getter (byteOffset, offset view): " + Other3.length);
// A JS getter, i.e. not a C function at all.
var Other4 = new Float64Array(7);
Object.setPrototypeOf(Other4,
    Object.create(Float64Array.prototype, {
        length: { get: function () { return "js-getter"; } }
    }));
print("js getter: " + Other4.length);

// --- 8. receiver only INHERITS from a typed array.  GUARD: receiver test.
show("inherits from instance", function () {
  return Object.create(new Uint8Array(7)).length;
});
show("inherits from prototype", function () {
  return Object.create(Uint8Array.prototype).length;
});
show("plain object with TA proto in chain", function () {
  var o = Object.create(Object.create(Uint8Array.prototype));
  return o.length;
});

// --- 9. patched intrinsic %TypedArray%.prototype.length.
var TAP = Object.getPrototypeOf(Uint8Array.prototype);
var savedDesc = Object.getOwnPropertyDescriptor(TAP, "length");
var live = new Uint8Array(7);
Object.defineProperty(TAP, "length",
                      { get: function () { return 123; }, configurable: true });
print("patched intrinsic: " + live.length);
Object.defineProperty(TAP, "length", { value: 321, configurable: true });
print("patched to data: " + live.length);
delete TAP.length;
print("deleted intrinsic: " + live.length);
Object.defineProperty(TAP, "length", savedDesc);
print("restored intrinsic: " + live.length);

// --- 10. re-pointed [[Prototype]] on the instance.
var rp = new Uint8Array(7);
Object.setPrototypeOf(rp, { length: 11 });
print("proto object: " + rp.length);
Object.setPrototypeOf(rp, null);
print("proto null: " + rp.length);
Object.setPrototypeOf(rp, Uint8Array.prototype);
print("proto restored: " + rp.length);

// --- 11. other exotics must keep their own [[Get]].  GUARD: exotic-class test.
print("string primitive: " + "abcd".length);
print("string object: " + new String("abcdef").length);
print("array: " + [1, 2, 3].length);
print("holey array: " + [1, , 3].length);
print("function: " + function (a, b, c) {}.length);
print("arguments: " +
      (function () { return arguments.length; })(1, 2, 3, 4));
var px = new Proxy(new Uint8Array(7), {
  get: function (t, k, r) { return k === "length" ? "proxied" : t[k]; }
});
print("proxy over typed array: " + px.length);
var px2 = new Proxy({}, { get: function () { return "any"; } });
print("proxy over plain: " + px2.length);
var px3 = new Proxy([1, 2, 3], {});
print("transparent proxy over array: " + px3.length);

// --- 12. DataView has no `length`.
print("dataview: " + new DataView(new ArrayBuffer(8)).length);

// --- 13. the value is a plain int, usable in arithmetic and strict compares.
var t = new Int32Array(3);
print("arith: " + (t.length + 1) + " " + (t.length === 3) + " " +
      (typeof t.length) + " " + (t.length | 0));

// --- 14. a hot loop, so the reading is the same after any quickening.
var hot = new Float64Array(9);
var acc = 0;
for (var i = 0; i < 200000; i++) acc += hot.length;
print("hot loop: " + acc);

// --- 15. the same loop shape where the prototype is patched half way.
var hot2 = new Int16Array(4);
var acc2 = 0;
for (var i = 0; i < 2000; i++) {
  if (i === 1000) {
    Object.defineProperty(TAP, "length",
        { get: function () { return 1000; }, configurable: true });
  }
  acc2 += hot2.length;
}
Object.defineProperty(TAP, "length", savedDesc);
print("patched mid-loop: " + acc2);
