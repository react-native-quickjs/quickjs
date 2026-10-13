/*
 * Register VM: the semantic lowering inventory and the register ISA families.
 *
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * Included by quickjs.c, under QJS_REGVM only. It is the exhaustive
 * classification of the frontend's emitted operations: every DEF in
 * quickjs-opcode.h has a REG_LOWERING_<id> line here, and quickjs.c builds
 * regvm_op_kind[] by including quickjs-opcode.h with DEF redefined to emit
 * REG_LOWERING_ ## id. An opcode added to quickjs-opcode.h without a line here
 * is an undeclared identifier and fails the build -- that is the completeness
 * gate, and it is a compile-time gate on purpose. n_pop, n_push and fmt
 * describe stack layout, not semantics, so they are never used to infer that
 * an opcode is supported.
 *
 * The register ISA is a separate numbering from the stack opcodes. A lowering
 * kind names the semantic family an operation belongs to; a family names the
 * register instruction shape that executes it. Stack shaping (dup, perm, rot,
 * ...) is REG_LOWER_ELIDED: it has no register instruction because lowering
 * removes it.
 *
 * See .local/reg_vm_design.md for the architecture this serves. Nothing here
 * executes yet; it is the frozen input's classification and the ISA inventory
 * the decoder, allocator and emitter are built against.
 */

#ifndef QUICKJS_REGVM_H
#define QUICKJS_REGVM_H

/*
 * Ownership carried by a virtual value. The register frame owns only what is
 * tagged owned; primitives and borrowed references perform no RC operation.
 */
typedef enum JSRegValueOwnership {
    REG_VALUE_PRIMITIVE = 0,
    REG_VALUE_BORROWED,
    REG_VALUE_OWNED,
} JSRegValueOwnership;

/*
 * What an emitted stack opcode lowers to. This is the inventory's value type.
 * UNSET is the sentinel a completeness failure leaves behind; it is never a
 * valid lowering and the verifier rejects it.
 */
typedef enum JSRegLoweringKind {
    REG_LOWER_UNSET = 0,   /* unclassified: a completeness-gate failure */
    REG_LOWER_RESERVED,    /* reserved/unreachable: must never be emitted */
    REG_LOWER_ELIDED,      /* pure stack shaping: no register instruction */
    REG_LOWER_CONST,       /* constant / literal materialization */
    REG_LOWER_MOVE,        /* value copy that survives lowering */
    REG_LOWER_CLOSURE,     /* closure creation */
    REG_LOWER_FRAME_READ,  /* local / argument / var-ref read */
    REG_LOWER_FRAME_WRITE, /* local / argument / var-ref write */
    REG_LOWER_UNARY,
    REG_LOWER_BINARY,
    REG_LOWER_PROPERTY,    /* named / private / super property */
    REG_LOWER_ELEMENT,     /* indexed property */
    REG_LOWER_BRANCH,
    REG_LOWER_CALL,        /* call / method / construct / apply / tail */
    REG_LOWER_RETURN,
    REG_LOWER_EXCEPTION,   /* throw / catch / iterator-close on unwind */
    REG_LOWER_FINALLY,     /* gosub / ret continuations */
    REG_LOWER_ITERATOR,
    REG_LOWER_SUSPEND,     /* yield / await */
    REG_LOWER_SCOPE,       /* scope, name, with, tdz, make-ref */
    REG_LOWER_SPECIAL,     /* engine control, boxing, class, module, eval */
    REG_LOWER_KIND_COUNT,
} JSRegLoweringKind;

/*
 * Register ISA families. A family is a shape of register instruction, not a
 * workload-specific recipe: one Call names destination, callee, receiver, an
 * argument window and a call kind; one property instruction names a
 * destination, a receiver, a key and an IC site.
 */
typedef enum JSRegFamily {
    REG_FAMILY_NONE = 0, /* elided: no register instruction */
    REG_FAMILY_CONST,
    REG_FAMILY_MOVE,
    REG_FAMILY_BINDING,
    REG_FAMILY_ARITH,
    REG_FAMILY_COMPARE,
    REG_FAMILY_PROPERTY,
    REG_FAMILY_ELEMENT,
    REG_FAMILY_BRANCH,
    REG_FAMILY_CALL,
    REG_FAMILY_RETURN,
    REG_FAMILY_EXCEPTION,
    REG_FAMILY_FINALLY,
    REG_FAMILY_ITERATOR,
    REG_FAMILY_SUSPEND,
    REG_FAMILY_SCOPE,
    REG_FAMILY_CLOSURE,
    REG_FAMILY_SPECIAL,
    REG_FAMILY_COUNT,
} JSRegFamily;

/* --- the inventory -------------------------------------------------------
 *
 * One line per opcode in quickjs-opcode.h, in file order. The order does not
 * matter to the generated array (initializers are positional, but the array is
 * built by re-including the opcode list), only completeness does. Grouped as
 * the opcode list groups them.
 */

/* escape prefix: a 2-byte prefix whose subop refines it. Subop 0/1 are the
   debugger traps (0 -> 0, elided); subop 2 is the fused get_loc0 get_loc1
   (0 -> 2 frame reads). The decoder dispatches on the subop; the table entry
   is the conservative family, never used to skip the subop check. */
#define REG_LOWERING_esc1 REG_LOWER_SPECIAL

/* push values */
#define REG_LOWERING_push_i32           REG_LOWER_CONST
#define REG_LOWERING_push_const         REG_LOWER_CONST
#define REG_LOWERING_fclosure           REG_LOWER_CLOSURE
#define REG_LOWERING_push_atom_value    REG_LOWER_CONST
#define REG_LOWERING_private_symbol     REG_LOWER_CONST
#define REG_LOWERING_undefined          REG_LOWER_CONST
#define REG_LOWERING_null               REG_LOWER_CONST
#define REG_LOWERING_push_this          REG_LOWER_SPECIAL
#define REG_LOWERING_push_false         REG_LOWER_CONST
#define REG_LOWERING_push_true          REG_LOWER_CONST
#define REG_LOWERING_object             REG_LOWER_CONST
#define REG_LOWERING_object_template    REG_LOWER_CONST
#define REG_LOWERING_special_object     REG_LOWER_CONST
#define REG_LOWERING_rest               REG_LOWER_SPECIAL

/* stack shaping: elided */
#define REG_LOWERING_drop               REG_LOWER_ELIDED
#define REG_LOWERING_nip                REG_LOWER_ELIDED
#define REG_LOWERING_object_fill        REG_LOWER_PROPERTY
#define REG_LOWERING_dup                REG_LOWER_ELIDED
#define REG_LOWERING_dup1               REG_LOWER_ELIDED
#define REG_LOWERING_dup2               REG_LOWER_ELIDED
#define REG_LOWERING_dup3               REG_LOWER_ELIDED
#define REG_LOWERING_insert2            REG_LOWER_ELIDED
#define REG_LOWERING_insert3            REG_LOWER_ELIDED
#define REG_LOWERING_insert4            REG_LOWER_ELIDED
#define REG_LOWERING_perm3              REG_LOWER_ELIDED
#define REG_LOWERING_perm4              REG_LOWER_ELIDED
#define REG_LOWERING_perm5              REG_LOWER_ELIDED
#define REG_LOWERING_swap               REG_LOWER_ELIDED
#define REG_LOWERING_swap2              REG_LOWER_ELIDED
#define REG_LOWERING_rot3l              REG_LOWER_ELIDED
#define REG_LOWERING_rot3r              REG_LOWER_ELIDED
#define REG_LOWERING_rot4l              REG_LOWER_ELIDED
#define REG_LOWERING_rot5l              REG_LOWER_ELIDED

/* calls / returns / construction */
#define REG_LOWERING_call_constructor   REG_LOWER_CALL
#define REG_LOWERING_call               REG_LOWER_CALL
#define REG_LOWERING_tail_call          REG_LOWER_CALL
#define REG_LOWERING_call_method        REG_LOWER_CALL
#define REG_LOWERING_tail_call_method   REG_LOWER_CALL
#define REG_LOWERING_array_from         REG_LOWER_CALL
#define REG_LOWERING_apply              REG_LOWER_CALL
#define REG_LOWERING_return             REG_LOWER_RETURN
#define REG_LOWERING_return_undef       REG_LOWER_RETURN
#define REG_LOWERING_check_ctor_return  REG_LOWER_SPECIAL
#define REG_LOWERING_check_ctor         REG_LOWER_SPECIAL
#define REG_LOWERING_init_ctor          REG_LOWER_SPECIAL
#define REG_LOWERING_check_brand        REG_LOWER_SPECIAL
#define REG_LOWERING_add_brand          REG_LOWER_SPECIAL
#define REG_LOWERING_return_async       REG_LOWER_RETURN
#define REG_LOWERING_throw              REG_LOWER_EXCEPTION
#define REG_LOWERING_throw_error        REG_LOWER_EXCEPTION
#define REG_LOWERING_eval               REG_LOWER_SPECIAL
#define REG_LOWERING_apply_eval         REG_LOWER_CALL
#define REG_LOWERING_regexp             REG_LOWER_SPECIAL
#define REG_LOWERING_get_super          REG_LOWER_SPECIAL
#define REG_LOWERING_import             REG_LOWER_SPECIAL

/* globals */
#define REG_LOWERING_get_var_undef      REG_LOWER_FRAME_READ
#define REG_LOWERING_get_var            REG_LOWER_FRAME_READ
#define REG_LOWERING_put_var            REG_LOWER_FRAME_WRITE
#define REG_LOWERING_put_var_init       REG_LOWER_FRAME_WRITE

#define REG_LOWERING_get_ref_value      REG_LOWER_SCOPE
#define REG_LOWERING_put_ref_value      REG_LOWER_SCOPE

#define REG_LOWERING_define_var         REG_LOWER_SCOPE
#define REG_LOWERING_check_define_var   REG_LOWER_SCOPE
#define REG_LOWERING_define_func        REG_LOWER_SCOPE

/* named property access */
#define REG_LOWERING_get_field          REG_LOWER_PROPERTY
#define REG_LOWERING_get_field2         REG_LOWER_PROPERTY
#define REG_LOWERING_put_field          REG_LOWER_PROPERTY
#define REG_LOWERING_get_field_ic       REG_LOWER_PROPERTY
#define REG_LOWERING_get_field2_ic      REG_LOWER_PROPERTY
#define REG_LOWERING_put_field_ic       REG_LOWER_PROPERTY

/* private / super property access */
#define REG_LOWERING_get_private_field  REG_LOWER_PROPERTY
#define REG_LOWERING_put_private_field  REG_LOWER_PROPERTY
#define REG_LOWERING_define_private_field REG_LOWER_PROPERTY

/* indexed property access */
#define REG_LOWERING_get_array_el       REG_LOWER_ELEMENT
#define REG_LOWERING_get_array_el2      REG_LOWER_ELEMENT
#define REG_LOWERING_put_array_el       REG_LOWER_ELEMENT
#define REG_LOWERING_get_super_value    REG_LOWER_PROPERTY
#define REG_LOWERING_put_super_value    REG_LOWER_PROPERTY

/* object / class construction */
#define REG_LOWERING_define_field       REG_LOWER_PROPERTY
#define REG_LOWERING_set_name           REG_LOWER_SPECIAL
#define REG_LOWERING_set_name_computed  REG_LOWER_SPECIAL
#define REG_LOWERING_set_proto          REG_LOWER_SPECIAL
#define REG_LOWERING_set_home_object    REG_LOWER_SPECIAL
#define REG_LOWERING_define_array_el    REG_LOWER_ELEMENT
#define REG_LOWERING_append             REG_LOWER_ELEMENT
#define REG_LOWERING_copy_data_properties REG_LOWER_SPECIAL
#define REG_LOWERING_define_method      REG_LOWER_PROPERTY
#define REG_LOWERING_define_method_computed REG_LOWER_PROPERTY
#define REG_LOWERING_define_class       REG_LOWER_SPECIAL
#define REG_LOWERING_define_class_computed REG_LOWER_SPECIAL

/* locals / arguments / closure references */
#define REG_LOWERING_get_loc            REG_LOWER_FRAME_READ
#define REG_LOWERING_put_loc            REG_LOWER_FRAME_WRITE
#define REG_LOWERING_set_loc            REG_LOWER_FRAME_WRITE
#define REG_LOWERING_get_arg            REG_LOWER_FRAME_READ
#define REG_LOWERING_put_arg            REG_LOWER_FRAME_WRITE
#define REG_LOWERING_set_arg            REG_LOWER_FRAME_WRITE
#define REG_LOWERING_get_var_ref        REG_LOWER_FRAME_READ
#define REG_LOWERING_put_var_ref        REG_LOWER_FRAME_WRITE
#define REG_LOWERING_set_var_ref        REG_LOWER_FRAME_WRITE

/* temporal dead zone and lexical scope */
#define REG_LOWERING_set_loc_uninitialized REG_LOWER_SCOPE
#define REG_LOWERING_get_loc_check      REG_LOWER_SCOPE
#define REG_LOWERING_put_loc_check      REG_LOWER_SCOPE
#define REG_LOWERING_put_loc_check_init REG_LOWER_SCOPE
#define REG_LOWERING_get_var_ref_check  REG_LOWER_SCOPE
#define REG_LOWERING_put_var_ref_check  REG_LOWER_SCOPE
#define REG_LOWERING_put_var_ref_check_init REG_LOWER_SCOPE
#define REG_LOWERING_close_loc          REG_LOWER_SCOPE

/* branches and exception control */
#define REG_LOWERING_if_false           REG_LOWER_BRANCH
#define REG_LOWERING_if_true            REG_LOWER_BRANCH
#define REG_LOWERING_goto               REG_LOWER_BRANCH
#define REG_LOWERING_catch              REG_LOWER_EXCEPTION
#define REG_LOWERING_gosub              REG_LOWER_FINALLY
#define REG_LOWERING_ret                REG_LOWER_FINALLY
#define REG_LOWERING_nip_catch          REG_LOWER_EXCEPTION

/* conversions */
#define REG_LOWERING_check_object       REG_LOWER_SPECIAL
#define REG_LOWERING_to_object          REG_LOWER_SPECIAL
#define REG_LOWERING_to_propkey         REG_LOWER_SPECIAL
#define REG_LOWERING_to_propkey2        REG_LOWER_SPECIAL

/* with / dynamic name resolution */
#define REG_LOWERING_with_get_var       REG_LOWER_SCOPE
#define REG_LOWERING_with_put_var       REG_LOWER_SCOPE
#define REG_LOWERING_with_delete_var    REG_LOWER_SCOPE
#define REG_LOWERING_with_make_ref      REG_LOWER_SCOPE
#define REG_LOWERING_with_get_ref       REG_LOWER_SCOPE
#define REG_LOWERING_with_get_ref_undef REG_LOWER_SCOPE
#define REG_LOWERING_make_loc_ref       REG_LOWER_SCOPE
#define REG_LOWERING_make_arg_ref       REG_LOWER_SCOPE
#define REG_LOWERING_make_var_ref_ref   REG_LOWER_SCOPE
#define REG_LOWERING_make_var_ref       REG_LOWER_SCOPE

/* iterators and suspension */
#define REG_LOWERING_for_in_start       REG_LOWER_ITERATOR
#define REG_LOWERING_for_of_start       REG_LOWER_ITERATOR
#define REG_LOWERING_for_await_of_start REG_LOWER_ITERATOR
#define REG_LOWERING_for_in_next        REG_LOWER_ITERATOR
#define REG_LOWERING_for_of_next        REG_LOWER_ITERATOR
#define REG_LOWERING_iterator_get_value_done REG_LOWER_ITERATOR
#define REG_LOWERING_iterator_close     REG_LOWER_ITERATOR
#define REG_LOWERING_iterator_next      REG_LOWER_ITERATOR
#define REG_LOWERING_iterator_call      REG_LOWER_ITERATOR
#define REG_LOWERING_initial_yield      REG_LOWER_SUSPEND
#define REG_LOWERING_yield              REG_LOWER_SUSPEND
#define REG_LOWERING_yield_star         REG_LOWER_SUSPEND
#define REG_LOWERING_async_yield_star   REG_LOWER_SUSPEND
#define REG_LOWERING_await              REG_LOWER_SUSPEND

/* arithmetic / logic */
#define REG_LOWERING_neg                REG_LOWER_UNARY
#define REG_LOWERING_plus               REG_LOWER_UNARY
#define REG_LOWERING_dec                REG_LOWER_UNARY
#define REG_LOWERING_inc                REG_LOWER_UNARY
#define REG_LOWERING_post_dec           REG_LOWER_UNARY
#define REG_LOWERING_post_inc           REG_LOWER_UNARY
#define REG_LOWERING_dec_loc            REG_LOWER_BINARY
#define REG_LOWERING_inc_loc            REG_LOWER_BINARY
#define REG_LOWERING_add_loc            REG_LOWER_BINARY
#define REG_LOWERING_not                REG_LOWER_UNARY
#define REG_LOWERING_lnot               REG_LOWER_UNARY
#define REG_LOWERING_typeof             REG_LOWER_UNARY
#define REG_LOWERING_delete             REG_LOWER_UNARY
#define REG_LOWERING_delete_var         REG_LOWER_SCOPE

#define REG_LOWERING_mul                REG_LOWER_BINARY
#define REG_LOWERING_div                REG_LOWER_BINARY
#define REG_LOWERING_mod                REG_LOWER_BINARY
#define REG_LOWERING_add                REG_LOWER_BINARY
#define REG_LOWERING_sub                REG_LOWER_BINARY
#define REG_LOWERING_shl                REG_LOWER_BINARY
#define REG_LOWERING_sar                REG_LOWER_BINARY
#define REG_LOWERING_shr                REG_LOWER_BINARY
#define REG_LOWERING_and                REG_LOWER_BINARY
#define REG_LOWERING_xor                REG_LOWER_BINARY
#define REG_LOWERING_or                 REG_LOWER_BINARY
#define REG_LOWERING_pow                REG_LOWER_BINARY

#define REG_LOWERING_lt                 REG_LOWER_BINARY
#define REG_LOWERING_lte                REG_LOWER_BINARY
#define REG_LOWERING_gt                 REG_LOWER_BINARY
#define REG_LOWERING_gte                REG_LOWER_BINARY
#define REG_LOWERING_instanceof         REG_LOWER_BINARY
#define REG_LOWERING_in                 REG_LOWER_BINARY
#define REG_LOWERING_eq                 REG_LOWER_BINARY
#define REG_LOWERING_neq                REG_LOWER_BINARY
#define REG_LOWERING_strict_eq          REG_LOWER_BINARY
#define REG_LOWERING_strict_neq         REG_LOWER_BINARY
#define REG_LOWERING_is_undefined_or_null REG_LOWER_UNARY
#define REG_LOWERING_private_in         REG_LOWER_BINARY
#define REG_LOWERING_push_bigint_i32    REG_LOWER_CONST

/* explicit resource management */
#define REG_LOWERING_using_dispose_init REG_LOWER_SPECIAL
#define REG_LOWERING_using_dispose      REG_LOWER_SPECIAL
#define REG_LOWERING_using_dispose_async REG_LOWER_SPECIAL
#define REG_LOWERING_using_dispose_merge REG_LOWER_SPECIAL
#define REG_LOWERING_using_dispose_end  REG_LOWER_SPECIAL
#define REG_LOWERING_using_check        REG_LOWER_SPECIAL

#define REG_LOWERING_nop                REG_LOWER_ELIDED

/* --- short forms ---------------------------------------------------------
 * The same operations, selected by the final short-opcode pass. Their stack
 * effects match their long forms, so their lowering kind does too.
 */

#define REG_LOWERING_push_minus1        REG_LOWER_CONST
#define REG_LOWERING_push_0             REG_LOWER_CONST
#define REG_LOWERING_push_1             REG_LOWER_CONST
#define REG_LOWERING_push_2             REG_LOWER_CONST
#define REG_LOWERING_push_3             REG_LOWER_CONST
#define REG_LOWERING_push_4             REG_LOWER_CONST
#define REG_LOWERING_push_5             REG_LOWER_CONST
#define REG_LOWERING_push_6             REG_LOWER_CONST
#define REG_LOWERING_push_7             REG_LOWER_CONST
#define REG_LOWERING_push_i8            REG_LOWER_CONST
#define REG_LOWERING_push_i16           REG_LOWER_CONST
#define REG_LOWERING_push_const8        REG_LOWER_CONST
#define REG_LOWERING_fclosure8          REG_LOWER_CLOSURE
#define REG_LOWERING_push_empty_string  REG_LOWER_CONST

#define REG_LOWERING_get_loc8           REG_LOWER_FRAME_READ
#define REG_LOWERING_put_loc8           REG_LOWER_FRAME_WRITE
#define REG_LOWERING_set_loc8           REG_LOWER_FRAME_WRITE

#define REG_LOWERING_get_loc_field_nr   REG_LOWER_PROPERTY

#define REG_LOWERING_get_loc0           REG_LOWER_FRAME_READ
#define REG_LOWERING_get_loc1           REG_LOWER_FRAME_READ
#define REG_LOWERING_get_loc2           REG_LOWER_FRAME_READ
#define REG_LOWERING_get_loc3           REG_LOWER_FRAME_READ
#define REG_LOWERING_put_loc0           REG_LOWER_FRAME_WRITE
#define REG_LOWERING_put_loc1           REG_LOWER_FRAME_WRITE
#define REG_LOWERING_put_loc2           REG_LOWER_FRAME_WRITE
#define REG_LOWERING_put_loc3           REG_LOWER_FRAME_WRITE
#define REG_LOWERING_set_loc0           REG_LOWER_FRAME_WRITE
#define REG_LOWERING_set_loc1           REG_LOWER_FRAME_WRITE
#define REG_LOWERING_set_loc2           REG_LOWER_FRAME_WRITE
#define REG_LOWERING_set_loc3           REG_LOWER_FRAME_WRITE
#define REG_LOWERING_get_arg0           REG_LOWER_FRAME_READ
#define REG_LOWERING_get_arg1           REG_LOWER_FRAME_READ
#define REG_LOWERING_get_arg2           REG_LOWER_FRAME_READ
#define REG_LOWERING_get_arg3           REG_LOWER_FRAME_READ
#define REG_LOWERING_put_arg0           REG_LOWER_FRAME_WRITE
#define REG_LOWERING_reserved12         REG_LOWER_RESERVED
#define REG_LOWERING_reserved7          REG_LOWER_RESERVED
#define REG_LOWERING_reserved6          REG_LOWER_RESERVED
#define REG_LOWERING_set_arg0           REG_LOWER_FRAME_WRITE
#define REG_LOWERING_reserved1          REG_LOWER_RESERVED
#define REG_LOWERING_reserved2          REG_LOWER_RESERVED
#define REG_LOWERING_put_el_recv        REG_LOWER_ELEMENT
#define REG_LOWERING_get_var_ref0       REG_LOWER_FRAME_READ
#define REG_LOWERING_get_var_ref1       REG_LOWER_FRAME_READ
#define REG_LOWERING_get_var_ref2       REG_LOWER_FRAME_READ
#define REG_LOWERING_get_var_ref3       REG_LOWER_FRAME_READ
#define REG_LOWERING_put_var_ref0       REG_LOWER_FRAME_WRITE
#define REG_LOWERING_get_arg_el         REG_LOWER_ELEMENT
#define REG_LOWERING_apply_arguments    REG_LOWER_CALL
#define REG_LOWERING_get_arg_field_nr   REG_LOWER_PROPERTY
#define REG_LOWERING_set_var_ref0       REG_LOWER_FRAME_WRITE
#define REG_LOWERING_get_loc8_el_recv   REG_LOWER_ELEMENT
#define REG_LOWERING_get_arg8_el_recv   REG_LOWER_ELEMENT
#define REG_LOWERING_get_arg8_array_el  REG_LOWER_ELEMENT
#define REG_LOWERING_get_length         REG_LOWER_PROPERTY

#define REG_LOWERING_if_false8          REG_LOWER_BRANCH
#define REG_LOWERING_if_true8           REG_LOWER_BRANCH
#define REG_LOWERING_goto8              REG_LOWER_BRANCH
#define REG_LOWERING_goto16             REG_LOWER_BRANCH

#define REG_LOWERING_call0              REG_LOWER_CALL
#define REG_LOWERING_call1              REG_LOWER_CALL
#define REG_LOWERING_call2              REG_LOWER_CALL
#define REG_LOWERING_call3              REG_LOWER_CALL

#define REG_LOWERING_is_undefined       REG_LOWER_UNARY
#define REG_LOWERING_is_null            REG_LOWER_UNARY
#define REG_LOWERING_typeof_is_undefined REG_LOWER_UNARY
#define REG_LOWERING_typeof_is_function REG_LOWER_UNARY

/*
 * --- the register ISA ---------------------------------------------------
 *
 * The instruction set the backend emits. Operands are u8 registers by default;
 * a function whose allocator needs more than 255 sets the wide form once in its
 * header, so no handler decodes an operand width per operand. There is no
 * stack-shaping instruction: a REG_LOWER_ELIDED operation has no register form
 * because lowering removes it. Every instruction declares its operands
 * explicitly -- a Call names destination, callee, receiver, argument-window
 * start and count; a property instruction names destination, receiver, key and
 * IC site -- rather than leaving them implicit in a stack.
 */
typedef enum JSRegOpcode {
    ROP_invalid = 0,

    /* constants and moves */
    ROP_load_const, ROP_private_symbol, ROP_load_i8, ROP_load_i32, ROP_load_undefined,
    ROP_load_null, ROP_load_true, ROP_load_false, ROP_load_this,
    ROP_load_bigint_i32, ROP_new_object, ROP_object_template,
    ROP_move,

    /*
     * bindings: locals, arguments, closure cells, globals, and the lexical
     * checks that guard a slot before it is read or written. A check is its own
     * instruction rather than a flag on the read or write, so the read and write
     * forms stay the ones the emitter already had.
     */
    ROP_get_local, ROP_put_local, ROP_get_arg, ROP_put_arg,
    ROP_get_var_ref, ROP_put_var_ref, ROP_get_global, ROP_put_global,
    ROP_local_uninit, ROP_local_check, ROP_local_check_init,
    ROP_var_ref_check, ROP_var_ref_check_init,
    ROP_check_define, ROP_define_global, ROP_define_global_func,
    ROP_close_local,

    /* closure creation */
    ROP_make_closure,
    ROP_rest,

    /* arithmetic, logic and comparison */
    ROP_unary, ROP_binary, ROP_binary_imm, ROP_to_number, ROP_compare,

    /* named, private, super and indexed properties */
    ROP_get_field, ROP_get_field2, ROP_put_field, ROP_define_field,
    ROP_object_fill, ROP_get_private, ROP_put_private, ROP_get_element,
    ROP_put_element, ROP_define_element, ROP_get_length, ROP_super_get,
    ROP_super_put,

    /*
     * classes: a method definition, the home object, the prototype link and the
     * constructor checks a derived constructor runs before and after super()
     */
    ROP_define_class, ROP_define_class_computed,
    ROP_define_method, ROP_define_method_computed, ROP_set_proto,
    ROP_set_home_object, ROP_get_super, ROP_check_ctor, ROP_check_ctor_return,
    ROP_init_ctor, ROP_check_brand, ROP_add_brand, ROP_define_private,

    /* control flow */
    ROP_branch, ROP_branch_if, ROP_branch_if_not,
    ROP_return, ROP_return_undefined,
    ROP_initial_yield, ROP_return_async, ROP_yield_star,

    /* calls, methods, constructors and tail calls */
    ROP_call, ROP_call_method, ROP_construct, ROP_tail_call,
    ROP_tail_call_method, ROP_array_from, ROP_apply, ROP_apply_arguments, ROP_append, ROP_eval, ROP_apply_eval,
    ROP_with_get, ROP_with_put, ROP_with_delete, ROP_with_make_ref,
    ROP_with_get_ref, ROP_with_get_ref_undef,
    ROP_make_local_ref, ROP_make_var_ref,
    ROP_get_ref_value, ROP_put_ref_value, ROP_put_super_value,

    /* exceptions and finally continuations */
    ROP_throw, ROP_throw_error, ROP_catch_enter, ROP_nip_catch,
    ROP_finally_enter, ROP_finally_leave,

    /* iterators and suspension */
    ROP_for_in_start, ROP_for_in_next, ROP_get_arg_el,
    ROP_iter_start, ROP_iter_next, ROP_iter_close, ROP_sentinel_drop, ROP_yield, ROP_await,
    ROP_iterator_next, ROP_for_await_of_start, ROP_iter_value_done,
    ROP_check_object, ROP_iterator_call,

    /* scope, eval and the engine-control remainder */
    ROP_delete_var, ROP_get_ref, ROP_put_ref, ROP_to_object, ROP_to_propkey, ROP_to_propkey2, ROP_make_ref,
    ROP_delete, ROP_typeof, ROP_special, ROP_special_object, ROP_set_name,
    ROP_set_name_computed, ROP_regexp, ROP_copy_props,
    /* dynamic import: specifier and options in, a promise out */
    ROP_dynamic_import,

    /*
     * Explicit resource management (`using`, `await using`). The error state a
     * dispose scope carries is an ordinary value here, so each of these is the
     * stack VM's handler with its operands named instead of a stack pointer.
     */
    ROP_using_init, ROP_using_dispose, ROP_using_dispose_async,
    ROP_using_merge, ROP_using_end, ROP_using_check,

    /*
     * A named field read whose receiver is a frame slot, borrowed rather than
     * copied into the scratch register: the pair the fused local/argument
     * receiver stack opcodes lower to, in one instruction.
     */
    ROP_get_local_field, ROP_get_arg_field,

    /*
     * ROP_binary with one or both sources named as a local slot instead of a
     * register. The frame read that would have produced the operand is folded
     * into the operation, which borrows the slot directly: loc_reg has the lhs
     * in a slot, reg_loc the rhs, loc_loc both.
     */
    ROP_binary_loc_reg, ROP_binary_reg_loc, ROP_binary_loc_loc,

    /* A definition whose source is dead: the reference moves instead of being
       duplicated and retained. */
    ROP_move_xfer, ROP_put_local_xfer, ROP_put_arg_xfer,
    ROP_put_local_probe,

    /* ROP_get_local_field with the slot in the opcode, for the slots that
       dominate: the frame read names no operand at all. */
    ROP_get_local_field_0, ROP_get_local_field_1,
    ROP_get_local_field_2, ROP_get_local_field_3,

    /* Frame reads and writes with the slot in the opcode (0-3 cover 55% of
       frame accesses): the slot operand is not in the stream at all. */
    ROP_get_local_s0, ROP_get_local_s1, ROP_get_local_s2, ROP_get_local_s3,
    ROP_put_local_s0, ROP_put_local_s1, ROP_put_local_s2, ROP_put_local_s3,
    ROP_put_local_xfer_s0, ROP_put_local_xfer_s1, ROP_put_local_xfer_s2, ROP_put_local_xfer_s3,

    /*
     * A local read-modify-write in one instruction: `slot = slot <subop> imm`,
     * with the same sub-operation space and the same semantics as ROP_binary_imm
     * but the destination is a frame slot, so no register is needed for the
     * intermediate. The stack VM has this as one opcode (`inc_loc`, `add_loc`);
     * lowering it as load / op / store costs three dispatches (four when the
     * constant is a separate push), which is most of a loop's instruction count.
     *
     * Appended at the end so no existing opcode is renumbered.
     */
    ROP_local_rmw_imm,

    /*
     * A comparison (or any binary) that only a branch consumes, fused with that
     * branch: the boolean never needs a register and the dispatch that carried
     * it goes away. One form per operand pair, as ROP_binary_* has. Bit
     * REG_CMP_INVERT in the sub-op is the branch sense (`if_false` is the
     * comparison inverted), which costs no operand of its own.
     */
    ROP_branch_cmp_loc_loc, ROP_branch_cmp_loc_reg,
    ROP_branch_cmp_reg_loc, ROP_branch_cmp_reg_reg,

    /* The loop's trailing read-modify-write and the unconditional branch that
       follows it: a counted loop's back edge and its update in one instruction. */
    ROP_local_rmw_branch,

    /*
     * A store whose value operand is dead: the reference moves into the
     * container instead of being duplicated and retained, exactly as
     * ROP_put_local_xfer moves it into a binding. Appended at the end so no
     * existing opcode is renumbered.
     */
    ROP_put_element_xfer, ROP_put_field_xfer,

    /*
     * ROP_binary with its right operand named as a constant-pool entry or an
     * atom rather than a register. The constant is borrowed -- a pool value is
     * owned by the function bytecode and an atom value is materialised for the
     * operation -- so no register holds it and no reference is duplicated, which
     * is the case a string literal in an arithmetic loop is. Appended at the end
     * so no existing opcode is renumbered.
     */
    ROP_binary_cpool,

    /*
     * The narrow encoding of three hot instructions: the same operations, with
     * one-byte register operands and a one-byte relative branch target, and for
     * the immediate form a one-byte immediate. A register program is a mix of
     * the two encodings -- the encoder picks per instruction and falls back to
     * the wide form when an operand does not fit -- so width is a property of
     * the opcode, never of the function.
     *
     * These are appended, so every existing opcode keeps its number, its format
     * and its width: a program serialized before them still parses, and no
     * format version changes. Operands are the wide forms' operands, one byte
     * each.
     */
    ROP_branch_cmp_reg_reg_s, ROP_binary_imm_s, ROP_branch_s,

    /*
     * The same two operations with the operation kind moved from an operand
     * into the opcode. `ROP_branch_cmp_reg_reg_s` and `ROP_binary_imm_s` carry a
     * `sub` operand their handler switches on at run time -- a second dispatch,
     * on top of the one that selected the instruction. One opcode per kind
     * makes that switch a constant the compiler folds into the handler.
     *
     * Each ordered comparison needs a `n` opcode for its inverted sense, because
     * the sense cannot be folded into the comparison: IEEE comparisons are not
     * complement-closed, and `!(a > b)` is *true* for NaN while `a <= b` is
     * false. The inverted opcodes carry REG_CMP_INVERT in their constant sub-op,
     * so the handler inverts the comparison's result exactly as the
     * operand-carrying form does. Equality is complement-closed (`!(a == b)` is
     * `a != b` for every value), so eq and neq serve both senses. Operands are
     * two registers and a relative target, one byte each.
     */
    ROP_branch_lt_s, ROP_branch_nlt_s, ROP_branch_lte_s, ROP_branch_nlte_s,
    ROP_branch_gt_s, ROP_branch_ngt_s, ROP_branch_gte_s, ROP_branch_ngte_s,
    ROP_branch_eq_s, ROP_branch_neq_s,

    /* The immediate arithmetic that loops use, with no kind operand. */
    ROP_add_imm_s, ROP_sub_imm_s,

    /*
     * The same, updating the source register in place -- the encoder emits
     * these only when the destination and the source are the same register, so
     * the operands are one register and the immediate.
     *
     * That equality is what lets the ownership check go: SET_REG has to test
     * whether the value it overwrites is refcounted and release it if so, but
     * here the overwritten value *is* the one the handler just loaded and
     * tag-tested as a JS_TAG_INT, and an integer has no reference to release.
     * The general path below still uses SET_REG, because a non-integer operand
     * is an owned value. Worth a register operand and a byte as well as the
     * test: the operands are read as one register and the immediate.
     */
    ROP_add_imm_self_s, ROP_sub_imm_self_s,

    /*
     * The in-place add with the block's trailing unconditional branch fused into
     * it: the loop counter's update and its back edge in one instruction, which
     * is one fewer dispatch per iteration and one fewer link in the chain from
     * an instruction's operands to the next instruction's address.
     *
     * The encoder only fuses when the add and the branch are adjacent in the
     * IR. That is the whole soundness condition: a phi copy for the edge is
     * emitted between them, so if one were needed the two would not be adjacent
     * and the fusion would not fire. Fusing across a copy would move the update
     * to after the copy and the phi would read the pre-update value -- the
     * reason an earlier attempt at this was reverted.
     */
    ROP_add_imm_self_branch, ROP_add_imm_self_branch_s,

    /*
     * One opcode per arithmetic operation, for the register-register form of
     * ROP_binary. The generic form carries its kind as an operand, so every
     * execution pays a *second* indirect branch -- a jump table on that operand
     * -- on top of the dispatch that selected the instruction, and the fast
     * paths it reaches cannot fold, because they switch on a value the compiler
     * does not know.
     *
     * Measured on box2d, per execution of the double multiply path: 60 native
     * instructions, of which the kind's jump table is 8 and the dispatch that
     * follows is 13. With the kind in the opcode the jump table is gone and the
     * remaining switches fold to the cases that can run.
     *
     * The set is sized by measurement, not symmetry: on box2d MUL, ADD and SUB
     * are 96.2% of all binary executions and NEG and INC are 93.0% of all
     * unary executions, while MOD, the shifts, XOR and POW never execute at
     * all (see .local/redesign/P1-ISA-AND-FAST-PATHS.md, section 1). The cold
     * kinds are appended anyway: they are the same shape, and leaving them out
     * would mean a second switch on the kind to decide which form to emit.
     *
     *
     * Appended at the end, so no existing opcode is renumbered, no operand
     * changes width or meaning, and REGVM_FORMAT_VERSION does not change: a
     * register program written before these opcodes still parses, and the
     * built-in blobs still contain the generic forms, which remain implemented.
     * Operands are the generic form's without the kind: destination, left,
     * right.
     */
    ROP_mul, ROP_add, ROP_sub, ROP_div, ROP_mod, ROP_pow,
    ROP_bitand, ROP_bitor, ROP_bitxor, ROP_shl, ROP_sar, ROP_shr,
    ROP_lt, ROP_lte, ROP_gt, ROP_gte, ROP_eq, ROP_neq,
    ROP_strict_eq, ROP_strict_neq, ROP_instanceof, ROP_in,
    ROP_neg, ROP_inc, ROP_dec, ROP_not, ROP_to_numeric,

    /*
     * D6 compact register forms: the same operations with every register
     * operand one byte wide. The D5 forms above remain available when a
     * physical register does not fit. Appended so no earlier opcode moves.
     */
    ROP_load_i8_s, ROP_move_s,

    /*
     * X3 quickened own-read forms. These are never emitted by the encoder:
     * the threaded interpreter rewrites a site's opcode byte to one of these
     * once its property cache is filled as an own data slot, and back to the
     * generic form when the site is refilled as a prototype or goes dead.
     * Appended at the very end so every existing opcode keeps its number and
     * the serialized bytecode format is unchanged.
     */
    ROP_get_field_own, ROP_get_field2_own,

    /*
     * X3b fused chain read. A `get_field`/`get_field2` whose receiver is the
     * immediately preceding `get_field`'s destination is emitted as this one
     * instruction: both reads and both destinations, one dispatch. Like the
     * own forms above it is appended at the very end, so no existing opcode
     * number moves and the serialized bytecode format is unchanged. The `_own`
     * spelling is produced only by the threaded interpreter's quickening
     * rewrite; both are aliased to the generic monolith label.
     */
    ROP_get_field_chain, ROP_get_field_chain_own,

    /*
     * X6 fused three-operand arithmetic. A `mul` immediately followed by an
     * `add`/`sub` that consumes the product as its left operand becomes one
     * instruction with five register operands -- the product's own destination
     * first, then the add's destination and its right operand. Only the left
     * operand is fused, because `c + p` and `c - p` are not the same operations
     * and the operand order of a JavaScript `+` is observable. Appended at the
     * end like the other fusions, so no existing opcode number moves.
     */
    ROP_mul_add, ROP_mul_sub,
    /* X6: the mirror image, the product as the right operand. */
    ROP_add_mul, ROP_sub_mul,

    /*
     * The debugger trap: the stack VM's `OP_esc1` debug sub-operations in
     * register form, one instruction per trapped statement. The operand is the
     * `JS_DEBUG_TRACE_*` flags byte; the handler captures the ip before calling
     * the embedder's hook, so a backtrace taken inside the hook names the
     * statement. It is a cold opcode and never emitted into a light-safe
     * function (a frame the debugger stops in keeps its record). Appended at
     * the very end so no existing opcode is renumbered.
     */
    ROP_debug,

    /*
     * The counted loop's increment and its latch test in one instruction: the
     * rotated loop ends in `inc r ; branch_lt r, bound, L_body`, two
     * dispatches, and this is the pair fused. `r` is incremented in place
     * (the encoder checks that the inc's destination and source are the same
     * physical register and that the branch reads it), then `r < bound`
     * selects the back edge; the taken arm carries the same back-edge
     * interrupt poll every other backward branch does. Anything the int fast
     * path cannot do -- a float or BigInt counter, `INT32_MAX`, a
     * non-numeric bound -- tail-calls a slow half that performs the two
     * operations in order. Appended at the very end so no existing opcode is
     * renumbered.
     */
    ROP_inc_branch_lt, ROP_inc_branch_lt_s,

    /*
     * The baseline (frame-slot) tier's full-width stack access. Its operand
     * stack can be deeper than the 16-bit register operand can name (4,096),
     * so a value above that range is spilt to a slot at the top of the frame's
     * register file and reached by an unscaled 16-bit slot index. slot_get
     * reads that slot (u) into a low register (r); slot_put writes the register
     * (r) back to the slot (u). Both borrow/copy exactly as ROP_move does, so
     * the reference-count model is unchanged. They index `regs`, not `var_buf`,
     * so no frame-layout change is needed. Appended at the very end so no
     * existing opcode is renumbered.
     */
    ROP_slot_get, ROP_slot_put,

    ROP_COUNT
} JSRegOpcode;

/*
 * --- register instruction encoding --------------------------------------
 *
 * A register instruction is a one-byte opcode followed by its operands, in the
 * order the opcode's format names them. A register operand is one byte, or two
 * when the function's register count exceeds 255 and its header selects the
 * wide form; the width belongs to the function, so no handler branches on it
 * per operand. Every other operand is little-endian and fixed width. A branch
 * target is a signed byte offset from the end of the branch instruction to an
 * instruction boundary, and the reader rejects an offset that lands inside an
 * instruction.
 *
 * A format names one character per operand:
 *
 *   r  a physical register (u8, or u16 in the wide form)
 *   i  signed 8-bit immediate
 *   I  signed 32-bit immediate
 *   u  unsigned 16-bit slot index
 *   b  unsigned 8-bit field
 *   c  unsigned 32-bit constant-pool index
 *   a  unsigned 32-bit atom index
 *   s  unsigned 16-bit inline-cache site index
 *   t  signed 32-bit branch target
 *
 * The per-opcode format table lives with the encoder in quickjs.c; a register
 * opcode with no format fails the encoder's completeness check, the same way
 * an unclassified stack opcode fails the lowering inventory.
 */
typedef enum JSRegOperandKind {
    REG_OPK_END = 0,
    REG_OPK_REG,
    REG_OPK_I8,
    REG_OPK_I32,
    REG_OPK_U16,
    REG_OPK_U8,
    REG_OPK_CONST,
    REG_OPK_ATOM,
    REG_OPK_IC,
    REG_OPK_TARGET,
} JSRegOperandKind;

/* ROP_load_const's kind field: where the named index points. */
enum { REG_CONST_CPOOL = 0, REG_CONST_ATOM };

/*
 * The register bytecode's own format version. A register program is decoded
 * with the format table, so a blob written by a build with a different
 * instruction set would decode into *something* -- possibly valid-looking and
 * wrong. The version is written once per function record and checked before a
 * single instruction is parsed, so a stale blob fails cleanly the way a
 * cross-VM one does. Bump it whenever an opcode is added, removed or
 * renumbered, or an operand's width or meaning changes.
 */
#define REGVM_FORMAT_VERSION 19

/* ROP_get_field and ROP_put_field use this site index when the source
   instruction has no inline cache. */
#define REG_IC_NONE 0xffffu

/* Sub-operation field of ROP_unary. */
typedef enum JSRegUnaryOp {
    REG_UN_NEG = 0, REG_UN_PLUS, REG_UN_NOT, REG_UN_LNOT, REG_UN_TYPEOF,
    REG_UN_IS_UNDEFINED, REG_UN_IS_NULL, REG_UN_TYPEOF_IS_UNDEFINED,
    REG_UN_TYPEOF_IS_FUNCTION, REG_UN_IS_UNDEFINED_OR_NULL,
    REG_UN_INC, REG_UN_DEC,

/* Bit set in ROP_local_rmw_imm's sub-op when it names the unary space (a numeric
   increment/decrement) rather than the binary space (a generic operation). */
#define REG_RMW_UNARY 0x80

/* Bit set in a ROP_branch_cmp_* sub-op when the branch is taken on the negated
   result, which is how `if_false` differs from `if_true`. */
#define REG_CMP_INVERT 0x40
    /*
     * ToNumeric, which is what the postfix forms publish as the value the
     * expression evaluates to: `var y = "1"` then `x++` leaves y as the number
     * 1, not the string. Not the same as unary plus, which rejects a BigInt.
     */
    REG_UN_TO_NUMERIC,
    /* the number of sub-operations; see REG_BIN_COUNT */
    REG_UN_COUNT,
} JSRegUnaryOp;

/* Sub-operation field of ROP_binary, ROP_binary_imm and ROP_compare. The order
   matches the engine's opcode order for the same operations (OP_mul .. OP_pow,
   OP_shl .. OP_or, OP_lt .. OP_gte), so the interpreter can map a sub-operation
   to the engine opcode the slow paths take by an offset. */
typedef enum JSRegBinaryOp {
    REG_BIN_MUL = 0, REG_BIN_DIV, REG_BIN_MOD, REG_BIN_ADD, REG_BIN_SUB,
    REG_BIN_SHL, REG_BIN_SAR, REG_BIN_SHR, REG_BIN_AND, REG_BIN_XOR,
    REG_BIN_OR, REG_BIN_POW,
    REG_BIN_LT, REG_BIN_LTE, REG_BIN_GT, REG_BIN_GTE,
    REG_BIN_INSTANCEOF, REG_BIN_IN,
    REG_BIN_EQ, REG_BIN_NEQ, REG_BIN_STRICT_EQ, REG_BIN_STRICT_NEQ,
    REG_BIN_PRIVATE_IN,
    /*
     * The number of sub-operations, for the arrays that are indexed by one.
     * Appended after every real member, so no sub-operation changes value and
     * nothing serialized is affected: a `sub` operand is a byte in the stream
     * and these are compile-time only.
     */
    REG_BIN_COUNT,
} JSRegBinaryOp;

/*
 * Pseudo sub-operations of ROP_branch_cmp_*. A fused strict compare against a
 * compile-time-undefined operand carries no pair of values to compare: the
 * operand that was loaded with undefined is gone, and the remaining operand's
 * tag is the whole test. These name that test directly. Both sit below
 * REG_CMP_INVERT (0x40) and above every real REG_BIN_* member, so no real
 * sub-operation changes value and the sub byte stays a single comparison.
 * They are produced only by the branch fusion (regvm_fuse_branches), never by
 * the bytecode lowering.
 */
#define REG_BIN_IS_UNDEF     0x3e
#define REG_BIN_IS_NOT_UNDEF 0x3f

/*
 * --- the register frame -------------------------------------------------
 *
 * What the register backend allocates per activation. The frame is
 * [arguments][locals][registers]: the first two are the addressable slots a
 * closure or eval can reach, the third is the allocator's physical registers,
 * which are not addressable and only ever hold live owned values. A temporary
 * register that was never defined is never initialized, marked or released, so
 * the frame costs its register count and not a zero-fill of the whole window.
 */
typedef struct JSRegFrameInfo {
    uint16_t arg_count;
    uint16_t var_count;
    uint16_t reg_count;      /* physical registers the allocator assigned */
    uint16_t captured_count; /* locals and arguments needing stable cells */
    uint32_t frame_registers; /* arg_count + var_count + reg_count */
} JSRegFrameInfo;

/* A protected region of register bytecode. The exception stage fills these in;
   the reader validates them. */
typedef struct JSRegExceptionEntry {
    uint32_t start_pc;
    uint32_t end_pc;
    uint32_t handler_pc;
    uint16_t exception_reg;
    uint16_t cleanup_map_id;
    uint8_t kind;
} JSRegExceptionEntry;

/*
 * One entry of the register frame's control stack. It replaces the stack VM's
 * catch-offset sentinel: a catch handler to run on an exception, a finally
 * continuation for gosub/ret, or an open for-of whose iterator is closed when
 * an exception leaves the frame. They share one stack so their order is the
 * push order, which is what decides which one an exception meets first.
 */
typedef struct JSRegCtl {
    uint32_t pc;    /* handler pc, or the continuation to return to */
    int32_t reg;    /* exception destination register, for a catch */
    uint8_t kind;   /* REG_CTL_* */
    JSValue iter;   /* for REG_CTL_ITER */
    JSValue next;
} JSRegCtl;

enum { REG_CTL_CATCH = 0, REG_CTL_GOSUB = 1, REG_CTL_ITER = 2 };

#endif /* QUICKJS_REGVM_H */
