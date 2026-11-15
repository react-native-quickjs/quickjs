/*
 * Differential corpus: a call argument that is the value of an assignment
 * expression. The stack bytecode keeps the value with a stack shape (`insert3`
 * for an element store, `dup` for a variable), and the argument is produced in
 * the call window only when nothing else reads or rewrites its producer's
 * slot: the shape reads the slot that holds the array, and rewrites the slot
 * the argument comes from.
 */
function g(x, y) { return x === undefined ? "u" : x; }
function elemFirst(arr, i) { g(arr[i] = 2, 3); return arr[i]; }
function elemSecond(arr, i) { g(0, arr[i] = 5); return arr[i]; }
function elemOnly(arr, i) { var x = 1; g(arr[i] = x + 1); return arr[i]; }
function elemArg(arr, i) { return g(arr[i] = 7, 0) + ":" + arr[i]; }
function fieldArg(o) { return g(o.p = 4) + ":" + o.p; }
function varArg(b) { var a; var r = g(a = b, 7); return r + ":" + a; }
function readThenStore(arr, i) { var x; g(x = arr[i], arr[i] = x + 1); return x + ":" + arr[i]; }
function postInc(o) { var r = g(o.n++, 0); return r + ":" + o.n; }
var out = [];
var fs = [elemFirst, elemSecond, elemOnly, elemArg, readThenStore];
for (var k = 0; k < fs.length; k++) {
    try { out.push(fs[k]([10, 20], 1)); } catch (e) { out.push(fs[k].name + " threw " + e.message); }
}
out.push(fieldArg({}), varArg(3), postInc({ n: 1 }));
print(out.join(" "));
