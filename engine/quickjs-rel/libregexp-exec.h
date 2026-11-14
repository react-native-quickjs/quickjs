/* The register matcher's execution context and character access, shared by the interpreter (libregexp.c) and by
   matchers generated ahead of time from a pattern's bytecode. */
#ifndef LIBREGEXP_EXEC_H
#define LIBREGEXP_EXEC_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "cutils.h"
#include "libregexp.h"
#include "libunicode.h"

#define RE_HEADER_FLAGS          0
#define RE_HEADER_CAPTURE_COUNT  2
#define RE_HEADER_REGISTER_COUNT 3
#define RE_HEADER_BYTECODE_LEN   4

#define RE_HEADER_LEN 8

#define CP_LS   0x2028
#define CP_PS   0x2029

typedef enum {
#define DEF(id, size) REOP_ ## id,
#include "libregexp-opcode.h"
#undef DEF
    REOP_COUNT,
} REOPCodeEnum;

/* must be large enough to have a negligible runtime cost and small
   enough to call the interrupt callback often. */
#define INTERRUPT_COUNTER_INIT 10000


static inline bool is_line_terminator(uint32_t c)
{
    return (c == '\n' || c == '\r' || c == CP_LS || c == CP_PS);
}

#define GET_CHAR(c, cptr, cbuf_end, cbuf_type)                          \
    do {                                                                \
        if (cbuf_type == 0) {                                           \
            c = *cptr++;                                                \
        } else {                                                        \
            const uint16_t *_p = (const uint16_t *)cptr;                \
            const uint16_t *_end = (const uint16_t *)cbuf_end;          \
            c = *_p++;                                                  \
            if (is_hi_surrogate(c) && cbuf_type == 2) {                 \
                if (_p < _end && is_lo_surrogate(*_p)) {                \
                    c = from_surrogate(c, *_p++);                       \
                }                                                       \
            }                                                           \
            cptr = (const void *)_p;                                    \
        }                                                               \
    } while (0)

#define PEEK_CHAR(c, cptr, cbuf_end, cbuf_type)                         \
    do {                                                                \
        if (cbuf_type == 0) {                                           \
            c = cptr[0];                                                \
        } else {                                                        \
            const uint16_t *_p = (const uint16_t *)cptr;                \
            const uint16_t *_end = (const uint16_t *)cbuf_end;          \
            c = *_p++;                                                  \
            if (is_hi_surrogate(c) && cbuf_type == 2) {                 \
                if (_p < _end && is_lo_surrogate(*_p)) {                \
                    c = from_surrogate(c, *_p);                         \
                }                                                       \
            }                                                           \
        }                                                               \
    } while (0)

#define PEEK_PREV_CHAR(c, cptr, cbuf_start, cbuf_type)                  \
    do {                                                                \
        if (cbuf_type == 0) {                                           \
            c = cptr[-1];                                               \
        } else {                                                        \
            const uint16_t *_p = (const uint16_t *)cptr - 1;            \
            const uint16_t *_start = (const uint16_t *)cbuf_start;      \
            c = *_p;                                                    \
            if (is_lo_surrogate(c) && cbuf_type == 2) {                 \
                if (_p > _start && is_hi_surrogate(_p[-1])) {           \
                    c = from_surrogate(*--_p, c);                       \
                }                                                       \
            }                                                           \
        }                                                               \
    } while (0)

#define GET_PREV_CHAR(c, cptr, cbuf_start, cbuf_type)                   \
    do {                                                                \
        if (cbuf_type == 0) {                                           \
            cptr--;                                                     \
            c = cptr[0];                                                \
        } else {                                                        \
            const uint16_t *_p = (const uint16_t *)cptr - 1;            \
            const uint16_t *_start = (const uint16_t *)cbuf_start;      \
            c = *_p;                                                    \
            if (is_lo_surrogate(c) && cbuf_type == 2) {                 \
                if (_p > _start && is_hi_surrogate(_p[-1])) {           \
                    c = from_surrogate(*--_p, c);                       \
                }                                                       \
            }                                                           \
            cptr = (const void *)_p;                                    \
        }                                                               \
    } while (0)

#define PREV_CHAR(cptr, cbuf_start, cbuf_type)                          \
    do {                                                                \
        if (cbuf_type == 0) {                                           \
            cptr--;                                                     \
        } else {                                                        \
            const uint16_t *_p = (const uint16_t *)cptr - 1;            \
            const uint16_t *_start = (const uint16_t *)cbuf_start;      \
            if (is_lo_surrogate(*_p) && cbuf_type == 2) {               \
                if (_p > _start && is_hi_surrogate(_p[-1])) {           \
                    --_p;                                               \
                }                                                       \
            }                                                           \
            cptr = (const void *)_p;                                    \
        }                                                               \
    } while (0)

typedef enum {
    RE_EXEC_STATE_SPLIT,
    RE_EXEC_STATE_LOOKAHEAD,
    RE_EXEC_STATE_NEGATIVE_LOOKAHEAD,
    /* a greedy run of a single-character atom: four slots (run start, exit
       pc, current end, link) that give back one character per backtrack */
    RE_EXEC_STATE_RUN,
} REExecStateEnum;

#if INTPTR_MAX >= INT64_MAX
#define BP_TYPE_BITS 3
#else
#define BP_TYPE_BITS 2
/* growth of the backtrack stack (libregexp.c); the caller re-derives its pointers from the offsets it saved */
int lre_stack_realloc(REExecContext *s, size_t n);
#define stack_realloc lre_stack_realloc

#endif

typedef union {
    uint8_t *ptr;
    intptr_t val; /* for bp, the low BP_SHIFT bits store REExecStateEnum */
    struct {
        uintptr_t val : sizeof(uintptr_t) * 8 - BP_TYPE_BITS;
        uintptr_t type : BP_TYPE_BITS;
    } bp;
} StackElem;

typedef struct REExecContext {
    const uint8_t *cbuf;
    const uint8_t *cbuf_end;
    /* 0 = 8 bit chars, 1 = 16 bit chars, 2 = 16 bit chars, UTF-16 */
    int cbuf_type;
    int capture_count;
    bool is_unicode;
    int interrupt_counter;
    void *opaque; /* used for stack overflow check */

    StackElem *stack_buf;
    size_t stack_size;
    StackElem static_stack_buf[32]; /* static stack to avoid allocation in most cases */
} REExecContext;

static inline int lre_poll_timeout(REExecContext *s)
{
    if (unlikely(--s->interrupt_counter <= 0)) {
        s->interrupt_counter = INTERRUPT_COUNTER_INIT;
        if (lre_check_timeout(s->opaque))
            return LRE_RET_TIMEOUT;
    }
    return 0;
}

/* ASCII fast path of lre_canonicalize(), called per character by the
   case-insensitive opcodes. */
static inline __attribute__((always_inline)) uint32_t lre_canon_fast(uint32_t c, bool is_unicode)
{
    if (c < 128) {
        if (is_unicode) {
            if (c - 'A' < 26u)
                c += 'a' - 'A';
        } else {
            if (c - 'a' < 26u)
                c -= 'a' - 'A';
        }
        return c;
    }
    return lre_canonicalize(c, is_unicode);
}

/* Length of the bytecode of a single-character atom that a loop can run
   without saving a state per iteration, or 0 for any other opcode. */
static inline __attribute__((always_inline)) int lre_simple_atom_len(const uint8_t *p)
{
    switch (p[0]) {
    case REOP_dot:
    case REOP_any:
    case REOP_space:
    case REOP_not_space:
        return 1;
    case REOP_char:
    case REOP_char_i:
        return 3;
    case REOP_range:
    case REOP_range_i:
        return 3 + 4 * get_u16(p + 1);
    default:
        return 0;
    }
}

/* Test c against the n ranges of a REOP_range operand. */
static inline __attribute__((always_inline)) bool lre_range_has(const uint8_t *pc, int n, uint32_t c)
{
    uint32_t low, high, idx_min, idx_max, idx;

    low = get_u16(pc);
    if (c < low)
        return false;
    idx_max = n - 1;
    high = get_u16(pc + idx_max * 4 + 2);
    /* 0xffff in for last value means +infinity */
    if (unlikely(c >= 0xffff) && high == 0xffff)
        return true;
    if (c > high)
        return false;
    idx_min = 0;
    while (idx_min <= idx_max) {
        idx = (idx_min + idx_max) / 2;
        low = get_u16(pc + idx * 4);
        high = get_u16(pc + idx * 4 + 2);
        if (c < low)
            idx_max = idx - 1;
        else if (c > high)
            idx_min = idx + 1;
        else
            return true;
    }
    return false;
}

/* Bitmap of the code units below 256 that a REOP_range operand accepts. */
static inline void lre_range_bitmap(uint64_t bm[4], const uint8_t *pc, int n)
{
    int i;
    bm[0] = bm[1] = bm[2] = bm[3] = 0;
    for (i = 0; i < n; i++) {
        uint32_t lo = get_u16(pc + i * 4), hi = get_u16(pc + i * 4 + 2);
        uint32_t w;
        if (lo > 255)
            break;
        if (hi > 255)
            hi = 255;
        for (w = lo >> 6; w <= (hi >> 6); w++) {
            uint64_t m = ~(uint64_t)0;
            if (w == (lo >> 6))
                m &= ~(uint64_t)0 << (lo & 63);
            if (w == (hi >> 6))
                m &= ~(uint64_t)0 >> (63 - (hi & 63));
            bm[w] |= m;
        }
    }
}

/* growth of the backtrack stack (libregexp.c); the caller re-derives its pointers from the offsets it saved */
int lre_stack_realloc(REExecContext *s, size_t n);
#define stack_realloc lre_stack_realloc

#endif
