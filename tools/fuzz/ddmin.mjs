// ddmin.mjs BIN SRC OUT [MODE] : minimise SRC (line-based) while `BIN SRC'` still dies with a signal (rc >= 128) and the
// stack interpreter (REF) finishes with rc < 124. BIN is a prebuilt AOT binary, so each probe is one process run.
import { spawnSync } from 'node:child_process';
import fs from 'node:fs';
const [bin, src, out, ref] = process.argv.slice(2);
let lines = fs.readFileSync(src, 'utf8').split('\n');
const tmp = out + '.probe.js';
function crashes(ls) {
  fs.writeFileSync(tmp, ls.join('\n'));
  const r = spawnSync(bin, [tmp], { timeout: 20000 });
  if (!(r.signal || (r.status !== null && r.status >= 128))) return false;
  if (ref) { const s = spawnSync(ref, [tmp], { timeout: 20000 }); if (s.signal || s.status === null || s.status >= 124) return false; }
  return true;
}
if (!crashes(lines)) { console.error('does not crash'); process.exit(1); }
let n = 2;
while (lines.length >= 2) {
  const chunk = Math.ceil(lines.length / n); let reduced = false;
  for (let i = 0; i < lines.length; i += chunk) {
    const cand = lines.slice(0, i).concat(lines.slice(i + chunk));
    if (cand.length && crashes(cand)) { lines = cand; n = Math.max(n - 1, 2); reduced = true; break; }
  }
  if (!reduced) { if (n >= lines.length) break; n = Math.min(lines.length, n * 2); }
}
fs.writeFileSync(out, lines.join('\n') + '\n'); fs.unlinkSync(tmp);
console.log(`minimised to ${lines.length} lines -> ${out}`);
