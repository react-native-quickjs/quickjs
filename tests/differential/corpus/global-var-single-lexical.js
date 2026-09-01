/* Second differential file for the `global_var_obj` empty-shape early-out.

   It exists because of a mutation-testing result, not because of a hypothesis.
   The main file, global-var-lookup.js, declares sixteen-plus top-level `let`
   bindings, and QuickJS creates every global lexical binding in
   `global_var_obj` at script entry (they are hoisted, in the TDZ).  So its
   `prop_count` is never 1 at any point during that file, and a mutant guarding
   on `prop_count != 1` instead of `!= 0` passed it byte-for-byte.

   This file has EXACTLY ONE top-level lexical declaration, so `prop_count`
   is 1 for the whole run and the off-by-one mutant reads the wrong value.

   MEASURED 2026-07-30: `prop_count != 1` fails on this file and passes on the
   other; `prop_count != 0` (the shipped guard) passes both. */

globalThis.only = 'only-from-globalThis';
let only = 'only-lexical';

function readOnly() { return only; }
function writeOnly(v) { only = v; }

var s = '';
for (var i = 0; i < 200; i++) s = readOnly();
print('1 ' + s);
print('2 ' + globalThis.only);
writeOnly('only-lexical-updated');
print('3 ' + readOnly() + ' | ' + globalThis.only);
