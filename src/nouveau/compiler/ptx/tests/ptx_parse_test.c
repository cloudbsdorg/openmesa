/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: MIT
 */

/*
 * nvptx_parse_test: the parser test of libnvptx, the PTX front end of native
 * CUDA (CloudBSD fork, Track #2426).
 *
 * A plain C program: no gtest, one line per check, exit status 1 when
 * anything failed.
 */

#include "ptx.h"

#include <locale.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "util/memstream.h"
#include "util/ralloc.h"

static unsigned ok_count;
static unsigned bad_count;

static void
check(bool cond, const char *name, const char *fmt, ...)
{
   va_list ap;

   if (cond) {
      ok_count++;
      printf("ok - %s\n", name);
      return;
   }

   bad_count++;
   printf("not ok - %s: ", name);
   va_start(ap, fmt);
   vprintf(fmt, ap);
   va_end(ap);
   printf("\n");
}

static char *
read_file(const char *path, size_t *len_out)
{
   FILE *f = fopen(path, "rb");
   char *buf;
   long size;

   if (!f)
      return NULL;

   if (fseek(f, 0, SEEK_END) != 0) {
      fclose(f);
      return NULL;
   }
   size = ftell(f);
   if (size < 0) {
      fclose(f);
      return NULL;
   }
   rewind(f);

   buf = malloc((size_t)size + 1);
   if (!buf) {
      fclose(f);
      return NULL;
   }

   if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
      free(buf);
      fclose(f);
      return NULL;
   }
   buf[size] = '\0';
   fclose(f);

   *len_out = (size_t)size;
   return buf;
}

static struct ptx_module *
parse_text(void *ctx, const char *text, size_t len, struct ptx_diag *diag)
{
   return ptx_parse(ctx, text, len, diag);
}

static struct ptx_module *
parse_file(void *ctx, const char *path, struct ptx_diag *diag)
{
   struct ptx_module *m;
   char *text;
   size_t len = 0;

   text = read_file(path, &len);
   if (!text) {
      snprintf(diag->msg, sizeof(diag->msg), "cannot read %s", path);
      diag->line = 1;
      diag->col = 1;
      return NULL;
   }

   m = parse_text(ctx, text, len, diag);
   free(text);
   return m;
}

/* Print a module into a memory stream and hand back its text. */
static void
print_to_string(const struct ptx_module *m, char **buf_out, size_t *len_out)
{
   struct u_memstream mem;
   char *buf = NULL;
   size_t len = 0;

   u_memstream_open(&mem, &buf, &len);
   ptx_print(u_memstream_get(&mem), m);
   u_memstream_close(&mem);

   *buf_out = buf;
   *len_out = len;
}

static const struct ptx_insn *
find_insn_opcode(const struct ptx_module *m, const char *opcode,
                 const char *guard)
{
   unsigned f, i;

   /* Time: O(n) Space: O(1) */
   for (f = 0; f < m->nfuncs; f++) {
      /* Time: O(n) Space: O(1) */
      for (i = 0; i < m->funcs[f].nstmts; i++) {
         const struct ptx_stmt *s = &m->funcs[f].stmts[i];

         if (s->kind != PTX_STMT_INSN)
            continue;
         if (strcmp(s->u.insn.opcode, opcode) != 0)
            continue;
         if (guard && (!s->u.insn.guard ||
                       strcmp(s->u.insn.guard, guard) != 0))
            continue;
         return &s->u.insn;
      }
   }

   return NULL;
}

/* Find an instruction with the given opcode, the given single modifier (or
 * NULL) and an operand of the given kind in the second place.
 */
static const struct ptx_insn *
find_insn_kind(const struct ptx_module *m, const char *opcode,
               const char *modifier, enum ptx_operand_kind kind)
{
   unsigned f, i;

   /* Time: O(n) Space: O(1) */
   for (f = 0; f < m->nfuncs; f++) {
      /* Time: O(n) Space: O(1) */
      for (i = 0; i < m->funcs[f].nstmts; i++) {
         const struct ptx_stmt *s = &m->funcs[f].stmts[i];
         const struct ptx_insn *insn;

         if (s->kind != PTX_STMT_INSN)
            continue;
         insn = &s->u.insn;
         if (strcmp(insn->opcode, opcode) != 0)
            continue;
         if (modifier && (insn->nmodifiers != 1 ||
                          strcmp(insn->modifiers[0], modifier) != 0))
            continue;
         if (insn->noperands < 2 || insn->operands[1].kind != kind)
            continue;
         return insn;
      }
   }

   return NULL;
}

/* The nth instruction with the given opcode, in the order of the module. */
static const struct ptx_insn *
find_insn_nth(const struct ptx_module *m, const char *opcode, unsigned nth)
{
   unsigned f, i;
   unsigned seen = 0;

   /* Time: O(n) Space: O(1) */
   for (f = 0; f < m->nfuncs; f++) {
      /* Time: O(n) Space: O(1) */
      for (i = 0; i < m->funcs[f].nstmts; i++) {
         const struct ptx_stmt *s = &m->funcs[f].stmts[i];

         if (s->kind != PTX_STMT_INSN)
            continue;
         if (strcmp(s->u.insn.opcode, opcode) != 0)
            continue;
         if (seen == nth)
            return &s->u.insn;
         seen++;
      }
   }

   return NULL;
}

static bool
has_label(const struct ptx_module *m, const char *name)
{
   unsigned f, i;

   /* Time: O(n) Space: O(1) */
   for (f = 0; f < m->nfuncs; f++) {
      /* Time: O(n) Space: O(1) */
      for (i = 0; i < m->funcs[f].nstmts; i++) {
         const struct ptx_stmt *s = &m->funcs[f].stmts[i];

         if (s->kind == PTX_STMT_LABEL &&
             strcmp(s->u.label, name) == 0)
            return true;
      }
   }

   return false;
}

static void
check_error_len(const char *name, const char *text, size_t len,
                const char *msg, unsigned line, unsigned col)
{
   void *ctx = ralloc_context(NULL);
   struct ptx_diag diag;
   struct ptx_module *m = parse_text(ctx, text, len, &diag);

   if (m) {
      check(false, name, "expected the error '%s', but the parse succeeded",
            msg);
   } else {
      check(strcmp(diag.msg, msg) == 0 && diag.line == line &&
            diag.col == col, name,
            "expected '%s' at %u:%u, got '%s' at %u:%u",
            msg, line, col, diag.msg, diag.line, diag.col);
   }

   ralloc_free(ctx);
}

static void
check_error(const char *name, const char *text,
            const char *msg, unsigned line, unsigned col)
{
   check_error_len(name, text, strlen(text), msg, line, col);
}

/*
 * The number of seconds a parse may take before the test gives up on it.
 *
 * A parser that loops on a token it never moves past would hang the test,
 * the build and the machine with it.  The parse runs under an alarm, so
 * that such a parser fails the case instead of waiting forever.
 */
#define PARSE_TIMEOUT_SECONDS 10

static sigjmp_buf parse_jump;
static volatile sig_atomic_t parse_timed_out;

static void
on_alarm(int sig)
{
   (void)sig;
   parse_timed_out = 1;
   siglongjmp(parse_jump, 1);
}

/*
 * Parse the text with a time limit.
 *
 * The parse that does not finish in PARSE_TIMEOUT_SECONDS is given up on:
 * the module is NULL and the diagnostic says so.
 */
static struct ptx_module *
parse_text_timed(void *ctx, const char *text, size_t len, struct ptx_diag *diag)
{
   struct ptx_module *m;
   struct sigaction sa;
   struct sigaction old;

   memset(&sa, 0, sizeof(sa));
   sa.sa_handler = on_alarm;
   sigemptyset(&sa.sa_mask);
   /* The alarm breaks the parse out; it must not restart it. */
   sa.sa_flags = 0;
   sigaction(SIGALRM, &sa, &old);

   parse_timed_out = 0;
   if (sigsetjmp(parse_jump, 1) == 0) {
      alarm(PARSE_TIMEOUT_SECONDS);
      m = parse_text(ctx, text, len, diag);
      alarm(0);
   } else {
      m = NULL;
      memset(diag, 0, sizeof(*diag));
      snprintf(diag->msg, sizeof(diag->msg),
               "the parse did not finish in %d seconds",
               PARSE_TIMEOUT_SECONDS);
      diag->line = 1;
      diag->col = 1;
   }

   sigaction(SIGALRM, &old, NULL);

   return m;
}

/*
 * The module must be refused with the named error, and the parse must not
 * hang: it runs under the alarm of parse_text_timed.
 */
static void
check_refused(const char *name, const char *text, const char *msg,
              unsigned line, unsigned col)
{
   void *ctx = ralloc_context(NULL);
   struct ptx_diag diag;
   struct ptx_module *m = parse_text_timed(ctx, text, strlen(text), &diag);

   if (m) {
      check(false, name, "expected the error '%s', but the parse succeeded",
            msg);
   } else {
      check(strcmp(diag.msg, msg) == 0 && diag.line == line &&
            diag.col == col, name,
            "expected '%s' at %u:%u, got '%s' at %u:%u",
            msg, line, col, diag.msg, diag.line, diag.col);
   }

   ralloc_free(ctx);
}

/* The first instruction the match function accepts. */
static const struct ptx_insn *
find_insn(const struct ptx_module *m,
          bool (*match)(const struct ptx_insn *, const void *),
          const void *arg)
{
   unsigned f, i;

   /* Time: O(n) Space: O(1) */
   for (f = 0; f < m->nfuncs; f++) {
      /* Time: O(n) Space: O(1) */
      for (i = 0; i < m->funcs[f].nstmts; i++) {
         const struct ptx_stmt *s = &m->funcs[f].stmts[i];

         if (s->kind != PTX_STMT_INSN)
            continue;
         if (match(&s->u.insn, arg))
            return &s->u.insn;
      }
   }

   return NULL;
}

/* Does the instruction load a global b32? */
static bool
match_ld_global_b32(const struct ptx_insn *insn, const void *arg)
{
   (void)arg;

   if (strcmp(insn->opcode, "ld") != 0)
      return false;
   if (insn->nmodifiers != 2)
      return false;
   if (strcmp(insn->modifiers[0], "global") != 0 ||
       strcmp(insn->modifiers[1], "b32") != 0)
      return false;
   return true;
}

/* Does the instruction load from the address with the given base? */
static bool
match_ld_with_base(const struct ptx_insn *insn, const void *arg)
{
   const char *base_name = arg;

   if (strcmp(insn->opcode, "ld") != 0)
      return false;
   if (insn->noperands != 2)
      return false;
   if (insn->operands[1].kind != PTX_OPERAND_ADDRESS)
      return false;
   if (!insn->operands[1].base ||
       strcmp(insn->operands[1].base->name, base_name) != 0)
      return false;
   return true;
}

/* The ld.global.b32 of the module, if there is one. */
static const struct ptx_insn *
find_ld_global_b32(const struct ptx_module *m)
{
   return find_insn(m, match_ld_global_b32, NULL);
}

/* The ld whose address has the given base, if there is one. */
static const struct ptx_insn *
find_ld_with_base(const struct ptx_module *m, const char *base_name)
{
   return find_insn(m, match_ld_with_base, base_name);
}

/* The .shared declaration of the module, if there is one. */
static const struct ptx_decl *
find_shared_decl(const struct ptx_module *m)
{
   unsigned f, i;

   /* Time: O(n) Space: O(1) */
   for (f = 0; f < m->nfuncs; f++) {
      /* Time: O(n) Space: O(1) */
      for (i = 0; i < m->funcs[f].nstmts; i++) {
         const struct ptx_stmt *s = &m->funcs[f].stmts[i];

         if (s->kind == PTX_STMT_DECL &&
             s->u.decl.space == PTX_SPACE_SHARED)
            return &s->u.decl;
      }
   }

   return NULL;
}

/* The two fixtures that our clang 23 produced. */
static void
test_vector_add_module(const struct ptx_module *m)
{
   check(m->version_major == 9 && m->version_minor == 0,
         "vector_add: the version is 9.0", "got %u.%u",
         m->version_major, m->version_minor);

   check(m->ntargets == 1 && m->targets[0].sm == 120 &&
         m->targets[0].suffix == 0 &&
         strcmp(m->targets[0].name, "sm_120") == 0,
         "vector_add: the target is sm_120", "got %u targets", m->ntargets);

   check(m->address_size == 64, "vector_add: the address size is 64",
         "got %u", m->address_size);

   check(m->nfuncs == 1, "vector_add: one function", "got %u", m->nfuncs);
}

/* The parameters of the vector_add entry. */
static void
test_vector_add_params(const struct ptx_decl *p)
{
   check(p[0].type == PTX_TYPE_U64 && p[0].is_ptr && p[0].align == 1 &&
         p[1].type == PTX_TYPE_U64 && p[1].is_ptr && p[1].align == 1 &&
         p[2].type == PTX_TYPE_U64 && p[2].is_ptr && p[2].align == 1 &&
         p[3].type == PTX_TYPE_U32 && !p[3].is_ptr,
         "vector_add: three .u64 .ptr .align 1 and one .u32 parameter",
         "got %u %u %u %u", p[0].type, p[1].type, p[2].type, p[3].type);
}

/* The .reg declarations of the vector_add entry. */
static void
test_vector_add_regs(const struct ptx_func *f)
{
   unsigned counts[3] = {0, 0, 0};
   unsigned n = 0, i;

   /* Time: O(n) Space: O(1) */
   for (i = 0; i < f->nstmts && n < 3; i++) {
      const struct ptx_stmt *s = &f->stmts[i];

      if (s->kind == PTX_STMT_DECL && s->u.decl.space == PTX_SPACE_REG)
         counts[n++] = s->u.decl.param_count;
   }
   check(n == 3 && counts[0] == 2 && counts[1] == 9 && counts[2] == 11,
         "vector_add: the three .reg declarations count 2, 9 and 11",
         "got %u declarations: %u %u %u", n, counts[0], counts[1], counts[2]);
}

/* The function of the vector_add module. */
static void
test_vector_add_func(const struct ptx_func *f)
{
   check(f->is_entry, "vector_add: it is an entry", "");
   check(strcmp(f->name, "_Z4vaddPKfS0_Pfi") == 0,
         "vector_add: the name is _Z4vaddPKfS0_Pfi", "got %s", f->name);
   check(f->nparams == 4, "vector_add: four parameters", "got %u",
         f->nparams);

   if (f->nparams == 4)
      test_vector_add_params(f->params);

   test_vector_add_regs(f);
}

/* The instructions of the vector_add module. */
static void
test_vector_add_insns(const struct ptx_module *m)
{
   const struct ptx_insn *insn;

   insn = find_insn_opcode(m, "bra", "%p1");
   check(insn != NULL && !insn->guard_neg,
         "vector_add: the guarded bra is guarded by %p1", "");

   check(has_label(m, "$L__BB0_2"), "vector_add: the label $L__BB0_2 is there",
         "");

   /* ld.global.b32 %r6, [%rd3]; */
   insn = find_ld_global_b32(m);
   check(insn != NULL, "vector_add: there is an ld.global.b32", "");
   if (insn) {
      check(insn->noperands == 2 &&
            insn->operands[1].kind == PTX_OPERAND_ADDRESS &&
            insn->operands[1].base != NULL &&
            insn->operands[1].base->kind == PTX_OPERAND_REG &&
            strcmp(insn->operands[1].base->name, "%rd3") == 0 &&
            insn->operands[1].offset == 0,
            "vector_add: ld.global.b32 reads [%rd3]", "got %u operands",
            insn->noperands);
   }
}

static void
test_vector_add(void)
{
   void *ctx = ralloc_context(NULL);
   struct ptx_diag diag;
   struct ptx_module *m = parse_file(ctx, "vector_add.ptx", &diag);

   if (!m) {
      check(false, "vector_add.ptx parses",
            "%s at %u:%u", diag.msg, diag.line, diag.col);
      ralloc_free(ctx);
      return;
   }
   check(true, "vector_add.ptx parses", "");

   test_vector_add_module(m);

   if (m->nfuncs == 1)
      test_vector_add_func(&m->funcs[0]);

   test_vector_add_insns(m);

   ralloc_free(ctx);
}

/* The functions of the reduce_sum module. */
static void
test_reduce_sum_funcs(const struct ptx_module *m)
{
   const struct ptx_decl *shared;

   check(m->nfuncs == 2, "reduce_sum: two entries", "got %u", m->nfuncs);

   shared = find_shared_decl(m);
   check(shared != NULL && shared->align == 4 &&
         shared->type == PTX_TYPE_B8 && shared->count == 1024,
         "reduce_sum: the .shared .align 4 .b8 array holds 1024 elements", "");
}

/* The instructions of the reduce_sum module. */
static void
test_reduce_sum_insns(const struct ptx_module *m)
{
   const struct ptx_insn *insn;

   insn = find_insn_opcode(m, "bra", "%p2");
   check(insn != NULL && insn->guard_neg,
         "reduce_sum: @!%p2 is a negated guard", "");

   insn = find_insn_opcode(m, "shfl", NULL);
   check(insn != NULL && insn->nmodifiers == 3 &&
         strcmp(insn->modifiers[0], "sync") == 0 &&
         strcmp(insn->modifiers[1], "down") == 0 &&
         strcmp(insn->modifiers[2], "b32") == 0,
         "reduce_sum: there is a shfl.sync.down.b32", "");
   if (insn) {
      /* shfl.sync.down.b32 %r11, %r10, 16, 31, -1; */
      check(insn->noperands == 5 &&
            insn->operands[4].kind == PTX_OPERAND_IMM_INT &&
            insn->operands[4].imm == -1,
            "reduce_sum: shfl.sync.down.b32 has four source operands and -1",
            "got %u operands", insn->noperands);
   }

   insn = find_insn_kind(m, "mov", NULL, PTX_OPERAND_IMM_FLOAT);
   check(insn != NULL && insn->operands[1].fwidth == 32 &&
         insn->operands[1].fbits == 0,
         "reduce_sum: 0f00000000 is a 32-bit float with the bits 0", "");

   insn = find_insn_kind(m, "mov", "b64", PTX_OPERAND_SYMBOL);
   check(insn != NULL &&
         strcmp(insn->operands[1].name, "_ZZ10reduce_sumPKfPfiE4part") == 0,
         "reduce_sum: mov.b64 takes the symbol _ZZ10reduce_sumPKfPfiE4part",
         "");
}

static void
test_reduce_sum(void)
{
   void *ctx = ralloc_context(NULL);
   struct ptx_diag diag;
   struct ptx_module *m = parse_file(ctx, "reduce_sum.ptx", &diag);

   if (!m) {
      check(false, "reduce_sum.ptx parses",
            "%s at %u:%u", diag.msg, diag.line, diag.col);
      ralloc_free(ctx);
      return;
   }
   check(true, "reduce_sum.ptx parses", "");

   test_reduce_sum_funcs(m);
   test_reduce_sum_insns(m);

   ralloc_free(ctx);
}

/* The hand written cases of the parser. */
static const char hand_func[] =
   ".version 9.0\n"
   ".target sm_120a\n"
   ".address_size 64\n"
   ".visible .func (.param .b32 %ret) foo(.param .u32 %in);\n"
   ".visible .entry bar(\n"
   ".param .u64 .ptr .align 1 p0\n"
   ")\n"
   "{\n"
   ".maxntid 256, 1, 1;\n"
   ".minnctapersm 2;\n"
   ".reg .v4 .b32 %v<4>;\n"
   "mov.b32 %r1, %r2;\n"
   "mov.v2.b32 {%r1, %r2}, {%r3, %r4};\n"
   "ld.global.b32 %r5, [p0+4];\n"
   "ld.global.b32 %r6, [%rd1+-8];\n"
   "mov.u32 %r7, 0x10;\n"
   "mov.u32 %r8, 0b1010;\n"
   "mov.u32 %r9, 017;\n"
   "mov.u32 %r12, 42U;\n"
   "mov.f64 %rd2, 0d3FF0000000000000;\n"
   "mov.f64 %rd3, 1.5;\n"
   "{\n"
   "mov.u32 %r10, 1;\n"
   "}\n"
   "mov.u32 %r11, _;\n"
   "}\n";

static const char hand_loc[] =
   ".version 9.0\n"
   ".file 1 \"foo.cu\"\n"
   ".target sm_120\n"
   ".visible .entry k(\n"
   ".param .u32 p\n"
   ")\n"
   "{\n"
   ".loc 1 42 3\n"
   "mov.u32 %r1, 0;\n"
   "}\n";

/* The header of the hand written module. */
static void
test_hand_module(const struct ptx_module *m)
{
   check(m->ntargets == 1 && m->targets[0].sm == 120 &&
         m->targets[0].suffix == 'a',
         "hand: the target sm_120a keeps its suffix", "");

   check(m->nfuncs == 2, "hand: two functions", "got %u", m->nfuncs);
}

/* The first function of the hand written module, the .func. */
static void
test_hand_func_foo(const struct ptx_func *foo)
{
   check(!foo->is_entry && foo->linkage == PTX_LINKAGE_VISIBLE &&
         strcmp(foo->name, "foo") == 0,
         "hand: the first function is a visible .func named foo", "");
   check(foo->nret_params == 1 && foo->ret_params[0].type == PTX_TYPE_B32 &&
         strcmp(foo->ret_params[0].name, "%ret") == 0,
         "hand: the .func returns through a .b32 parameter", "");
   check(foo->nparams == 1 && foo->params[0].type == PTX_TYPE_U32,
         "hand: the .func takes one .u32 parameter", "");
   check(!foo->has_body, "hand: the .func is a prototype", "");
}

/* The second function of the hand written module, the .entry. */
static void
test_hand_func_bar(const struct ptx_func *bar)
{
   check(bar->is_entry && bar->linkage == PTX_LINKAGE_VISIBLE &&
         strcmp(bar->name, "bar") == 0,
         "hand: the second function is a visible .entry named bar", "");
   check(bar->nparams == 1 && bar->params[0].type == PTX_TYPE_U64 &&
         bar->params[0].is_ptr && bar->params[0].align == 1,
         "hand: the .entry takes a .u64 .ptr .align 1 parameter", "");
   check(bar->nmaxntid == 3 && bar->maxntid[0] == 256 &&
         bar->maxntid[1] == 1 && bar->maxntid[2] == 1,
         "hand: .maxntid 256, 1, 1", "");
   check(bar->minnctapersm == 2, "hand: .minnctapersm 2", "got %u",
         bar->minnctapersm);
}

/* The functions of the hand written module. */
static void
test_hand_funcs(const struct ptx_module *m)
{
   if (m->nfuncs == 2) {
      test_hand_func_foo(&m->funcs[0]);
      test_hand_func_bar(&m->funcs[1]);
   }
}

/* The .reg declaration of the hand written module. */
static void
test_hand_decl(const struct ptx_module *m)
{
   unsigned i;

   /* the .reg .v4 declaration */
   /* Time: O(n) Space: O(1) */
   for (i = 0; i < m->funcs[1].nstmts; i++) {
      const struct ptx_stmt *s = &m->funcs[1].stmts[i];

      if (s->kind == PTX_STMT_DECL && s->u.decl.space == PTX_SPACE_REG) {
         check(s->u.decl.vec == 4 && s->u.decl.type == PTX_TYPE_B32 &&
               s->u.decl.param_count == 4,
               "hand: .reg .v4 .b32 %v<4>", "");
      }
   }
}

/* The vector operand of the hand written module. */
static void
test_hand_vector(const struct ptx_module *m)
{
   const struct ptx_insn *insn = NULL;
   unsigned i;

   /* Time: O(n) Space: O(1) */
   for (i = 0; i < m->funcs[1].nstmts; i++) {
      const struct ptx_stmt *s = &m->funcs[1].stmts[i];

      if (s->kind == PTX_STMT_INSN && strcmp(s->u.insn.opcode, "mov") == 0 &&
          s->u.insn.noperands == 2 &&
          s->u.insn.operands[0].kind == PTX_OPERAND_VECTOR)
         insn = &s->u.insn;
   }
   check(insn != NULL && insn->operands[0].nelems == 2 &&
         strcmp(insn->operands[0].elems[0].name, "%r1") == 0 &&
         strcmp(insn->operands[0].elems[1].name, "%r2") == 0,
         "hand: {%r1, %r2} is a vector of two registers", "");
}

/* The address operands of the hand written module. */
static void
test_hand_addresses(const struct ptx_module *m)
{
   const struct ptx_insn *insn;

   insn = find_insn_opcode(m, "ld", NULL);
   check(insn != NULL && insn->noperands == 2 &&
         insn->operands[1].kind == PTX_OPERAND_ADDRESS &&
         insn->operands[1].base->kind == PTX_OPERAND_SYMBOL &&
         strcmp(insn->operands[1].base->name, "p0") == 0 &&
         insn->operands[1].offset == 4,
         "hand: [p0+4] is an address with the symbol p0 and the offset 4", "");

   insn = find_ld_with_base(m, "%rd1");
   check(insn != NULL && insn->operands[1].offset == -8,
         "hand: [%rd1+-8] is an address with the offset -8", "");
}

/* The integer literals of the hand written module. */
static void
test_hand_numbers(const struct ptx_module *m)
{
   const int64_t want[] = {16, 10, 15};
   const char *names[] = {"0x10 is 16", "0b1010 is 10", "017 is 15"};
   unsigned found = 0;
   unsigned i;

   /* Time: O(n) Space: O(1) */
   for (i = 0; i < m->funcs[1].nstmts; i++) {
      const struct ptx_stmt *s = &m->funcs[1].stmts[i];

      if (s->kind != PTX_STMT_INSN)
         continue;
      if (s->u.insn.opcode != NULL &&
          strcmp(s->u.insn.opcode, "mov") != 0)
         continue;
      if (s->u.insn.noperands != 2)
         continue;
      if (s->u.insn.operands[1].kind != PTX_OPERAND_IMM_INT)
         continue;
      if (s->u.insn.operands[1].imm < 10 || s->u.insn.operands[1].imm > 16)
         continue;
      if (found < 3)
         check(s->u.insn.operands[1].imm == want[found], names[found],
               "got %lld", (long long)s->u.insn.operands[1].imm);
      found++;
   }
   check(found == 3, "hand: the hex, binary and octal integers are there",
         "got %u", found);
}

/* The float literals of the hand written module. */
static void
test_hand_floats(const struct ptx_module *m)
{
   const struct ptx_insn *insn = NULL;
   bool has_decimal = false;
   bool has_unsigned = false;
   unsigned i;

   /* the double */
   /* Time: O(n) Space: O(1) */
   for (i = 0; i < m->funcs[1].nstmts && !insn; i++) {
      const struct ptx_stmt *s = &m->funcs[1].stmts[i];

      if (s->u.insn.noperands != 2)
         continue;
      if (s->u.insn.operands[1].kind != PTX_OPERAND_IMM_FLOAT)
         continue;
      insn = &s->u.insn;
   }
   check(insn != NULL && insn->operands[1].fwidth == 64 &&
         insn->operands[1].fbits == 0x3ff0000000000000ULL,
         "hand: 0d3FF0000000000000 is a 64-bit float", "");

   /* the decimal float and the unsigned suffix */
   /* Time: O(n) Space: O(1) */
   for (i = 0; i < m->funcs[1].nstmts; i++) {
      const struct ptx_stmt *s = &m->funcs[1].stmts[i];

      if (s->kind != PTX_STMT_INSN || s->u.insn.noperands != 2)
         continue;
      if (s->u.insn.operands[1].kind == PTX_OPERAND_IMM_FLOAT &&
          s->u.insn.operands[1].fwidth == 64 &&
          s->u.insn.operands[1].fbits == 0x3ff8000000000000ULL)
         has_decimal = true;
      if (s->u.insn.operands[1].kind == PTX_OPERAND_IMM_INT &&
          s->u.insn.operands[1].imm == 42)
         has_unsigned = true;
   }
   check(has_decimal, "hand: 1.5 is a 64-bit float", "");
   check(has_unsigned, "hand: 42U is the integer 42", "");
}

/* The sink operand of the hand written module. */
static void
test_hand_sink(const struct ptx_module *m)
{
   const struct ptx_insn *insn = NULL;
   unsigned i;

   /* Time: O(n) Space: O(1) */
   for (i = 0; i < m->funcs[1].nstmts && !insn; i++) {
      const struct ptx_stmt *s = &m->funcs[1].stmts[i];

      if (s->u.insn.noperands != 2)
         continue;
      if (s->u.insn.operands[1].kind != PTX_OPERAND_SINK)
         continue;
      insn = &s->u.insn;
   }
   check(insn != NULL, "hand: _ is the sink operand", "");
}

/* The statements of the hand written module. */
static void
test_hand_scope(const struct ptx_module *m)
{
   const struct ptx_stmt *scope = NULL;
   unsigned i;

   /* Time: O(n) Space: O(1) */
   for (i = 0; i < m->funcs[1].nstmts; i++) {
      if (m->funcs[1].stmts[i].kind == PTX_STMT_SCOPE)
         scope = &m->funcs[1].stmts[i];
   }

   check(scope != NULL,
         "hand: the nested scope is a statement of its own", "");
   check(scope != NULL && scope->u.scope.nstmts == 1 &&
         scope->u.scope.stmts[0].kind == PTX_STMT_INSN &&
         strcmp(scope->u.scope.stmts[0].u.insn.opcode, "mov") == 0,
         "hand: the scope holds the statement it was read with",
         "got %u statements in the scope",
         scope ? scope->u.scope.nstmts : 0);
}

/* The module with the .file and the .loc lines. */
static void
test_hand_loc(void *ctx)
{
   struct ptx_diag diag;
   struct ptx_module *m = parse_text(ctx, hand_loc, sizeof(hand_loc) - 1,
                                      &diag);

   if (!m) {
      check(false, "the .file and .loc module parses",
            "%s at %u:%u", diag.msg, diag.line, diag.col);
      return;
   }

   check(true, "the .file and .loc module parses", "");
   check(m->nfuncs == 1 && m->funcs[0].nstmts == 1,
         "hand: the .file and the .loc are dropped", "got %u statements",
         m->funcs[0].nstmts);
}

static void
test_hand_cases(void)
{
   void *ctx = ralloc_context(NULL);
   struct ptx_diag diag;
   struct ptx_module *m = parse_text(ctx, hand_func, sizeof(hand_func) - 1,
                                      &diag);

   if (!m) {
      check(false, "the hand written module parses",
            "%s at %u:%u", diag.msg, diag.line, diag.col);
      ralloc_free(ctx);
      return;
   }
   check(true, "the hand written module parses", "");

   test_hand_module(m);
   test_hand_funcs(m);
   test_hand_decl(m);
   test_hand_vector(m);
   test_hand_addresses(m);
   test_hand_numbers(m);
   test_hand_floats(m);
   test_hand_sink(m);
   test_hand_scope(m);
   test_hand_loc(ctx);

   ralloc_free(ctx);
}

static void
check_round_trip(const char *name, const char *text, size_t len)
{
   void *ctx = ralloc_context(NULL);
   struct ptx_diag diag;
   struct ptx_module *m1 = parse_text(ctx, text, len, &diag);
   struct ptx_module *m2;
   char *t1 = NULL, *t2 = NULL;
   size_t l1 = 0, l2 = 0;

   if (!m1) {
      check(false, name, "the first parse failed: %s at %u:%u", diag.msg,
            diag.line, diag.col);
      ralloc_free(ctx);
      return;
   }

   print_to_string(m1, &t1, &l1);

   m2 = parse_text(ctx, t1, l1, &diag);
   if (!m2) {
      check(false, name, "the second parse failed: %s at %u:%u\n%s", diag.msg,
            diag.line, diag.col, t1);
      free(t1);
      ralloc_free(ctx);
      return;
   }

   print_to_string(m2, &t2, &l2);

   check(l1 == l2 && memcmp(t1, t2, l1) == 0, name,
         "the printed texts differ (%u and %u bytes)", (unsigned)l1,
         (unsigned)l2);

   free(t1);
   free(t2);
   ralloc_free(ctx);
}

static void
test_round_trips(void)
{
   void *ctx = ralloc_context(NULL);
   char *text;
   size_t len = 0;

   text = read_file("vector_add.ptx", &len);
   if (!text) {
      check(false, "the round trip of vector_add.ptx", "cannot read the file");
   } else {
      check_round_trip("the round trip of vector_add.ptx", text, len);
      free(text);
   }

   text = read_file("reduce_sum.ptx", &len);
   if (!text) {
      check(false, "the round trip of reduce_sum.ptx", "cannot read the file");
   } else {
      check_round_trip("the round trip of reduce_sum.ptx", text, len);
      free(text);
   }

   check_round_trip("the round trip of the hand written module", hand_func,
                    sizeof(hand_func) - 1);
   check_round_trip("the round trip of the .file and .loc module", hand_loc,
                    sizeof(hand_loc) - 1);

   ralloc_free(ctx);
}

static void
test_errors(void)
{
   char long_id[1025];
   char buf[1100];

   check_error("error: an empty input", "",
               "empty input", 1, 1);

   check_error("error: an unknown directive",
               ".version 9.0\n.bogus x;\n",
               "unknown directive .bogus", 2, 1);

   check_error("error: the version 6.4",
               ".version 6.4\n",
               "unsupported PTX version 6.4; the supported versions are 7.0 "
               "to 9.x", 1, 10);

   check_error("error: the version 10.0",
               ".version 10.0\n",
               "unsupported PTX version 10.0; the supported versions are 7.0 "
               "to 9.x", 1, 10);

   check_error("error: an unterminated block comment",
               ".version 9.0\n/* never closed\n",
               "unterminated block comment", 2, 1);

   check_error("error: a missing semicolon",
               ".version 9.0\n.visible .entry f()\n{\n\tmov.u32 %r1, %r2\n}\n",
               "expected ';'", 5, 1);

   check_error("error: an initializer",
               ".version 9.0\n.shared .align 4 .b8 buf[4] = {0};\n",
               "initializers are not supported yet", 2, 29);

   memset(long_id, 'a', 1024);
   long_id[1024] = '\0';
   snprintf(buf, sizeof(buf), ".version 9.0\n%s\n", long_id);
   check_error("error: an identifier of 1024 bytes", buf,
               "identifier too long", 2, 1);

   check_error("error: a bad target",
               ".version 9.0\n.target sm_xyz\n",
               "invalid target sm_xyz", 2, 9);

   check_error("error: a stray closing brace",
               ".version 9.0\n}\n",
               "unexpected '}'", 2, 1);
}

/*
 * The findings of the review of the second pass: an error of the lexer must
 * not be lost, an integer must fit in 64 bits, the module scope lists must not
 * grow one element at a time, a NUL byte must be named and a decimal float
 * must not follow the locale of the process.
 */

/* The module of the review, with the bad text at its end. */
static const char review_module[] =
   ".version 9.0\n"
   ".target sm_120\n"
   ".address_size 64\n"
   ".visible .entry k()\n"
   "{\n"
   "\tret;\n"
   "}\n";

/*
 * A lexical error must not be lost: the module is not accepted and the
 * message, the line and the column of the error are those of the lexer.
 */
static void
test_lexer_errors(void)
{
   char buf[256];

   /* A '#' after the last '}'. */
   snprintf(buf, sizeof(buf), "%s#\n", review_module);
   check_error_len("error: a trailing hash", buf, strlen(buf),
                   "unexpected character '#'", 8, 1);

   /* A bad float literal as the last operand of the last instruction. */
   snprintf(buf, sizeof(buf),
            ".version 9.0\n.target sm_120\n.address_size 64\n"
            ".visible .entry k()\n{\n\tmov.f32 %%f1, 0f123;\n}\n");
   check_error("error: a bad float literal", buf,
               "malformed float literal", 6, 15);

   /* An unterminated block comment after the last '}'. */
   snprintf(buf, sizeof(buf), "%s/* never closed\n", review_module);
   check_error_len("error: an unterminated block comment after the body",
                   buf, strlen(buf), "unterminated block comment", 8, 1);

   /* The same error inside a body is not lost either. */
   snprintf(buf, sizeof(buf),
            ".version 9.0\n.target sm_120\n.address_size 64\n"
            ".visible .entry k()\n{\n\tmov.f32 %%f1, 0f123;\n\tret;\n}\n");
   check_error("error: a bad float literal before the end of the body", buf,
               "malformed float literal", 6, 15);
}

/*
 * An integer literal that does not fit in 64 bits is an error and not a
 * silent clamp.
 */
static void
test_integer_range(void)
{
   void *ctx = ralloc_context(NULL);
   struct ptx_diag diag;
   struct ptx_module *m;
   const struct ptx_insn *insn;
   static const char text_ok[] =
      ".version 9.0\n.target sm_120\n.address_size 64\n"
      ".visible .entry k()\n{\n\t.reg .b64 %r<2>;\n"
      "\tmov.b64 %r1, 18446744073709551615;\n\tret;\n}\n";

   m = parse_text(ctx, text_ok, sizeof(text_ok) - 1, &diag);
   if (!m) {
      check(false, "the largest 64 bit integer is accepted",
            "%s at %u:%u", diag.msg, diag.line, diag.col);
   } else {
      check(true, "the largest 64 bit integer is accepted", "");
      insn = find_insn_opcode(m, "mov", NULL);
      check(insn && insn->noperands == 2 &&
            insn->operands[1].kind == PTX_OPERAND_IMM_INT &&
            insn->operands[1].imm_bits == UINT64_C(0xffffffffffffffff) &&
            insn->operands[1].imm_unsigned,
            "the largest 64 bit integer keeps all of its bits",
            "expected 0xffffffffffffffff, got 0x%016llx",
            insn ? (unsigned long long)insn->operands[1].imm_bits : 0ULL);
   }

   check_error("error: a decimal integer that does not fit in 64 bits",
               ".version 9.0\n.target sm_120\n.address_size 64\n"
               ".visible .entry k()\n{\n\t.reg .b64 %r<2>;\n"
               "\tmov.b64 %r1, 99999999999999999999999;\n\tret;\n}\n",
               "integer out of range", 7, 15);

   check_error("error: a hexadecimal integer that does not fit in 64 bits",
               ".version 9.0\n.target sm_120\n.address_size 64\n"
               ".visible .entry k()\n{\n\t.reg .b64 %r<2>;\n"
               "\tmov.b64 %r1, 0x10000000000000000;\n\tret;\n}\n",
               "integer out of range", 7, 15);

   ralloc_free(ctx);
}

/* The number of module scope declarations of the big module. */
#define BIG_NDECLS 20000

/* Build a module with many module scope declarations. */
static char *
build_big_module(unsigned n)
{
   size_t size = 64 + (size_t)n * 32;
   char *buf = malloc(size);
   size_t used;
   unsigned i;

   if (!buf)
      return NULL;

   used = (size_t)snprintf(buf, size,
                           ".version 9.0\n.target sm_120\n.address_size 64\n");

   /* Time: O(n) Space: O(n) */
   for (i = 0; i < n; i++)
      used += (size_t)snprintf(buf + used, size - used,
                               ".global .b32 g%u;\n", i);

   return buf;
}

/*
 * The module scope lists must not grow one element at a time with a copy of
 * everything that came before: the time and the memory of the parse must grow
 * with the number of declarations and not with its square.
 */
static void
test_many_declarations(void)
{
   void *ctx = ralloc_context(NULL);
   struct ptx_diag diag;
   struct ptx_module *m;
   struct timespec t0, t1;
   double elapsed;
   char *text = build_big_module(BIG_NDECLS);

   if (!text) {
      check(false, "the module with 20000 declarations parses",
            "cannot build the module");
      ralloc_free(ctx);
      return;
   }

   clock_gettime(CLOCK_MONOTONIC, &t0);
   m = parse_text(ctx, text, strlen(text), &diag);
   clock_gettime(CLOCK_MONOTONIC, &t1);

   elapsed = (double)(t1.tv_sec - t0.tv_sec) +
             (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
   printf("# the module with %u declarations parsed in %.3f s\n", BIG_NDECLS,
          elapsed);

   if (!m) {
      check(false, "the module with 20000 declarations parses",
            "%s at %u:%u", diag.msg, diag.line, diag.col);
   } else {
      check(m->ndecls == BIG_NDECLS,
            "the module with 20000 declarations parses",
            "expected %u declarations, got %u", BIG_NDECLS, m->ndecls);

      /*
       * The list of the declarations grows by doubling, so it copies
       * fewer elements than it holds.  The check is in every build: a
       * release build must fail too when the list grows one element at a
       * time.
       */
      check(m->list_copies <= 4 * (uint64_t)BIG_NDECLS,
            "the module with 20000 declarations copies a linear number of "
            "elements",
            "expected at most %llu copies, got %llu",
            (unsigned long long)(4 * (uint64_t)BIG_NDECLS),
            (unsigned long long)m->list_copies);
   }

   /* Every copy of the list that the parser kept would show up here. */
#ifndef NDEBUG
   {
      size_t total = ralloc_total_size(ctx);

      check(total < 64u * 1024u * 1024u,
            "the module with 20000 declarations uses a linear amount of memory",
            "expected less than 64 MiB, got %zu bytes", total);
   }
#endif

   free(text);
   ralloc_free(ctx);
}

/* A NUL byte in the input is named, not reported as an empty character. */
static void
test_nul_byte(void)
{
   static const char text[] = ".version 9.0\n.target sm_120\n\0";

   /* The NUL byte is inside the text, so the length is given. */
   check_error_len("error: a NUL byte", text, sizeof(text) - 1,
                   "unexpected NUL byte", 3, 1);
}

/*
 * A decimal float literal is the same number whatever the locale of the
 * process is: the bits are those of the IEEE 754 binary64 format.
 */
static void
test_float_bits(void)
{
   static const char text[] =
      ".version 9.0\n.target sm_120\n.address_size 64\n"
      ".visible .entry k()\n{\n"
      "\tmov.f64 %rd1, 1.5;\n"
      "\tmov.f64 %rd2, 2.5e-3;\n"
      "\tret;\n}\n";
   void *ctx = ralloc_context(NULL);
   struct ptx_diag diag;
   struct ptx_module *m;
   const struct ptx_insn *insn;
   const char *saved = setlocale(LC_NUMERIC, NULL);
   char *saved_copy = saved ? strdup(saved) : NULL;
   bool german = setlocale(LC_NUMERIC, "de_DE.UTF-8") != NULL;

   printf("# the locale de_DE.UTF-8 is %s\n",
          german ? "available" : "missing");

   m = parse_text(ctx, text, sizeof(text) - 1, &diag);

   if (saved_copy) {
      setlocale(LC_NUMERIC, saved_copy);
      free(saved_copy);
   } else {
      setlocale(LC_NUMERIC, "C");
   }

   if (!m) {
      check(false, "the decimal float literals parse",
            "%s at %u:%u", diag.msg, diag.line, diag.col);
      ralloc_free(ctx);
      return;
   }

   check(true, "the decimal float literals parse", "");

   insn = find_insn_nth(m, "mov", 0);
   check(insn && insn->noperands == 2 &&
         insn->operands[1].kind == PTX_OPERAND_IMM_FLOAT &&
         insn->operands[1].fwidth == 64 &&
         insn->operands[1].fbits == UINT64_C(0x3ff8000000000000),
         "the float literal 1.5 has the bits of 1.5",
         "expected 0x3ff8000000000000, got 0x%016llx",
         insn ? (unsigned long long)insn->operands[1].fbits : 0ULL);

   insn = find_insn_nth(m, "mov", 1);
   check(insn && insn->noperands == 2 &&
         insn->operands[1].kind == PTX_OPERAND_IMM_FLOAT &&
         insn->operands[1].fwidth == 64 &&
         insn->operands[1].fbits == UINT64_C(0x3f647ae147ae147b),
         "the float literal 2.5e-3 has the bits of 2.5e-3",
         "expected 0x3f647ae147ae147b, got 0x%016llx",
         insn ? (unsigned long long)insn->operands[1].fbits : 0ULL);

   ralloc_free(ctx);
}

/*
 * The findings of the review of the first round: the diagnostic of the
 * caller, the scopes of a body, the strings, the range of the integers and
 * of the fields, the suffix of a float literal, the semicolon of the
 * performance directives and the nested vectors.
 */

/* The diagnostic of the caller is cleared when the module is good. */
static void
test_diag_contract(void)
{
   void *ctx = ralloc_context(NULL);
   struct ptx_diag diag;
   struct ptx_module *m;
   static const char text[] =
      ".version 9.0\n.target sm_120\n.address_size 64\n"
      ".visible .entry k()\n{\n\tret;\n}\n";

   memset(&diag, 0, sizeof(diag));
   snprintf(diag.msg, sizeof(diag.msg), "sentinel");
   diag.line = 42;
   diag.col = 7;

   m = parse_text(ctx, text, sizeof(text) - 1, &diag);
   check(m != NULL, "a good module parses", "%s at %u:%u", diag.msg,
         diag.line, diag.col);
   check(diag.msg[0] == '\0' && diag.line == 0 && diag.col == 0,
         "the diagnostic of the caller is cleared when the module is good",
         "got '%s' at %u:%u", diag.msg, diag.line, diag.col);

   /* The diagnostic of the caller is optional. */
   m = ptx_parse(ctx, text, sizeof(text) - 1, NULL);
   check(m != NULL, "a good module parses without a diagnostic", "");
   m = ptx_parse(ctx, ".bogus\n", 7, NULL);
   check(m == NULL, "a bad module is refused without a diagnostic", "");

   ralloc_free(ctx);
}

/* The printer keeps the braces of the nested scopes. */
static void
test_scope_printing(void)
{
   void *ctx = ralloc_context(NULL);
   struct ptx_diag diag;
   struct ptx_module *m = parse_text(ctx, hand_func, sizeof(hand_func) - 1,
                                      &diag);
   char *text = NULL;
   size_t len = 0;

   if (!m) {
      check(false, "the hand written module parses",
            "%s at %u:%u", diag.msg, diag.line, diag.col);
      ralloc_free(ctx);
      return;
   }

   print_to_string(m, &text, &len);
   check(strstr(text, "\t{\n") != NULL && strstr(text, "\t}\n") != NULL,
         "the printer writes the braces of the nested scope", "%s", text);
   check(strstr(text, "\t\tmov.u32\t%r10, 1;\n") != NULL,
         "the statement of the scope is one level deeper", "%s", text);

   free(text);
   ralloc_free(ctx);
}

/* The strings keep the spelling they had in the source. */
static void
test_strings(void)
{
   void *ctx = ralloc_context(NULL);
   struct ptx_diag diag;
   struct ptx_module *m;
   struct ptx_module *m2;
   char *text = NULL;
   size_t len = 0;
   static const char source[] =
      ".version 9.0\n.target sm_120\n.address_size 64\n"
      ".visible .entry k()\n"
      ".pragma \"a\\\"b\";\n"
      "{\n\tret;\n}\n";

   m = parse_text(ctx, source, sizeof(source) - 1, &diag);
   if (!m) {
      check(false, "a pragma with an escaped quote parses",
            "%s at %u:%u", diag.msg, diag.line, diag.col);
      ralloc_free(ctx);
      return;
   }

   check(true, "a pragma with an escaped quote parses", "");
   check(m->funcs[0].pragma != NULL &&
         strcmp(m->funcs[0].pragma, "a\\\"b") == 0,
         "the pragma keeps the spelling of the source", "got '%s'",
         m->funcs[0].pragma ? m->funcs[0].pragma : "");

   print_to_string(m, &text, &len);
   check(strstr(text, ".pragma \"a\\\"b\";") != NULL,
         "the printer writes the string back as it was read", "%s", text);

   m2 = parse_text(ctx, text, len, &diag);
   check(m2 != NULL && m2->funcs[0].pragma != NULL &&
         strcmp(m2->funcs[0].pragma, "a\\\"b") == 0,
         "the string survives the round trip", "%s at %u:%u", diag.msg,
         diag.line, diag.col);

   free(text);
   ralloc_free(ctx);
}

/* A string that cannot be a token and a string that cannot be a label. */
static void
test_string_errors(void)
{
   static const char nul_text[] =
      ".version 9.0\n.target sm_120\n.address_size 64\n"
      ".visible .entry k()\n{\n\t.pragma \"a\0b\";\n\tret;\n}\n";

   check_error_len("error: a NUL byte in a string", nul_text,
                   sizeof(nul_text) - 1,
                   "unexpected NUL byte in a string literal", 6, 10);

   check_error("error: a string as a label",
               ".version 9.0\n.target sm_120\n.address_size 64\n"
               ".visible .entry k()\n{\n\t\"x\":\n\tret;\n}\n",
               "expected a label", 6, 2);
}

/* The value of an integer literal survives the round trip. */
static void
check_imm_literal(const char *name, const char *literal, int64_t want_imm,
                  uint64_t want_bits, bool want_unsigned)
{
   void *ctx = ralloc_context(NULL);
   struct ptx_diag diag;
   struct ptx_module *m;
   struct ptx_module *m2;
   const struct ptx_insn *insn;
   const struct ptx_insn *insn2;
   char text[256];
   char *printed = NULL;
   char round_trip[128];
   size_t len = 0;

   snprintf(text, sizeof(text),
            ".version 9.0\n.target sm_120\n.address_size 64\n"
            ".visible .entry k()\n{\n\tmov.b64 %%r1, %s;\n\tret;\n}\n",
            literal);

   m = parse_text(ctx, text, strlen(text), &diag);
   if (!m) {
      check(false, name, "the parse failed: %s at %u:%u", diag.msg, diag.line,
            diag.col);
      ralloc_free(ctx);
      return;
   }

   insn = find_insn_opcode(m, "mov", NULL);
   check(insn != NULL && insn->noperands == 2 &&
         insn->operands[1].kind == PTX_OPERAND_IMM_INT &&
         insn->operands[1].imm == want_imm &&
         insn->operands[1].imm_bits == want_bits &&
         insn->operands[1].imm_unsigned == want_unsigned,
         name, "expected 0x%016llx (unsigned %d), got 0x%016llx (unsigned %d)",
         (unsigned long long)want_bits, want_unsigned ? 1 : 0,
         insn ? (unsigned long long)insn->operands[1].imm_bits : 0ULL,
         insn && insn->operands[1].imm_unsigned ? 1 : 0);

   print_to_string(m, &printed, &len);
   m2 = parse_text(ctx, printed, len, &diag);
   insn2 = m2 ? find_insn_opcode(m2, "mov", NULL) : NULL;

   snprintf(round_trip, sizeof(round_trip), "%s survives the round trip",
            name);
   check(insn2 != NULL && insn2->noperands == 2 &&
         insn2->operands[1].imm == want_imm &&
         insn2->operands[1].imm_bits == want_bits &&
         insn2->operands[1].imm_unsigned == want_unsigned,
         round_trip, "%s at %u:%u", diag.msg, diag.line, diag.col);

   free(printed);
   ralloc_free(ctx);
}

/* The ends of the range of an integer literal. */
static void
test_integer_extremes(void)
{
   check_imm_literal("the largest signed integer",
                     "9223372036854775807", INT64_MAX,
                     UINT64_C(0x7fffffffffffffff), false);
   check_imm_literal("the smallest signed integer",
                     "-9223372036854775808", INT64_MIN,
                     UINT64_C(0x8000000000000000), false);
   check_imm_literal("the largest unsigned integer",
                     "18446744073709551615", 0,
                     UINT64_C(0xffffffffffffffff), true);
   check_imm_literal("an unsigned integer", "42U", 42, 42, true);

   check_error("error: a negative integer that does not fit",
               ".version 9.0\n.target sm_120\n.address_size 64\n"
               ".visible .entry k()\n{\n\tmov.b64 %r1, -9223372036854775809;\n"
               "\tret;\n}\n",
               "integer out of range", 6, 16);
}

/* A vector of vectors is refused, and the parser does not fall over. */
static void
test_nested_vectors(void)
{
   const unsigned depth = 10000;
   size_t size = 256 + 2 * depth;
   char *text = malloc(size);
   void *ctx = ralloc_context(NULL);
   struct ptx_diag diag;
   struct ptx_module *m;
   char *p;
   unsigned i;

   if (!text) {
      check(false, "the module with nested vectors is refused", "no memory");
      ralloc_free(ctx);
      return;
   }

   p = text;
   p += sprintf(p, ".version 9.0\n.target sm_120\n.address_size 64\n"
                    ".visible .entry k()\n{\n\tmov.b32 %%r1, ");
   /* Time: O(n) Space: O(n) */
   for (i = 0; i < depth; i++)
      *p++ = '{';
   /* Time: O(n) Space: O(n) */
   for (i = 0; i < depth; i++)
      *p++ = '}';
   p += sprintf(p, ";\n\tret;\n}\n");

   m = parse_text(ctx, text, (size_t)(p - text), &diag);
   check(m == NULL, "the module with nested vectors is refused", "");
   check(m == NULL &&
         strcmp(diag.msg, "a vector element may not be a vector") == 0,
         "the nested vector is named", "got '%s'", diag.msg);

   free(text);
   ralloc_free(ctx);
}

/* The fields of a declaration and of a directive have a range. */
static void
test_field_ranges(void)
{
   check_error("error: an alignment that does not fit",
               ".version 9.0\n.shared .align 4294967296 .b8 buf[4];\n",
               "the alignment is out of range", 2, 16);

   check_error("error: an array size that does not fit",
               ".version 9.0\n.shared .b8 buf[4294967296];\n",
               "the array size is out of range", 2, 17);

   check_error("error: an array of no elements",
               ".version 9.0\n.shared .b8 buf[0];\n",
               "the array size is out of range", 2, 17);

   check_error("error: a register count that does not fit",
               ".version 9.0\n.visible .entry k()\n{\n"
               "\t.reg .b64 %r<4294967296>;\n\tret;\n}\n",
               "the register count is out of range", 4, 15);

   check_error("error: a maxntid that does not fit",
               ".version 9.0\n.target sm_120\n"
               ".visible .entry k() .maxntid 4294967296;\n{\n\tret;\n}\n",
               "the value of .maxntid is out of range", 3, 30);
}

/* The float literals follow the PTX ISA. */
static void
test_float_literals(void)
{
   void *ctx = ralloc_context(NULL);
   struct ptx_diag diag;
   struct ptx_module *m;
   const struct ptx_insn *insn;
   static const char source[] =
      ".version 9.0\n.target sm_120\n.address_size 64\n"
      ".visible .entry k()\n{\n\tmov.f32 %f1, 0F3f800000;\n\tret;\n}\n";

   m = parse_text(ctx, source, sizeof(source) - 1, &diag);
   if (!m) {
      check(false, "a hex float with an upper case prefix parses",
            "%s at %u:%u", diag.msg, diag.line, diag.col);
   } else {
      check(true, "a hex float with an upper case prefix parses", "");
      insn = find_insn_opcode(m, "mov", NULL);
      check(insn != NULL && insn->noperands == 2 &&
            insn->operands[1].kind == PTX_OPERAND_IMM_FLOAT &&
            insn->operands[1].fwidth == 32 &&
            insn->operands[1].fbits == UINT64_C(0x3f800000),
            "0F3f800000 is the single precision 1.0", "");
   }

   check_error("error: a decimal float with a suffix",
               ".version 9.0\n.target sm_120\n.address_size 64\n"
               ".visible .entry k()\n{\n\tmov.f32 %f1, 1.5f;\n\tret;\n}\n",
               "a decimal float literal takes no suffix", 6, 15);

   check_error("error: a hex float with a letter after it",
               ".version 9.0\n.target sm_120\n.address_size 64\n"
               ".visible .entry k()\n{\n\tmov.f32 %f1, 0f00000000g;\n\tret;\n}\n",
               "malformed float literal", 6, 15);

   ralloc_free(ctx);
}

/* The performance directives end with a semicolon. */
static void
test_perf_directives(void)
{
   check_error("error: a maxntid without a semicolon",
               ".version 9.0\n.target sm_120\n.visible .entry k()\n{\n"
               "\t.maxntid 256\n\tret;\n}\n",
               "expected ';'", 6, 2);

   check_error("error: a pragma without a semicolon",
               ".version 9.0\n.target sm_120\n.visible .entry k()\n{\n"
               "\t.pragma \"nounroll\"\n\tret;\n}\n",
               "expected ';'", 6, 2);
}

/*
 * An array may have no size at all: the CUDA "extern __shared__" produces
 * ".extern .shared .align 16 .b8 smem[];", whose size is set at launch or at
 * link time.
 */
static void
test_unsized_array(void)
{
   void *ctx = ralloc_context(NULL);
   struct ptx_diag diag;
   struct ptx_module *m;
   char *text = NULL;
   size_t len = 0;
   static const char source[] =
      ".version 9.0\n.target sm_120\n.address_size 64\n"
      ".extern .shared .align 16 .b8 smem[];\n";

   m = parse_text(ctx, source, sizeof(source) - 1, &diag);
   if (!m) {
      check(false, "the module with the unsized array parses",
            "%s at %u:%u", diag.msg, diag.line, diag.col);
      ralloc_free(ctx);
      return;
   }
   check(true, "the module with the unsized array parses", "");

   check(m->ndecls == 1, "the unsized array is a module scope declaration",
         "got %u declarations", m->ndecls);
   if (m->ndecls == 1) {
      const struct ptx_decl *d = &m->decls[0];

      check(d->space == PTX_SPACE_SHARED, "the unsized array is shared",
            "got the space %u", (unsigned)d->space);
      check(d->align == 16, "the unsized array keeps its alignment",
            "got %u", d->align);
      check(d->unsized, "the unsized array has no size of its own", "");
      check(d->count == 0, "the unsized array has no element count",
            "got %u", d->count);
   }

   /* The printer writes the linkage and the empty marks back: the canonical
    * text of this module is the source itself.
    */
   print_to_string(m, &text, &len);
   check(len == sizeof(source) - 1 && memcmp(text, source, len) == 0,
         "the unsized array is printed back the way it was written",
         "got '%s'", text ? text : "");
   free(text);

   check_round_trip("the round trip of the unsized array", source,
                    sizeof(source) - 1);

   ralloc_free(ctx);
}

/*
 * The Review-open items of commit 355d79ed243 (Track #2426).
 */

/*
 * A lexical error must stop the parser.
 *
 * The lexer failed: no token comes any more.  A loop that reads the token
 * again without moving past it would run forever, so every loop that reads
 * a token stops when the token does not come.  Each case below is one of
 * the loops of the parser, and each one must be refused with the error of
 * the lexer.
 */
static void
test_lookahead_errors(void)
{
   /* The linkage loop of a module scope declaration. */
   check_refused("error: a hash after .visible",
                 ".version 9.0\n.visible #\n",
                 "unexpected character '#'", 2, 10);

   /* The same loop, with a second linkage word. */
   check_refused("error: a hash after two linkage words",
                 ".version 9.0\n.visible .visible #\n",
                 "unexpected character '#'", 2, 19);

   /* The loop that reads the numbers of a .loc line. */
   check_refused("error: a hash after the number of a .loc",
                 ".version 9.0\n.target sm_120\n.loc 1 #\n",
                 "unexpected character '#'", 3, 8);

   /* The loop that reads the attributes of a declaration. */
   check_refused("error: a hash after the type of a declaration",
                 ".version 9.0\n.target sm_120\n.shared .b32 #\n",
                 "unexpected character '#'", 3, 14);

   /* The loop that reads the list of the parameters. */
   check_refused("error: a hash after a comma in a parameter list",
                 ".version 9.0\n.target sm_120\n"
                 ".visible .entry f(.param .b32 a, #)\n{\n}\n",
                 "unexpected character '#'", 3, 34);

   /* The loop that reads the elements of a vector. */
   check_refused("error: a hash after a comma in a vector",
                 ".version 9.0\n.target sm_120\n.visible .entry k()\n"
                 "{\n\tmov.v2.b32 {%r1, #};\n}\n",
                 "unexpected character '#'", 5, 19);

   /* The loop that reads the modifiers of an instruction. */
   check_refused("error: a hash after a modifier",
                 ".version 9.0\n.target sm_120\n.visible .entry k()\n"
                 "{\n\tld.global #\n}\n",
                 "unexpected character '#'", 5, 12);

   /* The loop that reads the list of the targets. */
   check_refused("error: a hash after a comma in the target list",
                 ".version 9.0\n.target sm_120, #\n",
                 "unexpected character '#'", 2, 17);

   /* The loop that reads the numbers of a performance directive. */
   check_refused("error: a hash after a comma in .maxntid",
                 ".version 9.0\n.target sm_120\n.visible .entry k()\n"
                 "{\n\t.maxntid 1, #\n}\n",
                 "unexpected character '#'", 5, 14);
}

/*
 * The modifiers that carry a scope.
 *
 * PTX 8.x and 9.x write a scope inside a modifier: "ld.global.L1::
 * no_allocate", "cp.async.bulk.shared::cluster" and "mma.sync.aligned.
 * kind::mxf4".  The scope belongs to the word it stands in, and the
 * modifier is kept whole.
 */
static void
test_scope_modifiers(void)
{
   static const char source[] =
      ".version 9.0\n"
      ".target sm_120\n"
      ".address_size 64\n"
      ".visible .entry k(\n"
      ".param .u64 .ptr .align 1 p0,\n"
      ".param .u64 .ptr .align 1 p1\n"
      ")\n"
      "{\n"
      "\t.reg .b32 %r<4>;\n"
      "\tld.global.L1::no_allocate.b32 %r1, [p0];\n"
      "\tcp.async.bulk.shared::cluster.global [p0], [p1];\n"
      "\tmma.sync.aligned.kind::mxf4.row.col.f32.b16 "
      "{%r1, %r2}, {%r3, %r4};\n"
      "\tret;\n"
      "}\n";
   void *ctx = ralloc_context(NULL);
   struct ptx_diag diag;
   struct ptx_module *m = parse_text(ctx, source, sizeof(source) - 1, &diag);
   const struct ptx_insn *insn;
   char *text = NULL;
   size_t len = 0;

   if (!m) {
      check(false, "the module with the scoped modifiers parses",
            "%s at %u:%u", diag.msg, diag.line, diag.col);
      ralloc_free(ctx);
      return;
   }
   check(true, "the module with the scoped modifiers parses", "");

   insn = find_insn_nth(m, "ld", 0);
   check(insn && insn->nmodifiers == 3 &&
         !strcmp(insn->modifiers[0], "global") &&
         !strcmp(insn->modifiers[1], "L1::no_allocate") &&
         !strcmp(insn->modifiers[2], "b32"),
         "ld.global.L1::no_allocate.b32 keeps the modifier whole",
         "got %u modifiers", insn ? insn->nmodifiers : 0);

   insn = find_insn_nth(m, "cp", 0);
   check(insn && insn->nmodifiers == 4 &&
         !strcmp(insn->modifiers[0], "async") &&
         !strcmp(insn->modifiers[1], "bulk") &&
         !strcmp(insn->modifiers[2], "shared::cluster") &&
         !strcmp(insn->modifiers[3], "global"),
         "cp.async.bulk.shared::cluster.global keeps the modifier whole",
         "got %u modifiers", insn ? insn->nmodifiers : 0);

   insn = find_insn_nth(m, "mma", 0);
   check(insn && insn->nmodifiers == 7 &&
         !strcmp(insn->modifiers[2], "kind::mxf4"),
         "mma.sync.aligned.kind::mxf4 keeps the modifier whole",
         "got %u modifiers", insn ? insn->nmodifiers : 0);

   /* The printer writes the scope back, and the text parses again. */
   print_to_string(m, &text, &len);
   check(text && strstr(text, "ld.global.L1::no_allocate.b32") &&
         strstr(text, "cp.async.bulk.shared::cluster.global") &&
         strstr(text, "mma.sync.aligned.kind::mxf4"),
         "the printer writes the scoped modifiers back", "got '%s'",
         text ? text : "");
   free(text);

   check_round_trip("the round trip of the scoped modifiers", source,
                    sizeof(source) - 1);

   /*
    * A scope that is not followed by an identifier is not a scope: the
    * word ends there and the error names it.
    */
   check_refused("error: a scope with no identifier after it",
                 ".version 9.0\n.target sm_120\n.visible .entry k()\n"
                 "{\n\tld.global.L1:: b32 %r1, [%rd1];\n}\n",
                 "a '::' in a modifier must be followed by an identifier",
                 5, 14);

   /*
    * The scope of a cache modifier is an identifier: it starts with a
    * letter or an underscore, and it may hold digits.  "L1::123" names a
    * number and not a modifier.
    */
   check_refused("error: a cache modifier whose scope is a number",
                 ".version 9.0\n.target sm_120\n.visible .entry k()\n"
                 "{\n\tld.global.L1::123.b32 %r1, [%rd1];\n}\n",
                 "a '::' in a modifier must be followed by an identifier",
                 5, 14);

   /*
    * A "::" that follows no identifier of the word names no scope: there
    * is nothing to qualify.
    */
   check_refused("error: a modifier whose scope names nothing",
                 ".version 9.0\n.target sm_120\n.visible .entry k()\n"
                 "{\n\tld.::foo.b32 %r1, [%rd1];\n}\n",
                 "a '::' in a modifier must follow an identifier", 5, 5);

   ralloc_free(ctx);
}

/*
 * The header of a generated module: the version, the target and the
 * address size.  Every generated module starts with it, so that the tests
 * do not repeat it.
 */
static size_t
module_header(char *text, size_t size)
{
   return (size_t)snprintf(text, size,
                           ".version 9.0\n.target sm_120\n.address_size 64\n");
}

/* What a generated module holds. */
enum module_kind {
   MODULE_SCOPES,   /* one function whose body holds nested scopes */
   MODULE_FUNCTIONS, /* many functions that hold no operand */
   MODULE_DECLS,    /* many module declarations */
   MODULE_PARAMS,   /* one function with many parameters */
};

/*
 * A generated module: the header, then the body that the kind names.
 *
 * The count is the number of scopes, of functions, of declarations or of
 * parameters that the module holds.
 */
static char *
module_generated(enum module_kind kind, unsigned count, size_t *used_out)
{
   size_t size = 1024 + (size_t)count * 40;
   char *text = malloc(size);
   size_t used = 0;
   unsigned i;

   if (!text)
      return NULL;

   used = module_header(text, size);

   switch (kind) {
   case MODULE_SCOPES:
      used += (size_t)snprintf(text + used, size - used,
                               ".visible .entry k()\n{\n");
      for (i = 0; i < count; i++)
         used += (size_t)snprintf(text + used, size - used, "{\n");
      used += (size_t)snprintf(text + used, size - used, "\tret;\n");
      for (i = 0; i < count; i++)
         used += (size_t)snprintf(text + used, size - used, "}\n");
      used += (size_t)snprintf(text + used, size - used, "}\n");
      break;

   case MODULE_FUNCTIONS:
      for (i = 0; i < count; i++)
         used += (size_t)snprintf(text + used, size - used,
                                  ".visible .entry f%u()\n{\n}\n", i);
      break;

   case MODULE_DECLS:
      for (i = 0; i < count; i++)
         used += (size_t)snprintf(text + used, size - used, ".global .b8 g%u;\n",
                                  i);
      break;

   case MODULE_PARAMS:
      used += (size_t)snprintf(text + used, size - used, ".visible .entry k(\n");
      for (i = 0; i < count; i++)
         used += (size_t)snprintf(text + used, size - used, ".param .u64 p%u%s",
                                  i, i + 1 == count ? "\n" : ",\n");
      used += (size_t)snprintf(text + used, size - used, ")\n{\n\tret;\n}\n");
      break;
   }

   *used_out = used;
   return text;
}

/*
 * The scopes of a body are nested a little.
 *
 * The parser reads a nested scope with a recursive call, so the depth of
 * the nesting is the depth of the C stack that the parse uses.  A module
 * that opens more scopes than the limit is refused with a named error
 * before the stack is used, and a module that opens exactly as many as the
 * limit is read.
 */
static void
test_scope_depth(void)
{
   void *ctx = ralloc_context(NULL);
   struct ptx_diag diag;
   struct ptx_module *m;
   char expect[128];
   size_t used = 0;
   char *text;

   /* A body with the scopes at the limit. */
   text = module_generated(MODULE_SCOPES, PTX_MAX_SCOPE_DEPTH, &used);
   if (!text) {
      check(false, "the module with the scopes at the limit is read",
            "cannot build the module");
      ralloc_free(ctx);
      return;
   }

   m = parse_text_timed(ctx, text, used, &diag);
   check(m != NULL, "the module with the scopes at the limit is read",
         "%s at %u:%u", diag.msg, diag.line, diag.col);
   free(text);

   /* One scope more: the parse must be refused, not crash. */
   text = module_generated(MODULE_SCOPES, PTX_MAX_SCOPE_DEPTH + 1, &used);
   if (!text) {
      check(false, "the module with one scope too many is refused",
            "cannot build the module");
      ralloc_free(ctx);
      return;
   }

   snprintf(expect, sizeof(expect), "the scopes are nested more than %u deep",
            PTX_MAX_SCOPE_DEPTH);
   m = parse_text_timed(ctx, text, used, &diag);
   if (m) {
      check(false, "the module with one scope too many is refused",
            "the parse succeeded");
   } else {
      /*
       * The scope that is one too many stands on the line after the last
       * scope that the limit allows: the body opens on the fifth line and
       * every scope opens on a line of its own.
       */
      check(strcmp(diag.msg, expect) == 0 &&
            diag.line == 5 + PTX_MAX_SCOPE_DEPTH + 1 && diag.col == 1,
            "the module with one scope too many is refused",
            "expected '%s' at %u:1, got '%s' at %u:%u", expect,
            5 + PTX_MAX_SCOPE_DEPTH + 1, diag.msg, diag.line, diag.col);
   }

   free(text);
   ralloc_free(ctx);
}

/*
 * The tree of a module has a size.
 *
 * The limits above count the statements of one function and the operands
 * of the module.  The nodes of the tree of the whole module are counted
 * as well: a module with more functions, more module declarations or more
 * parameters than the budget is refused with a named error, before the
 * node that would pass the budget is allocated.
 */

/*
 * Check that a generated module is refused with the named error.
 *
 * The parse runs under the alarm of parse_text_timed, so that a parser
 * that loops on the module fails the case instead of waiting forever.
 */
static void
check_refused_generated(const char *name, const char *text, size_t len,
                        const char *msg)
{
   void *ctx = ralloc_context(NULL);
   struct ptx_diag diag;
   struct ptx_module *m = parse_text_timed(ctx, text, len, &diag);

   if (m) {
      check(false, name, "expected the error '%s', but the parse succeeded",
            msg);
   } else {
      check(strcmp(diag.msg, msg) == 0, name, "expected '%s', got '%s' at %u:%u",
            msg, diag.msg, diag.line, diag.col);
   }

   ralloc_free(ctx);
}

/* Check that a generated module is read. */
static void
check_parsed_generated(const char *name, const char *text, size_t len)
{
   void *ctx = ralloc_context(NULL);
   struct ptx_diag diag;
   struct ptx_module *m = parse_text_timed(ctx, text, len, &diag);

   check(m != NULL, name, "%s at %u:%u", diag.msg, diag.line, diag.col);

   ralloc_free(ctx);
}

static void
test_module_nodes(void)
{
   char expect[128];
   size_t len = 0;
   char *text;

   snprintf(expect, sizeof(expect), "the module holds more than %u nodes",
            PTX_MAX_MODULE_NODES);

   /* A module with as many functions as the budget holds is read. */
   text = module_generated(MODULE_FUNCTIONS, PTX_MAX_MODULE_NODES, &len);
   if (!text) {
      check(false, "the module with the functions at the budget is read",
            "cannot build the module");
   } else {
      check_parsed_generated("the module with the functions at the budget is "
                             "read",
                             text, len);
      free(text);
   }

   /* One function more: the module is refused. */
   text = module_generated(MODULE_FUNCTIONS, PTX_MAX_MODULE_NODES + 1, &len);
   if (!text) {
      check(false, "the module with one function too many is refused",
            "cannot build the module");
   } else {
      check_refused_generated("the module with one function too many is "
                              "refused",
                              text, len, expect);
      free(text);
   }

   /* One module declaration more: the module is refused. */
   text = module_generated(MODULE_DECLS, PTX_MAX_MODULE_NODES + 1, &len);
   if (!text) {
      check(false, "the module with one declaration too many is refused",
            "cannot build the module");
   } else {
      check_refused_generated("the module with one declaration too many is "
                              "refused",
                              text, len, expect);
      free(text);
   }

   /* One parameter more in one function: the module is refused. */
   text = module_generated(MODULE_PARAMS, PTX_MAX_MODULE_NODES + 1, &len);
   if (!text) {
      check(false, "the module with one parameter too many is refused",
            "cannot build the module");
   } else {
      check_refused_generated("the module with one parameter too many is "
                              "refused",
                              text, len, expect);
      free(text);
   }
}

/*
 * The tree of a module has a size.
 *
 * A module that holds more statements than the limit, or more operands
 * than the limit, is refused with a named error.
 */
static void
test_tree_limits(void)
{
   void *ctx = ralloc_context(NULL);
   struct ptx_diag diag;
   struct ptx_module *m;
   char expect[128];
   size_t size;
   size_t used;
   char *text;
   unsigned i;

   /* A function with one statement too many. */
   size = 1024 * 1024;
   text = malloc(size);
   if (!text) {
      check(false, "the module with too many statements is refused",
            "cannot build the module");
      ralloc_free(ctx);
      return;
   }

   used = module_header(text, size);
   used += (size_t)snprintf(text + used, size - used, ".visible .entry k()\n{\n");
   for (i = 0; i <= PTX_MAX_STMTS_PER_FUNC; i++)
      used += (size_t)snprintf(text + used, size - used, "\tret;\n");
   used += (size_t)snprintf(text + used, size - used, "}\n");

   snprintf(expect, sizeof(expect),
            "the function holds more than %u statements",
            PTX_MAX_STMTS_PER_FUNC);
   m = parse_text(ctx, text, used, &diag);
   if (m) {
      check(false, "the module with too many statements is refused",
            "the parse succeeded");
   } else {
      check(strcmp(diag.msg, expect) == 0,
            "the module with too many statements is refused",
            "expected '%s', got '%s' at %u:%u", expect, diag.msg, diag.line,
            diag.col);
   }
   free(text);

   /* A module with one operand too many. */
   size = 16 * 1024 * 1024;
   text = malloc(size);
   if (!text) {
      check(false, "the module with too many operands is refused",
            "cannot build the module");
      ralloc_free(ctx);
      return;
   }

   used = module_header(text, size);
   /*
    * Every instruction holds eight operands, so the module reaches the
    * limit in an eighth of the statements the limit of the statements
    * allows.
    */
   for (i = 0; i < PTX_MAX_OPERANDS_PER_MODULE / 8 + 1; i++) {
      used += (size_t)snprintf(text + used, size - used,
                               ".visible .entry k%u()\n{\n"
                               "\tadd.s64 %%r1, %%r2, %%r3, %%r4, %%r5, "
                               "%%r6, %%r7, %%r8;\n}\n",
                               i);
   }

   snprintf(expect, sizeof(expect),
            "the module holds more than %u operands",
            PTX_MAX_OPERANDS_PER_MODULE);
   m = parse_text(ctx, text, used, &diag);
   if (m) {
      check(false, "the module with too many operands is refused",
            "the parse succeeded");
   } else {
      check(strcmp(diag.msg, expect) == 0,
            "the module with too many operands is refused",
            "expected '%s', got '%s' at %u:%u", expect, diag.msg, diag.line,
            diag.col);
   }
   free(text);

   ralloc_free(ctx);
}

/*
 * The lists of the module copy their elements when they grow, and every
 * list of the tree is counted: the body of a function and the list of its
 * parameters as well as the lists of the module scope.
 */
static void
test_list_copies(void)
{
   void *ctx = ralloc_context(NULL);
   struct ptx_diag diag;
   struct ptx_module *m;
   char *text;
   size_t size = 64 * 1024;
   size_t used;
   unsigned i;

   /* A function with a body of five thousand statements. */
   text = malloc(size);
   if (!text) {
      check(false, "the copies of the body are counted",
            "cannot build the module");
      ralloc_free(ctx);
      return;
   }

   used = (size_t)snprintf(text, size,
                           ".version 9.0\n.target sm_120\n.address_size 64\n"
                           ".visible .entry k()\n{\n");
   for (i = 0; i < 5000; i++)
      used += (size_t)snprintf(text + used, size - used, "\tret;\n");
   used += (size_t)snprintf(text + used, size - used, "}\n");

   m = parse_text(ctx, text, used, &diag);
   if (!m) {
      check(false, "the copies of the body are counted",
            "%s at %u:%u", diag.msg, diag.line, diag.col);
   } else {
      check(m->list_copies >= 5000,
            "the copies of the body are counted",
            "expected at least 5000 copies, got %llu",
            (unsigned long long)m->list_copies);
   }
   free(text);

   /* A function with five hundred parameters. */
   text = malloc(size);
   if (!text) {
      check(false, "the copies of the parameter list are counted",
            "cannot build the module");
      ralloc_free(ctx);
      return;
   }

   used = (size_t)snprintf(text, size,
                           ".version 9.0\n.target sm_120\n.address_size 64\n"
                           ".visible .entry k(");
   for (i = 0; i < 500; i++)
      used += (size_t)snprintf(text + used, size - used,
                               "%s.param .u32 p%u", i ? ", " : "", i);
   used += (size_t)snprintf(text + used, size - used, ")\n{\n\tret;\n}\n");

   m = parse_text(ctx, text, used, &diag);
   if (!m) {
      check(false, "the copies of the parameter list are counted",
            "%s at %u:%u", diag.msg, diag.line, diag.col);
   } else {
      check(m->list_copies >= 500,
            "the copies of the parameter list are counted",
            "expected at least 500 copies, got %llu",
            (unsigned long long)m->list_copies);
   }
   free(text);

   ralloc_free(ctx);
}

/*
 * A module names its version and its target: without them it is not a
 * module.
 */
static void
test_required_directives(void)
{
   check_refused("error: a module without a .version",
                 ".target sm_120\n",
                 "the module has no .version directive", 2, 1);

   check_refused("error: a module without a .target",
                 ".version 9.0\n",
                 "the module has no .target directive", 2, 1);
}

int
main(void)
{
   test_vector_add();
   test_reduce_sum();
   test_hand_cases();
   test_round_trips();
   test_errors();
   test_lexer_errors();
   test_integer_range();
   test_many_declarations();
   test_nul_byte();
   test_float_bits();
   test_diag_contract();
   test_scope_printing();
   test_strings();
   test_string_errors();
   test_integer_extremes();
   test_nested_vectors();
   test_field_ranges();
   test_float_literals();
   test_perf_directives();
   test_unsized_array();
   test_lookahead_errors();
   test_scope_modifiers();
   test_scope_depth();
   test_tree_limits();
   test_module_nodes();
   test_list_copies();
   test_required_directives();

   printf("%u checks passed, %u failed\n", ok_count, bad_count);

   return bad_count == 0 ? 0 : 1;
}
