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

#ifndef LANELANG_RESOLVE_INTERNAL_H
#define LANELANG_RESOLVE_INTERNAL_H

#include "ast.h"
#include "dag.h"
#include "diagnostic.h"
#include "lane.h"

// SCOPE_CAPACITY is how many names may be in scope at once, a whole-program bound because
// inlining pushes a callee's parameters and bindings on top of the caller's. A binding
// past the limit is reported rather than dropped.
#define SCOPE_CAPACITY 128

// MAX_INLINE_DEPTH is how many calls deep inlining may go; reaching it is reported as a
// call nesting error, so recursion is refused before it exhausts the C stack.
#define MAX_INLINE_DEPTH 32

// MAX_INLINED_CALLS is how many call sites one lowering may inline, a bound on work
// rather than depth because inlining branches and the site count can double per nesting
// level. Reaching it is a refusal, because a checker that cannot answer says so rather
// than appearing to hang.
#define MAX_INLINED_CALLS 200000L

// CHOOSE_ARGUMENT_COUNT is how many operands choose() takes: the mask and the two arms.
// The arity check and the node's input count are the same number by construction, so a
// fourth operand cannot be read out of an array sized for three.
#define CHOOSE_ARGUMENT_COUNT 3

// CONST_LABEL is the label a literal node carries, a literal having no source name of its
// own.
static const char CONST_LABEL[] = "const";

// BINOP_SYMBOLS is the operator symbols an elementwise node may carry, in one string so
// that the node's label can point into it and stay valid for the life of the process.
static const char BINOP_SYMBOLS[] = "+-*/";

// binding_t is one name in scope, bound to the node it resolves to.
typedef struct {
    // name is the start of the name in the source, borrowed from the program text.
    const char *name;

    // name_length is how many bytes of `name` belong to the name.
    int name_length;

    // node is the index of the node the name resolves to.
    int node;
} binding_t;

// resolve_state_t is everything one lowering carries: where names are bound, what demand
// is in force, and how deep the inlining has gone.
typedef struct {
    // program is the program being lowered, for finding a called function.
    program_t *program;

    // dag is the graph being built.
    dag_t *dag;

    // diagnostics is the sink that receives one diagnostic per structural defect.
    diagnostic_t *diagnostics;

    // scope is the names in scope, innermost last.
    binding_t scope[SCOPE_CAPACITY];

    // scope_count is how many entries of `scope` are in use.
    int scope_count;

    // ambient is the lane stamped on every node built now.
    lane_t ambient;

    // promise_scope is the identity stamped on every node built now, naming the call
    // whose declared return lane the lowering is currently inside, or 0 outside every
    // such call.
    int promise_scope;

    // next_promise_scope is the identity the next lane-publishing call will take. Counted
    // per lowering, so two calls to one function are two scopes.
    int next_promise_scope;

    // depth is how many calls deep the inlining currently is.
    int depth;

    // inlined is how many call sites this lowering has inlined so far, separate from
    // `depth` because depth bounds the C stack and this bounds the work, which can double
    // per nesting level while depth stays small.
    long inlined;

    // malformed is set once any structural defect has been reported.
    int malformed;
} resolve_state_t;

// names_equal compares two source names, neither of which is NUL terminated.
//
// Takes left (char) which is the first operand.
// Takes left_length (int) which is the first run's length.
// Takes right (char) which is the second operand.
// Takes right_length (int) which is the second run's length.
static int names_equal(const char *left, const int left_length, const char *right, const int right_length) {
    return left_length == right_length && memcmp(left, right, (size_t) left_length) == 0;
}

// NAMES_EQUAL_LITERAL compares a counted source name against a string literal, taking the
// length from the literal itself rather than from a number written beside it.
//
// Written as a macro because `sizeof` keeps the pair together: a hand written length is a
// second copy of the spelling that a rename does not update, and it would still compile
// and silently match nothing once the word grew a letter.
#define NAMES_EQUAL_LITERAL(name, length, literal) \
    names_equal((name), (length), (literal), (int) (sizeof(literal) - 1))

// scope_bind puts a name in scope.
//
// Takes state (resolve_state_t) which is the lowering in progress.
// Takes name (char) which is the name being bound.
// Takes name_length (int) which is how many bytes the name occupies.
// Takes node (int) which is the node the name resolves to.
// Takes at (span_t) which is the source text to blame if the scope is full.
// Returns 1, or 0 after reporting that the scope is full.
int scope_bind(resolve_state_t *state, const char *name, const int name_length, const int node,
               const span_t at);

// declare_demand applies a demand the programmer wrote directly on a value, nesting it
// over any demand of another lane already carried there, and returns the node that
// carries it.
//
// Takes state (resolve_state_t) which is the lowering in progress.
// Takes node (int) which is the value the demand is about.
// Takes demand (lane_t) which is the demand written.
// Takes at (span_t) which is where it was written.
// Returns the demand node's index, or -1 after reporting a defect.
int declare_demand(resolve_state_t *state, const int node, const lane_t demand,
                   const span_t at);

// declare_return_promise applies a callee's declared return lane to the value it returns,
// carrying it on a node of its own, and reports a body that promises less than its
// signature.
//
// Takes state (resolve_state_t) which is the lowering in progress.
// Takes node (int) which is the node the function returns.
// Takes function (func_t) which is the function whose signature this is.
// Returns the node the promise is carried on, or -1 after reporting a weaker body.
int declare_return_promise(resolve_state_t *state, const int node, const func_t *function);

// declare_call_promise applies a callee's declared return lane at a call site, as a demand
// on the value its inlined body returns, nested over any lane that value states which does
// not satisfy it.
//
// Takes state (resolve_state_t) which is the lowering in progress.
// Takes node (int) which is the node the inlined body returns.
// Takes function (func_t) which is the function called.
// Returns the node the promise is carried on, or -1 after reporting a defect.
int declare_call_promise(resolve_state_t *state, const int node, const func_t *function);

// intern_or_report interns a node, reporting an allocation failure.
//
// Takes state (resolve_state_t) which is the lowering in progress.
// Takes node (dagnode_t) which is the node being added.
// Takes at (span_t) which is the source text to blame.
// Returns the node's index, or -1 after reporting a defect.
int intern_or_report(resolve_state_t *state, const dagnode_t *node, const span_t at);

// lower_expr lowers one expression, entering any demand written on it.
//
// Takes state (resolve_state_t) which is the lowering in progress.
// Takes expression (expr_t) which is the expression being lowered.
// Returns the node's index, or -1 after reporting a defect.
int lower_expr(resolve_state_t *state, expr_t *expression);

// lower_binding lowers one `name := expr` binding and puts the name in scope.
//
// Takes state (resolve_state_t) which is the lowering in progress.
// Takes binding (bind_t) which is the binding being resolved.
// Returns the node the binding lowered to, or -1 after reporting a defect.
int lower_binding(resolve_state_t *state, const bind_t *binding);

// call_names reports whether a call names the builtin `name`.
//
// Takes call (expr_t) which is the call site being resolved.
// Takes name (char) which is the builtin being looked for.
// Returns non-zero when the call names it.
int call_names(const expr_t *call, const char *name);

// lower_indices lowers indices() and integers(), which build a column from a count.
//
// Takes state (resolve_state_t) which is the lowering in progress.
// Takes call (expr_t) which is the call site being resolved.
// Takes integers (int) which selects integers() over indices().
// Returns the node's index, or -1 after reporting a defect.
int lower_indices(resolve_state_t *state, expr_t *call, const int integers);

// lower_scan lowers scan(), the prefix family.
//
// Takes state (resolve_state_t) which is the lowering in progress.
// Takes call (expr_t) which is the call site being resolved.
// Returns the node's index, or -1 after reporting a defect.
int lower_scan(resolve_state_t *state, expr_t *call);

// lower_choose lowers choose(), the elementwise conditional.
//
// Takes state (resolve_state_t) which is the lowering in progress.
// Takes call (expr_t) which is the call site being resolved.
// Returns the node's index, or -1 after reporting a defect.
int lower_choose(resolve_state_t *state, expr_t *call);

// lower_source lowers source(), a leaf the program does not fix; a call to an
// `ungrounded` declaration is the other.
//
// Takes state (resolve_state_t) which is the lowering in progress.
// Takes call (expr_t) which is the call site being resolved.
// Returns the node's index, or -1 after reporting a defect.
int lower_source(resolve_state_t *state, expr_t *call);

// lower_sum lowers sum() and isum(), the reduction family.
//
// Takes state (resolve_state_t) which is the lowering in progress.
// Takes call (expr_t) which is the call site being resolved.
// Takes integer (int) which selects isum() over sum().
// Returns the node's index, or -1 after reporting a defect.
int lower_sum(resolve_state_t *state, expr_t *call, const int integer);

// count_is_whole reports whether `value` is a whole number the checker can read as a
// count.
//
// Takes value (double) which is the literal being examined.
// Returns non-zero when it is whole.
int count_is_whole(const double value);

// lower_body_bindings lowers the bindings at the head of an inlined body, in order.
//
// Takes state (resolve_state_t) which is the lowering in progress.
// Takes function (func_t) which is the function being inlined.
// Returns 1, or 0 after reporting a defect.
int lower_body_bindings(resolve_state_t *state, const func_t *function);

// find_function returns the declared function of that name, or NULL when the program has
// none.
//
// Takes program (program_t) which is the parsed program.
// Takes name (char) which is the name being looked up.
// Takes name_length (int) which is how many bytes the name occupies.
// Returns the declaration, or NULL.
func_t *find_function(program_t *program, const char *name, const int name_length);

// lower_call lowers a call of a builtin or of a declared function.
//
// Takes state (resolve_state_t) which is the lowering in progress.
// Takes call (expr_t) which is the call site being resolved.
// Returns the node's index, or -1 after reporting a defect.
int lower_call(resolve_state_t *state, expr_t *call);

#endif
