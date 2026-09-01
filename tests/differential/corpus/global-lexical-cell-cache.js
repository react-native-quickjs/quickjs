/* Discriminator for patch 0077, the global cell cache for OP_get_var.

   The cache memoizes "this atom is own data property #offset of THIS object",
   where the object is ctx->global_obj (a global `var` or function declaration)
   or ctx->global_var_obj (a global `let`/`const`/`class`), validated by a
   runtime-wide generation counter bumped from add_shape_property(),
   add_property() and js_shape_prepare_update().

   Two properties have to survive, and they are what this file attacks:

     1. the cached read carries NO temporal-dead-zone test, so anything that
        can make an initialised global lexical observable as uninitialised
        again must invalidate first;
     2. the cached read carries no shadowing test, so a lexical binding
        appearing over a same-named global object property must invalidate.

   KNOWN LIMIT OF THIS FILE, stated rather than glossed: `let` is hoisted
   within a script, so a single script cannot express "name X is cached as a
   global object property and only THEN becomes a lexical binding".  That case
   needs three separate JS_Eval calls and lives in bench/spikes/gvlex/; it is
   the sole test that kills the `mut-noaddboth` pair mutation.  Everything a
   one-script corpus CAN reach is here.

   Also note the geometry the file is sized against: 128 sets x 2 ways, indexed
   by `atom & 127`.  The 400-name block below is there to force eviction, and
   the paired-name block to force a set collision. */

/* ---- 1. ordinary reads, repeated so the cell is filled and then hit ----- */
var v1 = 'var-one';
let l1 = 'let-one';
const c1 = 'const-one';
class K1 { static tag() { return 'class-one'; } }
function f1() { return 'fn-one'; }

function readAll() { return v1 + '|' + l1 + '|' + c1 + '|' + K1.tag() + '|' + f1(); }
var out = '';
for (var i = 0; i < 500; i++) out = readAll();
print('1 ' + out);

/* ---- 2. writes through the cached slot must be seen ------------------- */
v1 = 'var-one-B';
l1 = 'let-one-B';
print('2 ' + readAll());

/* ---- 3. const reassignment still throws ------------------------------- */
try { eval('c1 = "nope"'); } catch (e) { print('3 ' + e.constructor.name); }

/* ---- 4. TDZ on let / const / class ------------------------------------ */
try { print('x' + tdzLet); } catch (e) { print('4a ' + e.constructor.name); }
try { print('x' + tdzConst); } catch (e) { print('4b ' + e.constructor.name); }
try { print('x' + TdzClass); } catch (e) { print('4c ' + e.constructor.name); }
try { print('4d ' + typeof tdzLet); } catch (e) { print('4d ' + e.constructor.name); }
let tdzLet = 'tdz-let';
const tdzConst = 'tdz-const';
class TdzClass { }
print('4e ' + tdzLet + ' ' + tdzConst + ' ' + (typeof TdzClass));

/* a class that names itself in its own extends clause is in its own TDZ */
try { eval('class SelfRef extends SelfRef {}'); } catch (e) { print('4f ' + e.constructor.name); }

/* reading a TDZ binding many times must throw every time, not just once —
   a cache that filled on the throwing path would show up here */
var tdzThrows = 0;
for (var t = 0; t < 50; t++) {
  try { void laterLet; } catch (e) { tdzThrows++; }
}
print('4g ' + tdzThrows);
let laterLet = 'later';
print('4h ' + laterLet);

/* ---- 5. a lexical binding shadowing a global object property ---------- */
globalThis.shadowed = 'from-globalThis';
print('5a ' + globalThis.shadowed);
let shadowed = 'from-lexical';
function readShadowed() { return shadowed; }
var acc5 = '';
for (var s5 = 0; s5 < 300; s5++) acc5 = readShadowed();
print('5b ' + acc5 + ' | ' + globalThis.shadowed);
globalThis.shadowed = 'from-globalThis-B';
print('5c ' + readShadowed() + ' | ' + globalThis.shadowed);

/* ---- 6. the other order: property added AFTER the lexical is cached --- */
let ordered = 'lexical-first';
function readOrdered() { return ordered; }
for (var s6 = 0; s6 < 300; s6++) readOrdered();
globalThis.ordered = 'property-second';
print('6 ' + readOrdered() + ' | ' + globalThis.ordered);

/* ---- 7. deleting a global object property under a cached name --------- */
globalThis.deletable = 'delete-me';
function readDeletable() { return typeof deletable === 'undefined' ? 'gone' : deletable; }
for (var s7 = 0; s7 < 300; s7++) readDeletable();
print('7a ' + readDeletable());
delete globalThis.deletable;
print('7b ' + readDeletable());
globalThis.deletable = 'back-again';
print('7c ' + readDeletable());

/* A non-configurable property cannot be deleted, so the cached slot must stay
   valid.  (This deliberately does NOT use a top-level `var`: node runs this
   file as a CommonJS module, where a top-level `var` is module-scoped and
   never becomes a property of globalThis at all, so the two engines would
   disagree for a reason that has nothing to do with the cache.) */
Object.defineProperty(globalThis, 'undeletable', { value: 'stays', configurable: false, writable: true });
function readUndeletable() { return undeletable; }
for (var s7b = 0; s7b < 200; s7b++) readUndeletable();
print('7d ' + (delete globalThis.undeletable) + ' ' + readUndeletable());

/* ---- 8. turning a cached data property into an accessor --------------- */
globalThis.morph = 'data';
function readMorph() { return morph; }
for (var s8 = 0; s8 < 300; s8++) readMorph();
print('8a ' + readMorph());
Object.defineProperty(globalThis, 'morph', { get: function () { return 'accessor'; }, configurable: true });
print('8b ' + readMorph());
Object.defineProperty(globalThis, 'morph', { value: 'data-again', writable: true, configurable: true });
print('8c ' + readMorph());

/* and making one non-writable */
globalThis.frozenish = 'w';
function readFrozenish() { return frozenish; }
for (var s9 = 0; s9 < 200; s9++) readFrozenish();
Object.defineProperty(globalThis, 'frozenish', { writable: false });
print('8d ' + readFrozenish());

/* ---- 9. adding many globals: the fast branch of add_property ---------- */
/* add_property() has a find_hashed_shape_prop branch that installs a
   ready-made shape and returns WITHOUT calling add_shape_property.  A cache
   that only hooks add_shape_property goes stale here in principle; the same
   shape sequence is replayed so that branch is reached. */
function addBatch(prefix, n) {
  for (var a = 0; a < n; a++) globalThis[prefix + a] = prefix + '-' + a;
}
addBatch('bat', 40);
print('9a ' + bat0 + ' ' + bat39);
addBatch('bat', 40);
print('9b ' + bat0 + ' ' + bat39);
globalThis.bat0 = 'bat-0-rewritten';
print('9c ' + bat0);

/* ---- 10. 400 lexical names against 128 sets: eviction ----------------- */
/* The table is 128 sets x 2 ways indexed by `atom & 127`, so 400 distinct
   global lexical names guarantee that a cell filled early is evicted before it
   is read again.  They are written out literally rather than eval'd: `let` in
   a direct eval creates the binding in the EVAL's scope, not in
   global_var_obj, so an eval-generated version tests nothing here (and throws
   ReferenceError on node, which is how that mistake was caught). */
let ev0 = 0, ev1 = 1, ev2 = 2, ev3 = 3, ev4 = 4, ev5 = 5, ev6 = 6, ev7 = 7, ev8 = 8, ev9 = 9;
let ev10 = 10, ev11 = 11, ev12 = 12, ev13 = 13, ev14 = 14, ev15 = 15, ev16 = 16, ev17 = 17, ev18 = 18, ev19 = 19;
let ev20 = 20, ev21 = 21, ev22 = 22, ev23 = 23, ev24 = 24, ev25 = 25, ev26 = 26, ev27 = 27, ev28 = 28, ev29 = 29;
let ev30 = 30, ev31 = 31, ev32 = 32, ev33 = 33, ev34 = 34, ev35 = 35, ev36 = 36, ev37 = 37, ev38 = 38, ev39 = 39;
let ev40 = 40, ev41 = 41, ev42 = 42, ev43 = 43, ev44 = 44, ev45 = 45, ev46 = 46, ev47 = 47, ev48 = 48, ev49 = 49;
let ev50 = 50, ev51 = 51, ev52 = 52, ev53 = 53, ev54 = 54, ev55 = 55, ev56 = 56, ev57 = 57, ev58 = 58, ev59 = 59;
let ev60 = 60, ev61 = 61, ev62 = 62, ev63 = 63, ev64 = 64, ev65 = 65, ev66 = 66, ev67 = 67, ev68 = 68, ev69 = 69;
let ev70 = 70, ev71 = 71, ev72 = 72, ev73 = 73, ev74 = 74, ev75 = 75, ev76 = 76, ev77 = 77, ev78 = 78, ev79 = 79;
let ev80 = 80, ev81 = 81, ev82 = 82, ev83 = 83, ev84 = 84, ev85 = 85, ev86 = 86, ev87 = 87, ev88 = 88, ev89 = 89;
let ev90 = 90, ev91 = 91, ev92 = 92, ev93 = 93, ev94 = 94, ev95 = 95, ev96 = 96, ev97 = 97, ev98 = 98, ev99 = 99;
let ev100 = 100, ev101 = 101, ev102 = 102, ev103 = 103, ev104 = 104, ev105 = 105, ev106 = 106, ev107 = 107, ev108 = 108, ev109 = 109;
let ev110 = 110, ev111 = 111, ev112 = 112, ev113 = 113, ev114 = 114, ev115 = 115, ev116 = 116, ev117 = 117, ev118 = 118, ev119 = 119;
let ev120 = 120, ev121 = 121, ev122 = 122, ev123 = 123, ev124 = 124, ev125 = 125, ev126 = 126, ev127 = 127, ev128 = 128, ev129 = 129;
let ev130 = 130, ev131 = 131, ev132 = 132, ev133 = 133, ev134 = 134, ev135 = 135, ev136 = 136, ev137 = 137, ev138 = 138, ev139 = 139;
let ev140 = 140, ev141 = 141, ev142 = 142, ev143 = 143, ev144 = 144, ev145 = 145, ev146 = 146, ev147 = 147, ev148 = 148, ev149 = 149;
let ev150 = 150, ev151 = 151, ev152 = 152, ev153 = 153, ev154 = 154, ev155 = 155, ev156 = 156, ev157 = 157, ev158 = 158, ev159 = 159;
let ev160 = 160, ev161 = 161, ev162 = 162, ev163 = 163, ev164 = 164, ev165 = 165, ev166 = 166, ev167 = 167, ev168 = 168, ev169 = 169;
let ev170 = 170, ev171 = 171, ev172 = 172, ev173 = 173, ev174 = 174, ev175 = 175, ev176 = 176, ev177 = 177, ev178 = 178, ev179 = 179;
let ev180 = 180, ev181 = 181, ev182 = 182, ev183 = 183, ev184 = 184, ev185 = 185, ev186 = 186, ev187 = 187, ev188 = 188, ev189 = 189;
let ev190 = 190, ev191 = 191, ev192 = 192, ev193 = 193, ev194 = 194, ev195 = 195, ev196 = 196, ev197 = 197, ev198 = 198, ev199 = 199;
let ev200 = 200, ev201 = 201, ev202 = 202, ev203 = 203, ev204 = 204, ev205 = 205, ev206 = 206, ev207 = 207, ev208 = 208, ev209 = 209;
let ev210 = 210, ev211 = 211, ev212 = 212, ev213 = 213, ev214 = 214, ev215 = 215, ev216 = 216, ev217 = 217, ev218 = 218, ev219 = 219;
let ev220 = 220, ev221 = 221, ev222 = 222, ev223 = 223, ev224 = 224, ev225 = 225, ev226 = 226, ev227 = 227, ev228 = 228, ev229 = 229;
let ev230 = 230, ev231 = 231, ev232 = 232, ev233 = 233, ev234 = 234, ev235 = 235, ev236 = 236, ev237 = 237, ev238 = 238, ev239 = 239;
let ev240 = 240, ev241 = 241, ev242 = 242, ev243 = 243, ev244 = 244, ev245 = 245, ev246 = 246, ev247 = 247, ev248 = 248, ev249 = 249;
let ev250 = 250, ev251 = 251, ev252 = 252, ev253 = 253, ev254 = 254, ev255 = 255, ev256 = 256, ev257 = 257, ev258 = 258, ev259 = 259;
let ev260 = 260, ev261 = 261, ev262 = 262, ev263 = 263, ev264 = 264, ev265 = 265, ev266 = 266, ev267 = 267, ev268 = 268, ev269 = 269;
let ev270 = 270, ev271 = 271, ev272 = 272, ev273 = 273, ev274 = 274, ev275 = 275, ev276 = 276, ev277 = 277, ev278 = 278, ev279 = 279;
let ev280 = 280, ev281 = 281, ev282 = 282, ev283 = 283, ev284 = 284, ev285 = 285, ev286 = 286, ev287 = 287, ev288 = 288, ev289 = 289;
let ev290 = 290, ev291 = 291, ev292 = 292, ev293 = 293, ev294 = 294, ev295 = 295, ev296 = 296, ev297 = 297, ev298 = 298, ev299 = 299;
let ev300 = 300, ev301 = 301, ev302 = 302, ev303 = 303, ev304 = 304, ev305 = 305, ev306 = 306, ev307 = 307, ev308 = 308, ev309 = 309;
let ev310 = 310, ev311 = 311, ev312 = 312, ev313 = 313, ev314 = 314, ev315 = 315, ev316 = 316, ev317 = 317, ev318 = 318, ev319 = 319;
let ev320 = 320, ev321 = 321, ev322 = 322, ev323 = 323, ev324 = 324, ev325 = 325, ev326 = 326, ev327 = 327, ev328 = 328, ev329 = 329;
let ev330 = 330, ev331 = 331, ev332 = 332, ev333 = 333, ev334 = 334, ev335 = 335, ev336 = 336, ev337 = 337, ev338 = 338, ev339 = 339;
let ev340 = 340, ev341 = 341, ev342 = 342, ev343 = 343, ev344 = 344, ev345 = 345, ev346 = 346, ev347 = 347, ev348 = 348, ev349 = 349;
let ev350 = 350, ev351 = 351, ev352 = 352, ev353 = 353, ev354 = 354, ev355 = 355, ev356 = 356, ev357 = 357, ev358 = 358, ev359 = 359;
let ev360 = 360, ev361 = 361, ev362 = 362, ev363 = 363, ev364 = 364, ev365 = 365, ev366 = 366, ev367 = 367, ev368 = 368, ev369 = 369;
let ev370 = 370, ev371 = 371, ev372 = 372, ev373 = 373, ev374 = 374, ev375 = 375, ev376 = 376, ev377 = 377, ev378 = 378, ev379 = 379;
let ev380 = 380, ev381 = 381, ev382 = 382, ev383 = 383, ev384 = 384, ev385 = 385, ev386 = 386, ev387 = 387, ev388 = 388, ev389 = 389;
let ev390 = 390, ev391 = 391, ev392 = 392, ev393 = 393, ev394 = 394, ev395 = 395, ev396 = 396, ev397 = 397, ev398 = 398, ev399 = 399;
var evOut = 0;
for (var pass = 0; pass < 3; pass++) {
  evOut += ev0 + ev1 + ev2 + ev3 + ev4 + ev5 + ev6 + ev7 + ev8 + ev9 + ev10 + ev11 + ev12 + ev13 + ev14 + ev15 + ev16 + ev17 + ev18 + ev19;
  evOut += ev20 + ev21 + ev22 + ev23 + ev24 + ev25 + ev26 + ev27 + ev28 + ev29 + ev30 + ev31 + ev32 + ev33 + ev34 + ev35 + ev36 + ev37 + ev38 + ev39;
  evOut += ev40 + ev41 + ev42 + ev43 + ev44 + ev45 + ev46 + ev47 + ev48 + ev49 + ev50 + ev51 + ev52 + ev53 + ev54 + ev55 + ev56 + ev57 + ev58 + ev59;
  evOut += ev60 + ev61 + ev62 + ev63 + ev64 + ev65 + ev66 + ev67 + ev68 + ev69 + ev70 + ev71 + ev72 + ev73 + ev74 + ev75 + ev76 + ev77 + ev78 + ev79;
  evOut += ev80 + ev81 + ev82 + ev83 + ev84 + ev85 + ev86 + ev87 + ev88 + ev89 + ev90 + ev91 + ev92 + ev93 + ev94 + ev95 + ev96 + ev97 + ev98 + ev99;
  evOut += ev100 + ev101 + ev102 + ev103 + ev104 + ev105 + ev106 + ev107 + ev108 + ev109 + ev110 + ev111 + ev112 + ev113 + ev114 + ev115 + ev116 + ev117 + ev118 + ev119;
  evOut += ev120 + ev121 + ev122 + ev123 + ev124 + ev125 + ev126 + ev127 + ev128 + ev129 + ev130 + ev131 + ev132 + ev133 + ev134 + ev135 + ev136 + ev137 + ev138 + ev139;
  evOut += ev140 + ev141 + ev142 + ev143 + ev144 + ev145 + ev146 + ev147 + ev148 + ev149 + ev150 + ev151 + ev152 + ev153 + ev154 + ev155 + ev156 + ev157 + ev158 + ev159;
  evOut += ev160 + ev161 + ev162 + ev163 + ev164 + ev165 + ev166 + ev167 + ev168 + ev169 + ev170 + ev171 + ev172 + ev173 + ev174 + ev175 + ev176 + ev177 + ev178 + ev179;
  evOut += ev180 + ev181 + ev182 + ev183 + ev184 + ev185 + ev186 + ev187 + ev188 + ev189 + ev190 + ev191 + ev192 + ev193 + ev194 + ev195 + ev196 + ev197 + ev198 + ev199;
  evOut += ev200 + ev201 + ev202 + ev203 + ev204 + ev205 + ev206 + ev207 + ev208 + ev209 + ev210 + ev211 + ev212 + ev213 + ev214 + ev215 + ev216 + ev217 + ev218 + ev219;
  evOut += ev220 + ev221 + ev222 + ev223 + ev224 + ev225 + ev226 + ev227 + ev228 + ev229 + ev230 + ev231 + ev232 + ev233 + ev234 + ev235 + ev236 + ev237 + ev238 + ev239;
  evOut += ev240 + ev241 + ev242 + ev243 + ev244 + ev245 + ev246 + ev247 + ev248 + ev249 + ev250 + ev251 + ev252 + ev253 + ev254 + ev255 + ev256 + ev257 + ev258 + ev259;
  evOut += ev260 + ev261 + ev262 + ev263 + ev264 + ev265 + ev266 + ev267 + ev268 + ev269 + ev270 + ev271 + ev272 + ev273 + ev274 + ev275 + ev276 + ev277 + ev278 + ev279;
  evOut += ev280 + ev281 + ev282 + ev283 + ev284 + ev285 + ev286 + ev287 + ev288 + ev289 + ev290 + ev291 + ev292 + ev293 + ev294 + ev295 + ev296 + ev297 + ev298 + ev299;
  evOut += ev300 + ev301 + ev302 + ev303 + ev304 + ev305 + ev306 + ev307 + ev308 + ev309 + ev310 + ev311 + ev312 + ev313 + ev314 + ev315 + ev316 + ev317 + ev318 + ev319;
  evOut += ev320 + ev321 + ev322 + ev323 + ev324 + ev325 + ev326 + ev327 + ev328 + ev329 + ev330 + ev331 + ev332 + ev333 + ev334 + ev335 + ev336 + ev337 + ev338 + ev339;
  evOut += ev340 + ev341 + ev342 + ev343 + ev344 + ev345 + ev346 + ev347 + ev348 + ev349 + ev350 + ev351 + ev352 + ev353 + ev354 + ev355 + ev356 + ev357 + ev358 + ev359;
  evOut += ev360 + ev361 + ev362 + ev363 + ev364 + ev365 + ev366 + ev367 + ev368 + ev369 + ev370 + ev371 + ev372 + ev373 + ev374 + ev375 + ev376 + ev377 + ev378 + ev379;
  evOut += ev380 + ev381 + ev382 + ev383 + ev384 + ev385 + ev386 + ev387 + ev388 + ev389 + ev390 + ev391 + ev392 + ev393 + ev394 + ev395 + ev396 + ev397 + ev398 + ev399;
}
print('10 ' + evOut);

/* ---- 11. a lexical and a global property colliding in one set --------- */
/* Not something a script can control directly — atoms are interned in source
   order — so this simply interleaves many same-length names in both storages
   and reads them all. */
globalThis.pairA = 'propA';
globalThis.pairB = 'propB';
let pairC = 'lexC';
let pairD = 'lexD';
var pairOut = '';
for (var p = 0; p < 300; p++) pairOut = pairA + pairB + pairC + pairD;
print('11 ' + pairOut);

/* ---- 12. block-scoped shadowing must not consult the cache ------------ */
let blockName = 'outer';
function readBlockName() { return blockName; }
{
  let blockName = 'inner';
  print('12a ' + blockName + ' ' + readBlockName());
}
print('12b ' + blockName + ' ' + readBlockName());

/* ---- 13. `with` puts an object in the scope chain --------------------- */
var withTarget = { withName: 'from-with' };
let withName = 'from-lexical';
function readWithName() { return withName; }
with (withTarget) { print('13a ' + withName); }
print('13b ' + withName + ' ' + readWithName());

/* ---- 14. refcounting: a cached slot holding an object ----------------- */
let held = { n: 0 };
function bump() { held.n++; return held; }
for (var h = 0; h < 500; h++) bump();
print('14a ' + held.n);
held = { n: -1 };
print('14b ' + held.n);

/* ---- 15. typeof on an undeclared name must not fill or throw ---------- */
print('15a ' + typeof neverDeclared);
try { print('x' + neverDeclared); } catch (e) { print('15b ' + e.constructor.name); }
globalThis.neverDeclared = 'now-it-exists';
print('15c ' + neverDeclared);

/* ---- 16. Object.freeze on the global object --------------------------- */
globalThis.freezeMe = 'before';
function readFreezeMe() { return freezeMe; }
for (var z = 0; z < 200; z++) readFreezeMe();
print('16 ' + readFreezeMe());
