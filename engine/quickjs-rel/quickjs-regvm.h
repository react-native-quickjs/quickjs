/*
 * Register VM: the semantic lowering inventory and the register ISA families.
 *
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * Included by quickjs.c, under QJS_REGVM only. Every DEF in quickjs-opcode.h
 * has a REG_LOWERING_<id> line here, and quickjs.c builds regvm_op_kind[] by
 * including quickjs-opcode.h with DEF redefined to emit REG_LOWERING_ ## id, so
 * an opcode without a line is an undeclared identifier and fails the build.
 * n_pop, n_push and fmt describe stack layout and are never used to infer that
 * an opcode is supported.
 *
 * The register ISA is numbered separately from the stack opcodes. A lowering
 * kind names an operation's semantic family; a family names the register
 * instruction shape that executes it. Stack shaping (dup, perm, rot, ...) is
 * REG_LOWER_ELIDED: lowering removes it.
 */

#ifndef QUICKJS_REGVM_H
#define QUICKJS_REGVM_H

/* Ownership carried by a virtual value. Only owned values are released. */
typedef enum JSRegValueOwnership {
    REG_VALUE_PRIMITIVE = 0,
    REG_VALUE_BORROWED,
    REG_VALUE_OWNED,
} JSRegValueOwnership;

/* What an emitted stack opcode lowers to. UNSET, left by a completeness
   failure, is never valid. */
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

/* Register ISA families: a family is a shape of register instruction. */
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
 * One line per opcode in quickjs-opcode.h, grouped as the opcode list groups
 * them.
 */

/* escape prefix: a 2-byte prefix whose subop refines it. Subop 0/1 are the
   debugger traps (elided); subop 2 is the fused get_loc0 get_loc1 (two frame
   reads). The table entry is the conservative family. */
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
 * Same stack effects as their long forms, so the same lowering kind.
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
 * The instruction set the backend emits. There is no stack-shaping
 * instruction. Every instruction declares its operands explicitly: a Call
 * names destination, callee, receiver, argument-window start and count; a
 * property instruction names destination, receiver, key and IC site.
 */
typedef enum JSRegOpcode {
    ROP_invalid = 0,

    /* constants and moves */
    ROP_load_const, ROP_private_symbol, ROP_load_i8, ROP_load_i32, ROP_load_undefined,
    ROP_load_null, ROP_load_true, ROP_load_false, ROP_load_this,
    ROP_load_bigint_i32, ROP_new_object, ROP_object_template,
    ROP_move,

    /* bindings: locals, arguments, closure cells, globals, and the lexical
       checks that guard a slot. A check is its own instruction. */
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

    /* classes: method definition, home object, prototype link, and the derived
       constructor checks around super() */
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

    /* Explicit resource management (`using`, `await using`): the stack VM's
       handlers, with the dispose scope's error state as a named operand. */
    ROP_using_init, ROP_using_dispose, ROP_using_dispose_async,
    ROP_using_merge, ROP_using_end, ROP_using_check,

    /* A named field read whose receiver is a borrowed frame slot. */
    ROP_get_local_field, ROP_get_arg_field,

    /* ROP_binary with one or both sources a borrowed local slot: loc_reg has
       the lhs in a slot, reg_loc the rhs, loc_loc both. */
    ROP_binary_loc_reg, ROP_binary_reg_loc, ROP_binary_loc_loc,

    /* A definition whose source is dead: the reference moves instead of being
       duplicated and retained. */
    ROP_move_xfer, ROP_put_local_xfer, ROP_put_arg_xfer,
    ROP_put_local_probe,

    /* ROP_get_local_field with the slot in the opcode. */
    ROP_get_local_field_0, ROP_get_local_field_1,
    ROP_get_local_field_2, ROP_get_local_field_3,

    /* Frame reads and writes with the slot in the opcode. */
    ROP_get_local_s0, ROP_get_local_s1, ROP_get_local_s2, ROP_get_local_s3,
    ROP_put_local_s0, ROP_put_local_s1, ROP_put_local_s2, ROP_put_local_s3,
    ROP_put_local_xfer_s0, ROP_put_local_xfer_s1, ROP_put_local_xfer_s2, ROP_put_local_xfer_s3,

    /* A local read-modify-write in one instruction: `slot = slot <subop> imm`,
       with ROP_binary_imm's sub-operations, writing the frame slot directly. */
    ROP_local_rmw_imm,

    /* A binary that only a branch consumes, fused with it. One form per operand
       pair. REG_CMP_INVERT in the sub-op is the branch sense. */
    ROP_branch_cmp_loc_loc, ROP_branch_cmp_loc_reg,
    ROP_branch_cmp_reg_loc, ROP_branch_cmp_reg_reg,

    /* A loop's trailing read-modify-write and its unconditional back edge. */
    ROP_local_rmw_branch,

    /* A store whose value operand is dead: the reference moves into the
       container, as ROP_put_local_xfer moves it into a binding. */
    ROP_put_element_xfer, ROP_put_field_xfer,

    /* ROP_binary with its right operand a borrowed constant-pool entry or atom
       rather than a register. */
    ROP_binary_cpool,

    /* The narrow encoding of three hot instructions: one-byte register
       operands, one-byte relative branch target, and a one-byte immediate. The
       encoder picks per instruction, so width is a property of the opcode. */
    ROP_branch_cmp_reg_reg_s, ROP_binary_imm_s, ROP_branch_s,

    /* `ROP_branch_cmp_reg_reg_s` and `ROP_binary_imm_s` with the kind in the
       opcode, so the handler's switch folds. Ordered comparisons need an `n`
       opcode for the inverted sense (`!(a > b)` is true for NaN while `a <= b`
       is false), which carries REG_CMP_INVERT in its constant sub-op; eq and
       neq serve both senses. Operands: two registers and a relative target, one
       byte each. */
    ROP_branch_lt_s, ROP_branch_nlt_s, ROP_branch_lte_s, ROP_branch_nlte_s,
    ROP_branch_gt_s, ROP_branch_ngt_s, ROP_branch_gte_s, ROP_branch_ngte_s,
    ROP_branch_eq_s, ROP_branch_neq_s,

    /* The immediate arithmetic that loops use, with no kind operand. */
    ROP_add_imm_s, ROP_sub_imm_s,

    /* The same, updating the source register in place (dst == src). The
       overwritten value is the one just tag-tested as a JS_TAG_INT, so no
       ownership check is needed. Operands: one register and the immediate. */
    ROP_add_imm_self_s, ROP_sub_imm_self_s,

    /* The in-place add fused with the block's trailing unconditional branch,
       emitted only when the two are adjacent in the IR (see
       regvm_fuse_self_branch). */
    ROP_add_imm_self_branch, ROP_add_imm_self_branch_s,

    /* One opcode per operation for the register-register form of ROP_binary, so
       the kind is not a second indirect branch and the fast paths fold.
       Operands: destination, left, right. */
    ROP_mul, ROP_add, ROP_sub, ROP_div, ROP_mod, ROP_pow,
    ROP_bitand, ROP_bitor, ROP_bitxor, ROP_shl, ROP_sar, ROP_shr,
    ROP_lt, ROP_lte, ROP_gt, ROP_gte, ROP_eq, ROP_neq,
    ROP_strict_eq, ROP_strict_neq, ROP_instanceof, ROP_in,
    ROP_neg, ROP_inc, ROP_dec, ROP_not, ROP_to_numeric,

    /* Compact register forms: the same operations with every register operand
       one byte wide, used when every physical register fits. */
    ROP_load_i8_s, ROP_move_s,

    /* Quickened own-read forms, never emitted by the encoder: the threaded
       interpreter rewrites a site's opcode to one of these when its cache holds
       an own data slot, and back when it does not. */
    ROP_get_field_own, ROP_get_field2_own,

    /* Fused chain read: both reads and both destinations, one dispatch. The
       `_own` spelling is produced only by the threaded interpreter's
       quickening; both alias the generic monolith label. */
    ROP_get_field_chain, ROP_get_field_chain_own,

    /* Fused arithmetic: a `mul` followed by an `add`/`sub` consuming the
       product as its left operand. Operands: the product's destination, x, y,
       the add's destination, its other operand. */
    ROP_mul_add, ROP_mul_sub,
    /* the product as the right operand */
    ROP_add_mul, ROP_sub_mul,

    /* The debugger trap: the stack VM's `OP_esc1` debug sub-operations, one
       instruction per trapped statement, operand the `JS_DEBUG_TRACE_*` flags.
       The handler captures the ip before calling the hook. Never emitted into a
       light-safe function. */
    ROP_debug,

    /* The counted loop's increment and latch test in one instruction: `r` is
       incremented in place, then `r < bound` selects the back edge, with the
       back-edge interrupt poll. Anything the int fast path cannot do goes to a
       slow half that runs the two operations in order. */
    ROP_inc_branch_lt, ROP_inc_branch_lt_s,

    /* The baseline tier's full-width stack access. Its operand stack can be
       deeper than a 16-bit register operand can name, so deep values are spilt
       to slots at the top of the register file. slot_get reads slot (u) into
       register (r); slot_put writes (r) back to (u). Both behave as ROP_move.
       */
    ROP_slot_get, ROP_slot_put,

    ROP_get_this_field, ROP_get_this_field2,

    ROP_branch_null, ROP_branch_not_null,
    ROP_branch_nullish, ROP_branch_not_nullish,

    ROP_COUNT
} JSRegOpcode;

/*
 * --- register instruction encoding --------------------------------------
 *
 * A one-byte opcode followed by its operands, in the order the opcode's format
 * names them, little-endian and fixed width. A branch target is a signed byte
 * offset from the end of the branch instruction to an instruction boundary.
 *
 * A format names one character per operand:
 *
 *   r  a physical register (u16; u8 in the narrow forms)
 *   i  signed 8-bit immediate
 *   I  signed 32-bit immediate
 *   u  unsigned 16-bit slot index
 *   b  unsigned 8-bit field
 *   c  unsigned 32-bit constant-pool index
 *   a  unsigned 32-bit atom index
 *   s  unsigned 16-bit inline-cache site index
 *   t  signed 32-bit branch target (8-bit in the narrow forms)
 *
 * The per-opcode format table lives with the encoder in quickjs.c.
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

/* The register bytecode's format version, checked per function record before
   any instruction is parsed. Bump it whenever an opcode is added, removed or
   renumbered, or an operand's width or meaning changes. */
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
    /* ToNumeric, the value a postfix form evaluates to (`x++` on "1" gives 1).
       Unlike unary plus it accepts a BigInt. */
    REG_UN_TO_NUMERIC,
    /* the number of sub-operations; see REG_BIN_COUNT */
    REG_UN_COUNT,
} JSRegUnaryOp;

/* Sub-operation field of ROP_binary, ROP_binary_imm and ROP_compare, in the
   engine's opcode order (OP_mul .. OP_pow, OP_shl .. OP_or, OP_lt .. OP_gte)
   so a sub-operation maps to its engine opcode by an offset. */
typedef enum JSRegBinaryOp {
    REG_BIN_MUL = 0, REG_BIN_DIV, REG_BIN_MOD, REG_BIN_ADD, REG_BIN_SUB,
    REG_BIN_SHL, REG_BIN_SAR, REG_BIN_SHR, REG_BIN_AND, REG_BIN_XOR,
    REG_BIN_OR, REG_BIN_POW,
    REG_BIN_LT, REG_BIN_LTE, REG_BIN_GT, REG_BIN_GTE,
    REG_BIN_INSTANCEOF, REG_BIN_IN,
    REG_BIN_EQ, REG_BIN_NEQ, REG_BIN_STRICT_EQ, REG_BIN_STRICT_NEQ,
    REG_BIN_PRIVATE_IN,
    /* The number of sub-operations, for arrays indexed by one. Compile-time
       only. */
    REG_BIN_COUNT,
} JSRegBinaryOp;

/* Pseudo sub-operations of ROP_branch_cmp_*: a fused strict compare against a
   just-loaded undefined, where the remaining operand's tag is the whole test.
   They sit between the real REG_BIN_* members and REG_CMP_INVERT (0x40), and
   only regvm_fuse_branches produces them. */
#define REG_BIN_IS_UNDEF     0x3e
#define REG_BIN_IS_NOT_UNDEF 0x3f

/*
 * --- the register frame -------------------------------------------------
 *
 * [arguments][locals][registers]: the first two are the slots a closure or
 * eval can reach; the registers are the allocator's and only hold live values.
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

/* One entry of the register frame's control stack: a catch handler, a finally
   continuation for gosub/ret, or an open for-of to close when an exception
   leaves the frame. One stack, so an exception meets them in push order. */
typedef struct JSRegCtl {
    uint32_t pc;    /* handler pc, or the continuation to return to */
    int32_t reg;    /* exception destination register, for a catch */
    uint8_t kind;   /* REG_CTL_* */
    JSValue iter;   /* for REG_CTL_ITER */
    JSValue next;
} JSRegCtl;

enum { REG_CTL_CATCH = 0, REG_CTL_GOSUB = 1, REG_CTL_ITER = 2 };

#endif /* QUICKJS_REGVM_H */
