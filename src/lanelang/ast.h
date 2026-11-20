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

#ifndef LANELANG_AST_H
#define LANELANG_AST_H

#include "diagnostic.h"
#include "lane.h"

// MAX_CALL_ARGUMENTS is how many arguments one call may have, and how many argument nodes
// lowering will hold for it. The parser refuses a longer call before lowering sees it,
// and lowering sizes a stack array to this bound and relies on that refusal.
#define MAX_CALL_ARGUMENTS 8

// expr_kind_t is what an expression node is.
typedef enum {
    // EX_NUMBER is a numeric literal, whose value is in `number`.
    EX_NUMBER,

    // EX_IDENT is a reference to a parameter or a binding, named by `name`.
    EX_IDENT,

    // EX_CALL is a call of a builtin or of a declared function, named by `name`.
    EX_CALL,

    // EX_BINOP is a binary operation, whose operands are `left` and `right`.
    EX_BINOP,

    // EX_LAMBDA holds an inline step function such as `(acc, t) => e` in `lambda`. The
    // grammar admits a step only as a `with:` argument.
    EX_LAMBDA
} expr_kind_t;

// expr_t is one expression node, declared here so an argument can point at one.
typedef struct expr expr_t;

// arg_t is one argument at a call site, which may be positional or named.
typedef struct {
    // label is the start of the argument's label in the source, or NULL when the argument
    // is positional.
    const char *label;

    // label_length counts the bytes in the label and is zero for a positional argument.
    int label_length;

    // value is the argument's value.
    expr_t *value;
} arg_t;

// lambda_t is a step function, declared here so an expression node can point at one and
// defined below, after the bindings it holds.
typedef struct lambda lambda_t;

// expr is one expression node. Its kind selects the active payload fields. The arena
// clears the remaining fields on allocation.
struct expr {
    // kind is which of the payloads below this node uses.
    expr_kind_t kind;

    // span locates the token used for diagnostics. A call uses its name and a binary
    // operation uses its operator.
    span_t span;

    // number is the literal's value, for EX_NUMBER.
    double number;

    // name is the start of the name in the source, for EX_IDENT and EX_CALL.
    const char *name;

    // name_length is how many bytes of `name` belong to the name.
    int name_length;

    // args is the arguments, for EX_CALL, in the order they were written.
    arg_t *args;

    // arg_count is how many entries `args` has.
    int arg_count;

    // operator_symbol is which operation this is, for EX_BINOP, as one of the characters
    // '+', '-', '*' or '/'.
    int operator_symbol;

    // left is the left operand, for EX_BINOP.
    expr_t *left;

    // right is the right operand, for EX_BINOP.
    expr_t *right;

    // lambda is the step function, for EX_LAMBDA.
    lambda_t *lambda;
};

// bind_t is one `name := expr` binding inside a function body.
typedef struct {
    // name is the start of the bound name in the source.
    const char *name;

    // name_length is how many bytes of `name` belong to the name.
    int name_length;

    // span is the source text of the name, for diagnostics about the binding itself.
    span_t span;

    // value is the expression the name is bound to.
    expr_t *value;

    // lane is the demand written as a qualifier before the name, or LANE_NONE. The
    // qualifier covers the whole right-hand side, so it has a single reading.
    lane_t lane;

    // lane_span is the qualifier's source text, for blame.
    span_t lane_span;

    // bless records the waiver on this binding. Zero means no waiver, BLESS_PROMISE means
    // a bare waiver, and `c + 1` records checksum `c`. The bias distinguishes an absent
    // waiver from a checksum of zero. Each waiver covers the value of one binding.
    long long bless;

    // bless_span locates the waiver's opening `@` for diagnostics.
    span_t bless_span;
} bind_t;

// lambda is a step function, held behind a pointer so that an ordinary expression node
// does not carry a step's worth of unused fields.
//
// A step has two parameters. The second is the tick index, and the resolver emits a tick
// node only if the body reads it. A block body, `(acc, t) => { ... return e }`, holds
// bindings in the same order as a function body. An expression body has no bindings.
struct lambda {
    // carry_name is the start of the first parameter's name in the source, which the
    // recurrence's carried value is bound to.
    const char *carry_name;

    // carry_name_length is how many bytes of `carry_name` belong to the name.
    int carry_name_length;

    // carry_span is the source text of the first parameter, for diagnostics.
    span_t carry_span;

    // tick_name is the start of the second parameter's name in the source, which the
    // 0-based tick index is bound to.
    const char *tick_name;

    // tick_name_length is how many bytes of `tick_name` belong to the name.
    int tick_name_length;

    // tick_span is the source text of the second parameter, for diagnostics.
    span_t tick_span;

    // bindings is the block body's bindings, in source order, or NULL for an expression
    // body.
    bind_t *bindings;

    // binding_count is how many entries `bindings` has.
    int binding_count;

    // result is the expression the step returns, which is the carried value's next value.
    struct expr *result;

    // span is the source text of the parameter list's opening parenthesis, for
    // diagnostics about the step itself.
    span_t span;
};

// parameter_t is one declared parameter of a function.
typedef struct {
    // name is the start of the parameter name in the source.
    const char *name;

    // name_length is how many bytes of `name` belong to the name.
    int name_length;

    // span is the source text of the parameter name, for diagnostics.
    span_t span;

    // lane is the demand this parameter states on whatever the caller passes, or
    // LANE_NONE. The checker applies the demand to the argument at each call.
    lane_t lane;

    // lane_span locates the qualifier in the signature. A violation reports the demand
    // here.
    span_t lane_span;
} parameter_t;

// func_t represents a function declaration. The body consists of its bindings followed by
// the result expression.
typedef struct {
    // name is the start of the function name in the source.
    const char *name;

    // name_length is how many bytes of `name` belong to the name.
    int name_length;

    // span is the source text of the function name, for diagnostics.
    span_t span;

    // lane is the return qualifier, or LANE_NONE if the declaration omits it. The checker
    // compares this promise with the computed result lane. The return qualifier does not
    // set the ambient lane for the body.
    lane_t lane;

    // lane_span is the source text of the qualifier that set `lane`, so a violation
    // blames the word in the signature.
    span_t lane_span;

    // parameters is the declared parameters, in the order they were written.
    parameter_t *parameters;

    // parameter_count is how many entries `parameters` has.
    int parameter_count;

    // ungrounded records whether the declaration reserves the right to depend on external
    // inputs. The grounding sweep treats this separately from the lane join. When the
    // declaration gives both qualifiers, `ungrounded` appears first.
    int ungrounded;

    // bindings is the bindings in the body, in the order they were written.
    bind_t *bindings;

    // binding_count is how many entries `bindings` has.
    int binding_count;

    // result is the expression the function returns.
    expr_t *result;
} func_t;

// program_t is a parsed source file. Every node it points at lives in its arena, so the
// program is freed in one call and nothing inside it is freed individually.
typedef struct {
    // functions is the declared functions, in the order they were written.
    func_t *functions;

    // function_count is how many entries `functions` has.
    int function_count;

    // arena is the arena every node was allocated from, opaque here and released by
    // ast_free.
    void *arena;
} program_t;

// ast_free releases a program returned by parse_program, together with every node in its
// arena. Passing NULL is allowed and does nothing.
//
// Takes program (program_t) which is the parsed program.
void ast_free(program_t *program);

#endif  // LANELANG_AST_H
