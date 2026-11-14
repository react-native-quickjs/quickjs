/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * Ahead-of-time regexp matchers.
 *
 * One pattern's bytecode (the engine's own lre_compile output) becomes two C functions, one for a subject of 8-bit units and one for
 * 16-bit units. The functions run the machine of libregexp-backtrack.c.inc (capture array, backtrack stack of split and run frames, undo
 * log, register counters in the capture array) with everything the interpreter decodes per step known: every jump target is a label, the
 * immediates are literals, a character class is an inline test or a constant bitmap, a greedy run of a one-character atom is a tight
 * loop. The backtrack stack stores label addresses where the interpreter stores code pointers. The results are those of the interpreter
 * because the translation is of the same bytecode, opcode by opcode; a pattern with an opcode this file does not translate is left to the
 * interpreter (rx_emit returns 0 and writes nothing).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cutils.h"
#include "libregexp.h"
#include "libregexp-exec.h"

#include "regexp.h"

static const uint8_t rx_opsize[REOP_COUNT] = {
#define DEF(id, size) size,
#include "libregexp-opcode.h"
#undef DEF
};

typedef struct RX {
  FILE *o;
  const uint8_t *code;
  int len;
  int ncap;
  int uni;      /* unicode mode: canonicalisation folds to lower case */
  int w;        /* 8 or 16: subject unit width */
  uint8_t *tgt; /* tgt[pc]: a label is needed at pc */
  uint8_t *skip;/* skip[pc]: the instruction is the body of a greedy run, which the run code replaces */
  const char *name;
  int ncls;     /* classes emitted so far in this function */
} RX;

static int rx_insn_len(const uint8_t *code, int pc)
{
  switch (code[pc]) {
  case REOP_range: case REOP_range_i: return 3 + 4 * (int)get_u16(code + pc + 1);
  case REOP_range32: case REOP_range32_i: return 3 + 8 * (int)get_u16(code + pc + 1);
  case REOP_back_reference: case REOP_back_reference_i: case REOP_backward_back_reference: case REOP_backward_back_reference_i:
    return 2 + code[pc + 1];
  default: return rx_opsize[code[pc]];
  }
}

/* the one-character atom the greedy loop at the split at pc runs: the atom's pc and the exit pc, and its length; 0 when the split is not
   such a loop (the interpreter's own test, on the same bytes) */
static int rx_run_of(const uint8_t *code, int pc, int *atom, int *exitpc)
{
  int32_t val = (int32_t)get_u32(code + pc + 1);
  int next = pc + 5;
  if (code[pc] == REOP_split_next_first) {
    int alen = lre_simple_atom_len(code + next);
    if (alen && code[next + alen] == REOP_goto && next + alen + 5 + (int32_t)get_u32(code + next + alen + 1) == pc) {
      *atom = next; *exitpc = next + val; return alen;
    }
  } else if (code[pc] == REOP_split_goto_first && val < 0) {
    int a = next + val;
    int alen = lre_simple_atom_len(code + a);
    if (alen && a + alen == pc) { *atom = a; *exitpc = next; return alen; }
  }
  return 0;
}

static int rx_mark(RX *x, int pc)
{
  if (pc < 0 || pc >= x->len) return 0;
  x->tgt[pc] = 1;
  return 1;
}

/* labels at every jump target, split resume point and run exit; 0 when an opcode is not translated or the code is malformed */
static int rx_prepass(RX *x)
{
  const uint8_t *code = x->code;
  int pc = 0, ent = 11;
  x->tgt = calloc((size_t)x->len + 1, 1);
  x->skip = calloc((size_t)x->len + 1, 1);
  if (!x->tgt || !x->skip) return 0;
  while (pc < x->len) {
    int op = code[pc], n = rx_insn_len(code, pc), a, e;
    if (pc + n > x->len) return 0;
    if (x->skip[pc]) { pc += n; continue; }
    switch (op) {
    case REOP_char: case REOP_char_i: case REOP_char32: case REOP_char32_i: case REOP_dot: case REOP_any: case REOP_space: case REOP_not_space:
    case REOP_line_start: case REOP_line_start_m: case REOP_line_end: case REOP_line_end_m: case REOP_match:
      break;
    case REOP_save_start: case REOP_save_end:
      if (code[pc + 1] >= x->ncap) return 0;
      break;
    case REOP_save_reset:
      if (code[pc + 1] > code[pc + 2] || code[pc + 2] >= x->ncap) return 0;
      break;
    case REOP_set_i32: case REOP_set_char_pos: case REOP_check_advance:
    case REOP_word_boundary: case REOP_word_boundary_i: case REOP_not_word_boundary: case REOP_not_word_boundary_i:
    case REOP_range: case REOP_range_i:
      break;
    case REOP_goto:
      if (!rx_mark(x, pc + 5 + (int32_t)get_u32(code + pc + 1))) return 0;
      break;
    case REOP_split_goto_first: case REOP_split_next_first:
      if (rx_run_of(code, pc, &a, &e)) {
        if (!rx_mark(x, e)) return 0;
        if (op == REOP_split_next_first) { int k; for (k = pc + 5; k < pc + 5 + lre_simple_atom_len(code + pc + 5) + 5; k++) x->skip[k] = 1; }
        break;
      }
      if (!rx_mark(x, pc + 5 + (int32_t)get_u32(code + pc + 1)) || !rx_mark(x, pc + 5)) return 0;
      break;
    case REOP_loop:
      if (!rx_mark(x, pc + 6 + (int32_t)get_u32(code + pc + 2))) return 0;
      break;
    case REOP_loop_split_goto_first: case REOP_loop_split_next_first:
    case REOP_loop_check_adv_split_goto_first: case REOP_loop_check_adv_split_next_first:
      if (!rx_mark(x, pc + 10 + (int32_t)get_u32(code + pc + 6)) || !rx_mark(x, pc + 10)) return 0;
      break;
    default:
      return 0;   /* lookahead, back references, 32-bit ranges, prev: interpreted */
    }
    pc += n;
  }
  if (x->len > ent + 1 && code[ent] == REOP_save_start && code[ent + 1] == 0) x->tgt[ent] = 1;
  x->tgt[0] = 1;
  return 1;
}

/* the test of one unit against the class of the range operand at pc (n intervals), as a C expression over the unit c. 8-bit subjects use a
   bitmap built here from the interpreter's own lre_range_has (with the case folding when ci). */
static void rx_class(RX *x, int pc, int ci, char *expr, size_t en)
{
  FILE *o = x->o;
  const uint8_t *ops = x->code + pc + 3;
  int n = (int)get_u16(x->code + pc + 1), k = x->ncls++;
  if (x->w == 8) {
    uint64_t bm[4] = {0, 0, 0, 0};
    uint32_t c;
    for (c = 0; c < 256; c++) {
      uint32_t d = ci ? lre_canon_fast(c, x->uni) : c;
      if (lre_range_has(ops, n, d)) bm[c >> 6] |= (uint64_t)1 << (c & 63);
    }
    fprintf(o, "  static const uint64_t rb%d[4] = {0x%llxull, 0x%llxull, 0x%llxull, 0x%llxull};\n", k, (unsigned long long)bm[0], (unsigned long long)bm[1],
            (unsigned long long)bm[2], (unsigned long long)bm[3]);
    snprintf(expr, en, "((rb%d[(%%s) >> 6] >> ((%%s) & 63)) & 1)", k);
    return;
  }
  if (n <= 8) {
    int i;
    fprintf(o, "  #define RC%d(c_) (", k);
    for (i = 0; i < n; i++) {
      uint32_t lo = get_u16(ops + i * 4), hi = get_u16(ops + i * 4 + 2);
      if (i) fprintf(o, " || ");
      if (i == n - 1 && hi == 0xffff) fprintf(o, "(c_) >= %u", lo);
      else if (lo == hi) fprintf(o, "(c_) == %u", lo);
      else fprintf(o, "((c_) >= %u && (c_) <= %u)", lo, hi);
    }
    fprintf(o, ")\n");
  } else {
    int i;
    fprintf(o, "  static const uint8_t rr%d[%d] = {", k, n * 4);
    for (i = 0; i < n * 4; i++) fprintf(o, "%s%u", i ? "," : "", ops[i]);
    fprintf(o, "};\n  #define RC%d(c_) lre_range_has(rr%d, %d, (c_))\n", k, k, n);
  }
  snprintf(expr, en, "RC%d(%%s)", k);
}

/* the condition (true: the unit matches) of a one-character atom on the unit variable v; the class tables go before the code that uses them */
static void rx_atom_cond(RX *x, int apc, const char *v, char *cond, size_t cn)
{
  const uint8_t *code = x->code;
  char cls[160];
  switch (code[apc]) {
  case REOP_space: snprintf(cond, cn, "lre_is_space(%s)", v); break;
  case REOP_not_space: snprintf(cond, cn, "!lre_is_space(%s)", v); break;
  case REOP_dot: snprintf(cond, cn, "!is_line_terminator(%s)", v); break;
  case REOP_any: snprintf(cond, cn, "1"); break;
  case REOP_char: snprintf(cond, cn, "%s == %u", v, get_u16(code + apc + 1)); break;
  case REOP_char32: snprintf(cond, cn, "%s == %uu", v, get_u32(code + apc + 1)); break;
  case REOP_char_i: snprintf(cond, cn, "lre_canon_fast(%s, %d) == %u", v, x->uni, get_u16(code + apc + 1)); break;
  case REOP_char32_i: snprintf(cond, cn, "lre_canon_fast(%s, %d) == %uu", v, x->uni, get_u32(code + apc + 1)); break;
  case REOP_range: case REOP_range_i: {
    int ci = code[apc] == REOP_range_i;
    rx_class(x, apc, ci, cls, sizeof(cls));
    if (x->w == 8) snprintf(cond, cn, cls, v, v);
    else snprintf(cond, cn, cls, v);   /* a 16-bit case-insensitive class is tested on the folded unit: the caller folds first (rx_pre) */
    break;
  }
  default: snprintf(cond, cn, "0");
  }
}

#define LBL(pc) pc
static void rx_push_split(RX *x, int resume_pc)
{
  fprintf(x->o, "    RX_PUSH_SPLIT(&&L%d);\n", resume_pc);
}

static void rx_poll(RX *x) { fprintf(x->o, "    if (lre_poll_timeout(s)) return LRE_RET_TIMEOUT;\n"); }

/* the statement that folds the unit c before the class test of a 16-bit case-insensitive class, else empty */
static const char *rx_pre(RX *x, int apc)
{
  return x->w == 16 && x->code[apc] == REOP_range_i ? "c = lre_canon_fast(c, is_unicode); " : "";
}

/* a greedy run of the atom at apc, then the exit */
static void rx_run(RX *x, int apc, int alen, int exitpc)
{
  FILE *o = x->o;
  char cond[400];
  (void)alen;
  fprintf(o, "    {\n    const uint8_t *p = cptr, *e = cbuf_end;\n");
  if (x->code[apc] == REOP_any) {
    fprintf(o, "    p = e;\n");
  } else {
    rx_atom_cond(x, apc, "c", cond, sizeof(cond));
    if (x->w == 8) fprintf(o, "    while (p < e) { c = *p; if (!(%s)) break; p++; }\n", cond);
    else fprintf(o, "    while (p < e) { c = *(const uint16_t *)p; %sif (!(%s)) break; p += 2; }\n", rx_pre(x, apc), cond);
  }
  fprintf(o, "    if (p > cptr) { RX_PUSH_RUN(&&L%d, p); cptr = p; }\n", exitpc);
  rx_poll(x);
  fprintf(o, "    goto L%d;\n    }\n", exitpc);
}

static void rx_char_test(RX *x, int apc)
{
  FILE *o = x->o;
  char cond[400];
  const uint8_t *code = x->code;
  int op = code[apc];
  fprintf(o, "    if (cptr >= cbuf_end) goto no_match;\n");
  if (op == REOP_any || op == REOP_dot || op == REOP_space || op == REOP_not_space) {
    fprintf(o, "    GET_CHAR(c, cptr, cbuf_end, cbuf_type);\n");
    if (op != REOP_any) {
      rx_atom_cond(x, apc, "c", cond, sizeof(cond));
      fprintf(o, "    if (!(%s)) goto no_match;\n", cond);
    }
    return;
  }
  fprintf(o, "    GET_CHAR(c, cptr, cbuf_end, cbuf_type);\n");
  rx_atom_cond(x, apc, "c", cond, sizeof(cond));
  fprintf(o, "    %sif (!(%s)) goto no_match;\n", rx_pre(x, apc), cond);
}

static void rx_func(RX *x)
{
  FILE *o = x->o;
  const uint8_t *code = x->code;
  int pc = 0, ent = 11;
  fprintf(o, "static intptr_t rxf_%s_%d(REExecContext *s, uint8_t **capture, const uint8_t *cptr, uint32_t off)\n{\n", x->name, x->w);
  fprintf(o, "  const int cbuf_type = %d;\n  const bool is_unicode = %d;\n  uint32_t c;\n  const uint8_t *cbuf_end = s->cbuf_end;\n  void *rpc;\n", x->w == 8 ? 0 : 1, x->uni);
  fprintf(o, "  StackElem *sp = s->stack_buf, *bp = s->stack_buf, *stack_end = s->stack_buf + s->stack_size;\n  (void)is_unicode; (void)c; (void)cbuf_end;\n");
  fprintf(o, "  switch (off) {\n  case 0: goto L0;\n");
  if (x->tgt[ent] && x->len > ent + 1 && code[ent] == REOP_save_start && code[ent + 1] == 0) fprintf(o, "  case %d: goto L%d;\n", ent, ent);
  fprintf(o, "  default: return LRE_RET_AOT_NOENTRY;\n  }\n");
  while (pc < x->len) {
    int op = code[pc], n = rx_insn_len(code, pc), a, e;
    uint32_t idx;
    if (x->skip[pc]) { pc += n; continue; }
    if (x->tgt[pc]) fprintf(o, " L%d:;\n", pc);
    switch (op) {
    case REOP_match: fprintf(o, "    return 1;\n"); break;
    case REOP_char: case REOP_char_i: case REOP_char32: case REOP_char32_i: case REOP_dot: case REOP_any: case REOP_space: case REOP_not_space:
      rx_char_test(x, pc);
      break;
    case REOP_range: case REOP_range_i:
      rx_char_test(x, pc);
      break;
    case REOP_line_start: case REOP_line_start_m:
      fprintf(o, "    if (cptr != s->cbuf) {\n");
      if (op == REOP_line_start) fprintf(o, "      goto no_match;\n");
      else fprintf(o, "      PEEK_PREV_CHAR(c, cptr, s->cbuf, cbuf_type);\n      if (!is_line_terminator(c)) goto no_match;\n");
      fprintf(o, "    }\n");
      break;
    case REOP_line_end: case REOP_line_end_m:
      fprintf(o, "    if (cptr != cbuf_end) {\n");
      if (op == REOP_line_end) fprintf(o, "      goto no_match;\n");
      else fprintf(o, "      PEEK_CHAR(c, cptr, cbuf_end, cbuf_type);\n      if (!is_line_terminator(c)) goto no_match;\n");
      fprintf(o, "    }\n");
      break;
    case REOP_goto: {
      int t = pc + 5 + (int32_t)get_u32(code + pc + 1);
      if (t <= pc) rx_poll(x);
      fprintf(o, "    goto L%d;\n", t);
      break;
    }
    case REOP_split_goto_first: case REOP_split_next_first: {
      int alen = rx_run_of(code, pc, &a, &e);
      int t = pc + 5 + (int32_t)get_u32(code + pc + 1);
      if (alen) { rx_run(x, a, alen, e); break; }
      if (op == REOP_split_next_first) { rx_push_split(x, t); }
      else { rx_push_split(x, pc + 5); fprintf(o, "    goto L%d;\n", t); }
      break;
    }
    case REOP_save_start: case REOP_save_end:
      idx = 2u * code[pc + 1] + (op == REOP_save_end);
      if ((int)code[pc + 1] >= x->ncap) return;
      fprintf(o, "    SAVE_CAPTURE(%u, (uint8_t *)cptr);\n", idx);
      break;
    case REOP_save_reset: {
      uint32_t v1 = code[pc + 1], v2 = code[pc + 2], v;
      fprintf(o, "    CHECK_STACK_SPACE(%u);\n", 2 * (v2 - v1 + 1));
      for (v = v1; v <= v2; v++) fprintf(o, "    SAVE_CAPTURE(%u, NULL); SAVE_CAPTURE(%u, NULL);\n", 2 * v, 2 * v + 1);
      break;
    }
    case REOP_set_i32:
      idx = 2u * x->ncap + code[pc + 1];
      fprintf(o, "    SAVE_CAPTURE_CHECK(%u, (void *)(uintptr_t)%uu);\n", idx, get_u32(code + pc + 2));
      break;
    case REOP_set_char_pos:
      idx = 2u * x->ncap + code[pc + 1];
      fprintf(o, "    SAVE_CAPTURE_CHECK(%u, (uint8_t *)cptr);\n", idx);
      break;
    case REOP_check_advance:
      idx = 2u * x->ncap + code[pc + 1];
      fprintf(o, "    if (capture[%u] == cptr) goto no_match;\n", idx);
      break;
    case REOP_loop: {
      int t = pc + 6 + (int32_t)get_u32(code + pc + 2);
      idx = 2u * x->ncap + code[pc + 1];
      fprintf(o, "    { uint32_t val2 = (uintptr_t)capture[%u] - 1;\n      SAVE_CAPTURE_CHECK(%u, (void *)(uintptr_t)val2);\n      if (val2 != 0) {\n", idx, idx);
      if (t <= pc) fprintf(o, "        if (lre_poll_timeout(s)) return LRE_RET_TIMEOUT;\n");
      fprintf(o, "        goto L%d;\n      } }\n", t);
      break;
    }
    case REOP_loop_split_goto_first: case REOP_loop_split_next_first:
    case REOP_loop_check_adv_split_goto_first: case REOP_loop_check_adv_split_next_first: {
      int next = pc + 10, t = next + (int32_t)get_u32(code + pc + 6);
      int adv = op == REOP_loop_check_adv_split_goto_first || op == REOP_loop_check_adv_split_next_first;
      int nextfirst = op == REOP_loop_split_next_first || op == REOP_loop_check_adv_split_next_first;
      uint32_t limit = get_u32(code + pc + 2);
      idx = 2u * x->ncap + code[pc + 1];
      fprintf(o, "    { uint32_t val2 = (uintptr_t)capture[%u] - 1;\n      SAVE_CAPTURE_CHECK(%u, (void *)(uintptr_t)val2);\n", idx, idx);
      fprintf(o, "      if (val2 > %uu) {\n", limit);
      if (t <= pc) fprintf(o, "        if (lre_poll_timeout(s)) return LRE_RET_TIMEOUT;\n");
      fprintf(o, "        goto L%d;\n      }\n", t);
      if (adv) fprintf(o, "      if (capture[%u] == cptr && val2 != %uu) goto no_match;\n", idx + 1, limit);
      fprintf(o, "      if (val2 != 0) {\n");
      if (nextfirst) { fprintf(o, "        RX_PUSH_SPLIT(&&L%d);\n      }\n", t); }
      else { fprintf(o, "        RX_PUSH_SPLIT(&&L%d);\n        goto L%d;\n      }\n", next, t); }
      fprintf(o, "    }\n");
      break;
    }
    case REOP_word_boundary: case REOP_word_boundary_i: case REOP_not_word_boundary: case REOP_not_word_boundary_i: {
      int ci = op == REOP_word_boundary_i || op == REOP_not_word_boundary_i;
      int isb = op == REOP_word_boundary || op == REOP_word_boundary_i;
      fprintf(o, "    { bool v1, v2;\n      if (cptr == s->cbuf) v1 = false; else { PEEK_PREV_CHAR(c, cptr, s->cbuf, cbuf_type); v1 = c < 256 ? (lre_is_word_byte(c) != 0) : %s; }\n",
              ci ? "(c == 0x017f || c == 0x212a)" : "false");
      fprintf(o, "      if (cptr >= cbuf_end) v2 = false; else { PEEK_CHAR(c, cptr, cbuf_end, cbuf_type); v2 = c < 256 ? (lre_is_word_byte(c) != 0) : %s; }\n",
              ci ? "(c == 0x017f || c == 0x212a)" : "false");
      fprintf(o, "      if (v1 ^ v2 ^ %d) goto no_match;\n    }\n", isb);
      break;
    }
    default: return;
    }
    pc += n;
  }
  /* the end of the code is reached only through a match opcode: nothing falls off it */
  fprintf(o, "  no_match:\n  for (;;) {\n    REExecStateEnum type;\n    if (bp == s->stack_buf) return 0;\n");
  fprintf(o, "    while (sp > bp) { capture[sp[-2].val] = sp[-1].ptr; sp -= 2; }\n");
  fprintf(o, "    rpc = sp[-3].ptr; cptr = sp[-2].ptr; type = sp[-1].bp.type;\n");
  fprintf(o, "    if (type == RE_EXEC_STATE_RUN) {\n      PREV_CHAR(cptr, s->cbuf, cbuf_type);\n");
  fprintf(o, "      if (cptr > sp[-4].ptr) { sp[-2].ptr = (uint8_t *)cptr; } else { bp = s->stack_buf + sp[-1].bp.val; sp -= 4; }\n      break;\n    }\n");
  fprintf(o, "    bp = s->stack_buf + sp[-1].bp.val; sp -= 3;\n    break;\n  }\n");
  fprintf(o, "  if (lre_poll_timeout(s)) return LRE_RET_TIMEOUT;\n  goto *rpc;\n");
  fprintf(o, "  /* (the class macros end here) */\n}\n");
  { int k; for (k = 0; k < x->ncls; k++) fprintf(o, "#undef RC%d\n", k); }
}

/* the preamble every generated file needs once: the interpreter's stack macros */
void rx_preamble(FILE *o)
{
  fputs(
    "#ifndef RX_PREAMBLE\n#define RX_PREAMBLE 1\n#include \"libregexp-exec.h\"\n"
    "#define CHECK_STACK_SPACE(n) if (unlikely((stack_end - sp) < (n))) { size_t saved_sp = sp - s->stack_buf; size_t saved_bp = bp - s->stack_buf; \\\n"
    "    if (lre_stack_realloc(s, sp - s->stack_buf + (n))) return LRE_RET_MEMORY_ERROR; stack_end = s->stack_buf + s->stack_size; \\\n"
    "    sp = s->stack_buf + saved_sp; bp = s->stack_buf + saved_bp; }\n"
    "#define SAVE_CAPTURE(idx, value) { CHECK_STACK_SPACE(2); sp[0].val = idx; sp[1].ptr = capture[idx]; sp += 2; capture[idx] = (value); }\n"
    "#define SAVE_CAPTURE_CHECK(idx, value) { StackElem *sp1 = sp; for (;;) { if (sp1 > bp) { if (sp1[-2].val == idx) break; sp1 -= 2; } else { \\\n"
    "      CHECK_STACK_SPACE(2); sp[0].val = idx; sp[1].ptr = capture[idx]; sp += 2; break; } } capture[idx] = (value); }\n"
    "#define RX_PUSH_SPLIT(lbl) { CHECK_STACK_SPACE(3); sp[0].ptr = (uint8_t *)(lbl); sp[1].ptr = (uint8_t *)cptr; sp[2].bp.val = bp - s->stack_buf; \\\n"
    "    sp[2].bp.type = RE_EXEC_STATE_SPLIT; sp += 3; bp = sp; }\n"
    "#define RX_PUSH_RUN(lbl, end) { CHECK_STACK_SPACE(4); sp[0].ptr = (uint8_t *)cptr; sp[1].ptr = (uint8_t *)(lbl); sp[2].ptr = (uint8_t *)(end); \\\n"
    "    sp[3].bp.val = bp - s->stack_buf; sp[3].bp.type = RE_EXEC_STATE_RUN; sp += 4; bp = sp; }\n"
    "#endif\n", o);
}

int rx_emit(FILE *o, const char *name, const uint8_t *bc, size_t len, int want16, int *has16)
{
  RX x;
  int flags;
  memset(&x, 0, sizeof(x));
  if (len < RE_HEADER_LEN) return 0;
  x.len = (int)get_u32(bc + RE_HEADER_BYTECODE_LEN);
  if ((size_t)x.len > len - RE_HEADER_LEN) return 0;
  x.code = bc + RE_HEADER_LEN;
  x.ncap = bc[RE_HEADER_CAPTURE_COUNT];
  flags = lre_get_flags(bc);
  x.uni = (flags & (LRE_FLAG_UNICODE | LRE_FLAG_UNICODE_SETS)) != 0;
  x.name = name;
  x.o = o;
  if (!rx_prepass(&x)) { free(x.tgt); free(x.skip); return 0; }
  rx_preamble(o);
  x.w = 8; x.ncls = 0;
  rx_func(&x);
  *has16 = want16 && !x.uni;
  if (*has16) { x.w = 16; x.ncls = 0; rx_func(&x); }
  free(x.tgt); free(x.skip);
  return 1;
}
