/*
 * Differential corpus: a throw from inside a getter, a setter, a `valueOf`
 * called by `+`, and a `toString` called by a template literal.
 *
 * The frame that triggered the accessor or the coercion must keep its source
 * position. The accessor runs below it, and when it throws the stack has to
 * name the line that performed the read, the write or the conversion -- not
 * `(missing)`, which is what a register frame without a captured ip prints.
 *
 * Only the frame's name and line are compared. The column is dropped because
 * the two engines legitimately name different positions inside the same
 * expression, and node prefixes a method frame with its receiver class
 * (`Object.valueOf`) where quickjs prints the bare name; both are legal, so
 * neither is asserted. A frame that loses its position disappears from this
 * output entirely, which is the regression this file is for.
 */
function frames(e) {
  var out = [];
  var ls = String(e.stack).split("\n");
  for (var i = 0; i < ls.length; i++) {
    var m = /^\s*at\s+(.*?)\s+\((.*):(\d+):(\d+)\)\s*$/.exec(ls[i]);
    if (!m) continue;
    var name = m[1].replace(/\s*\[as\s+[^\]]+\]\s*$/, "").replace(/^Object\./, "");
    out.push(name + "@" + m[3]);
    if (out.length === 3) break;
  }
  return out.join(" | ");
}

function readX(o) { return o.x; }
function writeX(o) { o.x = 1; }
function addOne(o) { return o + 1; }
function toText(o) { return `${o}`; }

function getterCase() {
  return readX({ get x() { throw new Error("getter"); } });
}
function setterCase() {
  return writeX({ set x(v) { throw new Error("setter"); } });
}
function valueOfCase() {
  return addOne({ valueOf() { throw new Error("valueOf"); } });
}
function toStringCase() {
  return toText({ toString() { throw new Error("toString"); } });
}

function show(f) {
  try { f(); } catch (e) { print(frames(e)); }
}
show(getterCase);
show(setterCase);
show(valueOfCase);
show(toStringCase);
