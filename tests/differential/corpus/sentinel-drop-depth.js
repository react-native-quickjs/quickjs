/*
 * Differential corpus: control-stack sentinels at a depth that ordinary
 * values also use. A try's catch offset, a finally's return address and a
 * for-of iterator's close entry are dropped by hand only where the stack
 * bytecode discards them; an ordinary value dropped at the same depth
 * elsewhere must leave the control stack alone.
 */
var log = [];
function catchThenFinallyTry() {
    var c = 0;
    try { throw "ex1"; }
    catch (e) { log.push(e); }
    finally {
        try { throw "ex2"; }
        catch (e2) { log.push(e2); }
        finally { c = 1; }
    }
    return c;
}
function nestedRethrow() {
    try {
        try { throw "a"; }
        catch (e) { throw "b"; }
        finally { log.push("f1"); }
    } catch (e) { return e; }
}
function breakFromFinally() {
    var n = 0;
    for (var i = 0; i < 3; i++) {
        try { n += 1; }
        finally { if (i === 1) break; n += 10; }
    }
    return n;
}
function forOfInFinally(xs) {
    var s = 0;
    try { s = -1; }
    finally {
        for (var x of xs) { try { if (x === 2) throw x; s += x; } catch (e) { s += 100; } }
    }
    return s;
}
function forOfBreakInCatch(xs) {
    var s = 0;
    for (var x of xs) {
        try { throw x; }
        catch (e) { s += e; if (e === 2) break; }
        finally { s += 10; }
    }
    return s;
}
function* gen() {
    try { yield 1; }
    finally {
        try { throw "g"; } catch (e) { log.push(e); }
        yield 2;
    }
}
var g = gen(), gv = [g.next().value];
try { gv.push(g.throw("t").value); gv.push(g.next().done); } catch (e) { gv.push("threw " + e); }
print(catchThenFinallyTry(), nestedRethrow(), breakFromFinally(), forOfInFinally([1, 2, 3]),
      forOfBreakInCatch([1, 2, 3]), gv.join(","), log.join(","));
// A copy of the iterator's close slot (a `dup3`) is dropped by hand; the close
// entry stays, so the rejected value closes the iterator.
var returnCount = 0, aout = [];
var rejecting = { [Symbol.iterator]() { return { next() { return { value: Promise.reject("reject"), done: false }; }, return() { returnCount += 1; return {}; } }; } };
async function forAwaitClose() {
    try { for await (let _ of rejecting); } catch (e) { aout.push("caught " + e); }
    aout.push("returns=" + returnCount);
}
forAwaitClose().then(function () { print(aout.join(" ")); });
