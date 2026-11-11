/* Runtime-width backtracking VM body, included once from libregexp.c. */
static intptr_t LRE_BT_FUNC(REExecContext *s, uint8_t **capture,
                            const uint8_t *pc, const uint8_t *cptr)
{
    int opcode;
    const int cbuf_type = LRE_CBUF_TYPE_INIT;
    uint32_t val, c, idx;
    const uint8_t *cbuf_end;
    StackElem *sp, *bp, *stack_end;
    const uint8_t *run_atom, *run_exit;
#ifdef DUMP_EXEC
    const uint8_t *pc_start = pc; /* TEST */
#endif
    cbuf_end = s->cbuf_end;

    sp = s->stack_buf;
    bp = s->stack_buf;
    stack_end = s->stack_buf + s->stack_size;

#define CHECK_STACK_SPACE(n)                            \
    if (unlikely((stack_end - sp) < (n))) {             \
        size_t saved_sp = sp - s->stack_buf;            \
        size_t saved_bp = bp - s->stack_buf;            \
        if (stack_realloc(s, sp - s->stack_buf + (n)))  \
            return LRE_RET_MEMORY_ERROR;                \
        stack_end = s->stack_buf + s->stack_size;       \
        sp = s->stack_buf + saved_sp;                   \
        bp = s->stack_buf + saved_bp;                   \
    }

    /* XXX: could test if the value was saved to reduce the stack size
       but slower */
#define SAVE_CAPTURE(idx, value)                        \
    {                                                   \
        CHECK_STACK_SPACE(2);                           \
        sp[0].val = idx;                                \
        sp[1].ptr = capture[idx];                       \
        sp += 2;                                        \
        capture[idx] = (value);                         \
    }

    /* avoid saving the previous value if already saved */
#define SAVE_CAPTURE_CHECK(idx, value)          \
    {                                           \
        StackElem *sp1;                         \
        sp1 = sp;                               \
        for(;;) {                               \
            if (sp1 > bp) {                             \
                if (sp1[-2].val == idx)                 \
                    break;                              \
                sp1 -= 2;                               \
            } else {                                    \
                CHECK_STACK_SPACE(2);                   \
                sp[0].val = idx;                        \
                sp[1].ptr = capture[idx];               \
                sp += 2;                                \
                break;                                  \
            }                                           \
        }                                               \
        capture[idx] = (value);                         \
    }


#ifdef DUMP_EXEC
#define LRE_DUMP_OP()                                                   \
    printf("%5ld %5ld %5ld %5ld %s\n",                                  \
           (long)(pc - 1 - pc_start),                                   \
           (long)(cbuf_type == 0 ? cptr - s->cbuf : (cptr - s->cbuf) / 2), \
           (long)(bp - s->stack_buf),                                   \
           (long)(sp - s->stack_buf),                                   \
           reopcode_info[opcode].name)
#else
#define LRE_DUMP_OP()
#endif

    /* Threaded-code dispatch: on compilers with the GCC label-as-value
       extension, jump directly from one opcode handler to the next via a
       table instead of returning to a central switch. Falls back to a plain
       switch elsewhere (MSVC, Emscripten). */
#if !LRE_DIRECT_DISPATCH
#define SWITCH(pc)  opcode = *pc++; LRE_DUMP_OP(); switch(opcode)
#define CASE(op)    case op
#define DEFAULT     default
#define BREAK       break
#else
    static const void * const dispatch_table[256] = {
#define DEF(id, size) [REOP_ ## id] = && case_REOP_ ## id,
#include "libregexp-opcode.h"
#undef DEF
        [REOP_COUNT ... 255] = && case_default,
    };
/* Wrapped in a statement-expression with a trailing ';' so that BREAK is a
   single statement usable in unbraced contexts like 'if (cond) BREAK;'. */
#define SWITCH(pc)  __extension__ ({ opcode = *pc++; LRE_DUMP_OP(); goto *dispatch_table[opcode]; });
#define CASE(op)    case_ ## op
#define DEFAULT     case_default
#define BREAK       SWITCH(pc)
#endif

#ifdef DUMP_EXEC
    printf("%5s %5s %5s %5s %s\n", "PC", "CP", "BP", "SP", "OPCODE");
#endif
    for(;;) {
        SWITCH(pc) {
        CASE(REOP_match):
            return 1;
        no_match:
            for(;;) {
                REExecStateEnum type;
                if (bp == s->stack_buf)
                    return 0;
                /* undo the modifications to capture[] */
                while (sp > bp) {
                    capture[sp[-2].val] = sp[-1].ptr;
                    sp -= 2;
                }

                pc = sp[-3].ptr;
                cptr = sp[-2].ptr;
                type = sp[-1].bp.type;
                if (type == RE_EXEC_STATE_RUN) {
                    /* give back one character of the run; the frame stays
                       (sp == bp) until the run start is reached */
                    PREV_CHAR(cptr, s->cbuf, cbuf_type);
                    if (cptr > sp[-4].ptr) {
                        sp[-2].ptr = (uint8_t *)cptr;
                    } else {
                        bp = s->stack_buf + sp[-1].bp.val;
                        sp -= 4;
                    }
                    break;
                }
                bp = s->stack_buf + sp[-1].bp.val;
                sp -= 3;
                if (type != RE_EXEC_STATE_LOOKAHEAD)
                    break;
            }
            if (lre_poll_timeout(s))
                return LRE_RET_TIMEOUT;
            BREAK;
        CASE(REOP_lookahead_match):
            /* pop all the saved states until reaching the start of
               the lookahead and keep the updated captures and
               variables and the corresponding undo info. */
            {
                StackElem *sp1, *sp_top, *next_sp;
                REExecStateEnum type;

                sp_top = sp;
                for(;;) {
                    sp1 = sp;
                    sp = bp;
                    pc = sp[-3].ptr;
                    cptr = sp[-2].ptr;
                    type = sp[-1].bp.type;
                    bp = s->stack_buf + sp[-1].bp.val;
                    sp[-1].ptr = (void *)sp1; /* save the next value for the copy step */
                    if (type == RE_EXEC_STATE_RUN) {
                        /* mark the four slot frame for the copy step */
                        sp[-4].ptr = (uint8_t *)&lre_run_frame_mark;
                        sp -= 1;
                    }
                    sp -= 3;
                    if (type == RE_EXEC_STATE_LOOKAHEAD)
                        break;
                }
                if (sp != s->stack_buf) {
                    /* keep the undo info if there is a saved state */
                    sp1 = sp;
                    while (sp1 < sp_top) {
                        if (sp1[0].ptr == (uint8_t *)&lre_run_frame_mark) {
                            next_sp = (void *)sp1[3].ptr;
                            sp1 += 4;
                        } else {
                            next_sp = (void *)sp1[2].ptr;
                            sp1 += 3;
                        }
                        while (sp1 < next_sp)
                            *sp++ = *sp1++;
                    }
                }
            }
            BREAK;
        CASE(REOP_negative_lookahead_match):
            /* pop all the saved states until reaching start of the negative lookahead */
            for(;;) {
                REExecStateEnum type;
                type = bp[-1].bp.type;
                /* undo the modifications to capture[] */
                while (sp > bp) {
                    capture[sp[-2].val] = sp[-1].ptr;
                    sp -= 2;
                }
                pc = sp[-3].ptr;
                cptr = sp[-2].ptr;
                type = sp[-1].bp.type;
                bp = s->stack_buf + sp[-1].bp.val;
                sp -= 3 + (type == RE_EXEC_STATE_RUN);
                if (type == RE_EXEC_STATE_NEGATIVE_LOOKAHEAD)
                    break;
            }
            goto no_match;
        CASE(REOP_char32):
        CASE(REOP_char32_i):
            val = get_u32(pc);
            pc += 4;
            goto test_char;
        CASE(REOP_char):
        CASE(REOP_char_i):
            val = get_u16(pc);
            pc += 2;
        test_char:
            if (cptr >= cbuf_end)
                goto no_match;
            GET_CHAR(c, cptr, cbuf_end, cbuf_type);
            if (opcode == REOP_char_i || opcode == REOP_char32_i) {
                c = lre_canon_fast(c, s->is_unicode);
            }
            if (val != c)
                goto no_match;
            BREAK;
        CASE(REOP_split_goto_first):
        CASE(REOP_split_next_first):
            {
                const uint8_t *pc1;

                val = get_u32(pc);
                pc += 4;
                if (opcode == REOP_split_next_first) {
                    /* greedy loop over one character: X* is
                       split_next_first, X, goto back to the split */
                    int alen = lre_simple_atom_len(pc);
                    if (alen && pc[alen] == REOP_goto &&
                        pc + alen + 5 + (int)get_u32(pc + alen + 1) == pc - 5) {
                        run_atom = pc;
                        run_exit = pc + (int)val;
                        goto char_run;
                    }
                    pc1 = pc + (int)val;
                } else {
                    if ((int)val < 0) {
                        /* the tail of X+: X, split_goto_first back to X */
                        const uint8_t *a = pc + (int)val;
                        int alen = lre_simple_atom_len(a);
                        if (alen && a + alen == pc - 5) {
                            run_atom = a;
                            run_exit = pc;
                            goto char_run;
                        }
                    }
                    pc1 = pc;
                    pc = pc + (int)val;
                }
                CHECK_STACK_SPACE(3);
                sp[0].ptr = (uint8_t *)pc1;
                sp[1].ptr = (uint8_t *)cptr;
                sp[2].bp.val = bp - s->stack_buf;
                sp[2].bp.type = RE_EXEC_STATE_SPLIT;
                sp += 3;
                bp = sp;
            }
            BREAK;
        char_run:
            {
                const uint8_t *p = cptr, *e = cbuf_end;
                uint32_t aop = run_atom[0], v;
                const uint8_t *ra = run_atom + 3;
                uint64_t bm[4];
                int rn;

#define LRE_RUN(COND)                                                   \
    if (cbuf_type == 0) {                                               \
        while (p < e) { c = *p; if (!(COND)) break; p++; }              \
    } else {                                                            \
        while (p < e) {                                                 \
            const uint8_t *q = p;                                       \
            GET_CHAR(c, q, e, cbuf_type);                               \
            if (!(COND)) break;                                         \
            p = q;                                                      \
        }                                                               \
    }
                switch (aop) {
                case REOP_space:
                    LRE_RUN(lre_is_space(c));
                    break;
                case REOP_not_space:
                    LRE_RUN(!lre_is_space(c));
                    break;
                case REOP_dot:
                    LRE_RUN(!is_line_terminator(c));
                    break;
                case REOP_any:
                    p = e;
                    break;
                case REOP_char:
                    v = get_u16(run_atom + 1);
                    LRE_RUN(c == v);
                    break;
                case REOP_char_i:
                    v = get_u16(run_atom + 1);
                    LRE_RUN(lre_canon_fast(c, s->is_unicode) == v);
                    break;
                case REOP_range:
                    rn = get_u16(run_atom + 1);
                    if (cbuf_type == 0) {
                        /* test the first few characters directly; a run
                           that goes on gets a bitmap of the class */
                        const uint8_t *lim = (e - p > 8) ? p + 8 : e;
                        while (p < lim && lre_range_has(ra, rn, *p))
                            p++;
                        if (p == lim && lim < e) {
                            lre_range_bitmap(bm, ra, rn);
                            LRE_RUN((bm[c >> 6] >> (c & 63)) & 1);
                        }
                    } else {
                        LRE_RUN(lre_range_has(ra, rn, c));
                    }
                    break;
                default: /* REOP_range_i */
                    rn = get_u16(run_atom + 1);
                    LRE_RUN(lre_range_has(ra, rn, lre_canon_fast(c, s->is_unicode)));
                    break;
                }
#undef LRE_RUN
                if (p > cptr) {
                    CHECK_STACK_SPACE(4);
                    sp[0].ptr = (uint8_t *)cptr;
                    sp[1].ptr = (uint8_t *)run_exit;
                    sp[2].ptr = (uint8_t *)p;
                    sp[3].bp.val = bp - s->stack_buf;
                    sp[3].bp.type = RE_EXEC_STATE_RUN;
                    sp += 4;
                    bp = sp;
                    cptr = p;
                }
                pc = run_exit;
                if (lre_poll_timeout(s))
                    return LRE_RET_TIMEOUT;
            }
            BREAK;
        CASE(REOP_lookahead):
        CASE(REOP_negative_lookahead):
            val = get_u32(pc);
            pc += 4;
            CHECK_STACK_SPACE(3);
            sp[0].ptr = (uint8_t *)(pc + (int)val);
            sp[1].ptr = (uint8_t *)cptr;
            sp[2].bp.val = bp - s->stack_buf;
            sp[2].bp.type = RE_EXEC_STATE_LOOKAHEAD + opcode - REOP_lookahead;
            sp += 3;
            bp = sp;
            BREAK;
        CASE(REOP_goto):
            val = get_u32(pc);
            pc += 4 + (int)val;
            if (lre_poll_timeout(s))
                return LRE_RET_TIMEOUT;
            BREAK;
        CASE(REOP_line_start):
        CASE(REOP_line_start_m):
            if (cptr == s->cbuf)
                BREAK;
            if (opcode == REOP_line_start)
                goto no_match;
            PEEK_PREV_CHAR(c, cptr, s->cbuf, cbuf_type);
            if (!is_line_terminator(c))
                goto no_match;
            BREAK;
        CASE(REOP_line_end):
        CASE(REOP_line_end_m):
            if (cptr == cbuf_end)
                BREAK;
            if (opcode == REOP_line_end)
                goto no_match;
            PEEK_CHAR(c, cptr, cbuf_end, cbuf_type);
            if (!is_line_terminator(c))
                goto no_match;
            BREAK;
        CASE(REOP_dot):
            if (cptr == cbuf_end)
                goto no_match;
            GET_CHAR(c, cptr, cbuf_end, cbuf_type);
            if (is_line_terminator(c))
                goto no_match;
            BREAK;
        CASE(REOP_any):
            if (cptr == cbuf_end)
                goto no_match;
            GET_CHAR(c, cptr, cbuf_end, cbuf_type);
            BREAK;
        CASE(REOP_space):
            if (cptr == cbuf_end)
                goto no_match;
            GET_CHAR(c, cptr, cbuf_end, cbuf_type);
            if (!lre_is_space(c))
                goto no_match;
            BREAK;
        CASE(REOP_not_space):
            if (cptr == cbuf_end)
                goto no_match;
            GET_CHAR(c, cptr, cbuf_end, cbuf_type);
            if (lre_is_space(c))
                goto no_match;
            BREAK;
        CASE(REOP_save_start):
        CASE(REOP_save_end):
            val = *pc++;
            if (val >= (uint32_t)s->capture_count)
                return LRE_RET_BYTECODE_ERROR;
            idx = 2 * val + opcode - REOP_save_start;
            SAVE_CAPTURE(idx, (uint8_t *)cptr);
            BREAK;
        CASE(REOP_save_reset):
            {
                uint32_t val2;
                val = pc[0];
                val2 = pc[1];
                pc += 2;
                if (val2 >= (uint32_t)s->capture_count)
                    return LRE_RET_BYTECODE_ERROR;
                CHECK_STACK_SPACE(2 * (val2 - val + 1));
                while (val <= val2) {
                    idx = 2 * val;
                    SAVE_CAPTURE(idx, NULL);
                    idx = 2 * val + 1;
                    SAVE_CAPTURE(idx, NULL);
                    val++;
                }
            }
            BREAK;
        CASE(REOP_set_i32):
            idx = 2 * s->capture_count + pc[0];
            val = get_u32(pc + 1);
            pc += 5;
            SAVE_CAPTURE_CHECK(idx, (void *)(uintptr_t)val);
            BREAK;
        CASE(REOP_loop):
            {
                uint32_t val2;
                idx = 2 * s->capture_count + pc[0];
                val = get_u32(pc + 1);
                pc += 5;

                val2 = (uintptr_t)capture[idx] - 1;
                SAVE_CAPTURE_CHECK(idx, (void *)(uintptr_t)val2);
                if (val2 != 0) {
                    pc += (int)val;
                    if (lre_poll_timeout(s))
                        return LRE_RET_TIMEOUT;
                }
            }
            BREAK;
        CASE(REOP_loop_split_goto_first):
        CASE(REOP_loop_split_next_first):
        CASE(REOP_loop_check_adv_split_goto_first):
        CASE(REOP_loop_check_adv_split_next_first):
            {
                const uint8_t *pc1;
                uint32_t val2, limit;
                idx = 2 * s->capture_count + pc[0];
                limit = get_u32(pc + 1);
                val = get_u32(pc + 5);
                pc += 9;

                /* decrement the counter */
                val2 = (uintptr_t)capture[idx] - 1;
                SAVE_CAPTURE_CHECK(idx, (void *)(uintptr_t)val2);

                if (val2 > limit) {
                    /* normal loop if counter > limit */
                    pc += (int)val;
                    if (lre_poll_timeout(s))
                        return LRE_RET_TIMEOUT;
                } else {
                    /* check advance */
                    if ((opcode == REOP_loop_check_adv_split_goto_first ||
                         opcode == REOP_loop_check_adv_split_next_first) &&
                        capture[idx + 1] == cptr &&
                        val2 != limit) {
                        goto no_match;
                    }

                    /* otherwise conditional split */
                    if (val2 != 0) {
                        if (opcode == REOP_loop_split_next_first ||
                            opcode == REOP_loop_check_adv_split_next_first) {
                            pc1 = pc + (int)val;
                        } else {
                            pc1 = pc;
                            pc = pc + (int)val;
                        }
                        CHECK_STACK_SPACE(3);
                        sp[0].ptr = (uint8_t *)pc1;
                        sp[1].ptr = (uint8_t *)cptr;
                        sp[2].bp.val = bp - s->stack_buf;
                        sp[2].bp.type = RE_EXEC_STATE_SPLIT;
                        sp += 3;
                        bp = sp;
                    }
                }
            }
            BREAK;
        CASE(REOP_set_char_pos):
            idx = 2 * s->capture_count + pc[0];
            pc++;
            SAVE_CAPTURE_CHECK(idx, (uint8_t *)cptr);
            BREAK;
        CASE(REOP_check_advance):
            idx = 2 * s->capture_count + pc[0];
            pc++;
            if (capture[idx] == cptr)
                goto no_match;
            BREAK;
        CASE(REOP_word_boundary):
        CASE(REOP_word_boundary_i):
        CASE(REOP_not_word_boundary):
        CASE(REOP_not_word_boundary_i):
            {
                bool v1, v2;
                int ignore_case = (opcode == REOP_word_boundary_i || opcode == REOP_not_word_boundary_i);
                bool is_boundary = (opcode == REOP_word_boundary || opcode == REOP_word_boundary_i);
                /* char before */
                if (cptr == s->cbuf) {
                    v1 = false;
                } else {
                    PEEK_PREV_CHAR(c, cptr, s->cbuf, cbuf_type);
                    if (c < 256) {
                        v1 = (lre_is_word_byte(c) != 0);
                    } else {
                        v1 = ignore_case && (c == 0x017f || c == 0x212a);
                    }
                }
                /* current char */
                if (cptr >= cbuf_end) {
                    v2 = false;
                } else {
                    PEEK_CHAR(c, cptr, cbuf_end, cbuf_type);
                    if (c < 256) {
                        v2 = (lre_is_word_byte(c) != 0);
                    } else {
                        v2 = ignore_case && (c == 0x017f || c == 0x212a);
                    }
                }
                if (v1 ^ v2 ^ is_boundary)
                    goto no_match;
            }
            BREAK;
        CASE(REOP_back_reference):
        CASE(REOP_back_reference_i):
        CASE(REOP_backward_back_reference):
        CASE(REOP_backward_back_reference_i):
            {
                const uint8_t *cptr1, *cptr1_end, *cptr1_start;
                const uint8_t *pc1;
                uint32_t c1, c2;
                int i, n;

                n = *pc++;
                pc1 = pc;
                pc += n;

                for(i = 0; i < n; i++) {
                    val = pc1[i];
                    if (val >= s->capture_count)
                        goto no_match;
                    cptr1_start = capture[2 * val];
                    cptr1_end = capture[2 * val + 1];
                    /* test the first not empty capture */
                    if (cptr1_start && cptr1_end) {
                        if (opcode == REOP_back_reference ||
                            opcode == REOP_back_reference_i) {
                            cptr1 = cptr1_start;
                            while (cptr1 < cptr1_end) {
                                if (cptr >= cbuf_end)
                                    goto no_match;
                                GET_CHAR(c1, cptr1, cptr1_end, cbuf_type);
                                GET_CHAR(c2, cptr, cbuf_end, cbuf_type);
                                if (opcode == REOP_back_reference_i) {
                                    c1 = lre_canonicalize(c1, s->is_unicode);
                                    c2 = lre_canonicalize(c2, s->is_unicode);
                                }
                                if (c1 != c2)
                                    goto no_match;
                            }
                        } else {
                            cptr1 = cptr1_end;
                            while (cptr1 > cptr1_start) {
                                if (cptr == s->cbuf)
                                    goto no_match;
                                GET_PREV_CHAR(c1, cptr1, cptr1_start, cbuf_type);
                                GET_PREV_CHAR(c2, cptr, s->cbuf, cbuf_type);
                                if (opcode == REOP_backward_back_reference_i) {
                                    c1 = lre_canonicalize(c1, s->is_unicode);
                                    c2 = lre_canonicalize(c2, s->is_unicode);
                                }
                                if (c1 != c2)
                                    goto no_match;
                            }
                        }
                        break;
                    }
                }
            }
            BREAK;
        CASE(REOP_range):
        CASE(REOP_range_i):
            {
                int n;

                n = get_u16(pc); /* n must be >= 1 */
                pc += 2;
                if (cptr >= cbuf_end)
                    goto no_match;
                GET_CHAR(c, cptr, cbuf_end, cbuf_type);
                if (opcode == REOP_range_i) {
                    c = lre_canon_fast(c, s->is_unicode);
                }
                if (!lre_range_has(pc, n, c))
                    goto no_match;
                pc += 4 * n;
            }
            BREAK;
        CASE(REOP_range32):
        CASE(REOP_range32_i):
            {
                int n;
                uint32_t low, high, idx_min, idx_max, idx;

                n = get_u16(pc); /* n must be >= 1 */
                pc += 2;
                if (cptr >= cbuf_end)
                    goto no_match;
                GET_CHAR(c, cptr, cbuf_end, cbuf_type);
                if (opcode == REOP_range32_i) {
                    c = lre_canonicalize(c, s->is_unicode);
                }
                idx_min = 0;
                low = get_u32(pc + 0 * 8);
                if (c < low)
                    goto no_match;
                idx_max = n - 1;
                high = get_u32(pc + idx_max * 8 + 4);
                if (c > high)
                    goto no_match;
                while (idx_min <= idx_max) {
                    idx = (idx_min + idx_max) / 2;
                    low = get_u32(pc + idx * 8);
                    high = get_u32(pc + idx * 8 + 4);
                    if (c < low)
                        idx_max = idx - 1;
                    else if (c > high)
                        idx_min = idx + 1;
                    else
                        goto range32_match;
                }
                goto no_match;
            range32_match:
                pc += 8 * n;
            }
            BREAK;
        CASE(REOP_prev):
            /* go to the previous char */
            if (cptr == s->cbuf)
                goto no_match;
            PREV_CHAR(cptr, s->cbuf, cbuf_type);
            BREAK;
        CASE(REOP_invalid):
        DEFAULT:
#ifdef DUMP_EXEC
            printf("unknown opcode pc=%ld\n", (long)(pc - 1 - pc_start));
#endif
            abort();
        }
    }
#undef SWITCH
#undef CASE
#undef DEFAULT
#undef BREAK
#undef LRE_DUMP_OP
}
