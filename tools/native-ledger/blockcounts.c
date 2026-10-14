/*
 * Block counters for the native-work ledger.
 *
 * Built with -fsanitize-coverage=trace-pc-guard, which calls this hook at the entry
 * of every basic block. The hook keys on __builtin_return_address(0) -- the address
 * just after the instrumentation call, i.e. the block's own address -- so a count is
 * attached to an address rather than a source line. That matters because a macro
 * expanded in many functions has one line but many addresses, and a line-based join
 * multiplies a sum by a sum instead of summing products.
 *
 * Counts are exact and this build is never used for timing.
 *
 *   clang -c blockcounts.c -o blockcounts.o          (without the sanitizer flag)
 *   BLOCKCOUNTS=out.txt ./instrumented-build ...
 *
 * The output is: "#ref <addr>" (the runtime address of a known symbol, for rebasing),
 * "#calls <n>" (total hook invocations, for the coverage check), then one
 * "<address> <count>" line per block.
 *
 * Only one thread is supported: counters are plain increments, not atomics, because
 * an atomic on every block would distort exactly what is being measured. Profile a
 * single-threaded workload with this.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define TBL_BITS 22
#define TBL_SIZE (1u << TBL_BITS)
#define TBL_MASK (TBL_SIZE - 1)

static uintptr_t keys[TBL_SIZE];
static uint64_t vals[TBL_SIZE];
static unsigned long long ncalls;

void __sanitizer_cov_trace_pc_guard_init(uint32_t *start, uint32_t *stop) {
  uint32_t n = 0;
  for (; start < stop; start++, n++) *start = n ? n : 1;
}

void __sanitizer_cov_trace_pc_guard(uint32_t *guard) {
  uintptr_t pc = (uintptr_t)__builtin_return_address(0);
  size_t h;

  (void)guard;
  ncalls++;
  h = (size_t)((pc >> 4) * 2654435761u) & TBL_MASK;
  while (keys[h] && keys[h] != pc) h = (h + 1) & TBL_MASK;
  if (!keys[h]) keys[h] = pc;
  vals[h]++;
}

/* Referenced by the reducer to recover the image slide. */
__attribute__((noinline, used)) void blockcounts_ref(void) {}

__attribute__((destructor)) static void blockcounts_dump(void) {
  const char *path = getenv("BLOCKCOUNTS");
  size_t i;
  FILE *f;

  if (!path) path = "blockcounts.txt";
  f = fopen(path, "w");
  if (!f) return;
  fprintf(
      f, "#ref %zx\n", (uintptr_t)(void *)&__sanitizer_cov_trace_pc_guard_init);
  fprintf(f, "#calls %llu\n", ncalls);
  for (i = 0; i < TBL_SIZE; i++)
    if (keys[i]) fprintf(f, "%zx %llu\n", keys[i], (unsigned long long)vals[i]);
  fclose(f);
}
