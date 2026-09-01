// Polymorphic inline-cache sites: exercise entries BEYOND the first.
//
// WHY THIS EXISTS. The IC table stores one entry per site inline and the
// remaining JS_IC_ENTRIES-1 in an overflow region. Every existing corpus file
// together produces exactly ONE overflow-entry hit, so any bug in the overflow
// indexing -- wrong entry, wrong offset, wrong site -- is invisible to the
// differential. Measured with a hit counter before this file was written.
//
// Each shape below puts `f` at a DIFFERENT slot offset, so reading it through
// one call site that has seen many shapes will return the WRONG VALUE (not
// merely miss) if an overflow entry is indexed wrongly.

function shapeWithFAt(n, tag) {
    var o = {};
    for (var i = 0; i < n; i++) o["pad" + i] = i;   // push `f` to slot n
    o.f = "F" + tag;
    o.g = "G" + tag;
    return o;
}

// One site, many receiver shapes: fills entry 0 + the overflow entries and,
// past capacity, drives the site megamorphic.
function readF(o) { return o.f; }
function readG(o) { return o.g; }

var objs = [];
for (var n = 0; n < 6; n++) objs.push(shapeWithFAt(n, n));

var out = [];
for (var iter = 0; iter < 40; iter++) {
    for (var i = 0; i < objs.length; i++) {
        out.push(readF(objs[i]));
        out.push(readG(objs[i]));
    }
}
print("cycle:", out.join(","));

// Exactly-at-capacity: 4 shapes through one site, repeated, so entries 1..3
// are hit steadily rather than being evicted by a 5th shape.
var four = [shapeWithFAt(0, "a"), shapeWithFAt(1, "b"),
            shapeWithFAt(2, "c"), shapeWithFAt(3, "d")];
var out2 = [];
for (var iter2 = 0; iter2 < 60; iter2++)
    for (var j = 0; j < four.length; j++) out2.push(readF(four[j]));
print("four:", out2.join(","));

// Prototype-chain reads through a polymorphic site: the proto entries carry a
// second guard, so a wrong entry here also yields a wrong value.
function protoOf(tag, depth) {
    var base = { inherited: "I" + tag };
    var o = Object.create(base);
    for (var i = 0; i < depth; i++) o["q" + i] = i;
    return o;
}
var protos = [protoOf(0, 0), protoOf(1, 1), protoOf(2, 2), protoOf(3, 3), protoOf(4, 4)];
function readInherited(o) { return o.inherited; }
var out3 = [];
for (var k = 0; k < 40; k++)
    for (var p = 0; p < protos.length; p++) out3.push(readInherited(protos[p]));
print("proto:", out3.join(","));

// Writes through a polymorphic site: put_field caches transitions in the same
// split table, so overwrite/transition entries past entry 0 are covered too.
function writeF(o, v) { o.f = v; return o.f; }
var out4 = [];
for (var w = 0; w < 40; w++)
    for (var m = 0; m < objs.length; m++) out4.push(writeF(objs[m], "W" + m + "_" + w));
print("write:", out4.join(","));
