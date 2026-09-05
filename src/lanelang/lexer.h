// Copyright 2026 PolitePixels Limited
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef LANELANG_LEXER_H
#define LANELANG_LEXER_H

#include "diagnostic.h"

// token_kind_t is what a token is. Almost every keyword has no kind of its own: the lexer
// emits TK_IDENT for it and the parser decides, which keeps the grammar small. The lane
// qualifiers are the exception, and TK_QUALIFIER says why.
typedef enum {
    // TK_EOF is the end of the source text. The lexer returns this indefinitely once it
    // is reached.
    TK_EOF = 0,

    // TK_IDENT is an identifier, which is also how almost every keyword arrives.
    TK_IDENT,

    // TK_QUALIFIER is `replay`, `exact`, `approx` or `ungrounded`: the four words that
    // may prefix a value's name and state a fact about it. They are reserved and nothing
    // else is, because left as identifiers they can be bound, and a demand then vanishes
    // with no diagnostic: `replay := sum(x)` binds a variable called `replay`, states
    // nothing, and selects the free kernel. A program naming one of these words stops
    // compiling. `lexer_token_is` matches this kind as well as TK_IDENT, so a parser
    // asking whether a token spells a word need not know which kind carries it.
    TK_QUALIFIER,

    // TK_NUMBER is a numeric literal, whose value is in the token's `number` field.
    TK_NUMBER,

    // TK_AT is the annotation introducer, `@`.
    TK_AT,

    // TK_ASSIGN is the binding operator, `:=`.
    TK_ASSIGN,

    // TK_COLON is the named-argument separator, `:`.
    TK_COLON,

    // TK_COMMA is an argument or parameter separator, `,`.
    TK_COMMA,

    // TK_LPAREN is an opening parenthesis, `(`.
    TK_LPAREN,

    // TK_RPAREN is a closing parenthesis, `)`.
    TK_RPAREN,

    // TK_LBRACE is an opening brace, `{`.
    TK_LBRACE,

    // TK_RBRACE is a closing brace, `}`.
    TK_RBRACE,

    // TK_LBRACKET is an opening bracket, `[`, which only appears in `Column[Float]`.
    TK_LBRACKET,

    // TK_RBRACKET is a closing bracket, `]`.
    TK_RBRACKET,

    // TK_PLUS is the addition operator, `+`.
    TK_PLUS,

    // TK_MINUS is the subtraction operator, `-`.
    TK_MINUS,

    // TK_STAR is the multiplication operator, `*`.
    TK_STAR,

    // TK_SLASH is the division operator, `/`.
    TK_SLASH,

    // TK_ARROW is the step-function arrow, `=>`, which separates a step's parameters from
    // its body and appears nowhere else.
    TK_ARROW,

    // TK_ERROR is a character the lexer does not recognise. The lexer has already
    // reported it.
    TK_ERROR
} token_kind_t;

// token_t is one token, together with the source text it came from and where that text
// is.
typedef struct {
    // kind is which token this is.
    token_kind_t kind;

    // span is where the token's text lies in the source, for diagnostics.
    span_t span;

    // text is the start of the token's text inside the source buffer. It is not
    // terminated, so it is only ever read together with `length`.
    const char *text;

    // length is how many bytes of `text` belong to this token.
    int length;

    // number is the value of a numeric literal, valid when `kind` is TK_NUMBER and 0
    // otherwise.
    double number;
} token_t;

// lexer_t is the lexer's position in one source text.
typedef struct {
    // source is the whole source text, borrowed and required to outlive the lexer.
    const char *source;

    // file is the name of that source, copied into every span this lexer produces.
    const char *file;

    // cursor is where the next token will be read from, always within `source`.
    const char *cursor;

    // line is the line the cursor is on, counted from 1.
    int line;

    // column is the byte offset of the cursor within its line, counted from 1.
    int column;

    // diagnostics is where an unrecognised character is reported, or NULL to lex without
    // reporting.
    diagnostic_t *diagnostics;
} lexer_t;

// lexer_init prepares `lexer` to read `source`, whose name is `file` and whose errors are
// reported through `diagnostics`. Both `file` and `source` are borrowed and must outlive
// the lexer.
//
// Takes lexer (lexer_t) which is the token cursor.
// Takes file (char) which is the file being read.
// Takes source (char) which is the source text, borrowed.
// Takes diagnostics (diagnostic_t) which is where diagnostics are collected.
void lexer_init(lexer_t *lexer, const char *file, const char *source, diagnostic_t *diagnostics);

// lexer_next reads the next token, advancing the lexer past it. At the end of the source
// it returns TK_EOF every time, so a caller that fails to stop loops rather than reading
// past the buffer.
//
// Takes lexer (lexer_t) which is the token cursor.
token_t lexer_next(lexer_t *lexer);

// lexer_token_is reports whether the token's text equals `keyword` exactly. An identifier
// or a lane qualifier can match, which are the two kinds a word arrives as; the caller
// asks what a token spells and need not know which kind carries it.
//
// Takes token (token_t) which is the token under the cursor.
// Takes keyword (char) which is the word to match.
int lexer_token_is(const token_t *token, const char *keyword);

// lexer_token_kind_name names a token kind for a diagnostic, as an English phrase for the
// kinds that have no spelling and as the quoted spelling itself for the rest. An unknown
// kind renders as "invalid token".
//
// Takes kind (token_kind_t) which is the token kind to name.
const char *lexer_token_kind_name(token_kind_t kind);

#endif  // LANELANG_LEXER_H
