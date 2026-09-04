/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * Appended after every workload by `run.mjs`.
 *
 * Emits the loop-overhead reference row, flushes the collected rows, and
 * prints the `#END` sentinel plus the accumulated sink. `run.mjs` treats a
 * missing `#END` as a failed run regardless of exit status, because a workload
 * that throws inside `bench()` would otherwise have printed a partial, and
 * entirely plausible, table.
 */

bench("__loop_overhead", function (i) {
  return i & 1;
});

for (var __i = 0; __i < __rows.length; __i++) print(__rows[__i]);
print("#SINK\t" + (isFinite(__sink) ? __sink.toFixed(3) : String(__sink)));
print("#END\t" + __rows.length);
