/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: MIT
 */

/*
 * ptx_parse.c: the parser of libnvptx, the PTX front end of native CUDA.
 *
 * CloudBSD fork, Track #2426.
 *
 * The parser is a recursive descent over the tokens of the lexer.  It reads
 * the module header, the module scope declarations and the functions, and it
 * builds the syntax tree that ptx_print writes back out.
 */

#include "ptx_private.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/ralloc.h"

/* The biggest module the parser accepts. */
#define PTX_MAX_INPUT (64u * 1024u * 1024u)

/* The deepest nesting of scopes the parser accepts. */
#define PTX_MAX_SCOPE_DEPTH 64

/* The types, in the order of enum ptx_type.  The last entry ends the table. */
static const struct {
   const char *name;
   unsigned bits;
} ptx_types[] = {
   { "none", 0 },      { "pred", 1 },      { "b8", 8 },
   { "b16", 16 },      { "b32", 32 },      { "b64", 64 },
   { "b128", 128 },    { "u8", 8 },        { "u16", 16 },
   { "u32", 32 },      { "u64", 64 },      { "s8", 8 },
   { "s16", 16 },      { "s32", 32 },      { "s64", 64 },
   { "f16", 16 },      { "f16x2", 32 },    { "bf16", 16 },
   { "bf16x2", 32 },   { "f32", 32 },      { "f64", 64 },
   { NULL, 0 },
};

_Static_assert(sizeof(ptx_types) / sizeof(ptx_types[0]) == PTX_TYPE_F64 + 2,
               "the type table must match enum ptx_type");

const char *
ptx_type_name(enum ptx_type type)
{
   if ((unsigned)type >= PTX_TYPE_F64 + 1)
      return "none";

   return ptx_types[type].name;
}

unsigned
ptx_type_bits(enum ptx_type type)
{
   if ((unsigned)type >= PTX_TYPE_F64 + 1)
      return 0;

   return ptx_types[type].bits;
}

/* The special registers of the PTX ISA. */
static const char *const ptx_sregs[] = {
   "clock",     "clock64",  "clusterid",  "ctaid",       "globaltimer",
   "globaltimer_hi", "globaltimer_lo", "gridid",   "laneid",     "lanemask_eq",
   "lanemask_lt", "maxnreg", "maxntid",   "nctaid",      "nsmid",
   "ntid",      "nwarpid",  "pm",         "smid",        "tid",
   "warpid",    "warpsz",
};

#define PTX_N_SREGS (sizeof(ptx_sregs) / sizeof(ptx_sregs[0]))

/* A growable array of tree nodes. */
struct ptx_dyn {
   void *ctx;
   void *data;
   unsigned n;
   unsigned cap;
   size_t esize;
   uint64_t copies; /* the elements the array copied when it grew */
};

/* The state of the parser. */
struct ptx_parser {
   void *ctx;
   struct ptx_lexer lex;
   struct ptx_token tok;
   struct ptx_token ahead;
   bool has_ahead;
   bool failed; /* the lexer failed: no token comes any more */
   struct ptx_diag diag;
   /* The number of elements the lists of the module copied when they
    * grew.
    */
   uint64_t copies;
   /* The module scope lists.  They grow by doubling and they move into the
    * module when the whole module has been read.
    */
   struct ptx_dyn targets;
   struct ptx_dyn target_flags;
   struct ptx_dyn funcs;
   struct ptx_dyn decls;
};

static void
dyn_init(struct ptx_dyn *a, void *ctx, size_t esize)
{
   a->ctx = ctx;
   a->data = NULL;
   a->n = 0;
   a->cap = 0;
   a->esize = esize;
   a->copies = 0;
}

/*
 * Add one element to the array.  The array grows by doubling, so the time
 * and the memory of the whole list are linear in the number of elements.
 * The array counts the elements it copies, so that a caller can tell a
 * list that grows by doubling from one that grows one element at a time.
 * Returns false when the memory is gone or when the size of the array would
 * overflow.
 */
static bool
dyn_add(struct ptx_dyn *a, const void *elem)
{
   char *slot;

   if (a->n == a->cap) {
      unsigned cap = a->cap ? a->cap * 2 : 8;
      void *data;

      /* The size of the array must not overflow. */
      if (cap > SIZE_MAX / a->esize)
         return false;

      data = ralloc_size(a->ctx, (size_t)cap * a->esize);
      if (!data)
         return false;

      if (a->n) {
         memcpy(data, a->data, a->n * a->esize);
         a->copies += a->n;
      }
      if (a->data)
         ralloc_free(a->data);
      a->data = data;
      a->cap = cap;
   }

   slot = (char *)a->data + a->n * a->esize;
   memcpy(slot, elem, a->esize);
   a->n++;

   return true;
}

/* Record an error.  The first error wins. */
static bool
ptx_error(struct ptx_parser *p, const struct ptx_token *tok, const char *fmt, ...)
{
   va_list ap;

   if (p->diag.msg[0] == '\0') {
      va_start(ap, fmt);
      vsnprintf(p->diag.msg, sizeof(p->diag.msg), fmt, ap);
      va_end(ap);
      p->diag.line = tok->line;
      p->diag.col = tok->col;
   }

   return false;
}

/* Record that the memory is gone. */
static bool
ptx_oom(struct ptx_parser *p)
{
   return ptx_error(p, &p->tok, "out of memory");
}

/*
 * Read the next token into p->tok.
 *
 * An error of the lexer is sticky: the parser remembers it, the token
 * becomes an error token that no grammar rule accepts and no token comes
 * any more.  The caller may ignore the result: the module is refused at the
 * end of the parse.
 */
static bool
ptx_next(struct ptx_parser *p)
{
   if (p->failed)
      return false;

   if (p->has_ahead) {
      p->tok = p->ahead;
      p->has_ahead = false;
      return true;
   }

   if (!ptx_lex_next(&p->lex, &p->tok, &p->diag)) {
      p->failed = true;
      p->tok.kind = PTX_TOKEN_ERROR;
      p->tok.line = p->diag.line;
      p->tok.col = p->diag.col;
      return false;
   }

   return true;
}

/* The token after the current one. */
static const struct ptx_token *
ptx_peek(struct ptx_parser *p)
{
   if (p->failed)
      return NULL;

   if (!p->has_ahead) {
      if (!ptx_lex_next(&p->lex, &p->ahead, &p->diag)) {
         p->failed = true;
         p->ahead.kind = PTX_TOKEN_ERROR;
         p->ahead.line = p->diag.line;
         p->ahead.col = p->diag.col;
         return NULL;
      }
      p->has_ahead = true;
   }

   return &p->ahead;
}

/* Is the current token the given punctuation? */
static bool
ptx_is_punct(const struct ptx_parser *p, const char *punct)
{
   return p->tok.kind == PTX_TOKEN_PUNCT && !strcmp(p->tok.text, punct);
}

/* Is the current token the given dotted word? */
static bool
ptx_is_dotword(const struct ptx_parser *p, const char *word)
{
   return p->tok.kind == PTX_TOKEN_DOTWORD && !strcmp(p->tok.text, word);
}

/* The state space of a declaration. */
static bool
ptx_space_from_name(const char *name, enum ptx_space *out)
{
   if (!strcmp(name, ".reg")) {
      *out = PTX_SPACE_REG;
      return true;
   }
   if (!strcmp(name, ".sreg")) {
      *out = PTX_SPACE_SREG;
      return true;
   }
   if (!strcmp(name, ".param")) {
      *out = PTX_SPACE_PARAM;
      return true;
   }
   if (!strcmp(name, ".local")) {
      *out = PTX_SPACE_LOCAL;
      return true;
   }
   if (!strcmp(name, ".shared")) {
      *out = PTX_SPACE_SHARED;
      return true;
   }
   if (!strcmp(name, ".global")) {
      *out = PTX_SPACE_GLOBAL;
      return true;
   }
   if (!strcmp(name, ".const")) {
      *out = PTX_SPACE_CONST;
      return true;
   }

   return false;
}

/* The name of a state space, without the leading dot ("reg"). */
const char *
ptx_space_name(enum ptx_space space)
{
   switch (space) {
   case PTX_SPACE_REG:
      return "reg";
   case PTX_SPACE_SREG:
      return "sreg";
   case PTX_SPACE_PARAM:
      return "param";
   case PTX_SPACE_LOCAL:
      return "local";
   case PTX_SPACE_SHARED:
      return "shared";
   case PTX_SPACE_GLOBAL:
      return "global";
   case PTX_SPACE_CONST:
      return "const";
   }

   return "reg";
}

/* The type of a declaration, from its name. */
static bool
ptx_type_from_name(const char *name, enum ptx_type *out)
{
   unsigned i;

   /* Time: O(n) Space: O(1) */
   for (i = 0; i < PTX_TYPE_F64 + 1; i++) {
      if (!strcmp(ptx_types[i].name, name + 1)) {
         *out = (enum ptx_type)i;
         return true;
      }
   }

   return false;
}

/* Is the name one of the special registers? */
static bool
ptx_is_sreg(const char *name)
{
   unsigned i;

   if (name[0] != '%')
      return false;

   /* Time: O(n) Space: O(1) */
   for (i = 0; i < PTX_N_SREGS; i++) {
      size_t len = strlen(ptx_sregs[i]);

      if (strncmp(name + 1, ptx_sregs[i], len) != 0)
         continue;
      if (name[1 + len] == '\0')
         return true;
      if (name[1 + len] == '.' &&
          (name[2 + len] == 'x' || name[2 + len] == 'y' ||
           name[2 + len] == 'z' || name[2 + len] == 'w') &&
          name[3 + len] == '\0')
         return true;
   }

   return false;
}

/* Is the name one of the words that may follow .target? */
static bool
ptx_is_target_flag(const char *name)
{
   return !strcmp(name, "texmode_independent") || !strcmp(name, "debug") ||
          !strcmp(name, "map_f64_to_f32");
}

/* The linkage of a function. */
static bool
ptx_linkage_from_name(const char *name, enum ptx_linkage *out)
{
   if (!strcmp(name, ".visible")) {
      *out = PTX_LINKAGE_VISIBLE;
      return true;
   }
   if (!strcmp(name, ".extern")) {
      *out = PTX_LINKAGE_EXTERN;
      return true;
   }
   if (!strcmp(name, ".weak")) {
      *out = PTX_LINKAGE_WEAK;
      return true;
   }

   return false;
}

static bool ptx_parse_decl(struct ptx_parser *p, struct ptx_decl *d,
                            bool in_params);
static bool ptx_parse_body(struct ptx_parser *p, struct ptx_func *f);
static bool ptx_parse_operand(struct ptx_parser *p, struct ptx_operand *op,
                              bool in_vector);

/* Read the .version of the module. */
static bool
ptx_parse_version(struct ptx_parser *p, struct ptx_module *m)
{
   const struct ptx_token *t;
   const char *text;
   char *end;
   unsigned long major;
   unsigned long minor;

   ptx_next(p);
   t = &p->tok;

   if (t->kind != PTX_TOKEN_FLOAT && t->kind != PTX_TOKEN_INT)
      return ptx_error(p, t, "expected a version number");

   /*
    * The version is read with strtoul and not with sscanf: a number that
    * does not fit in an unsigned long is an error and not a wrapped
    * value.
    */
   text = t->text;
   errno = 0;
   major = strtoul(text, &end, 10);
   if (errno == ERANGE || end == text || *end != '.' || major > UINT_MAX)
      return ptx_error(p, t, "invalid version '%s'", t->text);

   text = end + 1;
   errno = 0;
   minor = strtoul(text, &end, 10);
   if (errno == ERANGE || end == text || *end != '\0' || minor > UINT_MAX)
      return ptx_error(p, t, "invalid version '%s'", t->text);

   if (major < 7 || major > 9) {
      return ptx_error(p, t,
                       "unsupported PTX version %lu.%lu; the supported "
                       "versions are 7.0 to 9.x", major, minor);
   }

   m->version_major = (unsigned)major;
   m->version_minor = (unsigned)minor;

   return ptx_next(p);
}

/* Read one word of the .target list. */
static bool
ptx_parse_target_word(struct ptx_parser *p)
{
   const struct ptx_token *t = &p->tok;
   struct ptx_target target;
   char *end;
   unsigned long sm;

   if (t->kind != PTX_TOKEN_IDENT)
      return ptx_error(p, t, "expected a target name");

   if (ptx_is_target_flag(t->text)) {
      const char *flag = ralloc_strdup(p->ctx, t->text);

      if (!flag)
         return ptx_oom(p);
      if (!dyn_add(&p->target_flags, &flag))
         return ptx_oom(p);

      return ptx_next(p);
   }

   if (strncmp(t->text, "sm_", 3) != 0)
      return ptx_error(p, t, "invalid target %s", t->text);

   sm = strtoul(t->text + 3, &end, 10);
   if (end == t->text + 3 || sm > 999)
      return ptx_error(p, t, "invalid target %s", t->text);

   target.name = ralloc_strdup(p->ctx, t->text);
   if (!target.name)
      return ptx_oom(p);
   target.sm = (unsigned)sm;
   target.suffix = 0;
   if (*end == 'a' || *end == 'f') {
      target.suffix = *end;
      end++;
   }
   if (*end != '\0')
      return ptx_error(p, t, "invalid target %s", t->text);

   if (!dyn_add(&p->targets, &target))
      return ptx_oom(p);

   return ptx_next(p);
}

/* Read the .target list of the module. */
static bool
ptx_parse_target(struct ptx_parser *p)
{
   ptx_next(p);

   /* Time: O(n) Space: O(n) */
   for (;;) {
      if (!ptx_parse_target_word(p))
         return false;
      if (ptx_is_punct(p, ",")) {
         ptx_next(p);
         continue;
      }
      break;
   }

   return true;
}

/* Read the .address_size of the module. */
static bool
ptx_parse_address_size(struct ptx_parser *p, struct ptx_module *m)
{
   const struct ptx_token *t;

   ptx_next(p);
   t = &p->tok;

   if (t->kind != PTX_TOKEN_INT)
      return ptx_error(p, t, "expected an address size");

   if (t->bits != 32 && t->bits != 64)
      return ptx_error(p, t, "invalid address size %" PRIu64, t->bits);

   m->address_size = (unsigned)t->bits;

   return ptx_next(p);
}

/* Skip a .file line: it is parsed and dropped. */
static bool
ptx_skip_file(struct ptx_parser *p)
{
   ptx_next(p); /* the .file */
   if (p->tok.kind == PTX_TOKEN_INT)
      ptx_next(p);
   if (p->tok.kind == PTX_TOKEN_STRING)
      ptx_next(p);
   if (ptx_is_punct(p, ";"))
      ptx_next(p);

   return true;
}

/* Skip a .loc line: it is parsed and dropped. */
static bool
ptx_skip_loc(struct ptx_parser *p)
{
   ptx_next(p); /* the .loc */
   if (ptx_is_dotword(p, ".file"))
      ptx_next(p);
   /* Time: O(n) Space: O(1) */
   while (p->tok.kind == PTX_TOKEN_INT)
      ptx_next(p);
   if (ptx_is_punct(p, ";"))
      ptx_next(p);

   return true;
}

/* Read the attributes of a declaration, in any order. */
static bool
ptx_parse_decl_attrs(struct ptx_parser *p, struct ptx_decl *d)
{
   const struct ptx_token *t;

   /* The attributes of the declaration, in any order. */
   /* Time: O(n) Space: O(1) */
   for (;;) {
      t = &p->tok;
      if (t->kind != PTX_TOKEN_DOTWORD)
         break;

      if (!strcmp(t->text, ".align")) {
         ptx_next(p);
         if (p->tok.kind != PTX_TOKEN_INT)
            return ptx_error(p, &p->tok, "expected an alignment");
         /* The alignment is a positive number that fits in the field. */
         if (p->tok.bits == 0 || p->tok.bits > UINT_MAX)
            return ptx_error(p, &p->tok, "the alignment is out of range");
         d->align = (unsigned)p->tok.bits;
         ptx_next(p);
         continue;
      }
      if (!strcmp(t->text, ".v2") || !strcmp(t->text, ".v4")) {
         d->vec = (unsigned)(t->text[2] - '0');
         ptx_next(p);
         continue;
      }
      if (!strcmp(t->text, ".ptr")) {
         d->is_ptr = true;
         ptx_next(p);
         continue;
      }
      if (!ptx_type_from_name(t->text, &d->type))
         return ptx_error(p, t, "unknown declaration attribute %s", t->text);
      ptx_next(p);
   }

   return true;
}

/*
 * Read a count between two marks: "open INT close".
 *
 * The count is an unsigned number that fits in an unsigned int.  The two
 * messages say what a count that is missing and a count that does not fit
 * are called.  An array may have no count at all ("buf[]"), whose size is
 * set at launch or at link time: unsized_out says where that flag goes,
 * and is NULL for a count that may not be left out.
 */
static bool
ptx_parse_count_between(struct ptx_parser *p, const char *open, const char *close,
                        unsigned *count, bool *unsized_out,
                        const char *expected_msg, const char *range_msg)
{
   if (!ptx_is_punct(p, open))
      return ptx_error(p, &p->tok, "expected '%s'", open);
   ptx_next(p);

   /* An array without a size is sized at launch or at link time. */
   if (unsized_out && ptx_is_punct(p, close)) {
      *unsized_out = true;
      ptx_next(p);
      return true;
   }

   if (p->tok.kind != PTX_TOKEN_INT)
      return ptx_error(p, &p->tok, "%s", expected_msg);
   if (p->tok.bits == 0 || p->tok.bits > UINT_MAX)
      return ptx_error(p, &p->tok, "%s", range_msg);

   *count = (unsigned)p->tok.bits;
   ptx_next(p);

   if (!ptx_is_punct(p, close))
      return ptx_error(p, &p->tok, "expected '%s'", close);
   ptx_next(p);

   return true;
}

/* Read the array size or the parameterized count of a declaration. */
static bool
ptx_parse_decl_suffix(struct ptx_parser *p, struct ptx_decl *d)
{
   if (ptx_is_punct(p, "[")) {
      if (!ptx_parse_count_between(p, "[", "]", &d->count, &d->unsized,
                                   "expected an array size",
                                   "the array size is out of range"))
         return false;
   } else if (ptx_is_punct(p, "<")) {
      if (!ptx_parse_count_between(p, "<", ">", &d->param_count, NULL,
                                   "expected a count",
                                   "the register count is out of range"))
         return false;
   }

   return true;
}

/* Read one declaration. */
static bool
ptx_parse_decl(struct ptx_parser *p, struct ptx_decl *d, bool in_params)
{
   const struct ptx_token *t = &p->tok;

   memset(d, 0, sizeof(*d));
   d->vec = 1;
   d->line = t->line;

   if (t->kind != PTX_TOKEN_DOTWORD)
      return ptx_error(p, t, "expected a declaration");

   if (!ptx_space_from_name(t->text, &d->space))
      return ptx_error(p, t, "unknown directive %s", t->text);

   ptx_next(p);

   if (!ptx_parse_decl_attrs(p, d))
      return false;

   /* The name. */
   t = &p->tok;
   if (t->kind != PTX_TOKEN_IDENT)
      return ptx_error(p, t, "expected a name");
   d->name = ralloc_strdup(p->ctx, t->text);
   if (!d->name)
      return ptx_oom(p);
   ptx_next(p);

   if (!ptx_parse_decl_suffix(p, d))
      return false;

   /* An initializer is not supported yet. */
   if (ptx_is_punct(p, "="))
      return ptx_error(p, &p->tok, "initializers are not supported yet");

   if (!in_params) {
      if (!ptx_is_punct(p, ";"))
         return ptx_error(p, &p->tok, "expected ';'");
      ptx_next(p);
   }

   return true;
}

/* Read the parameter list of a function. */
static bool
ptx_parse_param_list(struct ptx_parser *p, struct ptx_decl **out,
                     unsigned *nout)
{
   struct ptx_dyn params;

   dyn_init(&params, p->ctx, sizeof(struct ptx_decl));
   ptx_next(p); /* the '(' */

   if (ptx_is_punct(p, ")")) {
      ptx_next(p);
      *out = NULL;
      *nout = 0;
      return true;
   }

   /* Time: O(n) Space: O(n) */
   for (;;) {
      struct ptx_decl d;

      if (!ptx_parse_decl(p, &d, true))
         return false;
      if (!dyn_add(&params, &d))
         return ptx_oom(p);

      if (ptx_is_punct(p, ",")) {
         ptx_next(p);
         continue;
      }
      if (ptx_is_punct(p, ")")) {
         ptx_next(p);
         break;
      }
      return ptx_error(p, &p->tok, "expected ',' or ')'");
   }

   *out = params.data;
   *nout = params.n;

   return true;
}

/* Read the address operand of an instruction. */
static bool
ptx_parse_address(struct ptx_parser *p, struct ptx_operand *op)
{
   struct ptx_operand *base;

   op->kind = PTX_OPERAND_ADDRESS;
   ptx_next(p); /* the '[' */

   if (p->tok.kind != PTX_TOKEN_IDENT)
      return ptx_error(p, &p->tok, "expected the base of an address");

   base = ralloc(p->ctx, struct ptx_operand);
   if (!base)
      return ptx_oom(p);
   base->kind = p->tok.text[0] == '%' ? PTX_OPERAND_REG : PTX_OPERAND_SYMBOL;
   base->name = ralloc_strdup(p->ctx, p->tok.text);
   if (!base->name)
      return ptx_oom(p);
   op->base = base;
   ptx_next(p);

   if (ptx_is_punct(p, "+") || ptx_is_punct(p, "-")) {
      bool negative = ptx_is_punct(p, "-");
      uint64_t magnitude;

      ptx_next(p);
      if (ptx_is_punct(p, "-")) {
         negative = !negative;
         ptx_next(p);
      }
      if (p->tok.kind != PTX_TOKEN_INT)
         return ptx_error(p, &p->tok, "expected an offset");
      magnitude = p->tok.bits;
      /*
       * The offset is signed and the magnitude is unsigned: the sign is
       * applied with defined arithmetic, so that the smallest offset does
       * not overflow.
       */
      if (!negative) {
         if (magnitude > (uint64_t)INT64_MAX)
            return ptx_error(p, &p->tok, "the offset is out of range");
         op->offset = (int64_t)magnitude;
      } else if (magnitude == (uint64_t)INT64_MAX + 1u) {
         op->offset = INT64_MIN;
      } else if (magnitude > (uint64_t)INT64_MAX) {
         return ptx_error(p, &p->tok, "the offset is out of range");
      } else {
         op->offset = -(int64_t)magnitude;
      }
      /* The offset is also the immediate value of the operand. */
      op->imm = op->offset;
      ptx_next(p);
   }

   if (!ptx_is_punct(p, "]"))
      return ptx_error(p, &p->tok, "expected ']'");
   ptx_next(p);

   return true;
}

/* Read the vector operand of an instruction. */
static bool
ptx_parse_vector(struct ptx_parser *p, struct ptx_operand *op)
{
   op->kind = PTX_OPERAND_VECTOR;
   op->elems = ralloc_array(p->ctx, struct ptx_operand, PTX_MAX_VECTOR);
   if (!op->elems)
      return ptx_oom(p);
   ptx_next(p); /* the '{' */

   /* Time: O(n) Space: O(1) */
   for (;;) {
      struct ptx_operand e;

      if (op->nelems == PTX_MAX_VECTOR)
         return ptx_error(p, &p->tok, "too many elements in a vector");

      if (!ptx_parse_operand(p, &e, true))
         return false;
      op->elems[op->nelems++] = e;

      if (ptx_is_punct(p, ",")) {
         ptx_next(p);
         continue;
      }
      break;
   }

   if (!ptx_is_punct(p, "}"))
      return ptx_error(p, &p->tok, "expected '}'");
   ptx_next(p);

   return true;
}

/*
 * Fill an integer operand with the value of a token.
 *
 * The magnitude of a literal is an unsigned number and the sign is applied
 * with defined arithmetic, so that the smallest negative value does not
 * overflow (PTX ISA, section 4.5.1: a literal is signed unless its
 * magnitude does not fit in .s64 or it carries the U suffix, in which case
 * it is unsigned).
 */
static bool
ptx_set_int_operand(struct ptx_parser *p, struct ptx_operand *op,
                    const struct ptx_token *tok, bool negative)
{
   uint64_t magnitude = tok->bits;

   op->kind = PTX_OPERAND_IMM_INT;

   if (!negative) {
      if (magnitude > (uint64_t)INT64_MAX) {
         /* The literal does not fit in .s64: it is an unsigned one. */
         op->imm = 0;
         op->imm_bits = magnitude;
         op->imm_unsigned = true;
         return true;
      }
      op->imm = (int64_t)magnitude;
      op->imm_bits = magnitude;
      op->imm_unsigned = tok->is_unsigned;
      return true;
   }

   if (magnitude > (uint64_t)INT64_MAX + 1u)
      return ptx_error(p, tok, "integer out of range");

   if (magnitude == (uint64_t)INT64_MAX + 1u) {
      op->imm = INT64_MIN;
      op->imm_bits = (uint64_t)INT64_MAX + 1u;
      op->imm_unsigned = false;
      return true;
   }

   op->imm = -(int64_t)magnitude;
   op->imm_bits = (uint64_t)op->imm;
   op->imm_unsigned = false;

   return true;
}

/* Read one operand of an instruction. */
static bool
ptx_parse_operand(struct ptx_parser *p, struct ptx_operand *op, bool in_vector)
{
   const struct ptx_token *t = &p->tok;

   memset(op, 0, sizeof(*op));

   switch (t->kind) {
   case PTX_TOKEN_IDENT:
      if (!strcmp(t->text, "_")) {
         op->kind = PTX_OPERAND_SINK;
      } else if (t->text[0] == '%') {
         op->kind = ptx_is_sreg(t->text) ? PTX_OPERAND_SREG : PTX_OPERAND_REG;
      } else {
         op->kind = PTX_OPERAND_SYMBOL;
      }
      op->name = ralloc_strdup(p->ctx, t->text);
      if (!op->name)
         return ptx_oom(p);
      return ptx_next(p);

   case PTX_TOKEN_INT:
      if (!ptx_set_int_operand(p, op, t, false))
         return false;
      return ptx_next(p);

   case PTX_TOKEN_FLOAT:
      op->kind = PTX_OPERAND_IMM_FLOAT;
      op->fbits = t->fbits;
      op->fwidth = t->fwidth;
      return ptx_next(p);

   case PTX_TOKEN_PUNCT:
      if (!strcmp(t->text, "!")) {
         ptx_next(p);
         if (p->tok.kind != PTX_TOKEN_IDENT)
            return ptx_error(p, &p->tok, "expected a predicate");
         op->kind = PTX_OPERAND_REG;
         op->neg = true;
         op->name = ralloc_strdup(p->ctx, p->tok.text);
         if (!op->name)
            return ptx_oom(p);
         return ptx_next(p);
      }
      if (!strcmp(t->text, "["))
         return ptx_parse_address(p, op);
      if (!strcmp(t->text, "{")) {
         /*
          * A vector element may not be a vector: the PTX ISA has no
          * vector of vectors.  Refusing it here also keeps the recursion
          * of the parser and of the printer at one level.
          */
         if (in_vector)
            return ptx_error(p, t, "a vector element may not be a vector");
         return ptx_parse_vector(p, op);
      }
      if (!strcmp(t->text, "-")) {
         ptx_next(p);
         if (p->tok.kind != PTX_TOKEN_INT)
            return ptx_error(p, &p->tok, "expected an integer");
         if (!ptx_set_int_operand(p, op, &p->tok, true))
            return false;
         return ptx_next(p);
      }
      return ptx_error(p, t, "unexpected operand '%s'", t->text);

   default:
      return ptx_error(p, t, "unexpected operand");
   }
}

/* Read one instruction. */
static bool
ptx_parse_insn(struct ptx_parser *p, struct ptx_insn *insn)
{
   memset(insn, 0, sizeof(*insn));
   insn->line = p->tok.line;

   if (ptx_is_punct(p, "@")) {
      ptx_next(p);
      if (ptx_is_punct(p, "!")) {
         insn->guard_neg = true;
         ptx_next(p);
      }
      if (p->tok.kind != PTX_TOKEN_IDENT)
         return ptx_error(p, &p->tok, "expected a predicate");
      insn->guard = ralloc_strdup(p->ctx, p->tok.text);
      if (!insn->guard)
         return ptx_oom(p);
      ptx_next(p);
   }

   if (p->tok.kind != PTX_TOKEN_IDENT)
      return ptx_error(p, &p->tok, "expected an opcode");
   insn->opcode = ralloc_strdup(p->ctx, p->tok.text);
   if (!insn->opcode)
      return ptx_oom(p);
   ptx_next(p);

   /* Time: O(n) Space: O(1) */
   while (p->tok.kind == PTX_TOKEN_DOTWORD) {
      if (insn->nmodifiers == PTX_MAX_MODIFIERS)
         return ptx_error(p, &p->tok, "too many modifiers");
      /* The modifiers keep their order and lose the dot: "ld.global.b32"
       * has the modifiers "global" and "b32".
       */
      insn->modifiers[insn->nmodifiers] =
         ralloc_strdup(p->ctx, p->tok.text + 1);
      if (!insn->modifiers[insn->nmodifiers])
         return ptx_oom(p);
      insn->nmodifiers++;
      ptx_next(p);
   }

   if (!ptx_is_punct(p, ";")) {
      /* Time: O(n) Space: O(1) */
      for (;;) {
         struct ptx_operand op;

         if (insn->noperands == PTX_MAX_OPERANDS)
            return ptx_error(p, &p->tok, "too many operands");
         if (!ptx_parse_operand(p, &op, false))
            return false;
         insn->operands[insn->noperands++] = op;

         if (ptx_is_punct(p, ",")) {
            ptx_next(p);
            continue;
         }
         break;
      }
   }

   if (!ptx_is_punct(p, ";"))
      return ptx_error(p, &p->tok, "expected ';'");
   ptx_next(p);

   return true;
}

/* Read the numbers of a performance directive. */
static bool
ptx_parse_perf_values(struct ptx_parser *p, unsigned *values, unsigned *count,
                      const char *directive)
{
   /* Time: O(n) Space: O(1) */
   for (;;) {
      if (*count == 3)
         return ptx_error(p, &p->tok, "too many values");
      if (p->tok.kind != PTX_TOKEN_INT)
         return ptx_error(p, &p->tok, "expected a number");
      /* The value must fit in the field it is stored in. */
      if (p->tok.bits == 0 || p->tok.bits > UINT_MAX) {
         return ptx_error(p, &p->tok, "the value of %s is out of range",
                          directive);
      }
      values[(*count)++] = (unsigned)p->tok.bits;
      ptx_next(p);

      if (ptx_is_punct(p, ",")) {
         ptx_next(p);
         continue;
      }
      break;
   }

   return true;
}

/* Is the name one of the performance directives of a function? */
static bool
ptx_is_perf_directive(const char *name)
{
   return !strcmp(name, ".maxntid") || !strcmp(name, ".reqntid") ||
          !strcmp(name, ".minnctapersm") || !strcmp(name, ".maxnreg") ||
          !strcmp(name, ".pragma");
}

/*
 * Read the performance directives of a function.  They belong to the
 * function and not to its statement list, wherever they stand: the PTX ISA
 * puts them between the parameter list and the body, and some producers put
 * them inside the body.
 */
static bool
ptx_parse_perf_directives(struct ptx_parser *p, struct ptx_func *f)
{
   /* Time: O(n) Space: O(1) */
   while (p->tok.kind == PTX_TOKEN_DOTWORD &&
          ptx_is_perf_directive(p->tok.text)) {
      bool is_maxntid = !strcmp(p->tok.text, ".maxntid");
      bool is_reqntid = !strcmp(p->tok.text, ".reqntid");
      bool is_minnctapersm = !strcmp(p->tok.text, ".minnctapersm");
      bool is_maxnreg = !strcmp(p->tok.text, ".maxnreg");
      bool is_pragma = !strcmp(p->tok.text, ".pragma");

      ptx_next(p);

      if (is_maxntid) {
         if (!ptx_parse_perf_values(p, f->maxntid, &f->nmaxntid, ".maxntid"))
            return false;
      } else if (is_reqntid) {
         if (!ptx_parse_perf_values(p, f->reqntid, &f->nreqntid, ".reqntid"))
            return false;
      } else if (is_minnctapersm) {
         if (p->tok.kind != PTX_TOKEN_INT)
            return ptx_error(p, &p->tok, "expected a number");
         if (p->tok.bits == 0 || p->tok.bits > UINT_MAX) {
            return ptx_error(p, &p->tok,
                             "the value of .minnctapersm is out of range");
         }
         f->minnctapersm = (unsigned)p->tok.bits;
         ptx_next(p);
      } else if (is_maxnreg) {
         if (p->tok.kind != PTX_TOKEN_INT)
            return ptx_error(p, &p->tok, "expected a number");
         if (p->tok.bits == 0 || p->tok.bits > UINT_MAX) {
            return ptx_error(p, &p->tok,
                             "the value of .maxnreg is out of range");
         }
         f->maxnreg = (unsigned)p->tok.bits;
         ptx_next(p);
      } else if (is_pragma) {
         if (p->tok.kind != PTX_TOKEN_STRING)
            return ptx_error(p, &p->tok, "expected a string");
         f->pragma = ralloc_strdup(p->ctx, p->tok.text);
         if (!f->pragma)
            return ptx_oom(p);
         ptx_next(p);
      }

      /* The directives end with a semicolon. */
      if (!ptx_is_punct(p, ";"))
         return ptx_error(p, &p->tok, "expected ';'");
      ptx_next(p);
   }

   return true;
}

/* Read one statement of a function body. */
static bool
ptx_parse_stmt(struct ptx_parser *p, struct ptx_func *f, struct ptx_dyn *stmts)
{
   struct ptx_stmt stmt;
   const struct ptx_token *t = &p->tok;

   memset(&stmt, 0, sizeof(stmt));
   stmt.line = t->line;

   if (t->kind == PTX_TOKEN_DOTWORD) {
      if (!strcmp(t->text, ".file"))
         return ptx_skip_file(p);
      if (!strcmp(t->text, ".loc"))
         return ptx_skip_loc(p);
      if (ptx_is_perf_directive(t->text))
         return ptx_parse_perf_directives(p, f);

      {
         enum ptx_space space;

         if (!ptx_space_from_name(t->text, &space))
            return ptx_error(p, t, "unknown directive %s", t->text);

         if (!ptx_parse_decl(p, &stmt.u.decl, false))
            return false;
         stmt.kind = PTX_STMT_DECL;
      }
   } else {
      const struct ptx_token *nx = ptx_peek(p);

      if (!nx)
         return false;

      if (nx->kind == PTX_TOKEN_PUNCT && !strcmp(nx->text, ":")) {
         /*
          * A label is an identifier: a string or any other token that
          * stands before a colon is not a label, and printing it would
          * put text in the module that the parser did not read.
          */
         if (t->kind != PTX_TOKEN_IDENT)
            return ptx_error(p, t, "expected a label");
         stmt.kind = PTX_STMT_LABEL;
         stmt.u.label = ralloc_strdup(p->ctx, t->text);
         if (!stmt.u.label)
            return ptx_oom(p);
         ptx_next(p);
         ptx_next(p);
      } else {
         if (!ptx_parse_insn(p, &stmt.u.insn))
            return false;
         stmt.kind = PTX_STMT_INSN;
      }
   }

   if (!dyn_add(stmts, &stmt))
      return ptx_oom(p);

   return true;
}

/*
 * Read the statements of a scope, up to the brace that closes it.
 *
 * A nested scope is a statement of its own: it holds the statements it
 * contains, because a declaration of PTX lives in the scope it was made
 * in.  The depth of the nesting is bounded, so that a module that opens
 * scopes without closing them cannot exhaust the stack.
 */
static bool
ptx_parse_scope(struct ptx_parser *p, struct ptx_func *f, struct ptx_dyn *stmts,
                unsigned depth)
{
   /* Time: O(n) Space: O(n) */
   for (;;) {
      const struct ptx_token *t = &p->tok;

      if (t->kind == PTX_TOKEN_EOF)
         return ptx_error(p, t, "unexpected end of file");

      /* The brace that closes the scope belongs to the caller. */
      if (ptx_is_punct(p, "}"))
         return true;

      if (ptx_is_punct(p, "{")) {
         struct ptx_stmt stmt;
         struct ptx_dyn inner;

         if (depth >= PTX_MAX_SCOPE_DEPTH)
            return ptx_error(p, t, "the scopes are nested too deeply");

         memset(&stmt, 0, sizeof(stmt));
         stmt.kind = PTX_STMT_SCOPE;
         stmt.line = t->line;
         dyn_init(&inner, p->ctx, sizeof(struct ptx_stmt));
         ptx_next(p); /* the '{' */
         if (!ptx_parse_scope(p, f, &inner, depth + 1))
            return false;
         if (!ptx_is_punct(p, "}"))
            return ptx_error(p, &p->tok, "expected '}'");
         ptx_next(p); /* the '}' */
         stmt.u.scope.stmts = inner.data;
         stmt.u.scope.nstmts = inner.n;
         p->copies += inner.copies;
         if (!dyn_add(stmts, &stmt))
            return ptx_oom(p);
         continue;
      }

      if (!ptx_parse_stmt(p, f, stmts))
         return false;
   }
}

/* Read the body of a function. */
static bool
ptx_parse_body(struct ptx_parser *p, struct ptx_func *f)
{
   struct ptx_dyn stmts;

   dyn_init(&stmts, p->ctx, sizeof(struct ptx_stmt));
   ptx_next(p); /* the '{' */

   if (!ptx_parse_scope(p, f, &stmts, 0))
      return false;
   if (!ptx_is_punct(p, "}"))
      return ptx_error(p, &p->tok, "expected '}'");
   ptx_next(p); /* the '}' */

   f->stmts = stmts.data;
   f->nstmts = stmts.n;

   return true;
}

/* Read one function of the module. */
static bool
ptx_parse_func(struct ptx_parser *p)
{
   struct ptx_func f;
   enum ptx_linkage linkage = PTX_LINKAGE_NONE;
   bool is_entry;

   memset(&f, 0, sizeof(f));
   f.line = p->tok.line;

   /* The linkage words come first. */
   /* Time: O(n) Space: O(1) */
   while (p->tok.kind == PTX_TOKEN_DOTWORD &&
          ptx_linkage_from_name(p->tok.text, &linkage))
      ptx_next(p);

   if (ptx_is_dotword(p, ".entry")) {
      is_entry = true;
   } else if (ptx_is_dotword(p, ".func")) {
      is_entry = false;
   } else {
      return ptx_error(p, &p->tok, "expected .entry or .func");
   }
   ptx_next(p);

   f.is_entry = is_entry;
   f.linkage = linkage;

   /* The return parameters of a .func come before its name. */
   if (!is_entry && ptx_is_punct(p, "(")) {
      if (!ptx_parse_param_list(p, &f.ret_params, &f.nret_params))
         return false;
   }

   if (p->tok.kind != PTX_TOKEN_IDENT)
      return ptx_error(p, &p->tok, "expected a function name");
   f.name = ralloc_strdup(p->ctx, p->tok.text);
   if (!f.name)
      return ptx_oom(p);
   ptx_next(p);

   if (ptx_is_punct(p, "(")) {
      if (!ptx_parse_param_list(p, &f.params, &f.nparams))
         return false;
   }

   /* The performance directives stand between the parameters and the body. */
   if (!ptx_parse_perf_directives(p, &f))
      return false;

   if (ptx_is_punct(p, "{")) {
      f.has_body = true;
      if (!ptx_parse_body(p, &f))
         return false;
   } else if (ptx_is_punct(p, ";")) {
      ptx_next(p);
   } else {
      return ptx_error(p, &p->tok, "expected '{' or ';'");
   }

   if (!dyn_add(&p->funcs, &f))
      return ptx_oom(p);

   return true;
}

/* Read a module scope declaration. */
static bool
ptx_parse_module_decl(struct ptx_parser *p)
{
   struct ptx_decl d;
   enum ptx_linkage linkage = PTX_LINKAGE_NONE;

   /* A declaration may carry a linkage word: ".extern .shared ...". */
   /* Time: O(n) Space: O(1) */
   while (p->tok.kind == PTX_TOKEN_DOTWORD &&
          ptx_linkage_from_name(p->tok.text, &linkage))
      ptx_next(p);

   if (!ptx_parse_decl(p, &d, false))
      return false;

   d.linkage = linkage;

   if (!dyn_add(&p->decls, &d))
      return ptx_oom(p);

   return true;
}

/* Read one item of the module scope. */
static bool
ptx_parse_module_item(struct ptx_parser *p, struct ptx_module *m)
{
   const struct ptx_token *t = &p->tok;
   enum ptx_space space;

   if (t->kind != PTX_TOKEN_DOTWORD) {
      if (t->kind == PTX_TOKEN_PUNCT)
         return ptx_error(p, t, "unexpected '%s'", t->text);
      return ptx_error(p, t, "unexpected token '%s'", t->text);
   }

   if (!strcmp(t->text, ".version")) {
      return ptx_parse_version(p, m);
   }
   if (!strcmp(t->text, ".target")) {
      return ptx_parse_target(p);
   }
   if (!strcmp(t->text, ".address_size")) {
      return ptx_parse_address_size(p, m);
   }
   if (!strcmp(t->text, ".file")) {
      return ptx_skip_file(p);
   }
   if (!strcmp(t->text, ".loc")) {
      return ptx_skip_loc(p);
   }
   if (!strcmp(t->text, ".visible") || !strcmp(t->text, ".extern") ||
       !strcmp(t->text, ".weak")) {
      /* A linkage word may start a function or a declaration. */
      const struct ptx_token *next = ptx_peek(p);

      if (next && next->kind == PTX_TOKEN_DOTWORD &&
          (!strcmp(next->text, ".entry") || !strcmp(next->text, ".func")))
         return ptx_parse_func(p);

      return ptx_parse_module_decl(p);
   }
   if (!strcmp(t->text, ".entry") || !strcmp(t->text, ".func")) {
      return ptx_parse_func(p);
   }
   if (ptx_space_from_name(t->text, &space)) {
      return ptx_parse_module_decl(p);
   }

   return ptx_error(p, t, "unknown directive %s", t->text);
}

/* Read the whole module. */
static bool
ptx_parse_module(struct ptx_parser *p, struct ptx_module *m)
{
   /* Time: O(n) Space: O(n) */
   for (;;) {
      if (p->tok.kind == PTX_TOKEN_EOF)
         return true;

      if (!ptx_parse_module_item(p, m))
         return false;
   }
}

struct ptx_module *
ptx_parse(void *mem_ctx, const char *text, size_t len, struct ptx_diag *diag)
{
   struct ptx_parser p;
   struct ptx_module *m;
   struct ptx_diag early;

   memset(&early, 0, sizeof(early));

   if (len > PTX_MAX_INPUT) {
      snprintf(early.msg, sizeof(early.msg),
               "the module is larger than %u bytes", PTX_MAX_INPUT);
      early.line = 1;
      early.col = 1;
      if (diag)
         *diag = early;
      return NULL;
   }

   if (len == 0) {
      snprintf(early.msg, sizeof(early.msg), "empty input");
      early.line = 1;
      early.col = 1;
      if (diag)
         *diag = early;
      return NULL;
   }

   memset(&p, 0, sizeof(p));
   p.ctx = mem_ctx;
   ptx_lex_init(&p.lex, text, len);
   dyn_init(&p.targets, mem_ctx, sizeof(struct ptx_target));
   dyn_init(&p.target_flags, mem_ctx, sizeof(char *));
   dyn_init(&p.funcs, mem_ctx, sizeof(struct ptx_func));
   dyn_init(&p.decls, mem_ctx, sizeof(struct ptx_decl));

   m = rzalloc(mem_ctx, struct ptx_module);
   if (!m) {
      snprintf(early.msg, sizeof(early.msg), "out of memory");
      early.line = 1;
      early.col = 1;
      if (diag)
         *diag = early;
      return NULL;
   }
   m->address_size = 64;

   if (!ptx_next(&p) || !ptx_parse_module(&p, m)) {
      if (diag)
         *diag = p.diag;
      return NULL;
   }

   /*
    * An error of the lexer is sticky: the module is never accepted after
    * one, whatever the grammar made of the error token.
    */
   if (p.failed || p.diag.msg[0] != '\0') {
      if (diag)
         *diag = p.diag;
      return NULL;
   }

   /* The lists of the module scope move into the module. */
   m->targets = p.targets.data;
   m->ntargets = p.targets.n;
   m->target_flags = (const char **)p.target_flags.data;
   m->ntarget_flags = p.target_flags.n;
   m->funcs = p.funcs.data;
   m->nfuncs = p.funcs.n;
   m->decls = p.decls.data;
   m->ndecls = p.decls.n;

   /* The elements the lists of the module copied when they grew. */
   m->list_copies = p.copies + p.targets.copies + p.target_flags.copies +
                    p.funcs.copies + p.decls.copies;

   /* The module is good: the caller sees no error. */
   if (diag)
      memset(diag, 0, sizeof(*diag));

   return m;
}
