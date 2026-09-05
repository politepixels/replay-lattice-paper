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

#include "resolve.h"

#include "heal.h"
#include "lattice.h"
#include "resolve_internal.h"

#include <stdlib.h>
#include <string.h>

// scope_lookup returns the node a name is bound to, searching the innermost binding
// first, or -1 when the name is not in scope.
//
// Takes state (resolve_state_t) which is the lowering in progress.
// Takes name (char) which is the name being looked up.
// Takes name_length (int) which is how many bytes the name occupies.
static int scope_lookup(const resolve_state_t *state, const char *name, const int name_length) {
    for (int i = state->scope_count - 1; i >= 0; i--) {
        if (names_equal(state->scope[i].name, state->scope[i].name_length, name, name_length)) {
            return state->scope[i].node;
        }
    }
    return -1;
}

// scope_bind puts a name in scope, reporting a binding past SCOPE_CAPACITY as a
// structural defect at `at` and returning 0.
//
// Takes state (resolve_state_t) which is the lowering in progress.
// Takes name (char) which is the name being bound.
// Takes name_length (int) which is how many bytes the name occupies.
// Takes node (int) which is the node the name resolves to.
// Takes at (span_t) which is the source text to blame if the scope is full.
int scope_bind(resolve_state_t *state, const char *name, const int name_length,
                      const int node, const span_t at) {
    if (state->scope_count >= SCOPE_CAPACITY) {
        diagnostic_error(state->diagnostics, at, "scope capacity exceeded (limit %d names)",
                   SCOPE_CAPACITY);
        state->malformed = 1;
        return 0;
    }
    state->scope[state->scope_count].name = name;
    state->scope[state->scope_count].name_length = name_length;
    state->scope[state->scope_count].node = node;
    state->scope_count++;
    return 1;
}

// demand_carried_by returns the demand `node` already carries, and the value underneath
// it, so a second demand about the same value can be met with the first.
//
// Takes dag (dag_t) which is the graph being walked.
// Takes node (int) which is the node under examination.
// Takes inner (int) which receives the value the demand is about.
// Returns the demand carried, or LANE_NONE when the node carries none.
static lane_t demand_carried_by(const dag_t *dag, const int node, int *inner) {
    const dagnode_t *target = dag_at_const(dag, node);
    if (target->op != OP_DEMAND) {
        *inner = node;
        return LANE_NONE;
    }
    *inner = target->in[0];
    return target->declared;
}

// wrap_demand builds the node that carries `demand` about `value`, and returns its index.
//
// A demand is about a value other names may already hold, so it rides on a node of its
// own: written onto the shared value it would reach every other name and pin a fold the
// source never demanded, and because `declared` is in the interning key it would also
// decide which twins merge by the order the bindings were written in. A second demand
// about one value meets the first rather than nesting, the stronger winning, and the meet
// builds a fresh wrapper over the same inner value because the wrapper already there may
// itself be shared. The label is the lane rather than the inner node's label, which would
// read `b` or `sum` depending on whether a binding had already renamed the fold it sits
// over; a binding that names this node relabels it afterwards.
//
// Takes state (resolve_state_t) which is the lowering in progress.
// Takes value (int) which is the node the demand is about.
// Takes demand (lane_t) which is the lane the source demanded.
// Takes at (span_t) which is where the demand was written.
// Returns the demand node's index, or -1 after reporting a defect.
static int wrap_demand(resolve_state_t *state, const int value, const lane_t demand,
                       const span_t at) {
    int inner = value;
    const lane_t already = demand_carried_by(state->dag, value, &inner);
    const lane_t want = (already == LANE_NONE || demand < already) ? demand : already;
    const dagnode_t *under = dag_at_const(state->dag, inner);
    const char *const named_for = lane_name(want);

    dagnode_t node;
    dag_node_init(&node);
    node.op = OP_DEMAND;
    node.op_class = OPC_NEUTRAL;
    node.in[0] = inner;
    node.input_count = 1;
    node.lane = state->ambient;
    node.span = under->span;
    node.label = named_for;
    node.label_length = (int) strlen(named_for);
    node.declared = want;
    node.declared_span = at;
#ifdef LANELANG_DEMAND_STAMPS_NODE
    dagnode_t *target = dag_at(state->dag, inner);
    if (target->declared == LANE_NONE || want < target->declared) {
        target->declared = want;
        target->declared_span = at;
    }
    return inner;
#else
    return intern_or_report(state, &node, at);
#endif
}

// declare_demand_meet applies a demand stated about a value that may already carry one,
// and returns the node that carries the strongest demand stated anywhere on the value.
//
// Takes state (resolve_state_t) which is the lowering in progress.
// Takes node (int) which is the value the demand is about.
// Takes demand (lane_t) which is the lane the source demanded.
// Takes at (span_t) which is where the demand was written.
// Returns the demand node's index, or -1 after reporting a defect.
int declare_demand_meet(resolve_state_t *state, const int node, const lane_t demand,
                        const span_t at) {
    return wrap_demand(state, node, demand, at);
}

// declare_return_promise applies a function's declared return lane to the value it
// returns, reporting a body that promises less than the signature does.
//
// A promise is not a meet, because the body's own annotation is the thing the signature
// is a claim about rather than a second opinion the signature may overrule: meeting would
// let a body stating `approx` silently become a `replay` demand, and healing would then
// pin the fold the author had explicitly asked to leave free. No demand on the value
// means the signature supplies one, carried on a node of its own; a demand that satisfies
// the signature hands the value back untouched; a demand that does not is a body
// promising less than it publishes, which is reported.
//
// Takes state (resolve_state_t) which is the lowering in progress.
// Takes node (int) which is the node the function returns.
// Takes function (func_t) which is the function whose signature this is.
// Returns the node the promise is carried on, or -1 after reporting a weaker body.
int declare_return_promise(resolve_state_t *state, const int node, const func_t *function) {
    const span_t at = function->lane_span.line ? function->lane_span : function->span;
    int inner = node;
    const lane_t stated = demand_carried_by(state->dag, node, &inner);
    if (stated == LANE_NONE) {
        return wrap_demand(state, node, function->lane, at);
    }
    if (lane_satisfies(stated, function->lane)) {
        return node;
    }
    diagnostic_error(state->diagnostics, at,
               "this function returns %s, and its body states %s, which does not satisfy it; a "
               "declared return lane is a promise the body is checked against",
               lane_name(function->lane), lane_name(stated));
    state->malformed = 1;
    return -1;
}

// bless_is_legal_here reports whether a waiver may be written on the binding that lowered
// to `node`, reporting and failing when it may not.
//
// Two node kinds qualify, the two the grounding sweep seeds false: a `source()` leaf, and
// the OP_OPAQUE wrapper over a call declared `ungrounded`. A checksum is refused on an
// opaque call because `bless_checksum_holds` compares the recorded value against the
// column a source loads, and behind a call there is no loaded column, so a receipt
// nothing verifies would read stronger than the admitted promise the bare form states. A
// second waiver on one leaf is refused because the field holds one promise: two names for
// one source are two bindings and one node, so the later binding would silently replace a
// receipt a run can check with a promise nothing verifies.
//
// Takes state (resolve_state_t) which is the lowering in progress.
// Takes node (int) which is the node the binding lowered to.
// Takes binding (bind_t) which is the binding carrying the waiver.
static int bless_is_legal_here(resolve_state_t *state, const int node, const bind_t *binding) {
#ifdef LANELANG_BLESS_ON_ANY_NODE
    (void) state; (void) node; (void) binding;
    return 1;
#else
    const dagnode_t *target = dag_at_const(state->dag, node);
    const span_t where = binding->bless_span.line ? binding->bless_span : binding->span;
    if (target->bless != 0) {
        diagnostic_error(state->diagnostics, where,
                         "this value already carries a @bless, and one waiver covers one leaf; a "
                         "second promise about the same read of the world would replace the first, "
                         "so write the waiver once, on the binding that names the source");
        state->malformed = 1;
        return 0;
    }
    if (target->op == OP_OPAQUE) {
        if (binding->bless != BLESS_PROMISE) {
            diagnostic_error(state->diagnostics, where,
                             "a checksum is verified against the column a source loads, and a call "
                             "declared 'ungrounded' loads none; write a bare @bless, which promises "
                             "the same thing without claiming a receipt for it");
            state->malformed = 1;
            return 0;
        }
        return 1;
    }
    if (target->input_count == 0 && target->carried < 0 && target->op == OP_SOURCE) {
        return 1;
    }
    diagnostic_error(state->diagnostics, where,
                     "@bless may only be written on a binding whose value the program does not fix, "
                     "which is a source or a call declared 'ungrounded'; this one is computed, so "
                     "there is nothing to vouch for");
    state->malformed = 1;
    return 0;
#endif
}

// lower_binding lowers one `name := expr` binding, applies any qualifier and waiver
// written on it, and puts the name in scope.
//
// A qualifier on a binding is both a demand on the value it names and the ambient over
// the whole right-hand side, because a demand covers the values that feed it; declaring
// it on the result alone would lower every fold in the right-hand side free. The demand
// is written with declare_demand_meet rather than if-unset, because the programmer wrote
// it here and the stronger of two demands on one value is the one that holds. The waiver
// is applied to the node the binding lowered to, where the graph can answer whether that
// node is a leaf the program does not fix.
//
// Takes state (resolve_state_t) which is the lowering in progress.
// Takes binding (bind_t) which is the binding being resolved.
// Returns the node the binding lowered to, or -1 after reporting a defect.
int lower_binding(resolve_state_t *state, const bind_t *binding) {
    const lane_t saved_ambient = state->ambient;
    if (binding->lane != LANE_NONE) {
        state->ambient = binding->lane;
    }
    const int node = lower_expr(state, binding->value);
    state->ambient = saved_ambient;
    if (node < 0) {
        return -1;
    }
    if (binding->bless != 0) {
        if (!bless_is_legal_here(state, node, binding)) {
            return -1;
        }
        dag_at(state->dag, node)->bless = binding->bless;
    }
    int named = node;
    if (binding->lane != LANE_NONE) {
        named = declare_demand_meet(state, node, binding->lane,
                                    binding->lane_span.line ? binding->lane_span : binding->span);
        if (named < 0) {
            return -1;
        }
    }
    return !scope_bind(state, binding->name, binding->name_length, named, binding->span)
           ? -1 : named;
}

// lower_body_bindings lowers every binding of an inlined body, returning 0 once one has
// failed.
//
// Takes state (resolve_state_t) which is the lowering in progress.
// Takes function (func_t) which is the function being lowered.
int lower_body_bindings(resolve_state_t *state, const func_t *function) {
    for (int i = 0; i < function->binding_count; i++) {
        if (lower_binding(state, &function->bindings[i]) < 0) {
            return 0;
        }
    }
    return 1;
}

// lower_number builds the node for a literal and returns its index.
//
// Takes state (resolve_state_t) which is the lowering in progress.
// Takes expression (expr_t) which is the expression being lowered.
static int lower_number(resolve_state_t *state, const expr_t *expression) {
    dagnode_t node;
    dag_node_init(&node);
    node.op = OP_CONST;
    node.op_class = OPC_NEUTRAL;
    node.lane = state->ambient;
    node.span = expression->span;
    node.label = CONST_LABEL;
    node.label_length = (int) (sizeof CONST_LABEL - 1);
    node.constant = expression->number;
    return intern_or_report(state, &node, expression->span);
}

// lower_identifier resolves a name to the node it is bound to, or reports it and returns
// -1.
//
// Takes state (resolve_state_t) which is the lowering in progress.
// Takes expression (expr_t) which is the expression being lowered.
static int lower_identifier(resolve_state_t *state, const expr_t *expression) {
    const int node = scope_lookup(state, expression->name, expression->name_length);
    if (node < 0) {
        diagnostic_error(state->diagnostics, expression->span,
                   "undefined name '%.*s' (a binding is not in scope within its own "
                   "right-hand side, so this is also what a self reference looks like)",
                   expression->name_length, expression->name);
        state->malformed = 1;
    }
    return node;
}

// binop_label returns the one-character label for an operator symbol.
//
// Takes operator_symbol (int) which is the operator character.
static const char *binop_label(const int operator_symbol) {
    const char *found = strchr(BINOP_SYMBOLS, operator_symbol);
    return found ? found : BINOP_SYMBOLS;
}

// lower_binop builds the node for a binary operation and returns its index, or -1 when an
// operand failed to lower.
//
// Takes state (resolve_state_t) which is the lowering in progress.
// Takes expression (expr_t) which is the expression being lowered.
static int lower_binop(resolve_state_t *state, expr_t *expression) {
    const int left = lower_expr(state, expression->left);
    const int right = lower_expr(state, expression->right);
    if (left < 0 || right < 0) {
        return -1;
    }

    dagnode_t node;
    dag_node_init(&node);
    node.op = OP_ELEM;
    node.op_class = OPC_NEUTRAL;
    node.lane = state->ambient;
    node.in[0] = left;
    node.in[1] = right;
    node.input_count = 2;
    node.span = expression->span;
    node.label = binop_label(expression->operator_symbol);
    node.label_length = 1;
    node.binary_operator = expression->operator_symbol;
    return intern_or_report(state, &node, expression->span);
}

// lower_step_out_of_place reports a step function reaching a position that wants a value,
// and always returns -1.
//
// The grammar admits `(acc, t) => ...` only as the value of a `with:` label, so the only
// way here is a `with:` written on a call that does not take one, such as `sum(x, with:
// (a, b) => a)`. The parser cannot refuse that, because it does not know what the callee
// is; this pass does. A step is not a value, and the message says so rather than
// reporting a type nobody can write down.
//
// Takes state (resolve_state_t) which is the lowering in progress.
// Takes expression (expr_t) which is the step function found out of place.
static int lower_step_out_of_place(resolve_state_t *state, const expr_t *expression) {
    diagnostic_error(state->diagnostics, expression->span,
               "a step function is not a value: 'with:' takes one, and nothing else does");
    state->malformed = 1;
    return -1;
}

// lower_expr_kind dispatches on the kind of expression, the ambient lane having already
// been set.
//
// Takes state (resolve_state_t) which is the lowering in progress.
// Takes expression (expr_t) which is the expression being lowered.
static int lower_expr_kind(resolve_state_t *state, expr_t *expression) {
    switch (expression->kind) {
    case EX_NUMBER: return lower_number(state, expression);
    case EX_IDENT:  return lower_identifier(state, expression);
    case EX_CALL:   return lower_call(state, expression);
    case EX_BINOP:  return lower_binop(state, expression);
    case EX_LAMBDA: return lower_step_out_of_place(state, expression);
    }
    return -1;
}

// lower_expr lowers one expression, dispatching on its kind.
//
// An expression states no demand of its own, so there is no ambient to set here and none
// to restore; the ambient is introduced in exactly one place, lower_binding, and descends
// from there.
//
// Takes state (resolve_state_t) which is the lowering in progress.
// Takes expression (expr_t) which is the expression being lowered.
// Returns the node's index, or -1 after reporting a defect.
int lower_expr(resolve_state_t *state, expr_t *expression) {
    return expression ? lower_expr_kind(state, expression) : -1;
}

// find_entry_function returns the entry function, or NULL after reporting that the
// program has no function of that name.
//
// Takes program (program_t) which is the parsed program.
// Takes entry (char) which names the function to lower.
// Takes diagnostics (diagnostic_t) which is where diagnostics are collected.
static func_t *find_entry_function(program_t *program, const char *entry, diagnostic_t *diagnostics) {
    func_t *function = find_function(program, entry, (int) strlen(entry));
    if (!function) {
        span_t at = SPAN_NONE;
        at.line = 1;
        at.column = 1;
        at.length = 1;
        diagnostic_error(diagnostics, at, "no function named '%s'", entry);
    }
    return function;
}

// bind_entry_parameters appends one input node per declared parameter and puts each name
// in scope.
//
// The nodes are appended rather than interned, because two parameters are two values
// however alike their declarations look. Every node is built with dag_node_init and never
// with `= {0}`: zeroing leaves `carried` at 0, which is a real node index rather than a
// blank, so a zeroed parameter carries a phantom edge to node 0 and the entry function
// reports a carried depth with no recurrence in it.
//
// Takes state (resolve_state_t) which is the lowering in progress.
// Takes function (func_t) which is the function being lowered.
// Returns 1 on success, or 0 after reporting a defect.
static int bind_entry_parameters(resolve_state_t *state, const func_t *function) {
    for (int i = 0; i < function->parameter_count; i++) {
        dagnode_t node;
        dag_node_init(&node);
        node.op = OP_INPUT;
        node.op_class = OPC_NEUTRAL;
        node.lane = state->ambient;
        node.span = function->parameters[i].span;
        node.label = function->parameters[i].name;
        node.label_length = function->parameters[i].name_length;
        const int index = dag_append(state->dag, &node);
        if (index < 0) {
            diagnostic_error(state->diagnostics, node.span,
                       "out of memory while building the graph");
            state->malformed = 1;
            return 0;
        }
        if (!scope_bind(state, function->parameters[i].name,
                        function->parameters[i].name_length, index,
                        function->parameters[i].span)) {
            return 0;
        }
    }
    return 1;
}

// label_node_with_binding_name carries a binding's source name onto the node it lowered
// to, so that diagnostics and figures can name the value the programmer named.
//
// Takes dag (dag_t) which is the graph being walked.
// Takes node_index (int) which is the node's index in the graph.
// Takes binding (bind_t) which is the binding being resolved.
static void label_node_with_binding_name(dag_t *dag, const int node_index, const bind_t *binding) {
    dagnode_t *node = dag_at(dag, node_index);
    if (node->op == OP_FOLD || node->op == OP_ELEM || node->op == OP_DEMAND) {
        node->label = binding->name;
        node->label_length = binding->name_length;
    }
}

// lower_entry_bindings lowers every binding of the entry function's body, naming each
// node after the binding it came from.
//
// Takes state (resolve_state_t) which is the lowering in progress.
// Takes function (func_t) which is the function being lowered.
// Returns 0 once one has failed.
static int lower_entry_bindings(resolve_state_t *state, const func_t *function) {
    for (int i = 0; i < function->binding_count; i++) {
        const int node = lower_binding(state, &function->bindings[i]);
        if (node < 0) {
            return 0;
        }
        label_node_with_binding_name(state->dag, node, &function->bindings[i]);
    }
    return 1;
}

// lower_one_function lowers `function`'s parameters, bindings and result into `out`, and
// applies its return promise.
//
// Takes state (resolve_state_t) which is the lowering in progress.
// Takes function (func_t) which is the function being lowered.
// Takes out (resolved_t) which receives the result node and the status.
static void lower_one_function(resolve_state_t *state, func_t *function, resolved_t *out) {
    if (!bind_entry_parameters(state, function) || !lower_entry_bindings(state, function)) {
        out->status = RESOLVE_MALFORMED; return;
    }
    out->result = lower_expr(state, function->result);
    if (out->result < 0 || state->malformed) {
        out->status = RESOLVE_MALFORMED; return;
    }
    if (function->lane != LANE_NONE) {
        out->result = declare_return_promise(state, out->result, function);
        if (out->result < 0) {
            out->status = RESOLVE_MALFORMED; return;
        }
    }
    dag_recount_uses(out->dag, out->result);
}

// resolve_declaration lowers one declared function on its own, with its parameters as
// leaves.
//
// Takes program (program_t) which is the parsed program.
// Takes function (func_t) which is the declaration to lower.
// Takes diagnostics (diagnostic_t) which is where a refusal is reported.
// Returns the lowered declaration, whose DAG the caller owns.
static resolved_t resolve_declaration(program_t *program, func_t *function,
                                      diagnostic_t *diagnostics);

// sweep_declaration heals and lane-checks one lowered declaration, so that what the
// standalone pass verifies is the lane the body computes and not merely a lane it
// happened to write down.
//
// declare_return_promise compares the signature against a demand declared syntactically,
// so a body that computes `approx` without saying so declares nothing and the promise
// holds only because there is nothing to check; the sweep computes what the body actually
// reaches. A lane violation sets `rejected` and a body that never resolved does not,
// because a lane verdict and a structural failure are different answers. An allocation
// failure here reports and returns 0 with `rejected` left alone, because returning 1
// would accept a body the checker had not looked at.
//
// Takes one (resolved_t) which is the lowered declaration.
// Takes diagnostics (diagnostic_t) which is where a refusal is reported.
// Takes rejected (int) which is set when the body violates a demand.
// Takes found (check_result_t) which accumulates what the bodies here got wrong.
// Returns 1 when the body checks, 0 when it does not.
static int sweep_declaration(const resolved_t *one, diagnostic_t *diagnostics, int *rejected,
                             check_result_t *found) {
    const int count = dag_count(one->dag);
    if (count <= 0) {
        return 1;
    }
    lane_t *computed = (lane_t *) calloc((size_t) count, sizeof(lane_t));
    if (!computed) {
        span_t at = SPAN_NONE;
        at.line = 1;
        at.column = 1;
        at.length = 1;
        diagnostic_error(diagnostics, at,
                   "out of memory while checking a declaration no call reaches");
        return 0;
    }
    (void) heal_program(one->dag, one->result);
    const check_result_t swept = lattice_check(one->dag, one->result, computed, diagnostics);
    free(computed);
    if (check_rejected(&swept)) {
        *rejected = 1;
        found->violations += swept.violations;
        found->ungrounded += swept.ungrounded;
    }
    return !check_rejected(&swept) && !check_malformed(&swept);
}

// resolve_verify_declarations checks every function the entry path would not reach, so a
// published return lane, and every demand written inside a body, is verified whether or
// not anything calls it.
//
// Only the entry is skipped. A call to a declaration marked `ungrounded` lowers to an
// opaque leaf, so its body is never inlined anywhere, and an uncalled helper is reached
// by no lowering at all; verifying only the functions carrying a declared lane would
// leave both checked by nothing.
//
// Takes program (program_t) which is the parsed program.
// Takes entry (char) which is the entry function's name.
// Takes diagnostics (diagnostic_t) which is where a refusal is reported.
// Takes rejected (int) which is set when a body violated a demand rather than failing to
// resolve, so the caller reports a lane verdict rather than a structural one.
// Takes found (check_result_t) which receives what was wrong across every declaration
// here. The pass checks all of them, so any number may fail, and the two kinds of refusal
// are counted apart for the reason report_refusal in main.c gives.
// Returns 1 when every declaration checks, 0 when one did not.
int resolve_verify_declarations(program_t *program, const char *entry,
                                    diagnostic_t *diagnostics, int *rejected,
                                    check_result_t *found) {
    int ok = 1;
    *rejected = 0;
    *found = check_result_none();
    for (int i = 0; i < program->function_count; i++) {
        func_t *function = &program->functions[i];
        if (entry && (int) strlen(entry) == function->name_length
            && memcmp(function->name, entry, (size_t) function->name_length) == 0) {
            continue;
        }
        const resolved_t one = resolve_declaration(program, function, diagnostics);
        if (one.status != RESOLVE_OK
            || !sweep_declaration(&one, diagnostics, rejected, found)) {
            ok = 0;
        }
        dag_free(one.dag);
    }
    return ok;
}

// resolve_program lowers `entry` and everything it calls into one DAG, inlining every
// call.
//
// The ambient starts at LANE_NONE, including for the entry's own declared lane, because
// that lane is a promise about the returned value, applied to the result node and checked
// there; applied as an outermost ambient it would pin every fold in the body and make the
// promise true by construction rather than by checking.
//
// Takes program (program_t) which is the parsed program.
// Takes entry (char) which names the function to lower.
// Takes diagnostics (diagnostic_t) which is where diagnostics are collected.
resolved_t resolve_program(program_t *program, const char *entry, diagnostic_t *diagnostics) {
    resolved_t out;
    out.status = RESOLVE_OK;
    out.dag = dag_new();
    out.result = -1;
    if (!out.dag) {
        span_t at = SPAN_NONE;
        at.line = 1;
        at.column = 1;
        at.length = 1;
        diagnostic_error(diagnostics, at, "out of memory while building the graph");
        out.status = RESOLVE_MALFORMED;
        return out;
    }

    resolve_state_t state = {0};
    state.program = program;
    state.dag = out.dag;
    state.diagnostics = diagnostics;
    state.ambient = LANE_NONE;

    func_t *function = find_entry_function(program, entry, diagnostics);
    if (!function) {
        out.status = RESOLVE_MALFORMED; return out;
    }
    lower_one_function(&state, function, &out);
    return out;
}

// resolve_declaration lowers one function on its own, with its parameters bound as
// leaves, so that what it promises can be checked whether or not anything calls it.
//
// resolve_program lowers the entry function and inlines what it reaches, so without this
// pass a declared function no call reaches would never be lowered and could promise
// anything. Parameters become leaves because what a parameter holds is the caller's
// obligation, and from inside the callee it is grounded by assumption; the lowering asks
// the one question the signature is a claim about, and asks it of the body alone.
//
// Takes program (program_t) which is the parsed program.
// Takes function (func_t) which is the declaration to lower.
// Takes diagnostics (diagnostic_t) which is where a refusal is reported.
static resolved_t resolve_declaration(program_t *program, func_t *function,
                                     diagnostic_t *diagnostics) {
    resolved_t out;
    out.status = RESOLVE_OK;
    out.dag = dag_new();
    out.result = -1;
    if (!out.dag) {
        out.status = RESOLVE_MALFORMED;
        return out;
    }
    resolve_state_t state = {0};
    state.program = program;
    state.dag = out.dag;
    state.diagnostics = diagnostics;
    state.ambient = LANE_NONE;
    lower_one_function(&state, function, &out);
    return out;
}
