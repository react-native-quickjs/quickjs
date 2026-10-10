/* Alternating light-function / try-frame recursion, run to fixed depths.

   Guards the implicit-frame-top change (0205) and its seal fix (0208): a record
   push that opens a new arena chunk re-seals the light caller, which now lives
   in the previous chunk. Without 0208 that seal measured the light frame
   against the new chunk's base, marked the chunk full, and opened a fresh
   64 KB chunk on every push, so this recursion overflowed at depth ~141
   (0196: ~5120).

   Fixed depths, not a measured maximum, so the output is engine-independent
   and compares byte for byte with node. */

function leaf(k) { return k; }
function lightThenTry(k) { return k === 0 ? 0 : 1 + viaTry(k - 1); }
function viaTry(k) { try { return lightThenTry(k); } finally { leaf(k); } }

print(lightThenTry(100));
print(lightThenTry(500));
print(lightThenTry(1000));
