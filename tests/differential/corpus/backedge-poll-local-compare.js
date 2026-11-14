// Register VM: a `for (let u = ...; u >= 0; --u)` loop whose test compares a frame local with a register
// (branch_cmp_loc_reg) ran the back-edge poll on the branch offset measured from the wrong end of the instruction.
// When the poll fired (interrupt countdown or the deferred-count safepoint) the loop resumed ten bytes early, in the
// middle of an earlier instruction: "unimplemented opcode 0" (JetStream 3 typescript-lib, forEachChildRecursively).
// Enough calls for the poll to fire on that back edge many times.
function walk(e) {
  var r = [e], n = 0;
  for (; 0 !== r.length;) {
    var x = r.pop();
    if (Array.isArray(x)) {
      for (let u = x.length - 1; u >= 0; --u) r.push(x[u]);
    } else {
      n += x.kind;
      if (x.kind >= 167) for (const w of x.kids) r.push(w);
    }
  }
  return n;
}
var tree = [{ kind: 200, kids: [{ kind: 1 }, [{ kind: 2 }, { kind: 3 }]] }, { kind: 4 }];
var total = 0;
for (var k = 0; k < 20000; k++) total += walk(tree);
print("backedge-poll-local-compare " + total);
