/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: MIT
 */

/*
 * ptx.h: libnvptx, the PTX front end of native CUDA (CloudBSD fork).
 *
 * libnvptx reads a PTX module into a syntax tree.  A later slice of
 * Track #2426 turns that tree into NIR for Mesa's NAK compiler; this
 * header is the whole public surface of slice 1: the tree, the parser and
 * the printer.
 */

#ifndef PTX_H
#define PTX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The state spaces a declaration can live in. */
enum ptx_space {
   PTX_SPACE_REG,
   PTX_SPACE_SREG,
   PTX_SPACE_PARAM,
   PTX_SPACE_LOCAL,
   PTX_SPACE_SHARED,
   PTX_SPACE_GLOBAL,
   PTX_SPACE_CONST,
};

/* The base types of a declaration. */
enum ptx_type {
   PTX_TYPE_NONE,
   PTX_TYPE_PRED,
   PTX_TYPE_B8,
   PTX_TYPE_B16,
   PTX_TYPE_B32,
   PTX_TYPE_B64,
   PTX_TYPE_B128,
   PTX_TYPE_U8,
   PTX_TYPE_U16,
   PTX_TYPE_U32,
   PTX_TYPE_U64,
   PTX_TYPE_S8,
   PTX_TYPE_S16,
   PTX_TYPE_S32,
   PTX_TYPE_S64,
   PTX_TYPE_F16,
   PTX_TYPE_F16X2,
   PTX_TYPE_BF16,
   PTX_TYPE_BF16X2,
   PTX_TYPE_F32,
   PTX_TYPE_F64,
};

/* The name of a type, without the leading dot ("b32"). */
const char *
ptx_type_name(enum ptx_type type);

/* The width of a type in bits (0 for PTX_TYPE_NONE). */
unsigned
ptx_type_bits(enum ptx_type type);

/* The linkage of a function or a declaration. */
enum ptx_linkage {
   PTX_LINKAGE_NONE,
   PTX_LINKAGE_VISIBLE,
   PTX_LINKAGE_EXTERN,
   PTX_LINKAGE_WEAK,
};

/*
 * One declaration: a register, a parameter or a piece of storage.
 *
 * A register is declared as ".reg .b32 %r<9>": the name is "%r" and the
 * parameterized count is 9.  Storage is declared as ".shared .align 4 .b8
 * name[1024]": the name is "name" and the array element count is 1024.
 *
 * An array may have no size of its own: ".extern .shared .align 16 .b8
 * smem[]" is the dynamic shared memory of CUDA, whose size is set at launch
 * or at link time.  Such an array carries the unsized flag and a count of 0.
 */
struct ptx_decl {
   enum ptx_linkage linkage;
   enum ptx_space space;
   enum ptx_type type;
   unsigned vec;         /* 1, 2 or 4, from .v2/.v4 */
   unsigned align;       /* 0 when the declaration has no .align */
   const char *name;
   unsigned count;       /* elements of an array, 0 when not an array */
   bool unsized;         /* the array's size is set at launch or link time */
   unsigned param_count; /* the <N> of %r<N>, 0 when not parameterized */
   bool is_ptr;          /* the .ptr attribute of a parameter */
   unsigned line;
};

/* What an operand of an instruction is. */
enum ptx_operand_kind {
   PTX_OPERAND_REG,
   PTX_OPERAND_SREG,
   PTX_OPERAND_IMM_INT,
   PTX_OPERAND_IMM_FLOAT,
   PTX_OPERAND_SYMBOL,
   PTX_OPERAND_ADDRESS,
   PTX_OPERAND_VECTOR,
   PTX_OPERAND_SINK,
};

/* The largest number of operands one instruction may have. */
#define PTX_MAX_OPERANDS 8

/* The largest number of elements a vector operand may have. */
#define PTX_MAX_VECTOR 4

/*
 * One operand of an instruction.
 *
 * A register, a special register and a symbol carry their name ("%rd3",
 * "%tid.x", "name").  An integer carries its value in imm, its bits in
 * imm_bits and whether it was written as an unsigned literal in
 * imm_unsigned.  A float carries its bits in fbits and its width in fwidth
 * (32 for "0fXXXXXXXX", 64 for "0dXXXXXXXXXXXXXXXX"; a decimal float literal
 * is kept as 64-bit bits, because the PTX ISA gives a decimal literal no
 * suffix and always 64 bits).  An address carries its base operand and its
 * offset.  A vector carries its elements.
 *
 * The value of an integer literal follows the PTX ISA (section 4.5.1): the
 * literal is signed unless its magnitude does not fit in int64_t or it
 * carries the U suffix, in which case it is unsigned.  imm holds the signed
 * value and is 0 when the value does not fit in int64_t; imm_bits always
 * holds the exact bits of the literal.
 */
struct ptx_operand {
   enum ptx_operand_kind kind;
   const char *name;
   int64_t imm;
   uint64_t imm_bits;
   bool imm_unsigned;
   uint64_t fbits;
   unsigned fwidth;
   struct ptx_operand *base;  /* the base of an address */
   int64_t offset;            /* the offset of an address */
   struct ptx_operand *elems; /* the elements of a vector */
   unsigned nelems;
   bool neg;                  /* a leading '!' on a predicate operand */
};

/* The largest number of modifiers one instruction may have. */
#define PTX_MAX_MODIFIERS 8

/*
 * One instruction: an optional guard, an opcode, the dotted modifiers that
 * follow it and the operands.
 *
 * The opcode is the first word ("ld" of "ld.global.b32") and the modifiers
 * are the dotted words after it, in order and without the dot ("global" and
 * "b32").
 */
struct ptx_insn {
   const char *guard; /* the predicate of an "@%p1" guard, NULL when none */
   bool guard_neg;    /* the guard was written "@!%p1" */
   const char *opcode;
   const char *modifiers[PTX_MAX_MODIFIERS];
   unsigned nmodifiers;
   struct ptx_operand operands[PTX_MAX_OPERANDS];
   unsigned noperands;
   unsigned line;
};

/* What a statement of a function body is. */
enum ptx_stmt_kind {
   PTX_STMT_LABEL,
   PTX_STMT_INSN,
   PTX_STMT_DECL,
   PTX_STMT_SCOPE,
};

/* One statement of a function body. */
struct ptx_stmt {
   enum ptx_stmt_kind kind;
   unsigned line;
   union {
      const char *label; /* a PTX_STMT_LABEL */
      struct ptx_insn insn;
      struct ptx_decl decl;
      struct {
         struct ptx_stmt *stmts;
         unsigned nstmts;
      } scope; /* a PTX_STMT_SCOPE */
   } u;
};

/* One function of the module: an entry or a plain function. */
struct ptx_func {
   bool is_entry;
   enum ptx_linkage linkage;
   const char *name;
   struct ptx_decl *ret_params; /* the return parameters of a .func */
   unsigned nret_params;
   struct ptx_decl *params;
   unsigned nparams;
   struct ptx_stmt *stmts;
   unsigned nstmts;
   bool has_body; /* false for a prototype, which ends in ';' */
   const char *pragma;
   unsigned maxntid[3];
   unsigned nmaxntid;
   unsigned reqntid[3];
   unsigned nreqntid;
   unsigned minnctapersm; /* 0 when absent */
   unsigned maxnreg;     /* 0 when absent */
   unsigned line;
};

/* One entry of the .target list. */
struct ptx_target {
   const char *name; /* "sm_120", "sm_120a" */
   unsigned sm;      /* 120 */
   char suffix;      /* 'a', 'f' or 0 */
};

/* One PTX module: the header, the module scope declarations and the
 * functions.
 */
struct ptx_module {
   unsigned version_major;
   unsigned version_minor;
   struct ptx_target *targets;
   unsigned ntargets;
   const char **target_flags; /* texmode_independent, debug, ... */
   unsigned ntarget_flags;
   unsigned address_size; /* 32 or 64, 64 by default */
   struct ptx_decl *decls;
   unsigned ndecls;
   struct ptx_func *funcs;
   unsigned nfuncs;
   /* The elements the lists of the module copied when they grew.  A list
    * that grows by doubling copies fewer elements than it holds, so this
    * number stays below twice the size of the module; a list that grows one
    * element at a time copies the square of it.
    */
   uint64_t list_copies;
};

/* Where an error is and what it says. */
struct ptx_diag {
   unsigned line;
   unsigned col;
   char msg[256];
};

/*
 * Parse a PTX module.
 *
 * The text is len bytes long and need not be null terminated.  Everything the
 * module needs is allocated under mem_ctx.  On success the module is
 * returned and diag is cleared; on failure NULL is returned and diag holds
 * the line, the column and the message of the first error.  diag may be
 * NULL, in which case the error is only reported by the NULL return.
 */
struct ptx_module *
ptx_parse(void *mem_ctx, const char *text, size_t len, struct ptx_diag *diag);

/*
 * Print a module as canonical PTX text: one statement per line, a single
 * tab after the opcode, ", " between the operands and no comments.  The
 * result parses back into an equal module.
 */
void
ptx_print(FILE *f, const struct ptx_module *m);

#ifdef __cplusplus
}
#endif

#endif
