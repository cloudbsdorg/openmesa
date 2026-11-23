/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: MIT
 */

/*
 * ptx_private.h: what the lexer and the printer of libnvptx share with the
 * parser.
 *
 * CloudBSD fork, Track #2426.
 */

#ifndef PTX_PRIVATE_H
#define PTX_PRIVATE_H

#include "ptx.h"

/* The name of a state space, without the leading dot ("reg"). */
const char *
ptx_space_name(enum ptx_space space);

/* The kinds of token the lexer produces. */
enum ptx_token_kind {
   PTX_TOKEN_EOF,
   PTX_TOKEN_IDENT,   /* an identifier, a register or a label */
   PTX_TOKEN_DOTWORD, /* a directive or a modifier, with the dot */
   PTX_TOKEN_INT,
   PTX_TOKEN_FLOAT,
   PTX_TOKEN_PUNCT,
   PTX_TOKEN_STRING,
   PTX_TOKEN_ERROR,   /* the lexer failed: no grammar rule accepts it */
};

/* The longest identifier the lexer accepts. */
#define PTX_MAX_IDENT 1023

/* One token.  The text is never longer than PTX_MAX_IDENT bytes. */
struct ptx_token {
   enum ptx_token_kind kind;
   char text[PTX_MAX_IDENT + 1];
   size_t len;
   uint64_t bits;   /* the magnitude of an integer token */
   bool is_unsigned; /* the integer token carried the U suffix */
   uint64_t fbits;   /* the bits of a float token */
   unsigned fwidth;
   unsigned line;
   unsigned col;
};

/* The state of the lexer. */
struct ptx_lexer {
   const char *text;
   size_t len;
   size_t pos;
   unsigned line;
   unsigned col;
};

/* Start the lexer at the start of the text. */
void
ptx_lex_init(struct ptx_lexer *lex, const char *text, size_t len);

/*
 * Read the next token.  Returns false on a lexical error, which is then
 * described in diag.
 */
bool
ptx_lex_next(struct ptx_lexer *lex, struct ptx_token *tok, struct ptx_diag *diag);

#endif
