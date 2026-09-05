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

#include "parse.h"

#include "lexer.h"
#include "parse_internal.h"

#include <stdlib.h>
#include <string.h>

// arena_alloc hands out `size` bytes of arena storage, aligned and zeroed. Returns NULL
// if allocation fails.
//
// Takes arena (arena_t) which is the allocator the tree is built in.
// Takes size (size_t) which is how many bytes are wanted.
void *arena_alloc(arena_t *arena, const size_t size) {
    const size_t aligned = (size + (ARENA_ALIGNMENT - 1u)) & ~(size_t) (ARENA_ALIGNMENT - 1u);
    if (!arena->head || arena->head->used + aligned > arena->head->capacity) {
        const size_t capacity = aligned > ARENA_CHUNK_BYTES ? aligned : ARENA_CHUNK_BYTES;
        chunk_t *chunk = (chunk_t *) calloc(1, sizeof(chunk_t) + capacity);
        if (!chunk) {
            return NULL;
        }
        chunk->capacity = capacity;
        chunk->next = arena->head;
        arena->head = chunk;
    }
    void *block = arena->head->data + arena->head->used;
    arena->head->used += aligned;
    return block;
}

// arena_free releases every chunk an arena holds, leaving the arena itself empty and
// reusable.
//
// Takes arena (arena_t) which is the allocator the tree is built in.
static void arena_free(arena_t *arena) {
    chunk_t *chunk = arena->head;
    while (chunk) {
        chunk_t *next = chunk->next;
        free(chunk);
        chunk = next;
    }
    arena->head = NULL;
}

// ast_free releases a program and every node its arena holds; a NULL program is allowed
// and does nothing.
//
// Takes program (program_t) which is the parsed program.
void ast_free(program_t *program) {
    if (!program) {
        return;
    }
    arena_t *arena = (arena_t *) program->arena;
    if (arena) {
        arena_free(arena); free(arena);
    }
    free(program);
}

// parse_copy_to_arena copies `count` items of `item_size` bytes each into the arena.
// Returns the copy, or NULL both when `count` is zero and when the allocation failed; the
// two are told apart by `parser->failed`, which is set only in the second case.
//
// Takes parser (parser_t) which is the parser's cursor and diagnostic sink.
// Takes items (void) which is the array being copied.
// Takes item_size (size_t) which is how many bytes each entry takes.
// Takes count (int) which is how many items there are.
void *parse_copy_to_arena(parser_t *parser, const void *items, const size_t item_size, const int count) {
    if (count <= 0) {
        return NULL;
    }
    void *copy = arena_alloc(parser->arena, item_size * (size_t) count);
    if (!copy) {
        parser->failed = 1; return NULL;
    }
    memcpy(copy, items, item_size * (size_t) count);
    return copy;
}

// parse_advance consumes the current token and reads the next one.
//
// Takes parser (parser_t) which is the parser's cursor and diagnostic sink.
void parse_advance(parser_t *parser) {
    parser->token = lexer_next(&parser->lexer);
}

// parse_accept consumes the current token if it is of kind `kind`. Returns 1 if it was
// consumed.
//
// Takes parser (parser_t) which is the parser's cursor and diagnostic sink.
// Takes kind (token_kind_t) which is the token kind to accept.
int parse_accept(parser_t *parser, const token_kind_t kind) {
    if (parser->token.kind == kind) {
        parse_advance(parser); return 1;
    }
    return 0;
}

// parse_expect consumes the current token, which must be of kind `kind`. Returns 1 if it
// was, and otherwise reports what was found instead, marks the parse failed and returns
// 0.
//
// Takes parser (parser_t) which is the parser's cursor and diagnostic sink.
// Takes kind (token_kind_t) which is the token kind required.
int parse_expect(parser_t *parser, const token_kind_t kind) {
    if (parser->token.kind == kind) {
        parse_advance(parser); return 1;
    }
    if (!parser->failed) {
        diagnostic_error(parser->diagnostics, parser->token.span, "expected %s, found %s",
                   lexer_token_kind_name(kind), lexer_token_kind_name(parser->token.kind));
    }
    parser->failed = 1;
    return 0;
}

// parse_accept_keyword consumes the current token if it is the identifier `keyword`.
// Returns 1 if it was consumed.
//
// Takes parser (parser_t) which is the parser's cursor and diagnostic sink.
// Takes keyword (char) which is the word to match.
int parse_accept_keyword(parser_t *parser, const char *keyword) {
    if (lexer_token_is(&parser->token, keyword)) {
        parse_advance(parser); return 1;
    }
    return 0;
}

// parse_expect_keyword consumes the current token, which must be the identifier
// `keyword`. Returns 1 if it was, and otherwise reports the absence, marks the parse
// failed and returns 0.
//
// Takes parser (parser_t) which is the parser's cursor and diagnostic sink.
// Takes keyword (char) which is the word to match.
int parse_expect_keyword(parser_t *parser, const char *keyword) {
    if (parse_accept_keyword(parser, keyword)) {
        return 1;
    }
    if (!parser->failed) {
        diagnostic_error(parser->diagnostics, parser->token.span, "expected '%s'", keyword);
    }
    parser->failed = 1;
    return 0;
}

// QUALIFIER_WORDS is the four reserved words, for the near-miss check below.
static const char *const QUALIFIER_WORDS[] = { "replay", "exact", "approx", "ungrounded" };

// word_is_near_miss reports whether `name` is one edit away from `word`, counting a
// substitution, an insertion or a deletion.
//
// Takes name (char) which is the name as written, not NUL-terminated.
// Takes length (int) which is how many bytes the name occupies.
// Takes word (char) which is the reserved word to compare against.
// Returns 1 when the two differ by at most one edit and are not equal.
static int word_is_near_miss(const char *name, const int length, const char *word) {
    const int wlen = (int) strlen(word);
    if (length < wlen - 1 || length > wlen + 1) {
        return 0;
    }
    int i = 0;
    int j = 0;
    int edits = 0;
    while (i < length && j < wlen) {
        if (name[i] == word[j]) {
            i++; j++; continue;
        }
        if (++edits > 1) {
            return 0;
        }
        if (length > wlen) {
            i++;
        } else if (length < wlen) {
            j++;
        } else {
            i++; j++;
        }
    }
    edits += (length - i) + (wlen - j);
    return edits == 1;
}

// qualifier_near_miss returns the reserved word `name` was probably meant to be, or NULL.
//
// Reserving the four words does not catch a misspelled one: `replayy v := sum(x)` binds a
// name the lexer has no reason to doubt, and the failure surfaces one token later as a
// missing `:=`, which describes the punctuation rather than the word. Naming the word
// costs one comparison at a position that has already failed.
//
// Takes name (char) which is the name as written, not NUL-terminated.
// Takes length (int) which is how many bytes the name occupies.
// Returns the intended word, or NULL when the name resembles none of them.
static const char *qualifier_near_miss(const char *name, const int length) {
    for (size_t i = 0; i < sizeof QUALIFIER_WORDS / sizeof QUALIFIER_WORDS[0]; i++) {
        if (word_is_near_miss(name, length, QUALIFIER_WORDS[i])) {
            return QUALIFIER_WORDS[i];
        }
    }
    return NULL;
}

// parse_report_expected reports that a name was expected, naming the replacement when a
// qualifier was just consumed and nothing was left for it to qualify.
//
// A programmer writing `replay := sum(x)` meant a demand and omitted the name, so the
// message says so rather than describing the parser's state. The blame goes on the
// qualifier and not on the cursor, because by the time a caller finds no name,
// parse_qualifiers has consumed the word and the cursor sits on whatever followed it.
//
// Takes parser (parser_t) which is the parser's cursor and diagnostic sink.
// Takes qualifiers (qualifiers_t) which is the prefix just read, or NULL where none was.
// Takes expected (char) which is what the position wanted, for the ordinary message.
static void parse_report_expected(parser_t *parser, const qualifiers_t *qualifiers,
                                  const char *expected) {
    parser->failed = 1;
    if (qualifiers && qualifiers->lane_span.line) {
        diagnostic_error(parser->diagnostics, qualifiers->lane_span,
                   "a lane qualifier states a fact about a NAMED value, and this one names "
                   "nothing: write `%s <name> := <expression>`. The word is reserved, so it "
                   "cannot be the name itself",
                   lane_name(qualifiers->lane));
        return;
    }
    diagnostic_error(parser->diagnostics, parser->token.span, "%s", expected);
}

// parse_qualifier_word reports which qualifier the token at the cursor names, or 0 for
// none.
//
// Takes parser (parser_t) which is the parser's cursor. Outputs: LANE_EXACT, LANE_REPLAY
// or LANE_APPROX for a lane, -1 for `ungrounded`, 0 otherwise.
static int parse_qualifier_word(const parser_t *parser) {
    if (lexer_token_is(&parser->token, "exact"))  {
        return LANE_EXACT;
    }
    if (lexer_token_is(&parser->token, "replay")) {
        return LANE_REPLAY;
    }
    if (lexer_token_is(&parser->token, "approx")) {
        return LANE_APPROX;
    }
    return lexer_token_is(&parser->token, "ungrounded") ? -1 : 0;
}

// record_qualifier stores one qualifier word in `out`, refusing a second of its kind and
// a lane written before `ungrounded`. Refusing the second arrangement leaves one spelling
// of one meaning, so the grammar has no ordering rule to discover, only one to obey.
//
// Takes parser (parser_t) which is the parser's diagnostic sink.
// Takes out (qualifiers_t) which receives the word.
// Takes word (int) which is parse_qualifier_word's verdict: a lane, or -1 for
// `ungrounded`.
// Takes span (span_t) which is the word's source text, for blame.
// Returns 1, or 0 after reporting a qualifier written out of order or twice.
static int record_qualifier(parser_t *parser, qualifiers_t *out, const int word,
                            const span_t span) {
    if (word == -1) {
        if (out->ungrounded || out->lane != LANE_NONE) {
            diagnostic_error(parser->diagnostics, span,
                       "'ungrounded' is written once and before the lane");
            parser->failed = 1;
            return 0;
        }
        out->ungrounded = 1;
        out->ungrounded_span = span;
    } else {
        if (out->lane != LANE_NONE) {
            diagnostic_error(parser->diagnostics, span, "a lane qualifier is written once");
            parser->failed = 1;
            return 0;
        }
        out->lane = (lane_t) word;
        out->lane_span = span;
    }
    return 1;
}

// parse_qualifiers reads the qualifier prefix at the cursor into `out`, consuming nothing
// when there is none.
//
// The lexer gives the four qualifier words their own kind, so the prefix needs no
// lookahead and no save-and-restore: a word that cannot be a name cannot be mistaken for
// one. The `return` veto is separate: that word opens a statement, so a qualifier
// immediately before one is a demand with nothing to attach to, and `replay return
// sum(x)` would otherwise read as a qualified binding of a variable called `return`,
// shadowing the keyword. Left here, the qualifier is not consumed and the caller meets
// the statement it expected.
//
// Takes parser (parser_t) which is the parser's cursor and diagnostic sink.
// Takes out (qualifiers_t) which receives the prefix read.
// Returns 1, or 0 after reporting a qualifier written out of order or twice.
#ifdef LANELANG_QUALIFIERS_UNRESERVED
// parse_qualifiers reads the qualifier prefix at the cursor into `out`, deciding by one
// token of lookahead: a word with no name after it is left as a name.
//
// Takes parser (parser_t) which is the parser's cursor and diagnostic sink.
// Takes out (qualifiers_t) which receives the prefix read.
// Returns 1, or 0 after reporting a qualifier written out of order or twice.
static int parse_qualifiers(parser_t *parser, qualifiers_t *out) {
    memset(out, 0, sizeof *out);
    out->lane = LANE_NONE;
    for (;;) {
        if (parser->token.kind != TK_IDENT) {
            return 1;
        }
        const int word = parse_qualifier_word(parser);
        if (word == 0) {
            return 1;
        }
        const span_t span = parser->token.span;
        const parser_t saved = *parser;
        const int mark = diagnostic_mark(parser->diagnostics);
        parse_advance(parser);
        if (parser->token.kind != TK_IDENT
            || lexer_token_is(&parser->token, "return")) {
            *parser = saved;
            diagnostic_rewind(parser->diagnostics, mark);
            return 1;
        }
        if (!record_qualifier(parser, out, word, span)) {
            return 0;
        }
    }
}
#else
// parse_qualifiers reads the qualifier prefix at the cursor into `out`, consuming nothing
// when there is none. See the block above the guard for what the prefix means.
//
// Takes parser (parser_t) which is the parser's cursor and diagnostic sink.
// Takes out (qualifiers_t) which receives the prefix read.
// Returns 1, or 0 after reporting a qualifier written out of order or twice.
static int parse_qualifiers(parser_t *parser, qualifiers_t *out) {
    memset(out, 0, sizeof *out);
    out->lane = LANE_NONE;
    while (parser->token.kind == TK_QUALIFIER) {
        const int word = parse_qualifier_word(parser);
        const span_t span = parser->token.span;
        const parser_t saved = *parser;
        parse_advance(parser);
        if (lexer_token_is(&parser->token, "return")) {
            *parser = saved;
            return 1;
        }
        if (!record_qualifier(parser, out, word, span)) {
            return 0;
        }
    }
    return 1;
}
#endif

// parse_type accepts a type and discards it. Returns 1 on success, or 0 after reporting a
// failure.
//
// It keeps nothing because nothing reads it: no judgement in this compiler consults a
// declared type, and the checker's whole subject is the lane. The function stays so that
// `func main() Banana` is still refused at the point it is written.
//
// Takes parser (parser_t) which is the parser's cursor and diagnostic sink.
static int parse_type(parser_t *parser) {
    if (parse_accept_keyword(parser, "Float")) {
        return 1;
    }
    if (parse_accept_keyword(parser, "Column")) {
        return parse_expect(parser, TK_LBRACKET)
               && parse_expect_keyword(parser, "Float")
               && parse_expect(parser, TK_RBRACKET);
    }
    parse_report_expected(parser, NULL, "expected a type: Float or Column[Float]");
    parser->failed = 1;
    return 0;
}

// parse_parameter_type reads a parameter's type, adding a note when a qualifier is what
// went wrong.
//
// A qualified parameter is `<lane> <name> <type>`, so writing the lane and the type but
// no name puts the type where the name belongs. The parser takes it as the name, then
// finds no type, and the failure lands one token past the mistake describing neither. The
// note points back at the qualifier and says what the shape is.
//
// Takes parser (parser_t) which is the parser's cursor and diagnostic sink.
// Takes qualifiers (qualifiers_t) which is the prefix read before the name.
// Returns 1, or 0 after reporting a failure.
static int parse_parameter_type(parser_t *parser, const qualifiers_t *qualifiers) {
    if (parse_type(parser)) {
        return 1;
    }
    if (qualifiers->lane_span.line) {
        diagnostic_note(parser->diagnostics, qualifiers->lane_span,
                  "a qualified parameter is written `%s <name> <type>`, so this qualifier "
                  "needs a name between it and the type",
                  lane_name(qualifiers->lane));
    }
    return 0;
}

// parameter_name_is_taken reports whether `name` is already declared among the first
// `count` entries of `parameters`.
//
// Takes parameters (parameter_t) which holds the parameters read so far.
// Takes count (int) which is how many of them are in use.
// Takes name (char) which is the candidate name, not NUL terminated.
// Takes name_length (int) which is how many bytes of `name` belong to it.
static int parameter_name_is_taken(
    const parameter_t *parameters,
    const int count,
    const char *name,
    const int name_length
) {
    for (int i = 0; i < count; i++) {
        if (parameters[i].name_length == name_length
            && memcmp(parameters[i].name, name, (size_t) name_length) == 0) {
            return 1;
        }
    }
    return 0;
}

// parse_parameter_list reads a non-empty comma-separated parameter list into
// `parameters`, which holds MAX_PARAMETERS entries. Returns how many were read, or -1
// after reporting a failure.
//
// A parameter may carry a lane qualifier of its own, written bare before the name.
//
// Takes parser (parser_t) which is the parser's cursor and diagnostic sink.
// Takes parameters (parameter_t) which holds the declared parameters.
static int parse_parameter_list(parser_t *parser, parameter_t *parameters) {
    int count = 0;
    for (;;) {
        if (count >= MAX_PARAMETERS) {
            diagnostic_error(parser->diagnostics, parser->token.span, "too many parameters");
            parser->failed = 1;
            return -1;
        }
        qualifiers_t qualifiers;
        if (!parse_qualifiers(parser, &qualifiers)) {
            return -1;
        }
        if (qualifiers.ungrounded) {
            diagnostic_error(parser->diagnostics, qualifiers.ungrounded_span,
                       "'ungrounded' says the checker cannot see how a value was made, and a "
                       "parameter is the caller's obligation rather than an opaque body, so it "
                       "cannot be written here");
            parser->failed = 1;
            return -1;
        }
        if (parser->token.kind != TK_IDENT) {
            parse_report_expected(parser, &qualifiers, "expected a parameter name");
            return -1;
        }
        if (parameter_name_is_taken(parameters, count, parser->token.text,
                                    parser->token.length)) {
            diagnostic_error(parser->diagnostics, parser->token.span,
                       "parameter '%.*s' is declared twice, and the body can read only the "
                       "later one, so a demand written on the earlier would hold of a value "
                       "the body never reads", parser->token.length, parser->token.text);
            parser->failed = 1;
            return -1;
        }
        parameters[count].lane = qualifiers.lane;
        parameters[count].lane_span = qualifiers.lane_span;
        parameters[count].name = parser->token.text;
        parameters[count].name_length = parser->token.length;
        parameters[count].span = parser->token.span;
        parse_advance(parser);
        if (!parse_parameter_type(parser, &qualifiers)) {
            return -1;
        }
        count++;
        if (!parse_accept(parser, TK_COMMA)) {
            break;
        }
    }
    return count;
}

// parse_parameters reads a declaration's parameter list together with both parentheses.
// Returns how many parameters were read, or -1 after reporting a failure.
//
// Takes parser (parser_t) which is the parser's cursor and diagnostic sink.
// Takes parameters (parameter_t) which holds the declared parameters.
static int parse_parameters(parser_t *parser, parameter_t *parameters) {
    if (!parse_expect(parser, TK_LPAREN)) {
        return -1;
    }
    int count = 0;
    if (parser->token.kind != TK_RPAREN) {
        count = parse_parameter_list(parser, parameters);
        if (count < 0) {
            return -1;
        }
    }
    return !parse_expect(parser, TK_RPAREN) ? -1 : count;
}

// parse_one_binding reads one `@bless(N)? lane? name := expr` binding into `binding`.
//
// The waiver is read first and the qualifier second, so `@bless(N) replay x := ...` is
// the one legal arrangement. `ungrounded` is refused rather than read, because it
// describes what a function returns, a claim about a body the checker cannot see, while a
// binding's right-hand side is a body the checker is looking at.
//
// Takes parser (parser_t) which is the parser's cursor and diagnostic sink.
// Takes binding (bind_t) which receives the binding read.
// Returns 1, or 0 after reporting a failure.
static int parse_one_binding(parser_t *parser, bind_t *binding) {
    const span_t bless_span = parser->token.span;
    binding->bless = parse_bless(parser);
    binding->bless_span = binding->bless ? bless_span : (span_t) {0};
    if (parser->failed) {
        return 0;
    }
    qualifiers_t qualifiers;
    if (!parse_qualifiers(parser, &qualifiers)) {
        return 0;
    }
    if (qualifiers.ungrounded) {
        diagnostic_error(parser->diagnostics, qualifiers.ungrounded_span,
                   "'ungrounded' describes what a function returns, not what a binding "
                   "holds; write it on the declaration whose body the checker cannot see");
        parser->failed = 1;
        return 0;
    }
    if (parser->token.kind != TK_IDENT) {
        parse_report_expected(parser, &qualifiers, "expected a bound name");
        return 0;
    }
    binding->lane = qualifiers.lane;
    binding->lane_span = qualifiers.lane_span;
    binding->name = parser->token.text;
    binding->name_length = parser->token.length;
    binding->span = parser->token.span;
    parse_advance(parser);
    if (parser->token.kind != TK_ASSIGN) {
        const char *meant = qualifier_near_miss(binding->name, binding->name_length);
        if (meant && parser->token.kind == TK_IDENT) {
            diagnostic_error(parser->diagnostics, binding->span,
                       "'%.*s' is not a lane qualifier; did you mean `%s`? The four are "
                       "`exact`, `replay`, `approx` and `ungrounded`, and a word that is not "
                       "one of them reads as the bound name",
                       binding->name_length, binding->name, meant);
            parser->failed = 1;
            return 0;
        }
    }
    if (!parse_expect(parser, TK_ASSIGN)) {
        return 0;
    }
    binding->value = parse_expr(parser);
    return binding->value != NULL;
}

// parse_bindings reads the `name := expr` bindings at the head of a body into `bindings`,
// which holds MAX_BINDINGS entries, stopping at `return`.
//
// Takes parser (parser_t) which is the parser's cursor and diagnostic sink.
// Takes bindings (bind_t) which holds the bindings in scope.
// Returns how many were read, or -1 after reporting a failure.
int parse_bindings(parser_t *parser, bind_t *bindings) {
    int count = 0;
    while ((parser->token.kind == TK_IDENT || parser->token.kind == TK_AT
            || parser->token.kind == TK_QUALIFIER)
           && !lexer_token_is(&parser->token, "return")) {
        if (count >= MAX_BINDINGS) {
            diagnostic_error(parser->diagnostics, parser->token.span,
                       "too many bindings in one function");
            parser->failed = 1;
            return -1;
        }
        if (!parse_one_binding(parser, &bindings[count])) {
            return -1;
        }
        count++;
    }
    return count;
}

// parse_func_signature reads a declaration's `func` keyword and its name into `function`.
//
// A lane before `func` is refused rather than ignored, and the refusal names the spelling
// that works. The guard reads TK_QUALIFIER as well as TK_AT, so `replay func f()` and
// `@replay func f()` are both refused here rather than failing one token later as
// "expected 'func'", which is a true message about the punctuation for a mistake that is
// the word. The declared lane is what the function returns, so it belongs in the return
// type where the body is checked against it.
//
// Takes parser (parser_t) which is the parser's cursor and diagnostic sink.
// Takes function (func_t) which is the function being read into.
// Returns 1 on success, or 0 after reporting a failure.
static int parse_func_signature(parser_t *parser, func_t *function) {
    if (parser->token.kind == TK_AT || parser->token.kind == TK_QUALIFIER) {
        diagnostic_error(parser->diagnostics, parser->token.span,
                   "a function's lane is what it returns, so it is written in the return type: "
                   "func f(...) replay Float");
        parser->failed = 1;
        return 0;
    }
    if (!parse_expect_keyword(parser, "func")) {
        return 0;
    }
    if (parser->token.kind != TK_IDENT) {
        parse_report_expected(parser, NULL, "expected a function name");
        return 0;
    }
    function->name = parser->token.text;
    function->name_length = parser->token.length;
    function->span = parser->token.span;
    parse_advance(parser);
    return 1;
}

// parse_func_body reads a declaration's braced body into `function`: its bindings and its
// result expression.
// Returns 1 on success, or 0 after reporting a failure.
//
// Takes parser (parser_t) which is the parser's cursor and diagnostic sink.
// Takes function (func_t) which is the function being lowered.
static int parse_func_body(parser_t *parser, func_t *function) {
    if (!parse_expect(parser, TK_LBRACE)) {
        return 0;
    }
    bind_t bindings[MAX_BINDINGS];
    const int count = parse_bindings(parser, bindings);
    if (count < 0) {
        return 0;
    }
    function->binding_count = count;
    function->bindings = (bind_t *) parse_copy_to_arena(parser, bindings, sizeof(bind_t), count);
    if (count > 0 && !function->bindings) {
        return 0;
    }

    if (!parse_expect_keyword(parser, "return")) {
        return 0;
    }
    function->result = parse_expr(parser);
    return !function->result ? 0 : parse_expect(parser, TK_RBRACE);
}

// parse_func parses one function declaration into `function`. The lane goes in the return
// type: it is a promise about the value handed back, checked against the body, and it
// sets no ambient inside it.
//
// Takes parser (parser_t) which is the parser's cursor and diagnostic sink.
// Takes function (func_t) which receives the declaration.
// Returns 1 on success, or 0 after reporting a failure.
static int parse_func(parser_t *parser, func_t *function) {
    memset(function, 0, sizeof *function);
    function->lane = LANE_NONE;
    if (!parse_func_signature(parser, function)) {
        return 0;
    }

    parameter_t parameters[MAX_PARAMETERS];
    const int count = parse_parameters(parser, parameters);
    if (count < 0) {
        return 0;
    }
    function->parameter_count = count;
    function->parameters =
        (parameter_t *) parse_copy_to_arena(parser, parameters, sizeof(parameter_t), count);
    if (count > 0 && !function->parameters) {
        return 0;
    }

    qualifiers_t qualifiers;
    if (!parse_qualifiers(parser, &qualifiers)) {
        return 0;
    }
    function->lane = qualifiers.lane;
    function->lane_span = qualifiers.lane_span;
    function->ungrounded = qualifiers.ungrounded;
    return !parse_type(parser) ? 0 : parse_func_body(parser, function);
}

// parse_package_clause reads the `package NAME` clause and keeps nothing from it. Returns
// 1 on success, or 0 after reporting a failure.
//
// The name is validated and discarded, because nothing in this compiler has a second
// package to tell it apart from: there is no import form, no qualified name and one
// translation unit. Refusing `package 3` is the whole of what the clause buys.
//
// Takes parser (parser_t) which is the parser's cursor and diagnostic sink.
static int parse_package_clause(parser_t *parser) {
    if (!parse_expect_keyword(parser, "package")) {
        return 0;
    }
    if (parser->token.kind != TK_IDENT) {
        diagnostic_error(parser->diagnostics, parser->token.span, "expected a package name");
        return 0;
    }
    parse_advance(parser);
    return 1;
}

// function_name_is_taken reports whether `name` is already declared among the first
// `count` entries of `functions`.
//
// Takes functions (func_t) which holds the functions read so far.
// Takes count (int) which is how many of them are in use.
// Takes name (char) which is the candidate name, not NUL terminated.
// Takes name_length (int) which is how many bytes of `name` belong to it.
static int function_name_is_taken(
    const func_t *functions,
    const int count,
    const char *name,
    const int name_length
) {
    for (int i = 0; i < count; i++) {
        if (functions[i].name_length == name_length
            && memcmp(functions[i].name, name, (size_t) name_length) == 0) {
            return 1;
        }
    }
    return 0;
}

// parse_functions reads declarations into `functions`, which holds MAX_FUNCTIONS entries,
// until the source ends or the parse fails. Returns how many were read; the caller
// discards them all if anything failed.
//
// Takes parser (parser_t) which is the parser's cursor and diagnostic sink.
// Takes functions (func_t) which holds the declared functions.
static int parse_functions(parser_t *parser, func_t *functions) {
    int count = 0;
    while (parser->token.kind != TK_EOF && !parser->failed) {
        if (count >= MAX_FUNCTIONS) {
            diagnostic_error(parser->diagnostics, parser->token.span, "too many functions");
            parser->failed = 1;
            break;
        }
        if (!parse_func(parser, &functions[count])) {
            break;
        }
        if (function_name_is_taken(functions, count, functions[count].name,
                                   functions[count].name_length)) {
            diagnostic_error(parser->diagnostics, functions[count].span,
                       "function '%.*s' is declared twice, and every call resolves to the "
                       "first, so the second body is compiled and never run",
                       functions[count].name_length, functions[count].name);
            parser->failed = 1;
            break;
        }
        count++;
    }
    return count;
}

// parse_abandon releases the parser's arena and returns NULL, which is every failure path
// out of parse_program.
//
// Takes parser (parser_t) which is the parser's cursor and diagnostic sink.
static program_t *parse_abandon(parser_t *parser) {
    arena_free(parser->arena);
    free(parser->arena);
    return NULL;
}

// parse_finish allocates the program and moves the parsed declarations and the arena into
// it. Returns the program, or NULL after releasing everything if allocation fails.
//
// Takes parser (parser_t) which is the parser's cursor and diagnostic sink.
// Takes functions (func_t) which holds the declared functions.
// Takes count (int) which is how many functions were read.
static program_t *parse_finish(
    parser_t *parser,
    const func_t *functions,
    const int count
) {
    program_t *program = (program_t *) calloc(1, sizeof *program);
    if (!program) {
        return parse_abandon(parser);
    }
    program->function_count = count;
    program->functions = (func_t *) parse_copy_to_arena(parser, functions, sizeof(func_t), count);
    if (!program->functions) {
        free(program); return parse_abandon(parser);
    }
    program->arena = parser->arena;
    return program;
}

// parse_program parses one source file into a program, or returns NULL after reporting at
// least one error. The returned program borrows `source`, which must outlive it.
//
// Takes file (char) which is the file being read.
// Takes source (char) which is the source text, borrowed.
// Takes diagnostics (diagnostic_t) which is where diagnostics are collected.
program_t *parse_program(const char *file, const char *source, diagnostic_t *diagnostics) {
    parser_t parser = {0};
    parser.diagnostics = diagnostics;
    parser.arena = (arena_t *) calloc(1, sizeof(arena_t));
    if (!parser.arena) {
        return NULL;
    }
    lexer_init(&parser.lexer, file, source, diagnostics);
    parse_advance(&parser);

    if (!parse_package_clause(&parser)) {
        return parse_abandon(&parser);
    }

    func_t functions[MAX_FUNCTIONS];
    const int count = parse_functions(&parser, functions);
    if (parser.failed || diagnostic_error_count(diagnostics) > 0) {
        return parse_abandon(&parser);
    }
    if (count == 0) {
        diagnostic_error(diagnostics, parser.token.span, "a program needs at least one function");
        return parse_abandon(&parser);
    }
    return parse_finish(&parser, functions, count);
}
