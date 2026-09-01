/* SWEEP-2 differential corpus -- exercises the PER-CYCLE SWEEP ADMISSION.
 *
 * ⛔ WHY test262 CANNOT DO THIS JOB.  The admission sits behind
 * `rt->trc_incr_active` and short-circuits before the predicate is even
 * evaluated.  MEASURED: the arming witness `TRACEGC sw2pc_arm` printed ZERO
 * times across all 81,924 tests at both QJS_GC_CONC settings, so the suite's
 * 72/81924 is a NO-REGRESSION result and says nothing about this code.
 *
 * ⛔⛔ AND THE FIRST TWO VERSIONS OF THIS FILE TESTED NOTHING EITHER, FOR A
 * REASON WORTH KEEPING.  They allocated garbage in WAVES and retired each wave
 * whole, so every arena ended up UNIFORMLY DEAD -- and QJS_REGION_WHOLESALE=1
 * then reclaimed those arenas outright, before sweep-to-allocate ever saw
 * them.  MEASURED: `TRACEGC s2a armed=0`, `pend_max=0`, `admitted=0`.  There
 * was no pend backlog to admit from, so `sw2_pick` was never called once.
 *
 * ⇒ THE BACKLOG ONLY EXISTS WHERE ARENAS ARE MIXED.  Wholesale release takes
 * an arena whose every cell is dead; sweep-to-allocate is what handles the
 * rest.  So this file INTERLEAVES survivors with garbage at cell granularity:
 * every arena ends up holding at least one live block, wholesale refuses it,
 * it is armed onto pend_arena_list, and the mechanism under test has something
 * to decide about.  That is what `splay` does incidentally and what the wave
 * version did not do at all.
 *
 * ⛔⛔⛔ AND EVEN INTERLEAVED IT STILL REACHES `admitted=0`.  MEASURED on the
 * graded profile: `REGION wholesale released=57,439`, and then
 * `TRACEGC otfree on=1 arenas=3,788,304 blocks=102,179,468 freed=7,480,712
 * released=106,951` against `TRACEGC s2a armed=0`.  The PARALLEL OT SWEEP
 * (QJS_S2A_OTFREE=1) keeps up with this workload completely and leaves the
 * mutator's lazy sweeper nothing to arm.  `splay` under the identical
 * environment reads `armed=318,171` because its allocation rate outruns those
 * workers; this corpus's does not.
 *
 * ⇒ THE BACKLOG IS NOT A PROPERTY OF THE HEAP SHAPE ALONE.  It needs the
 * allocation rate to outrun BOTH wholesale release AND the parallel sweep, and
 * reproducing that synthetically was not achieved.  This file is kept because
 * the two negative results above are worth more than a deleted file -- they
 * name exactly which two mechanisms must be outrun -- but IT DOES NOT CURRENTLY
 * GRADE THE MECHANISM, and it must not be cited as if it did.  Until its
 * witness prints a non-zero `admitted=`, the only differential for this work is
 * the 13-row Octane campaign.
 *
 * WHAT IT MUST PRINT.  A checksum of data a wrong free would corrupt: the walk
 * folds in the CONTENTS of the survivors, not their count, because a freed and
 * reused string gives different characters while a freed and unreused one
 * usually still reads back correctly.
 */
function h(s, acc) {
    for (var i = 0; i < s.length; i++)
        acc = (acc * 31 + s.charCodeAt(i)) >>> 0;
    return acc;
}

var ROUNDS   = 260;
var PER      = 5000;   /* allocations per round */
var KEEP_1_IN = 6;     /* every 6th allocation survives -- this is the whole
                          point: it pins the arena it lands in */
var LIVE_CAP = 90000;  /* retained set: big enough that marking is still
                          running when the allocator next refills */

var live = [];
var sum = 0;

for (var r = 0; r < ROUNDS; r++) {
    var junk = null;
    for (var i = 0; i < PER; i++) {
        /* ~176 bytes of string payload: the size class that holds splay's
           entire backlog (arena_block_sizes[17]) */
        var s = "sweep2-" + r + "-" + i + "-0123456789abcdef0123456789abcdef"
                + "0123456789abcdef0123456789abcdef0123456789abcdef";
        if (i % KEEP_1_IN === 0) {
            live.push(s);                 /* SURVIVOR: pins this arena */
            if (live.length > LIVE_CAP) live.shift();
        } else {
            junk = s + "x";               /* rope, immediately unreachable */
        }
    }
    junk = null;
    /* touch a stride of the live set every round: keeps it reachable AND
       reads bytes that a wrong free would have corrupted */
    for (var j = r % 251; j < live.length; j += 251)
        sum = h(live[j], sum);
}

var n = 0;
for (var j = 0; j < live.length; j++) { sum = h(live[j], sum); n++; }
print("sweep2-stress survivors=" + n + " checksum=" + sum);
