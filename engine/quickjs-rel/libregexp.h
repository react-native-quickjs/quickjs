/*
 * Regular Expression Engine
 *
 * Copyright (c) 2017-2018 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#ifndef LIBREGEXP_H
#define LIBREGEXP_H

#include <stdbool.h>
#include <stddef.h>

#include "libunicode.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LRE_FLAG_GLOBAL     (1 << 0)
#define LRE_FLAG_IGNORECASE (1 << 1)
#define LRE_FLAG_MULTILINE  (1 << 2)
#define LRE_FLAG_DOTALL     (1 << 3)
#define LRE_FLAG_UNICODE    (1 << 4)
#define LRE_FLAG_STICKY     (1 << 5)
#define LRE_FLAG_INDICES    (1 << 6) /* Unused by libregexp, just recorded. */
#define LRE_FLAG_NAMED_GROUPS (1 << 7) /* named groups are present in the regexp */
#define LRE_FLAG_UNICODE_SETS (1 << 8)
#define LRE_FLAG_ATOM       (1 << 9) /* internal: raw literal pattern */
#define LRE_FLAG_HAS_META   (1 << 10) /* internal optimization metadata */

/* RegExp optimization metadata is serialized little-endian after the opcode
   stream. Source literals borrow their payload from JSRegExp.pattern; all
   other payloads are bounded so untrusted bytecode cannot request an
   unbounded fast-path allocation or comparison. */
#define LRE_META_VERSION 3
#define LRE_META_NONE 0
#define LRE_META_LITERAL 1
#define LRE_META_PREFIX 2
#define LRE_META_FIRST_SET 3 /* 256-bit bitmap of the mandatory first char */

/* LRE_META_FIRST_SET payload: 1 flag byte (whether the first char may be
   > 0xff) followed by a 32-byte little-endian bitmap covering code points
   0..255. */
#define LRE_FIRST_SET_BITMAP_LEN 32
#define LRE_FIRST_SET_PAYLOAD_LEN (1 + LRE_FIRST_SET_BITMAP_LEN)

#define LRE_META_FLAG_SOURCE 1 /* literal uses the source pattern; no payload */

#define LRE_LITERAL_MAX_LENGTH 4096
#define LRE_PREFIX_MIN_LENGTH 3
#define LRE_PREFIX_MAX_LENGTH 32

#define LRE_META_VERSION_OFFSET 0
#define LRE_META_KIND_OFFSET 1
#define LRE_META_ENCODING_OFFSET 2
#define LRE_META_FLAGS_OFFSET 3
#define LRE_META_PAYLOAD_SIZE_OFFSET 4
#define LRE_META_LENGTH_OFFSET 8
#define LRE_META_ENTRY_OFFSET 12
#define LRE_META_HEADER_LEN 16

typedef struct LREMetadata {
    const uint8_t *payload;
    const uint8_t *leading_chars;
    uint32_t payload_size;
    uint32_t primary_payload_size;
    uint32_t length;
    uint32_t entry_offset;
    uint8_t kind;
    uint8_t encoding;
    uint8_t flags;
} LREMetadata;

#define LRE_RET_MEMORY_ERROR   (-1)
#define LRE_RET_TIMEOUT        (-2)
#define LRE_RET_BYTECODE_ERROR (-3)

/* trailer length after the group name including the trailing '\0' */
#define LRE_GROUP_NAME_TRAILER_LEN 2

uint8_t *lre_compile(int *plen, char *error_msg, int error_msg_size,
                     const char *buf, size_t buf_len, int re_flags,
                     void *opaque);
int lre_get_alloc_count(const uint8_t *bc_buf);
int lre_check_bytecode(const uint8_t *bc_buf, int bc_buf_len, void *opaque);
int lre_get_capture_count(const uint8_t *bc_buf);
int lre_get_flags(const uint8_t *bc_buf);
int lre_is_raw_atom(const char *buf, size_t len, int re_flags);
int lre_get_metadata(const uint8_t *bc_buf, size_t bc_buf_len,
                     LREMetadata *meta);
const char *lre_get_groupnames(const uint8_t *bc_buf, size_t bc_buf_len);
int lre_exec(uint8_t **capture,
             const uint8_t *bc_buf, const uint8_t *cbuf, int cindex, int clen,
             int cbuf_type, void *opaque);
int lre_exec_at(uint8_t **capture,
                const uint8_t *bc_buf, uint32_t bytecode_offset,
                const uint8_t *cbuf, int cindex, int clen,
                int cbuf_type, void *opaque);

/* A matcher generated ahead of time from one pattern's bytecode (see libregexp-exec.h). It runs the same machine as the
   interpreter from the entry at byte offset 'off' of the code (0, or the anchored entry after the scan prologue) and returns what
   the interpreter's backtracking function returns, or LRE_RET_AOT_NOENTRY when it has no code for that entry (the caller then
   runs the interpreter). */
struct REExecContext;
typedef intptr_t (*LreAotFn)(struct REExecContext *s, uint8_t **capture, const uint8_t *cptr, uint32_t off);
#define LRE_RET_AOT_NOENTRY (-100)
typedef struct LreAotEntry {
    const uint8_t *bc;  /* the exact bytecode the matcher was generated from */
    uint32_t len;
    LreAotFn fn8;       /* subject of 8-bit units, or NULL */
    LreAotFn fn16;      /* subject of 16-bit units (not unicode mode), or NULL */
} LreAotEntry;
void lre_aot_register(const LreAotEntry *tab, int n);
/* the entry whose bytecode equals bc[0..len), or NULL */
const LreAotEntry *lre_aot_find(const uint8_t *bc, size_t len);
extern int lre_aot_count;
int lre_exec_ex(const LreAotEntry *aot, uint8_t **capture,
                const uint8_t *bc_buf, const uint8_t *cbuf, int cindex, int clen,
                int cbuf_type, void *opaque);
int lre_exec_at_ex(const LreAotEntry *aot, uint8_t **capture,
                   const uint8_t *bc_buf, uint32_t bytecode_offset,
                   const uint8_t *cbuf, int cindex, int clen,
                   int cbuf_type, void *opaque);

#define LRE_REQ_MAX 32
typedef struct LRERequired {
    uint16_t lit[LRE_REQ_MAX]; /* code units every match contains, in order */
    int len;                   /* 0 if none */
    uint32_t ci;               /* bit i: lit[i] is a lower-case ASCII letter matched in either case */
    int rare;                  /* index in lit of the unit to search for first */
    uint8_t apre[LRE_REQ_MAX]; /* with anchored: the exact 8-bit units that follow the ^ */
    int alen;
    int prefix_max;            /* most code units a match can start before lit, -1 if unbounded */
    uint32_t entry;            /* bytecode offset of the anchored entry (save_start 0) */
    int anchored;              /* a match can only start at index 0 */
} LRERequired;
/* Analyse compiled bytecode. Returns 1 if anything was found, else 0. */
int lre_analyze2(const uint8_t *bc_buf, size_t bc_buf_len, LRERequired *out);

int lre_parse_escape(const uint8_t **pp, int allow_utf16);
/* lre_is_space() is provided as an inline in libunicode.h */

/* must be provided by the user */
bool lre_check_stack_overflow(void *opaque, size_t alloca_size);
/* must be provided by the user, return non zero if time out */
int lre_check_timeout(void *opaque);
void *lre_realloc(void *opaque, void *ptr, size_t size);

/* JS identifier test */
extern uint32_t const lre_id_start_table_ascii[4];
extern uint32_t const lre_id_continue_table_ascii[4];

static inline int lre_js_is_ident_first(int c)
{
    if ((uint32_t)c < 128) {
        return (lre_id_start_table_ascii[c >> 5] >> (c & 31)) & 1;
    } else {
        return lre_is_id_start(c);
    }
}

static inline int lre_js_is_ident_next(int c)
{
    if ((uint32_t)c < 128) {
        return (lre_id_continue_table_ascii[c >> 5] >> (c & 31)) & 1;
    } else {
        /* ZWNJ and ZWJ are accepted in identifiers */
        return lre_is_id_continue(c) || c == 0x200C || c == 0x200D;
    }
}

#ifdef __cplusplus
} /* extern "C" { */
#endif

#endif /* LIBREGEXP_H */
