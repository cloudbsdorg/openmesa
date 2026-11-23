/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: MIT
 */

/*
 * ptx_lex.c: the lexer of libnvptx, the PTX front end of native CUDA.
 *
 * CloudBSD fork, Track #2426.
 *
 * The lexer turns PTX text into tokens.  It knows the shapes of the PTX
 * lexical elements: identifiers (which may start with '%' or '$'), the
 * dotted words of the directives and the modifiers, integers in four bases,
 * float literals, the punctuation, the comments and the strings of the
 * ".file" directive.
 *
 * The parser caps the size of the module it accepts; the lexer only sees
 * the text of one module.
 */

#include "ptx_private.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "util/strtod.h"

static bool
lex_error(struct ptx_diag *diag, unsigned line, unsigned col, const char *fmt,
          ...)
{
   va_list ap;

   /* The first error is the one that counts. */
   if (diag->msg[0] == '\0') {
      va_start(ap, fmt);
      vsnprintf(diag->msg, sizeof(diag->msg), fmt, ap);
      va_end(ap);
      diag->line = line;
      diag->col = col;
   }

   return false;
}

void
ptx_lex_init(struct ptx_lexer *lex, const char *text, size_t len)
{
   lex->text = text;
   lex->len = len;
   lex->pos = 0;
   lex->line = 1;
   lex->col = 1;
}

/* Move the cursor over n bytes, keeping the line and the column. */
static void
lex_skip(struct ptx_lexer *lex, size_t n)
{
   size_t i;

   /* Time: O(n) Space: O(1) */
   for (i = 0; i < n && lex->pos < lex->len; i++) {
      char c = lex->text[lex->pos++];

      if (c == '\n') {
         lex->line++;
         lex->col = 1;
      } else {
         lex->col++;
      }
   }
}

static bool
lex_is_ident_start(char c)
{
   return isalpha((unsigned char)c) || c == '_' || c == '$' || c == '%';
}

static bool
lex_is_ident_char(char c)
{
   return isalnum((unsigned char)c) || c == '_' || c == '$';
}

/*
 * A register or a label may carry a dot in it: "%tid.x" is one token.  The
 * dot is only taken when a name follows it, so that ".b32" after an opcode
 * stays a word of its own.
 */
static bool
lex_ident_char(char c, bool dotted)
{
   if (lex_is_ident_char(c))
      return true;

   return dotted && c == '.';
}

/* Skip a line comment. */
static void
lex_skip_line_comment(struct ptx_lexer *lex)
{
   /* Time: O(n) Space: O(1) */
   while (lex->pos < lex->len && lex->text[lex->pos] != '\n')
      lex_skip(lex, 1);
}

/* Skip a block comment.  Returns false on an error. */
static bool
lex_skip_block_comment(struct ptx_lexer *lex, struct ptx_diag *diag)
{
   unsigned line = lex->line;
   unsigned col = lex->col;
   bool closed = false;

   lex_skip(lex, 2);
   /* Time: O(n) Space: O(1) */
   while (lex->pos < lex->len) {
      if (lex->text[lex->pos] == '*' && lex->pos + 1 < lex->len &&
          lex->text[lex->pos + 1] == '/') {
         lex_skip(lex, 2);
         closed = true;
         break;
      }
      lex_skip(lex, 1);
   }
   if (!closed)
      return lex_error(diag, line, col, "unterminated block comment");

   return true;
}

/* Skip the whitespace and the comments.  Returns false on an error. */
static bool
lex_skip_space(struct ptx_lexer *lex, struct ptx_diag *diag)
{
   /* Time: O(n) Space: O(1) */
   while (lex->pos < lex->len) {
      char c = lex->text[lex->pos];

      if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' ||
          c == '\v') {
         lex_skip(lex, 1);
         continue;
      }

      if (c == '/' && lex->pos + 1 < lex->len &&
          lex->text[lex->pos + 1] == '/') {
         lex_skip_line_comment(lex);
         continue;
      }

      if (c == '/' && lex->pos + 1 < lex->len &&
          lex->text[lex->pos + 1] == '*') {
         if (!lex_skip_block_comment(lex, diag))
            return false;
         continue;
      }

      break;
   }

   return true;
}

/* Copy the text of a token and check that it fits. */
static bool
lex_set_text(struct ptx_token *tok, const char *text, size_t len,
             struct ptx_diag *diag)
{
   if (len > PTX_MAX_IDENT) {
      return lex_error(diag, tok->line, tok->col, "identifier too long");
   }

   memcpy(tok->text, text, len);
   tok->text[len] = '\0';
   tok->len = len;

   return true;
}

/* Read an identifier, a register or a label. */
static bool
lex_ident(struct ptx_lexer *lex, struct ptx_token *tok, struct ptx_diag *diag)
{
   size_t start = lex->pos;
   bool dotted = lex->text[start] == '%' || lex->text[start] == '$';

   lex_skip(lex, 1); /* the first character */
   /* Time: O(n) Space: O(1) */
   while (lex->pos < lex->len) {
      char c = lex->text[lex->pos];

      if (!lex_ident_char(c, dotted))
         break;
      if (c == '.' && (lex->pos + 1 >= lex->len ||
                       !lex_is_ident_char(lex->text[lex->pos + 1])))
         break;
      lex_skip(lex, 1);
   }

   tok->kind = PTX_TOKEN_IDENT;
   return lex_set_text(tok, lex->text + start, lex->pos - start, diag);
}

/* Read a dotted word: a directive or a modifier. */
static bool
lex_dotword(struct ptx_lexer *lex, struct ptx_token *tok, struct ptx_diag *diag)
{
   size_t start = lex->pos;

   lex_skip(lex, 1); /* the dot */
   /* Time: O(n) Space: O(1) */
   while (lex->pos < lex->len && lex_is_ident_char(lex->text[lex->pos]))
      lex_skip(lex, 1);

   if (lex->pos == start + 1) {
      return lex_error(diag, tok->line, tok->col,
                       "unexpected character '%c'", lex->text[start]);
   }

   tok->kind = PTX_TOKEN_DOTWORD;
   return lex_set_text(tok, lex->text + start, lex->pos - start, diag);
}

/*
 * Read a string, as the ".file" and the ".pragma" directives carry one.
 *
 * The text of the string keeps the spelling it had in the source: the
 * escapes stay in it, so that the printer writes back exactly what was
 * read.  A NUL byte is not a character of the language and is refused.
 */
static bool
lex_string(struct ptx_lexer *lex, struct ptx_token *tok, struct ptx_diag *diag)
{
   size_t start;
   size_t len;

   lex_skip(lex, 1); /* the quote */
   start = lex->pos;

   /* Time: O(n) Space: O(1) */
   while (lex->pos < lex->len) {
      char c = lex->text[lex->pos];

      if (c == '"')
         break;
      if (c == '\n')
         return lex_error(diag, tok->line, tok->col,
                          "unterminated string literal");
      if (c == '\0')
         return lex_error(diag, tok->line, tok->col,
                          "unexpected NUL byte in a string literal");
      if (c == '\\' && lex->pos + 1 < lex->len) {
         if (lex->text[lex->pos + 1] == '\0')
            return lex_error(diag, tok->line, tok->col,
                             "unexpected NUL byte in a string literal");
         lex_skip(lex, 2);
         continue;
      }
      lex_skip(lex, 1);
   }

   if (lex->pos >= lex->len) {
      return lex_error(diag, tok->line, tok->col,
                       "unterminated string literal");
   }

   len = lex->pos - start;
   if (len > PTX_MAX_IDENT) {
      return lex_error(diag, tok->line, tok->col, "identifier too long");
   }

   memcpy(tok->text, lex->text + start, len);
   tok->text[len] = '\0';
   tok->len = len;
   tok->kind = PTX_TOKEN_STRING;
   lex_skip(lex, 1); /* the closing quote */

   return true;
}

/* Count the hex digits that follow, at most max of them. */
static unsigned
lex_count_hex(const char *text, size_t len, size_t pos, unsigned max)
{
   unsigned n = 0;

   /* Time: O(n) Space: O(1) */
   while (pos + n < len && n < max && isxdigit((unsigned char)text[pos + n]))
      n++;

   return n;
}

/* Is the character the end of a literal? */
static bool
lex_is_delimiter(char c)
{
   if (c == '\0' || isspace((unsigned char)c))
      return true;

   return strchr("{}()[]<>,;:+-!@=|", c) != NULL;
}

/* Read the "0fXXXXXXXX" and the "0dXXXXXXXXXXXXXXXX" float literals. */
static bool
lex_hex_float(struct ptx_lexer *lex, struct ptx_token *tok, struct ptx_diag *diag)
{
   char kind = lex->text[lex->pos + 1];
   unsigned width = (kind == 'f' || kind == 'F') ? 8 : 16;
   unsigned digits = lex_count_hex(lex->text, lex->len, lex->pos + 2, width);
   uint64_t bits = 0;
   unsigned i;

   if (digits != width) {
      return lex_error(diag, tok->line, tok->col,
                       "malformed float literal");
   }

   /* The literal ends here: another character is not part of it. */
   if (lex->pos + 2 + digits < lex->len &&
       !lex_is_delimiter(lex->text[lex->pos + 2 + digits])) {
      return lex_error(diag, tok->line, tok->col,
                       "malformed float literal");
   }

   /* Time: O(n) Space: O(1) */
   for (i = 0; i < digits; i++) {
      char c = lex->text[lex->pos + 2 + i];
      unsigned v = isdigit((unsigned char)c) ? (unsigned)(c - '0')
                                             : (unsigned)(tolower(c) - 'a' + 10);

      bits = bits * 16 + v;
   }

   tok->kind = PTX_TOKEN_FLOAT;
   tok->fbits = bits;
   tok->fwidth = width == 8 ? 32 : 64;
   lex_skip(lex, 2 + digits);

   return lex_set_text(tok, lex->text + lex->pos - (2 + digits), 2 + digits,
                       diag);
}

/* The bits of a double, in the order of the IEEE 754 binary64 format. */
static uint64_t
lex_double_bits(double d)
{
   uint64_t bits;

   /* The bytes of the double and the bytes of the integer keep the same
    * order on the host, so the copy is the IEEE 754 bit pattern whichever
    * order the host keeps its bytes in.
    */
   memcpy(&bits, &d, sizeof(bits));

   return bits;
}

/* Is the character the suffix of a float literal? */
static bool
lex_is_float_suffix(char c)
{
   return c == 'f' || c == 'F' || c == 'd' || c == 'D';
}

/* Read the digits of a hexadecimal integer. */
static bool
lex_hex_integer(struct ptx_lexer *lex, struct ptx_token *tok,
                struct ptx_diag *diag)
{
   size_t start = lex->pos;

   lex_skip(lex, 2);
   /* Time: O(n) Space: O(1) */
   while (lex->pos < lex->len && isxdigit((unsigned char)lex->text[lex->pos]))
      lex_skip(lex, 1);
   if (lex->pos == start + 2) {
      return lex_error(diag, tok->line, tok->col,
                       "malformed hexadecimal integer");
   }

   return true;
}

/* Read the digits of a binary integer. */
static bool
lex_binary_integer(struct ptx_lexer *lex, struct ptx_token *tok,
                   struct ptx_diag *diag)
{
   size_t start = lex->pos;

   lex_skip(lex, 2);
   /* Time: O(n) Space: O(1) */
   while (lex->pos < lex->len &&
          (lex->text[lex->pos] == '0' || lex->text[lex->pos] == '1'))
      lex_skip(lex, 1);
   if (lex->pos == start + 2) {
      return lex_error(diag, tok->line, tok->col, "malformed binary integer");
   }

   return true;
}

/*
 * Read the digits of a decimal integer.  A leading zero means octal, as in
 * C, and octal is told apart from the other bases here.
 */
static bool
lex_decimal_integer(struct ptx_lexer *lex, struct ptx_token *tok,
                    struct ptx_diag *diag, bool *octal)
{
   size_t start = lex->pos;

   /* Time: O(n) Space: O(1) */
   while (lex->pos < lex->len && isdigit((unsigned char)lex->text[lex->pos]))
      lex_skip(lex, 1);

   if (lex->pos - start > 1 && lex->text[start] == '0') {
      size_t i;

      *octal = true;
      /* Time: O(n) Space: O(1) */
      for (i = start; i < lex->pos; i++) {
         if (lex->text[i] > '7') {
            return lex_error(diag, tok->line, tok->col,
                             "malformed octal integer");
         }
      }
   }

   return true;
}

/* Read the fraction and the exponent of a decimal number. */
static void
lex_decimal_float_part(struct ptx_lexer *lex, bool *is_float)
{
   /* A dot or an exponent makes it a float. */
   if (lex->pos < lex->len && lex->text[lex->pos] == '.') {
      *is_float = true;
      lex_skip(lex, 1);
      /* Time: O(n) Space: O(1) */
      while (lex->pos < lex->len && isdigit((unsigned char)lex->text[lex->pos]))
         lex_skip(lex, 1);
   }
   if (lex->pos < lex->len &&
       (lex->text[lex->pos] == 'e' || lex->text[lex->pos] == 'E')) {
      size_t save = lex->pos;

      lex_skip(lex, 1);
      if (lex->pos < lex->len &&
          (lex->text[lex->pos] == '+' || lex->text[lex->pos] == '-'))
         lex_skip(lex, 1);
      if (lex->pos < lex->len && isdigit((unsigned char)lex->text[lex->pos])) {
         *is_float = true;
         /* Time: O(n) Space: O(1) */
         while (lex->pos < lex->len &&
                isdigit((unsigned char)lex->text[lex->pos]))
            lex_skip(lex, 1);
      } else {
         size_t back = lex->pos - save;

         lex->pos = save;
         lex->col -= (unsigned)back;
      }
   }
}

/* Take the suffix of a number: the unsigned 'U' and the float 'f' or 'd'. */
static void
lex_number_suffix(struct ptx_lexer *lex, bool is_float)
{
   /* An unsigned suffix. */
   if (lex->pos < lex->len &&
       (lex->text[lex->pos] == 'U' || lex->text[lex->pos] == 'u')) {
      lex_skip(lex, 1);
   }

   /* A float suffix, as in "1.5f". */
   if (is_float && lex->pos < lex->len &&
       lex_is_float_suffix(lex->text[lex->pos])) {
      lex_skip(lex, 1);
   }
}

/* Read the value of a float literal. */
static bool
lex_float_value(struct ptx_token *tok, struct ptx_diag *diag)
{
   char *end;
   double d;

   /*
    * The decimal point of a PTX literal is a dot, whatever the locale of
    * the process is: _mesa_strtod always reads the number in the C
    * locale.
    */
   d = _mesa_strtod(tok->text, &end);

   if (end == tok->text) {
      return lex_error(diag, tok->line, tok->col, "malformed float %s",
                       tok->text);
   }
   /*
    * The PTX ISA (section 4.5.2) gives a decimal float literal no suffix:
    * "Unlike C and C++, there is no suffix letter to specify size; literals
    * are always represented in 64-bit double-precision format."  A letter
    * is refused and not ignored, because the width of the literal is the
    * width of the instruction that takes it.
    */
   if (*end != '\0') {
      if (lex_is_float_suffix(*end) && end[1] == '\0') {
         return lex_error(diag, tok->line, tok->col,
                          "a decimal float literal takes no suffix");
      }
      return lex_error(diag, tok->line, tok->col, "malformed float %s",
                       tok->text);
   }
   tok->kind = PTX_TOKEN_FLOAT;
   tok->fbits = lex_double_bits(d);
   tok->fwidth = 64;

   return true;
}

/* Read the value of an integer literal. */
static bool
lex_int_value(struct ptx_token *tok, struct ptx_diag *diag, bool octal)
{
   const char *digits = tok->text;
   int base = 10;
   char *end;
   unsigned long long value;

   tok->kind = PTX_TOKEN_INT;

   if (digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) {
      digits += 2;
      base = 16;
   } else if (digits[0] == '0' && (digits[1] == 'b' || digits[1] == 'B')) {
      digits += 2;
      base = 2;
   } else if (octal) {
      base = 8;
   }

   /*
    * An integer that does not fit in 64 bits is an error and not a
    * silent clamp: strtoull returns ULLONG_MAX and sets ERANGE.  The
    * value is kept as an unsigned magnitude, so that the parser can tell
    * a value that does not fit in int64_t from a negative one.
    */
   errno = 0;
   value = strtoull(digits, &end, base);
   if (errno == ERANGE) {
      return lex_error(diag, tok->line, tok->col, "integer out of range");
   }
   /* The unsigned suffix may follow the number, and nothing else. */
   if (end == digits)
      return lex_error(diag, tok->line, tok->col, "malformed integer %s",
                       tok->text);
   if (*end != '\0' && (*end != 'U' && *end != 'u'))
      return lex_error(diag, tok->line, tok->col, "malformed integer %s",
                       tok->text);
   if (*end != '\0' && end[1] != '\0')
      return lex_error(diag, tok->line, tok->col, "malformed integer %s",
                       tok->text);

   tok->bits = (uint64_t)value;
   /* The U suffix makes the literal unsigned (PTX ISA, section 4.5.1). */
   tok->is_unsigned = *end != '\0';

   return true;
}

/*
 * The character that follows the leading zero of a number, if there is one.
 * It tells the base of the number apart: 'x' and 'b' are the hexadecimal and
 * the binary bases, 'f' and 'd' are the hexadecimal float literals.
 */
static char
lex_number_prefix(const struct ptx_lexer *lex, size_t start)
{
   if (start + 1 >= lex->len || lex->text[start] != '0')
      return '\0';

   return lex->text[start + 1];
}

/* Read a number: an integer in any base or a float. */
static bool
lex_number(struct ptx_lexer *lex, struct ptx_token *tok, struct ptx_diag *diag)
{
   size_t start = lex->pos;
   bool is_float = false;
   bool octal = false;
   char prefix;

   prefix = lex_number_prefix(lex, start);

   if (prefix == 'f' || prefix == 'F' || prefix == 'd' || prefix == 'D')
      return lex_hex_float(lex, tok, diag);

   if (prefix == 'x' || prefix == 'X') {
      if (!lex_hex_integer(lex, tok, diag))
         return false;
   } else if (prefix == 'b' || prefix == 'B') {
      if (!lex_binary_integer(lex, tok, diag))
         return false;
   } else {
      if (!lex_decimal_integer(lex, tok, diag, &octal))
         return false;
      lex_decimal_float_part(lex, &is_float);
   }

   lex_number_suffix(lex, is_float);

   if (!lex_set_text(tok, lex->text + start, lex->pos - start, diag))
      return false;

   if (is_float)
      return lex_float_value(tok, diag);

   return lex_int_value(tok, diag, octal);
}

/* Read one character of punctuation. */
static bool
lex_punct(struct ptx_lexer *lex, struct ptx_token *tok, struct ptx_diag *diag)
{
   static const char punct[] = "{}()[]<>,;:+-!@=|";
   char c = lex->text[lex->pos];

   if (!strchr(punct, c)) {
      return lex_error(diag, tok->line, tok->col, "unexpected character '%c'",
                       c);
   }

   tok->kind = PTX_TOKEN_PUNCT;
   lex_skip(lex, 1);

   return lex_set_text(tok, lex->text + lex->pos - 1, 1, diag);
}

bool
ptx_lex_next(struct ptx_lexer *lex, struct ptx_token *tok, struct ptx_diag *diag)
{
   char c;

   memset(tok, 0, sizeof(*tok));

   if (!lex_skip_space(lex, diag))
      return false;

   tok->line = lex->line;
   tok->col = lex->col;

   if (lex->pos >= lex->len) {
      tok->kind = PTX_TOKEN_EOF;
      return true;
   }

   c = lex->text[lex->pos];

   /*
    * A NUL byte is not a character of the language.  It must be named and
    * not mistaken for the end of the text: strchr would match the
    * terminator of the punctuation table.
    */
   if (c == '\0')
      return lex_error(diag, tok->line, tok->col, "unexpected NUL byte");

   if (c == '"')
      return lex_string(lex, tok, diag);
   if (c == '.')
      return lex_dotword(lex, tok, diag);
   if (lex_is_ident_start(c))
      return lex_ident(lex, tok, diag);
   if (isdigit((unsigned char)c))
      return lex_number(lex, tok, diag);

   return lex_punct(lex, tok, diag);
}
