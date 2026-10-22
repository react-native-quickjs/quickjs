var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 3;
var PUSH = 0, ADD = 1, SUB = 2, MUL = 3, JNZ = 4, DUP = 5, POP = 6, LOAD = 7, STORE = 8, HALT = 9, LT = 10, JMP = 11, MOD = 12;
var prog = [
  PUSH, 0, STORE, 0,
  PUSH, 0, STORE, 1,
  LOAD, 1, PUSH, 3, ADD, STORE, 1,
  LOAD, 0, LOAD, 1, MUL, PUSH, 1000, MOD, ADD, STORE, 0,
  LOAD, 1, PUSH, 20000, LT, JNZ, 8,
  LOAD, 0, HALT
];
function run(p) {
  var st = [], mem = [0, 0, 0, 0], pc = 0, sp = 0;
  for (;;) {
    switch (p[pc++]) {
      case PUSH: st[sp++] = p[pc++]; break;
      case ADD: sp--; st[sp - 1] = (st[sp - 1] + st[sp]) | 0; break;
      case SUB: sp--; st[sp - 1] = (st[sp - 1] - st[sp]) | 0; break;
      case MUL: sp--; st[sp - 1] = Math.imul(st[sp - 1], st[sp]); break;
      case MOD: sp--; st[sp - 1] = st[sp - 1] % st[sp]; break;
      case LT: sp--; st[sp - 1] = st[sp - 1] < st[sp] ? 1 : 0; break;
      case JNZ: { var t = p[pc++]; if (st[--sp]) pc = t; break; }
      case JMP: pc = p[pc]; break;
      case DUP: st[sp] = st[sp - 1]; sp++; break;
      case POP: sp--; break;
      case LOAD: st[sp++] = mem[p[pc++]]; break;
      case STORE: mem[p[pc++]] = st[--sp]; break;
      case HALT: return st[sp - 1];
      default: throw new Error("bad op " + p[pc - 1]);
    }
  }
}
for (var i = 0; i < N; i++) mix(run(prog));
print('mini-bytecode-vm ' + h);
