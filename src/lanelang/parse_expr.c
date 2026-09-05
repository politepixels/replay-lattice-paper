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

#include "parse_internal.h"

#include <limits.h>
#include <string.h>

// parse_new_expr allocates a cleared expression node of kind `kind` covering `span`.
// Returns NULL after marking the parse failed if the arena is exhausted.
//
// Takes parser (parser_t) which is the parser's cursor and diagnostic sink.
// Takes kind (expr_kind_t) which is the kind of node to build.
// Takes span (span_t) which is the source position to blame.
static expr_t *parse_new_expr(parser_t *parser, const expr_kind_t kind, const span_t span) {
    expr_t *node = (expr_t *) arena_alloc(parser->arena, sizeof *node);
    if (!node) {
        parser->failed = 1; return NULL;
    }
    memset(node, 0, sizeof *node);
    node->kind = kind;
    node->span = span;
    return node;
}

// parse_argument_label reads an argument's `name:` label into `argument` if the next
// tokens are one, and leaves the argument positional if they are not.
//
// One token of lookahead cannot tell a label from a bare identifier, since both begin
// with an identifier, so the parser state is saved and put back when the colon does not
// arrive.
//
// Takes parser (parser_t) which is the parser's cursor and diagnostic sink.
// Takes argument (arg_t) which is the argument expression.
static void parse_argument_label(parser_t *parser, arg_t *argument) {
    if (parser->token.kind != TK_IDENT) {
        return;
    }
    const parser_t saved = *parser;
    const char *label = parser->token.text;
    const int label_length = parser->token.length;
    parse_advance(parser);
    if (parser->token.kind != TK_COLON) {
        *parser = saved; return;
    }
    parse_advance(parser);
    argument->label = label;
    argument->label_length = label_length;
}

// parse_lambda parses a step function, defined below and forward-declared here because
// the argument list is what reaches it.
//
// Takes parser (parser_t) which is the parser's cursor and diagnostic sink.
static expr_t *parse_lambda(parser_t *parser);

// parse_argument_value reads one argument's value: a step function after a `with:` label,
// and an ordinary expression after every other.
//
// `with:` is the one label a step function may follow, so `(acc, t) => e` is parsed here
// and nowhere else, and the same text in any other position is a syntax error by
// construction. That keeps a step out of the value grammar: no production reaches it, so
// it cannot be bound, returned or put in a column. A `with:` written on a call with no
// use for one is refused when that call is lowered, which is where the callee is known.
//
// Takes parser (parser_t) which is the parser's cursor and diagnostic sink.
// Takes argument (arg_t) which is the argument whose label has been read.
static expr_t *parse_argument_value(parser_t *parser, const arg_t *argument) {
    const int labelled_with = argument->label != NULL && argument->label_length == 4
                              && memcmp(argument->label, "with", 4) == 0;
    return labelled_with ? parse_lambda(parser) : parse_expr(parser);
}

// parse_argument_list reads a non-empty comma-separated argument list into `arguments`,
// which holds MAX_CALL_ARGUMENTS entries. Returns how many were read, or -1 after
// reporting a failure.
//
// Takes parser (parser_t) which is the parser's cursor and diagnostic sink.
// Takes arguments (arg_t) which holds the call's argument expressions.
static int parse_argument_list(parser_t *parser, arg_t *arguments) {
    int count = 0;
    for (;;) {
        if (count >= MAX_CALL_ARGUMENTS) {
            diagnostic_error(parser->diagnostics, parser->token.span, "too many arguments (limit %d)",
                       MAX_CALL_ARGUMENTS);
            parser->failed = 1;
            return -1;
        }
        arguments[count].label = NULL;
        arguments[count].label_length = 0;
        parse_argument_label(parser, &arguments[count]);
        arguments[count].value = parse_argument_value(parser, &arguments[count]);
        if (!arguments[count].value) {
            return -1;
        }
        count++;
        if (!parse_accept(parser, TK_COMMA)) {
            break;
        }
    }
    return count;
}

// parse_call_arguments reads a call's arguments and its closing parenthesis, the opening
// one having been consumed.
// Returns how many arguments were read, or -1 after reporting a failure.
//
// Takes parser (parser_t) which is the parser's cursor and diagnostic sink.
// Takes arguments (arg_t) which holds the call's argument expressions.
static int parse_call_arguments(parser_t *parser, arg_t *arguments) {
    int count = 0;
    if (parser->token.kind != TK_RPAREN) {
        count = parse_argument_list(parser, arguments);
        if (count < 0) {
            return -1;
        }
    }
    return !parse_expect(parser, TK_RPAREN) ? -1 : count;
}

// parse_call parses a call of the function named by `name`, whose opening parenthesis has
// been consumed and whose name lies at `span`.
//
// Takes parser (parser_t) which is the parser's cursor and diagnostic sink.
// Takes span (span_t) which is the source position to blame.
// Takes name (char) which is the name being looked up.
// Takes name_length (int) which is how many bytes the name occupies.
static expr_t *parse_call(parser_t *parser, const span_t span, const char *name, const int name_length) {
    arg_t arguments[MAX_CALL_ARGUMENTS];
    const int count = parse_call_arguments(parser, arguments);
    if (count < 0) {
        return NULL;
    }
    expr_t *node = parse_new_expr(parser, EX_CALL, span);
    if (!node) {
        return NULL;
    }
    node->name = name;
    node->name_length = name_length;
    node->arg_count = count;
    node->args = (arg_t *) parse_copy_to_arena(parser, arguments, sizeof(arg_t), count);
    return count > 0 && !node->args ? NULL : node;
}

// parse_identifier_or_call parses an identifier, which is either a name on its own or the
// head of a call.
//
// Takes parser (parser_t) which is the parser's cursor and diagnostic sink.
// Takes span (span_t) which is the source position to blame.
static expr_t *parse_identifier_or_call(parser_t *parser, const span_t span) {
    const char *name = parser->token.text;
    const int name_length = parser->token.length;
    parse_advance(parser);
    if (parser->token.kind != TK_LPAREN) {
        expr_t *node = parse_new_expr(parser, EX_IDENT, span);
        if (node) {
            node->name = name; node->name_length = name_length;
        }
        return node;
    }
    parse_advance(parser);
    return parse_call(parser, span, name, name_length);
}

// parse_step_parameter reads one of a step function's parameter names into the three
// fields that describe it.
//
// A qualifier here is refused rather than ignored. Both parameters are bound by the
// recurrence, the first to the carried value, whose lane is the recurrence's, and the
// second to a tick index that is a function of the program. Neither has a lane of its own
// to state, so a qualifier written here could only be dropped silently.
//
// Takes parser (parser_t) which is the parser's cursor and diagnostic sink.
// Takes name (char) which receives the start of the name in the source.
// Takes length (int) which receives how many bytes of the source the name occupies.
// Takes span (span_t) which receives the name's source text.
// Returns 1, or 0 after reporting a failure.
static int parse_step_parameter(parser_t *parser, const char **name, int *length, span_t *span) {
    if (parser->token.kind == TK_QUALIFIER) {
        diagnostic_error(parser->diagnostics, parser->token.span,
                   "a step's parameter takes no qualifier: the carried value's lane is the "
                   "recurrence's rather than the parameter's. Write the demand on a binding in "
                   "the step's body and return that");
        parser->failed = 1;
        return 0;
    }
    if (parser->token.kind != TK_IDENT) {
        diagnostic_error(parser->diagnostics, parser->token.span,
                   "expected a step parameter name, found %s",
                   lexer_token_kind_name(parser->token.kind));
        parser->failed = 1;
        return 0;
    }
    *name = parser->token.text;
    *length = parser->token.length;
    *span = parser->token.span;
    parse_advance(parser);
    return 1;
}

// parse_step_body reads a step function's body, which is either one expression or a
// braced body of bindings ending in `return`.
//
// A block body is an ordinary body, read by the same parse_bindings a function body uses,
// so `replay part := sum(x)` inside a step means exactly what it means outside one and
// the waiver is available in both. The expression form is the same body with no bindings.
//
// Takes parser (parser_t) which is the parser's cursor and diagnostic sink.
// Takes step (lambda_t) which receives the body.
// Returns 1, or 0 after reporting a failure.
static int parse_step_body(parser_t *parser, lambda_t *step) {
    if (parser->token.kind != TK_LBRACE) {
        step->result = parse_expr(parser);
        return step->result != NULL;
    }
    parse_advance(parser);
    bind_t bindings[MAX_BINDINGS];
    const int count = parse_bindings(parser, bindings);
    if (count < 0) {
        return 0;
    }
    step->binding_count = count;
    step->bindings = (bind_t *) parse_copy_to_arena(parser, bindings, sizeof(bind_t), count);
    if (count > 0 && !step->bindings) {
        return 0;
    }
    if (!parse_expect_keyword(parser, "return")) {
        return 0;
    }
    step->result = parse_expr(parser);
    return !step->result ? 0 : parse_expect(parser, TK_RBRACE);
}

// parse_lambda parses a step function, `(acc, t) => e` or `(acc, t) => { ... return e }`.
//
// The arity is exactly two and the refusals say so: one parameter has no tick index to
// advance by and three has no protocol at all, so neither is accepted with a silent
// reinterpretation. A step that does not need its tick index still writes one and ignores
// it, which costs nothing, because the index is lowered on first reference and a body
// that never names it mints no node. The two names must also differ, because both are
// bound in one scope and the later one would capture every read.
//
// Takes parser (parser_t) which is the parser's cursor and diagnostic sink.
static expr_t *parse_lambda(parser_t *parser) {
    const span_t span = parser->token.span;
    expr_t *node = parse_new_expr(parser, EX_LAMBDA, span);
    if (!node) {
        return NULL;
    }
    lambda_t *step = (lambda_t *) arena_alloc(parser->arena, sizeof *step);
    if (!step) {
        parser->failed = 1; return NULL;
    }
    memset(step, 0, sizeof *step);
    step->span = span;
    node->lambda = step;

    if (!parse_expect(parser, TK_LPAREN)
        || !parse_step_parameter(parser, &step->carry_name, &step->carry_name_length,
                                 &step->carry_span)) {
        return NULL;
    }
    if (parser->token.kind == TK_RPAREN) {
        diagnostic_error(parser->diagnostics, parser->token.span,
                   "a step takes two parameters, the carried value and the tick index, and this "
                   "one takes one: write `(%.*s, t) =>` and leave `t` unread if the step has no "
                   "use for it", step->carry_name_length, step->carry_name);
        parser->failed = 1;
        return NULL;
    }
    if (!parse_expect(parser, TK_COMMA)
        || !parse_step_parameter(parser, &step->tick_name, &step->tick_name_length,
                                 &step->tick_span)) {
        return NULL;
    }
    if (parser->token.kind == TK_COMMA) {
        diagnostic_error(parser->diagnostics, parser->token.span,
                   "a step takes two parameters, the carried value and the tick index, and this "
                   "one takes more");
        parser->failed = 1;
        return NULL;
    }
    if (step->tick_name_length == step->carry_name_length
        && memcmp(step->tick_name, step->carry_name, (size_t) step->carry_name_length) == 0) {
        diagnostic_error(parser->diagnostics, step->tick_span,
                   "a step takes two parameters, the carried value and the tick index, and this "
                   "one names both '%.*s', so the body would read the tick index wherever it "
                   "means the carried value", step->carry_name_length, step->carry_name);
        parser->failed = 1;
        return NULL;
    }
    if (!parse_expect(parser, TK_RPAREN) || !parse_expect(parser, TK_ARROW)) {
        return NULL;
    }
    return parse_step_body(parser, step) ? node : NULL;
}

// parse_primary parses one primary expression: a literal, a parenthesised expression, or
// an identifier that may open a call.
//
// Takes parser (parser_t) which is the parser's cursor and diagnostic sink.
static expr_t *parse_primary(parser_t *parser) {
    const span_t span = parser->token.span;
    if (parser->token.kind == TK_NUMBER) {
        expr_t *node = parse_new_expr(parser, EX_NUMBER, span);
        if (node) {
            node->number = parser->token.number;
        }
        parse_advance(parser);
        return node;
    }
    if (parser->token.kind == TK_LPAREN) {
        parse_advance(parser);
        expr_t *node = parse_expr(parser);
        parse_expect(parser, TK_RPAREN);
        return node;
    }
    if (parser->token.kind == TK_IDENT) {
        return parse_identifier_or_call(parser, span);
    }
    if (parser->token.kind == TK_QUALIFIER) {
        diagnostic_error(parser->diagnostics, span,
                   "a lane qualifier states a fact about a NAMED value, and an expression is not "
                   "one: bind it to a name and write `%.*s` before that name",
                   parser->token.length, parser->token.text);
        parser->failed = 1;
        return NULL;
    }
    if (!parser->failed) {
        diagnostic_error(parser->diagnostics, span, "expected an expression, found %s",
                   lexer_token_kind_name(parser->token.kind));
    }
    parser->failed = 1;
    return NULL;
}

// refuse_unknown_at_word reports an `@` that does not open a waiver, and always returns
// 0.
//
// The two cases get different messages because they are different mistakes. A lane word
// after the `@` gets a refusal naming the two spellings that work, because somebody who
// writes it knows exactly what they meant. Anything else is a misspelling or an
// invention, and there is nothing to suggest.
//
// Takes parser (parser_t) which is the parser's cursor and diagnostic sink.
// Returns 0, always, so parse_bless can report and return in one expression.
static long long refuse_unknown_at_word(parser_t *parser) {
    const parser_t at = *parser;
    parse_advance(parser);
    const int retired = parser->token.kind == TK_QUALIFIER;
    const span_t where = parser->token.span;
    *parser = at;
    if (retired) {
        diagnostic_error(parser->diagnostics, where,
                   "a lane is a fact about a value, so it is written where the value is named: "
                   "`replay <name> := <expression>`, or `func f() replay T` for a return");
    } else {
        diagnostic_error(parser->diagnostics, where,
                   "@bless is the only annotation this language has; nothing else may follow '@'");
    }
    parser->failed = 1;
    return 0;
}

// parse_bless reads a `@bless` or `@bless(N)` at the cursor, returning what the binding
// should store: 0 when there is none, BLESS_PROMISE for the bare form, and `checksum + 1`
// for the conditional one.
//
// It is not an arm of parse_qualifiers, which reads lane demands: kept apart, an `@bless`
// where a lane belongs reaches the unknown-annotation path, a lane where a waiver belongs
// is simply not a waiver, and neither reader needs a rule about the other. The checksum
// is range-checked before it is converted, and both halves matter: a fractional one
// truncates, so `@bless(3.5)` would vouch for checksum 3, and one past what a `long long`
// holds is undefined behaviour on the conversion itself (C11 6.3.1.4). The bound is
// UINT_MAX because eval_bless_checksum returns `unsigned`, so anything above it could
// never match. The argument is a bare number: parse_argument_label discards labels, so a
// labelled form here would accept `@bless(banana: a + b)`.
//
// Takes parser (parser_t) which is the parser's cursor and diagnostic sink.
// Returns 0 for absent, BLESS_PROMISE for the bare form, or a checksum biased by one.
long long parse_bless(parser_t *parser) {
    if (parser->token.kind != TK_AT) {
        return 0;
    }
    const parser_t saved = *parser;
    if (!parse_accept(parser, TK_AT) || !parse_accept_keyword(parser, "bless")) {
        *parser = saved;
        return refuse_unknown_at_word(parser);
    }
    if (!parse_accept(parser, TK_LPAREN)) {
        return BLESS_PROMISE;
    }
    if (parser->token.kind != TK_NUMBER) {
        diagnostic_error(parser->diagnostics, parser->token.span,
                   "@bless(...) wants a numeric checksum");
        parser->failed = 1;
        return BLESS_PROMISE;
    }
    const double written = parser->token.number;
    if (!(written >= 0.0) || written > (double) UINT_MAX
        || (double) (long long) written != written) {
        diagnostic_error(parser->diagnostics, parser->token.span,
                   "@bless(...) wants a whole checksum from 0 to %u, and this is %g; the value is "
                   "compared against a 32-bit checksum of the loaded bytes",
                   UINT_MAX, written);
        parser->failed = 1;
        return BLESS_PROMISE;
    }
    const long long value = (long long) written;
    parse_advance(parser);
    return parse_expect(parser, TK_RPAREN) ? value + 1 : BLESS_PROMISE;
}

// refuse_bless_on_factor reports a waiver written inside an expression rather than on the
// binding that names the value, and always returns 0.
//
// A waiver belongs to a binding because a binding holds one value, so one waiver covers
// one leaf by construction and cannot be made to cover two. Whether the value is such a
// leaf is a question about the graph, and it is asked where the program is resolved.
//
// Takes parser (parser_t) which is the parser's diagnostic sink.
// Takes node (expr_t) which is the factor the waiver was written on.
// Returns 0, always, so a caller can refuse in the same expression that reports.
static int refuse_bless_on_factor(parser_t *parser, const expr_t *node) {
    diagnostic_error(parser->diagnostics, node->span,
               "@bless is written on the binding that names the value, not inside an "
               "expression: write @bless(...) name := source(...)");
    parser->failed = 1;
    return 0;
}

// parse_factor parses a factor, which is a primary with a waiver possibly written before
// it.
//
// A lane cannot be written here. A lane is a fact about a value, so it is written where a
// value is named; an expression names nothing, so there is no lane to read at this
// position. The waiver remains, and is refused here for its own reason: it belongs to the
// binding that names the value, which is where one promise covers one leaf by
// construction.
//
// Takes parser (parser_t) which is the parser's cursor and diagnostic sink.
// Returns the factor, or NULL after reporting a defect.
static expr_t *parse_factor(parser_t *parser) {
    const long long bless = parse_bless(parser);
    if (parser->failed) {
        return NULL;
    }

    expr_t *node = parse_primary(parser);
    if (!node) {
        return NULL;
    }

    return bless != 0 && !refuse_bless_on_factor(parser, node) ? NULL : node;
}

// parse_binary builds a binary node at `span` over two operands that have already been
// parsed.
//
// Takes parser (parser_t) which is the parser's cursor and diagnostic sink.
// Takes span (span_t) which is the source position to blame.
// Takes operator_symbol (int) which is the operator character.
// Takes left (expr_t) which is the first operand.
// Takes right (expr_t) which is the second operand.
static expr_t *parse_binary(
    parser_t *parser,
    const span_t span,
    const int operator_symbol,
    expr_t *left,
    expr_t *right
) {
    expr_t *node = parse_new_expr(parser, EX_BINOP, span);
    if (!node) {
        return NULL;
    }
    node->operator_symbol = operator_symbol;
    node->left = left;
    node->right = right;
    return node;
}

// parse_depth_exceeded reports whether the expression cycle may descend no further,
// reporting the structural failure once.
//
// Takes parser (parser_t) which is the parser's cursor and diagnostic sink.
static int parse_depth_exceeded(parser_t *parser) {
    if (parser->depth < LANELANG_MAX_EXPR_DEPTH) {
        return 0;
    }
    if (!parser->failed) {
        diagnostic_error(parser->diagnostics, parser->token.span,
                   "expression nested deeper than %d; the parser is recursive and this is the "
                   "structural limit", LANELANG_MAX_EXPR_DEPTH);
    }
    parser->failed = 1;
    return 1;
}

// parse_term parses a term: factors joined left to right by `*` and `/`.
//
// Takes parser (parser_t) which is the parser's cursor and diagnostic sink.
static expr_t *parse_term(parser_t *parser) {
    expr_t *left = parse_factor(parser);
    while (left && (parser->token.kind == TK_STAR || parser->token.kind == TK_SLASH)) {
        const int operator_symbol = parser->token.kind == TK_STAR ? '*' : '/';
        const span_t span = parser->token.span;
        parse_advance(parser);
        expr_t *right = parse_factor(parser);
        if (!right) {
            return NULL;
        }
        left = parse_binary(parser, span, operator_symbol, left, right);
    }
    return left;
}

// parse_expr_inner parses an expression: terms joined left to right by `+` and `-`. The
// depth guard is in parse_expr, which wraps this.
//
// Takes parser (parser_t) which is the parser's cursor and diagnostic sink.
static expr_t *parse_expr_inner(parser_t *parser) {
    expr_t *left = parse_term(parser);
    while (left && (parser->token.kind == TK_PLUS || parser->token.kind == TK_MINUS)) {
        const int operator_symbol = parser->token.kind == TK_PLUS ? '+' : '-';
        const span_t span = parser->token.span;
        parse_advance(parser);
        expr_t *right = parse_term(parser);
        if (!right) {
            return NULL;
        }
        left = parse_binary(parser, span, operator_symbol, left, right);
    }
    return left;
}

// parse_expr parses one expression, bounding the recursion. This is a wrapper rather than
// a check inside parse_expr_inner, so that the decrement cannot be missed on an
// early-return path.
//
// What it bounds: the cycle expr -> term -> factor -> primary -> "(" expr ")" passes
// through here once per nested parenthesis, so one counter bounds nesting by parentheses
// and by call arguments.
//
// Takes parser (parser_t) which is the parser's cursor and diagnostic sink.
expr_t *parse_expr(parser_t *parser) {
    if (parse_depth_exceeded(parser)) {
        return NULL;
    }
    parser->depth++;
    expr_t *node = parse_expr_inner(parser);
    parser->depth--;
    return node;
}
