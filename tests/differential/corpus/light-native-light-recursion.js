/* Light function -> native Array.prototype.map -> light function recursion,
   run to fixed depths.

   Robustness fixture for the light -> native -> light re-entry (the shape
   `probe-recursion.js`'s `mid`/`infNative` exercise). It does NOT distinguish
   the 0208 seal fix: the native re-entry never pushes a record from a light
   frame, so the arena-chunk crossing that 0208 is about does not occur here
   (verified: it behaves identically with 0208 reverted). The record-push path
   is covered by `light-try-recursion.js`.

   Fixed depths, not a measured maximum, so the output is engine-independent
   and compares byte for byte with node. */

function mapRec(k) { return k === 0 ? 0 : 1 + [k - 1].map(mapRec)[0]; }

print(mapRec(100));
print(mapRec(500));
print(mapRec(1000));
