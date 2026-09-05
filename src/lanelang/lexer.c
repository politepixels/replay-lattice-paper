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

#include "lexer.h"

// errno and ERANGE: strtod reports an out-of-range literal only through errno.
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

// MAX_NUMBER_DIGITS is bytes of a numeric literal that are handed to strtod, including
// the terminator. A longer literal is a lexing error: truncating it could silently drop
// an exponent and change the value.
#define MAX_NUMBER_DIGITS 64

// PUNCTUATION is the punctuation that is one character long, paired with the token kind
// it produces. A colon is absent from this table because it may begin `:=`, which needs a
// second character to decide.
static const struct {
    // character is the character as it appears in the source.
    char character;

    // kind is the token kind emitted when that character is read.
    token_kind_t kind;
} PUNCTUATION[] = {
    {'@', TK_AT},
    {',', TK_COMMA},
    {'(', TK_LPAREN},
    {')', TK_RPAREN},
    {'{', TK_LBRACE},
    {'}', TK_RBRACE},
    {'[', TK_LBRACKET},
    {']', TK_RBRACKET},
    {'+', TK_PLUS},
    {'-', TK_MINUS},
    {'*', TK_STAR},
    {'/', TK_SLASH},
};

// KIND_NAMES pairs each token kind with how a diagnostic should name it: an English
// phrase for the kinds that have no spelling, and the quoted spelling itself for the
// rest. A kind absent here renders as "invalid token", which is what the caller wants for
// TK_ERROR.
static const struct {
    // kind is the token kind being named.
    token_kind_t kind;

    // name is what a diagnostic calls it.
    const char *name;
} KIND_NAMES[] = {
    {TK_EOF, "end of file"},
    {TK_IDENT, "identifier"},
    {TK_QUALIFIER, "lane qualifier"},
    {TK_NUMBER, "number"},
    {TK_AT, "'@'"},
    {TK_ASSIGN, "':='"},
    {TK_COLON, "':'"},
    {TK_COMMA, "','"},
    {TK_LPAREN, "'('"},
    {TK_RPAREN, "')'"},
    {TK_LBRACE, "'{'"},
    {TK_RBRACE, "'}'"},
    {TK_LBRACKET, "'['"},
    {TK_RBRACKET, "']'"},
    {TK_PLUS, "'+'"},
    {TK_MINUS, "'-'"},
    {TK_STAR, "'*'"},
    {TK_SLASH, "'/'"},
    {TK_ARROW, "'=>'"},
};

// lexer_init initialises a lexer over `source`, reporting errors through `diagnostics`.
//
// Takes lexer (lexer_t) which is the token cursor.
// Takes file (char) which is the file being read.
// Takes source (char) which is the source text, borrowed.
// Takes diagnostics (diagnostic_t) which is where diagnostics are collected.
void lexer_init(lexer_t *lexer, const char *file, const char *source, diagnostic_t *diagnostics) {
    lexer->source = source;
    lexer->file = file;
    lexer->cursor = source;
    lexer->line = 1;
    lexer->column = 1;
    lexer->diagnostics = diagnostics;
}

// lexer_advance steps the cursor over one byte, keeping the line and column in step with
// it.
//
// Takes lexer (lexer_t) which is the token cursor.
static void lexer_advance(lexer_t *lexer) {
    if (*lexer->cursor == '\n') {
        lexer->line++; lexer->column = 1;
    }
    else                        {
        lexer->column++;
    }
    lexer->cursor++;
}

// lexer_is_identifier_start reports whether `character` may open an identifier.
//
// Takes character (int) which is the byte under the cursor.
static int lexer_is_identifier_start(const int character) {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z')
           || character == '_';
}

// lexer_is_identifier_continue reports whether `character` may continue an identifier
// already begun.
//
// Takes character (int) which is the byte under the cursor.
static int lexer_is_identifier_continue(const int character) {
    return lexer_is_identifier_start(character) || (character >= '0' && character <= '9');
}

// lexer_is_digit reports whether `character` is a decimal digit.
//
// Takes character (int) which is the byte under the cursor.
static int lexer_is_digit(const int character) {
    return character >= '0' && character <= '9';
}

// lexer_skip_blanks advances past a run of spaces, tabs, carriage returns and newlines.
//
// Takes lexer (lexer_t) which is the token cursor.
static int lexer_skip_blanks(lexer_t *lexer) {
    const char *before = lexer->cursor;
    while (*lexer->cursor == ' ' || *lexer->cursor == '\t'
           || *lexer->cursor == '\r' || *lexer->cursor == '\n') {
        lexer_advance(lexer);
    }
    return lexer->cursor != before;
}

// lexer_skip_line_comment advances past a `//` comment to the end of its line, if the
// cursor is on one. The newline itself is left for the blank run to consume.
//
// Takes lexer (lexer_t) which is the token cursor.
static int lexer_skip_line_comment(lexer_t *lexer) {
    if (lexer->cursor[0] != '/' || lexer->cursor[1] != '/') {
        return 0;
    }
    while (*lexer->cursor && *lexer->cursor != '\n') {
        lexer_advance(lexer);
    }
    return 1;
}

// lexer_skip_trivia advances past everything between tokens: whitespace and `//`
// comments, in any order and any number.
//
// Takes lexer (lexer_t) which is the token cursor.
static void lexer_skip_trivia(lexer_t *lexer) {
    while (lexer_skip_blanks(lexer) || lexer_skip_line_comment(lexer)) { }
}

// lexer_make_token builds a token of kind `kind` from the source between `start` and the
// cursor, positioned at `line` and `column`. A token with no width still reports a span
// one byte wide, so that the renderer has something to draw a caret under.
//
// Takes lexer (lexer_t) which is the token cursor.
// Takes kind (token_kind_t) which is the token kind to emit.
// Takes start (char) which is where the run begins.
// Takes line (int) which is the line number, counted from 1.
// Takes column (int) which is the byte offset within the line, counted from 1.
static token_t lexer_make_token(
    lexer_t *lexer,
    const token_kind_t kind,
    const char *start,
    const int line,
    const int column
) {
    token_t token;
    token.kind = kind;
    token.text = start;
    token.length = (int) (lexer->cursor - start);
    token.number = 0;
    token.span.file = lexer->file;
    token.span.line = line;
    token.span.column = column;
    token.span.length = token.length > 0 ? token.length : 1;
    return token;
}

#ifndef LANELANG_QUALIFIERS_UNRESERVED
// lexer_word_is compares a scanned run against a word.
//
// It sits inside the guard with its only caller, because the build that defines
// LANELANG_QUALIFIERS_UNRESERVED removes that caller, and a helper left outside would
// then be defined and unused, failing that build under -Wall -Wextra.
//
// Takes start (char) which is where the run begins.
// Takes length (size_t) which is how many bytes the run occupies.
// Takes word (char) which is the word to compare against.
// Returns 1 when the run is exactly that word.
static int lexer_word_is(const char *start, const size_t length, const char *word) {
    return length == strlen(word) && memcmp(start, word, length) == 0;
}
#endif

// lexer_scan_identifier scans an identifier, giving the four lane qualifiers their own
// kind and leaving every other keyword for the parser to match itself.
//
// The qualifiers are the only reserved words, because they are the only ones a program
// can bind such that a demand disappears with no diagnostic; every other keyword fails
// loudly when misused.
//
// Takes lexer (lexer_t) which is the token cursor.
// Takes start (char) which is where the run begins.
// Takes line (int) which is the line number, counted from 1.
// Takes column (int) which is the byte offset within the line, counted from 1.
static token_t lexer_scan_identifier(lexer_t *lexer, const char *start, const int line, const int column) {
    while (lexer_is_identifier_continue((unsigned char) *lexer->cursor)) {
        lexer_advance(lexer);
    }
#ifdef LANELANG_QUALIFIERS_UNRESERVED
    return lexer_make_token(lexer, TK_IDENT, start, line, column);
#else
    const size_t length = (size_t) (lexer->cursor - start);
    const int qualifier = lexer_word_is(start, length, "replay")
                          || lexer_word_is(start, length, "exact")
                          || lexer_word_is(start, length, "approx")
                          || lexer_word_is(start, length, "ungrounded");
    return lexer_make_token(lexer, qualifier ? TK_QUALIFIER : TK_IDENT, start, line, column);
#endif
}

// lexer_scan_digits advances past a run of decimal digits, which may be empty.
//
// Takes lexer (lexer_t) which is the token cursor.
static void lexer_scan_digits(lexer_t *lexer) {
    while (lexer_is_digit((unsigned char) *lexer->cursor)) {
        lexer_advance(lexer);
    }
}

// lexer_is_hex_digit reports whether `character` is a hexadecimal digit.
//
// Takes character (int) which is the byte under the cursor.
static int lexer_is_hex_digit(const int character) {
    return lexer_is_digit(character)
        || (character >= 'a' && character <= 'f')
        || (character >= 'A' && character <= 'F');
}

// lexer_scan_hex_digits advances past a run of hexadecimal digits, which may be empty.
//
// Takes lexer (lexer_t) which is the token cursor.
static void lexer_scan_hex_digits(lexer_t *lexer) {
    while (lexer_is_hex_digit((unsigned char) *lexer->cursor)) {
        lexer_advance(lexer);
    }
}

// lexer_scan_exponent scans an `e` or `E` exponent suffix if one is present. A suffix
// with no digits after it is not an exponent at all but the start of whatever follows the
// number, so the cursor is put back.
//
// Takes lexer (lexer_t) which is the token cursor.
static void lexer_scan_exponent(lexer_t *lexer) {
    if (*lexer->cursor != 'e' && *lexer->cursor != 'E') {
        return;
    }
    const char *saved_cursor = lexer->cursor;
    const int saved_line = lexer->line;
    const int saved_column = lexer->column;
    lexer_advance(lexer);
    if (*lexer->cursor == '+' || *lexer->cursor == '-') {
        lexer_advance(lexer);
    }
    if (lexer_is_digit((unsigned char) *lexer->cursor)) {
        lexer_scan_digits(lexer);
        return;
    }
    lexer->cursor = saved_cursor;
    lexer->line = saved_line;
    lexer->column = saved_column;
}

// lexer_number_value reads the value of a numeric token. Its text is a slice of the
// source rather than a terminated string, so it is copied into a bounded buffer before
// strtod sees it.
//
// No end pointer is passed to strtod, because the lexer has already established that
// every byte of `digits` is part of a number literal, so there is no unparseable input
// for it to answer 0 for. The range failure is read, which is the whole reason this takes
// an out parameter: strtod reports overflow through errno and still returns a value, so
// discarding it turns `1e999` into a silent positive infinity and `1e-999` into a silent
// zero.
//
// Takes token (token_t) which is the token under the cursor.
// Takes out_of_range (int) which receives 1 when the literal does not fit a binary64.
static double lexer_number_value(const token_t *token, int *out_of_range) {
    char digits[MAX_NUMBER_DIGITS];
    const int limit = (int) sizeof digits - 1;
    const int copy_length = token->length < limit ? token->length : limit;
    memcpy(digits, token->text, (size_t) copy_length);
    digits[copy_length] = '\0';
    errno = 0;
    const double value = strtod(digits, NULL);
    *out_of_range = (errno == ERANGE);
    return value;
}

// lexer_scan_number scans a numeric literal: either `0x` and a run of hexadecimal digits,
// or a run of decimal digits with an optional fractional part and an optional exponent. A
// literal too long for the value buffer is reported and returned as TK_ERROR.
//
// The `0x` prefix is recognised because without it `0x1234abcd` scans as the number `0`
// followed by the identifier `x1234abcd`: two tokens and no diagnostic, in the one place
// a checksum is written. A `0x` with no digit after it is an error rather than a zero.
// strtod parses what is scanned here without further help, since C99 gives it hexadecimal
// floating constants, and a 32-bit checksum is exact in a binary64 well below 2^53. The
// `p` exponent is not scanned, because a checksum is an integer and `0x1p3` would mean 8;
// written anyway, it scans as `0x1` and the identifier `p3`, which the parser refuses
// where it expects a closing parenthesis.
//
// Takes lexer (lexer_t) which is the token cursor.
// Takes start (char) which is where the run begins.
// Takes line (int) which is the line number, counted from 1.
// Takes column (int) which is the byte offset within the line, counted from 1.
static token_t lexer_scan_number(lexer_t *lexer, const char *start, const int line, const int column) {
    if (*lexer->cursor == '0' && (lexer->cursor[1] == 'x' || lexer->cursor[1] == 'X')) {
        lexer_advance(lexer);
        lexer_advance(lexer);
        if (!lexer_is_hex_digit((unsigned char) *lexer->cursor)) {
            const token_t empty = lexer_make_token(lexer, TK_ERROR, start, line, column);
            if (lexer->diagnostics) {
                diagnostic_error(lexer->diagnostics, empty.span, "'0x' with no hexadecimal digit after it");
            }
            return empty;
        }
        lexer_scan_hex_digits(lexer);
    } else {
        lexer_scan_digits(lexer);
        if (*lexer->cursor == '.') {
            lexer_advance(lexer);
            lexer_scan_digits(lexer);
        }
        lexer_scan_exponent(lexer);
    }
    token_t token = lexer_make_token(lexer, TK_NUMBER, start, line, column);
    if (token.length >= MAX_NUMBER_DIGITS) {
        token.kind = TK_ERROR;
        if (lexer->diagnostics) {
            diagnostic_error(lexer->diagnostics, token.span,
                       "numeric literal longer than %d bytes", MAX_NUMBER_DIGITS - 1);
        }
        return token;
    }
    int out_of_range = 0;
    token.number = lexer_number_value(&token, &out_of_range);
    if (out_of_range) {
        token.kind = TK_ERROR;
        if (lexer->diagnostics) {
            diagnostic_error(lexer->diagnostics, token.span,
                       "numeric literal is outside the range of a binary64");
        }
    }
    return token;
}

// lexer_punctuation_kind finds the token kind for a single punctuation character, or
// TK_ERROR when it is not punctuation.
//
// Takes character (char) which is the byte under the cursor.
static token_kind_t lexer_punctuation_kind(const char character) {
    for (size_t i = 0; i < sizeof PUNCTUATION / sizeof PUNCTUATION[0]; i++) {
        if (PUNCTUATION[i].character == character) {
            return PUNCTUATION[i].kind;
        }
    }
    return TK_ERROR;
}

// lexer_scan_punctuation scans one punctuation token, reporting an unrecognised character
// through the lexer's sink and returning TK_ERROR for it so the parser stops rather than
// guessing.
//
// Takes lexer (lexer_t) which is the token cursor.
// Takes start (char) which is where the run begins.
// Takes line (int) which is the line number, counted from 1.
// Takes column (int) which is the byte offset within the line, counted from 1.
static token_t lexer_scan_punctuation(lexer_t *lexer, const char *start, const int line, const int column) {
    const char character = *lexer->cursor;
    lexer_advance(lexer);
    if (character == ':') {
        if (*lexer->cursor != '=') {
            return lexer_make_token(lexer, TK_COLON, start, line, column);
        }
        lexer_advance(lexer);
        return lexer_make_token(lexer, TK_ASSIGN, start, line, column);
    }
    if (character == '=') {
        if (*lexer->cursor == '>') {
            lexer_advance(lexer);
            return lexer_make_token(lexer, TK_ARROW, start, line, column);
        }
        const token_t bare = lexer_make_token(lexer, TK_ERROR, start, line, column);
        if (lexer->diagnostics) {
            diagnostic_error(lexer->diagnostics, bare.span,
                       "'=' on its own is not an operator: a binding is written ':=' and a "
                       "step function's arrow '=>'");
        }
        return bare;
    }
    const token_kind_t kind = lexer_punctuation_kind(character);
    if (kind != TK_ERROR) {
        return lexer_make_token(lexer, kind, start, line, column);
    }
    const token_t token = lexer_make_token(lexer, TK_ERROR, start, line, column);
    if (lexer->diagnostics) {
        diagnostic_error(lexer->diagnostics, token.span, "unexpected character '%c'", character);
    }
    return token;
}

// lexer_next scans the next token from the source.
//
// Takes lexer (lexer_t) which is the token cursor.
token_t lexer_next(lexer_t *lexer) {
    lexer_skip_trivia(lexer);
    const char *start = lexer->cursor;
    const int line = lexer->line;
    const int column = lexer->column;
    if (*lexer->cursor == '\0') {
        return lexer_make_token(lexer, TK_EOF, start, line, column);
    }
    if (lexer_is_identifier_start((unsigned char) *lexer->cursor)) {
        return lexer_scan_identifier(lexer, start, line, column);
    }
    return lexer_is_digit((unsigned char) *lexer->cursor)
           ? lexer_scan_number(lexer, start, line, column)
           : lexer_scan_punctuation(lexer, start, line, column);
}

// lexer_token_is reports whether `token` is an identifier or a lane qualifier spelling
// exactly `keyword`.
//
// Takes token (token_t) which is the token under the cursor.
// Takes keyword (char) which is the word to match.
int lexer_token_is(const token_t *token, const char *keyword) {
    const int length = (int) strlen(keyword);
    return (token->kind == TK_IDENT || token->kind == TK_QUALIFIER)
           && token->length == length
           && memcmp(token->text, keyword, (size_t) length) == 0;
}

// lexer_token_kind_name is the human-readable name of a token kind, for error messages.
//
// Takes kind (token_kind_t) which is the token kind to name.
const char *lexer_token_kind_name(const token_kind_t kind) {
    for (size_t i = 0; i < sizeof KIND_NAMES / sizeof KIND_NAMES[0]; i++) {
        if (KIND_NAMES[i].kind == kind) {
            return KIND_NAMES[i].name;
        }
    }
    return "invalid token";
}
