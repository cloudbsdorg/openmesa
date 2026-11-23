/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: MIT
 */

/*
 * ptx_print.c: the printer of libnvptx, the PTX front end of native CUDA.
 *
 * CloudBSD fork, Track #2426.
 *
 * The printer writes a module back as canonical PTX text: one statement per
 * line, a single tab after the opcode, ", " between the operands and no
 * comments.  The text it writes parses back into an equal module, which is
 * what the round trip test of the parser proves.
 *
 * The scopes keep their nesting: a scope is written as the braces it was
 * read with, one level of indentation deeper than the statements around
 * it.  The strings keep the spelling they had in the source, so that a
 * pragma with an escape in it is written back with the same escape.
 */

#include "ptx_private.h"

#include <inttypes.h>
#include <string.h>

/* Write the indentation of a statement: one tab per level. */
static void
print_indent(FILE *f, unsigned indent)
{
   unsigned i;

   /* Time: O(n) Space: O(1) */
   for (i = 0; i < indent; i++)
      fputc('\t', f);
}

/* Write one operand of an instruction. */
static void
print_operand(FILE *f, const struct ptx_operand *op)
{
   unsigned i;

   switch (op->kind) {
   case PTX_OPERAND_REG:
   case PTX_OPERAND_SREG:
   case PTX_OPERAND_SYMBOL:
      if (op->neg)
         fputc('!', f);
      fprintf(f, "%s", op->name ? op->name : "");
      break;

   case PTX_OPERAND_IMM_INT:
      /*
       * An integer literal is unsigned when it does not fit in .s64 or
       * when it carries the U suffix (PTX ISA, section 4.5.1).  The
       * suffix keeps that type over the round trip.
       */
      if (op->imm_unsigned)
         fprintf(f, "%" PRIu64 "U", op->imm_bits);
      else
         fprintf(f, "%" PRId64, op->imm);
      break;

   case PTX_OPERAND_IMM_FLOAT:
      /* A float keeps its width: "0fXXXXXXXX" is 32 bits wide and
       * "0dXXXXXXXXXXXXXXXX" is 64.  A decimal literal of the parser was
       * kept as 64 bits and goes out in the 64 bit form: the PTX ISA gives
       * a decimal literal no suffix and always 64 bits.
       */
      if (op->fwidth == 32)
         fprintf(f, "0f%08" PRIx32, (uint32_t)op->fbits);
      else
         fprintf(f, "0d%016" PRIx64, op->fbits);
      break;

   case PTX_OPERAND_ADDRESS:
      fputc('[', f);
      if (op->base)
         print_operand(f, op->base);
      if (op->offset > 0)
         fprintf(f, "+%" PRId64, op->offset);
      else if (op->offset < 0)
         /* The magnitude of the offset is unsigned, so that the smallest
          * offset does not overflow when it is negated.
          */
         fprintf(f, "-%" PRIu64, (uint64_t)(-(uint64_t)op->offset));
      fputc(']', f);
      break;

   case PTX_OPERAND_VECTOR:
      fputc('{', f);
      /* Time: O(n) Space: O(1) */
      for (i = 0; i < op->nelems; i++) {
         if (i)
            fputs(", ", f);
         print_operand(f, &op->elems[i]);
      }
      fputc('}', f);
      break;

   case PTX_OPERAND_SINK:
      fputs("_", f);
      break;
   }
}

/* Write the linkage of a function or a declaration. */
static void
print_linkage(FILE *f, enum ptx_linkage linkage)
{
   switch (linkage) {
   case PTX_LINKAGE_VISIBLE:
      fputs(".visible ", f);
      break;
   case PTX_LINKAGE_EXTERN:
      fputs(".extern ", f);
      break;
   case PTX_LINKAGE_WEAK:
      fputs(".weak ", f);
      break;
   case PTX_LINKAGE_NONE:
      break;
   }
}

/*
 * Write one declaration.  The indent puts the tabs in front of it and the
 * semicolon ends it; the parameters of a function carry neither.
 */
static void
print_decl(FILE *f, const struct ptx_decl *d, unsigned indent, bool semicolon)
{
   print_indent(f, indent);

   print_linkage(f, d->linkage);
   fprintf(f, ".%s", ptx_space_name(d->space));
   if (d->align)
      fprintf(f, " .align %u", d->align);
   if (d->vec > 1)
      fprintf(f, " .v%u", d->vec);
   if (d->type != PTX_TYPE_NONE)
      fprintf(f, " .%s", ptx_type_name(d->type));
   if (d->is_ptr)
      fputs(" .ptr", f);
   fprintf(f, " %s", d->name ? d->name : "");
   if (d->param_count)
      fprintf(f, "<%u>", d->param_count);
   /* An array with no size of its own is written back with empty marks. */
   if (d->unsized)
      fputs("[]", f);
   else if (d->count)
      fprintf(f, "[%u]", d->count);
   if (semicolon)
      fputc(';', f);
}

/* Write the parameters of a function, one per line. */
static void
print_params(FILE *f, const struct ptx_decl *params, unsigned nparams)
{
   unsigned i;

   /* Time: O(n) Space: O(1) */
   for (i = 0; i < nparams; i++) {
      if (i)
         fputs(",\n", f);
      print_decl(f, &params[i], 1, false);
   }
}

/* Write the performance directives of a function. */
static void
print_perf_directives(FILE *f, const struct ptx_func *fn)
{
   unsigned i;

   if (fn->nmaxntid) {
      fputs(".maxntid", f);
      /* Time: O(n) Space: O(1) */
      for (i = 0; i < fn->nmaxntid; i++)
         fprintf(f, "%s%u", i ? ", " : " ", fn->maxntid[i]);
      fputs(";\n", f);
   }
   if (fn->nreqntid) {
      fputs(".reqntid", f);
      /* Time: O(n) Space: O(1) */
      for (i = 0; i < fn->nreqntid; i++)
         fprintf(f, "%s%u", i ? ", " : " ", fn->reqntid[i]);
      fputs(";\n", f);
   }
   if (fn->minnctapersm)
      fprintf(f, ".minnctapersm %u;\n", fn->minnctapersm);
   if (fn->maxnreg)
      fprintf(f, ".maxnreg %u;\n", fn->maxnreg);
   if (fn->pragma)
      fprintf(f, ".pragma \"%s\";\n", fn->pragma);
}

/* Write one instruction. */
static void
print_insn(FILE *f, const struct ptx_insn *insn, unsigned indent)
{
   unsigned i;

   print_indent(f, indent);
   if (insn->guard) {
      fputc('@', f);
      if (insn->guard_neg)
         fputc('!', f);
      fprintf(f, "%s ", insn->guard);
   }
   fprintf(f, "%s", insn->opcode);
   /* Time: O(n) Space: O(1) */
   for (i = 0; i < insn->nmodifiers; i++)
      fprintf(f, ".%s", insn->modifiers[i]);

   if (insn->noperands) {
      fputc('\t', f);
      /* Time: O(n) Space: O(1) */
      for (i = 0; i < insn->noperands; i++) {
         if (i)
            fputs(", ", f);
         print_operand(f, &insn->operands[i]);
      }
   }
   fputc(';', f);
}

/* Write one statement of a function body. */
static void
print_stmt(FILE *f, const struct ptx_stmt *stmt, unsigned indent)
{
   unsigned i;

   switch (stmt->kind) {
   case PTX_STMT_LABEL:
      print_indent(f, indent);
      fprintf(f, "%s:\n", stmt->u.label ? stmt->u.label : "");
      break;

   case PTX_STMT_INSN:
      print_insn(f, &stmt->u.insn, indent);
      fputc('\n', f);
      break;

   case PTX_STMT_DECL:
      print_decl(f, &stmt->u.decl, indent, true);
      fputc('\n', f);
      break;

   case PTX_STMT_SCOPE:
      print_indent(f, indent);
      fputs("{\n", f);
      /* Time: O(n) Space: O(1) */
      for (i = 0; i < stmt->u.scope.nstmts; i++)
         print_stmt(f, &stmt->u.scope.stmts[i], indent + 1);
      print_indent(f, indent);
      fputs("}\n", f);
      break;
   }
}

/* Write one function of the module. */
static void
print_func(FILE *f, const struct ptx_func *fn)
{
   unsigned i;

   print_linkage(f, fn->linkage);

   fputs(fn->is_entry ? ".entry " : ".func ", f);

   /* The return parameters of a .func come before its name. */
   if (!fn->is_entry && fn->nret_params) {
      fputc('(', f);
      /* Time: O(n) Space: O(1) */
      for (i = 0; i < fn->nret_params; i++) {
         if (i)
            fputs(", ", f);
         print_decl(f, &fn->ret_params[i], 0, false);
      }
      fputs(") ", f);
   }

   fprintf(f, "%s(", fn->name ? fn->name : "");
   if (fn->nparams) {
      fputc('\n', f);
      print_params(f, fn->params, fn->nparams);
      fputc('\n', f);
   }
   fputs(")\n", f);

   print_perf_directives(f, fn);

   if (!fn->has_body) {
      fputs(";\n", f);
      return;
   }

   fputs("{\n", f);
   /* Time: O(n) Space: O(1) */
   for (i = 0; i < fn->nstmts; i++)
      print_stmt(f, &fn->stmts[i], 1);
   fputs("}\n", f);
}

void
ptx_print(FILE *f, const struct ptx_module *m)
{
   unsigned i;

   if (m->version_major) {
      fprintf(f, ".version %u.%u\n", m->version_major, m->version_minor);
   }

   if (m->ntargets || m->ntarget_flags) {
      bool first = true;

      fputs(".target", f);
      /* Time: O(n) Space: O(1) */
      for (i = 0; i < m->ntargets; i++) {
         fprintf(f, "%s %s", first ? "" : ",", m->targets[i].name);
         first = false;
      }
      /* Time: O(n) Space: O(1) */
      for (i = 0; i < m->ntarget_flags; i++) {
         fprintf(f, "%s %s", first ? "" : ",", m->target_flags[i]);
         first = false;
      }
      fputc('\n', f);
   }

   fprintf(f, ".address_size %u\n", m->address_size);

   /* Time: O(n) Space: O(1) */
   for (i = 0; i < m->ndecls; i++) {
      print_decl(f, &m->decls[i], 0, true);
      fputc('\n', f);
   }

   /* Time: O(n) Space: O(1) */
   for (i = 0; i < m->nfuncs; i++)
      print_func(f, &m->funcs[i]);
}
