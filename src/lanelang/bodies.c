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

#include "resolve_internal.h"

#include <string.h>

// step_reads_name answers whether a step function reads `name`, defined below and
// forward-declared here because expr_reads_name descends into a nested step.
//
// Takes step (lambda_t) which is the step function being searched.
// Takes name (char) which is the name being looked for.
// Takes name_length (int) which is how many bytes the name occupies.
static int step_reads_name(const lambda_t *step, const char *name, const int name_length);

// step_shadows_name answers whether a step function's own parameters carry `name`, which
// shadows it over that step's whole body since both are bound before anything in it is
// lowered.
//
// Takes step (lambda_t) which is the step function being searched.
// Takes name (char) which is the name being looked for.
// Takes name_length (int) which is how many bytes the name occupies.
// Returns 1 when the step rebinds the name, 0 when it does not.
static int step_shadows_name(const lambda_t *step, const char *name, const int name_length) {
    return names_equal(step->carry_name, step->carry_name_length, name, name_length)
        || names_equal(step->tick_name, step->tick_name_length, name, name_length);
}

// expr_reads_name reports whether an expression reads the binding named `name`. A nested
// step can shadow the binding.
//
// Takes expression (expr_t) which is the expression being searched, and may be NULL.
// Takes name (char) which is the name being looked for.
// Takes name_length (int) which is how many bytes the name occupies.
// Returns 1 when the name is read, 0 when it is not.
static int expr_reads_name(const expr_t *expression, const char *name, const int name_length) {
    if (!expression) {
        return 0;
    }
    switch (expression->kind) {
    case EX_NUMBER:
        return 0;
    case EX_IDENT:
        return names_equal(expression->name, expression->name_length, name, name_length);
    case EX_BINOP:
        return expr_reads_name(expression->left, name, name_length)
            || expr_reads_name(expression->right, name, name_length);
    case EX_CALL:
        for (int i = 0; i < expression->arg_count; i++) {
            if (expr_reads_name(expression->args[i].value, name, name_length)) {
                return 1;
            }
        }
        return 0;
    case EX_LAMBDA:
        return step_shadows_name(expression->lambda, name, name_length)
               ? 0
               : step_reads_name(expression->lambda, name, name_length);
    }
    return 0;
}

// step_reads_name answers whether a step function reads `name`, stopping at a binding
// that shadows it.
//
// References after a binding of the same name belong to that binding. The search includes
// the binding's own value because it is evaluated before the name is rebound.
//
// Takes step (lambda_t) which is the step function being searched.
// Takes name (char) which is the name being looked for.
// Takes name_length (int) which is how many bytes the name occupies.
// Returns 1 when the name is read, 0 when it is not.
static int step_reads_name(const lambda_t *step, const char *name, const int name_length) {
    for (int i = 0; i < step->binding_count; i++) {
        if (expr_reads_name(step->bindings[i].value, name, name_length)) {
            return 1;
        }
        if (names_equal(step->bindings[i].name, step->bindings[i].name_length, name, name_length)) {
            return 0;
        }
    }
    return expr_reads_name(step->result, name, name_length);
}

// iterate_argument returns the call's argument carrying `label`, or NULL when it has
// none.
//
// Takes call (expr_t) which is the call site being resolved.
// Takes label (char) which is the label being looked for, NUL-terminated.
static const arg_t *iterate_argument(const expr_t *call, const char *label) {
    const int length = (int) strlen(label);
    for (int i = 0; i < call->arg_count; i++) {
        if (call->args[i].label
            && names_equal(call->args[i].label, call->args[i].label_length, label, length)) {
            return &call->args[i];
        }
    }
    return NULL;
}

// ITERATE_ARGUMENT_COUNT counts the three required named arguments `times:`, `from:` and
// `with:`.
#define ITERATE_ARGUMENT_COUNT 3

// MAX_ITERATE_TICKS caps an `iterate` clock at the largest signed 32-bit integer. The
// resolver checks the bound before the evaluator converts the count to int.
#define MAX_ITERATE_TICKS 2147483647.0

// iterate_missing_label names the first required label the call omits. Its caller has
// already established that at least one label is missing.
//
// Takes times (arg_t) which is the tick count argument, or NULL.
// Takes from (arg_t) which is the seed argument, or NULL.
// Takes with (arg_t) which is the step argument, or NULL.
// Returns the missing label, quoted.
static const char *iterate_missing_label(const arg_t *times, const arg_t *from,
                                         const arg_t *with) {
    static const char *const NAMES[ITERATE_ARGUMENT_COUNT] = { "'times:'", "'from:'", "'with:'" };
    const arg_t *const given[ITERATE_ARGUMENT_COUNT] = { times, from, with };
    for (int i = 0; i < ITERATE_ARGUMENT_COUNT; i++) {
        if (!given[i]) {
            return NAMES[i];
        }
    }
    return NAMES[0];
}

// iterate_is_well_formed checks an `iterate` call's shape, writing its three arguments
// through the out parameters.
//
// parse_argument_value requires a step function after `with:`. All three labels are
// mandatory so a missing seed cannot be mistaken for a zero seed. A literal tick count
// fixes the number of iterations before evaluation starts.
//
// Takes state (resolve_state_t) which is the lowering in progress.
// Takes call (expr_t) which is the call site being resolved.
// Takes times (arg_t) which receives the tick count argument.
// Takes from (arg_t) which receives the seed argument.
// Takes with (arg_t) which receives the step argument.
// Returns 1 when the call has the shape, 0 after reporting the departure.
static int iterate_is_well_formed(resolve_state_t *state, const expr_t *call,
                                  const arg_t **times, const arg_t **from, const arg_t **with) {
    *times = iterate_argument(call, "times");
    *from = iterate_argument(call, "from");
    *with = iterate_argument(call, "with");
    if (!*times || !*from || !*with) {
        diagnostic_error(state->diagnostics, call->span,
                   "iterate takes 'times:', 'from:' and 'with:', and this call is missing %s",
                   iterate_missing_label(*times, *from, *with));
        state->malformed = 1;
        return 0;
    }
    if (call->arg_count != ITERATE_ARGUMENT_COUNT) {
        diagnostic_error(state->diagnostics, call->span,
                   "iterate takes exactly 'times:', 'from:' and 'with:', and this call passes %d "
                   "arguments", call->arg_count);
        state->malformed = 1;
        return 0;
    }
    const expr_t *ticks = (*times)->value;
    if (ticks->kind != EX_NUMBER || !(ticks->number >= 1.0) || !count_is_whole(ticks->number)
        || ticks->number > MAX_ITERATE_TICKS) {
        diagnostic_error(state->diagnostics, ticks->span,
                   "iterate's 'times:' must be a whole number literal of at least 1 and at "
                   "most %.0f, the largest count the evaluator's tick counter can hold",
                   MAX_ITERATE_TICKS);
        state->malformed = 1;
        return 0;
    }
    return 1;
}

// lower_carried_name lowers the seed and builds the node that holds the carried value for
// this tick, returning its index or -1 after reporting a defect.
//
// The seed is lowered while the carried name is not in scope, because a seed is computed
// once before the clock starts and may not read the value it seeds. That rule then
// follows from ordinary scoping and needs no check of its own, so this runs before the
// caller binds the name.
//
// The carried edge is left unset here and filled in by the caller once the step's result
// has been lowered, since that expression may read the name this node provides.
//
// Takes state (resolve_state_t) which is the lowering in progress.
// Takes call (expr_t) which is the call site being resolved.
// Takes seed (expr_t) which is the `from:` expression.
// Takes step (lambda_t) which is the step function.
// Takes ticks (double) which is how many ticks the clock runs for.
// Returns the carried node's index, or -1 after reporting a defect.
static int lower_carried_name(resolve_state_t *state, const expr_t *call, expr_t *seed,
                              const lambda_t *step, const double ticks) {
    const int seed_node = lower_expr(state, seed);
    if (seed_node < 0) {
        return -1;
    }

    dagnode_t node;
    dag_node_init(&node);
    node.op = OP_RECUR;
    node.op_class = OPC_NEUTRAL;
    node.lane = state->ambient;
    node.in[0] = seed_node;
    node.input_count = 1;
    node.constant = ticks;
    node.span = call->span;
    node.label = step->carry_name;
    node.label_length = step->carry_name_length;
    return intern_or_report(state, &node, call->span);
}

// lower_tick_index builds the node holding the recurrence's 0-based tick index, returning
// its index or -1 after reporting a defect.
//
// The tick node is a leaf. Its interning key includes `tick_of`, the recurrence whose
// counter it reads, so nested clocks keep distinct indices.
//
// Takes state (resolve_state_t) which is the lowering in progress.
// Takes owner (int) which is the OP_RECUR node whose ticks are counted.
// Takes step (lambda_t) which is the step function naming the index.
// Returns the tick node's index, or -1 after reporting a defect.
static int lower_tick_index(resolve_state_t *state, const int owner, const lambda_t *step) {
    dagnode_t node;
    dag_node_init(&node);
    node.op = OP_TICK;
    node.op_class = OPC_NEUTRAL;
    node.lane = state->ambient;
    node.tick_of = owner;
    node.span = step->tick_span;
    node.label = step->tick_name;
    node.label_length = step->tick_name_length;
    return intern_or_report(state, &node, step->tick_span);
}

// lower_iterate builds the nodes for an `iterate` call and returns the node its value is,
// or -1 after reporting a defect.
//
// The order of the steps below is the semantics. The seed is lowered while the carried
// name is not in scope, because a seed is computed once before the clock starts and may
// not read the value it seeds. The carried name becomes its own node holding this tick's
// value, with the seed as its input and the step's result as its carried edge, so one
// node per recurrence means the join sweep needs no notion of what a recurrence carries.
// The step's bindings are lowered after the name enters scope. Its result is lowered
// last, so it reads the tick's current values. The parser represents the call as an
// ordinary EX_CALL.
//
// Takes state (resolve_state_t) which is the lowering in progress.
// Takes call (expr_t) which is the call site being resolved.
static int lower_iterate(resolve_state_t *state, expr_t *call) {
    const arg_t *times = NULL;
    const arg_t *from = NULL;
    const arg_t *with = NULL;
    if (!iterate_is_well_formed(state, call, &times, &from, &with)) {
        return -1;
    }
    const lambda_t *step = with->value->lambda;

    const int saved_scope = state->scope_count;

    const int carried_node = lower_carried_name(state, call, from->value, step,
                                                times->value->number);
    if (carried_node < 0) {
        return -1;
    }
    if (!scope_bind(state, step->carry_name, step->carry_name_length, carried_node,
                    step->carry_span)) {
        return -1;
    }

    if (step_reads_name(step, step->tick_name, step->tick_name_length)) {
        const int tick_node = lower_tick_index(state, carried_node, step);
        if (tick_node < 0) {
            return -1;
        }
        if (!scope_bind(state, step->tick_name, step->tick_name_length, tick_node,
                        step->tick_span)) {
            return -1;
        }
    }

    for (int i = 0; i < step->binding_count; i++) {
        if (lower_binding(state, &step->bindings[i]) < 0) {
            return -1;
        }
    }

    const int next_node = lower_expr(state, step->result);
    if (next_node < 0) {
        return -1;
    }
    dagnode_t *recurrence = dag_at(state->dag, carried_node);
    recurrence->carried = next_node;
    recurrence->step_end = dag_count(state->dag) - 1;

    state->scope_count = saved_scope;
    return carried_node;
}

// find_function returns the declared function of that name, or NULL when the program has
// none.
//
// Takes program (program_t) which is the parsed program.
// Takes name (char) which is the name being looked up.
// Takes name_length (int) which is how many bytes the name occupies.
func_t *find_function(program_t *program, const char *name, const int name_length) {
    for (int i = 0; i < program->function_count; i++) {
        if (names_equal(program->functions[i].name, program->functions[i].name_length,
                        name, name_length)) {
            return &program->functions[i];
        }
    }
    return NULL;
}

// call_is_well_formed reports whether a call may be inlined, having reported the defect
// if not.
//
// Arguments bind by position. When an argument has a label, it must name the parameter at
// that position. Step functions skip this check so the resolver can diagnose a `with:`
// argument used where a value belongs.
//
// Takes state (resolve_state_t) which is the lowering in progress.
// Takes function (func_t) which is the function being lowered.
// Takes call (expr_t) which is the call site being resolved.
static int call_is_well_formed(
    resolve_state_t *state,
    const func_t *function,
    const expr_t *call
) {
    if (function->parameter_count != call->arg_count) {
        diagnostic_error(state->diagnostics, call->span, "'%.*s' expects %d argument%s, got %d",
                   call->name_length, call->name, function->parameter_count,
                   function->parameter_count == 1 ? "" : "s", call->arg_count);
        state->malformed = 1;
        return 0;
    }
    for (int i = 0; i < call->arg_count; i++) {
        if (!call->args[i].label || call->args[i].value->kind == EX_LAMBDA) {
            continue;
        }
        if (names_equal(call->args[i].label, call->args[i].label_length,
                        function->parameters[i].name, function->parameters[i].name_length)) {
            continue;
        }
        diagnostic_error(state->diagnostics, call->args[i].value->span,
                   "argument %d of '%.*s' is '%.*s', and this call labels it '%.*s'. A label names "
                   "the parameter it fills rather than reordering the arguments",
                   i + 1, call->name_length, call->name,
                   function->parameters[i].name_length, function->parameters[i].name,
                   call->args[i].label_length, call->args[i].label);
        state->malformed = 1;
        return 0;
    }
    for (int i = 0; i <= state->depth; i++) {
        if (state->active[i] == function) {
            diagnostic_error(state->diagnostics, call->span,
                       "'%.*s' is called while it is still being lowered (recursion is not "
                       "supported)", call->name_length, call->name);
            state->malformed = 1;
            return 0;
        }
    }
    if (state->depth >= MAX_INLINE_DEPTH) {
        diagnostic_error(state->diagnostics, call->span,
                   "call nesting too deep");
        state->malformed = 1;
        return 0;
    }
    if (state->inlined >= MAX_INLINED_CALLS) {
        diagnostic_error(state->diagnostics, call->span,
                   "this declaration inlines more than %ld call sites; the checker refuses rather "
                   "than run for an unbounded time", MAX_INLINED_CALLS);
        state->malformed = 1;
        return 0;
    }
    state->inlined++;
    return 1;
}

// lower_call_arguments lowers every argument of a call, writing one node index per
// argument. The output array `argument_nodes` must hold MAX_CALL_ARGUMENTS entries.
//
// Takes state (resolve_state_t) which is the lowering in progress.
// Takes call (expr_t) which is the call site being resolved.
// Takes argument_nodes (int) which holds the lowered argument nodes.
static int lower_call_arguments(resolve_state_t *state, expr_t *call, int *argument_nodes) {
    for (int i = 0; i < call->arg_count; i++) {
        argument_nodes[i] = lower_expr(state, call->args[i].value);
        if (argument_nodes[i] < 0) {
            return 0;
        }
    }
    return 1;
}

// inline_body lowers a called function's body into the caller's graph and returns the
// node its result evaluates to, or -1 after a defect has been reported.
//
// The caller's scope, depth and promise scope are restored before return. A parameter's
// demand wraps the caller's argument, preserving any lane already stated on it. Blame
// points to the demand in the signature, to the origin of the argument's weaker lane and
// to the argument in the call.
//
// A function's return lane is checked against its body. Making it the ambient lane would
// select the promised kernels before that check. A separate promise scope prevents
// interning the body's nodes with the caller's and keeps the escape test from counting a
// use introduced only by that sharing.
//
// Takes state (resolve_state_t) which is the lowering in progress.
// Takes function (func_t) which is the function being lowered.
// Takes call (expr_t) which is the call site whose arguments a parameter's demand blames.
// Takes argument_nodes (int) which holds the lowered argument nodes.
static int inline_body(
    resolve_state_t *state,
    const func_t *function,
    const expr_t *call,
    const int *argument_nodes
) {
    const int saved_scope_count = state->scope_count;
    const int saved_promise = state->promise_scope;
    state->depth++;
    state->active[state->depth] = function;
    if (function->lane != LANE_NONE) {
        state->next_promise_scope++;
        state->promise_scope = state->next_promise_scope;
    }

    int bound = 1;
    for (int i = 0; i < function->parameter_count; i++) {
        int supplied = argument_nodes[i];
        if (function->parameters[i].lane != LANE_NONE) {
            supplied = declare_demand(state, supplied, function->parameters[i].lane,
                                      function->parameters[i].lane_span);
            if (supplied < 0) {
                state->depth--;
                state->scope_count = saved_scope_count;
                state->promise_scope = saved_promise;
                return -1;
            }
            dagnode_t *demand = dag_at(state->dag, supplied);
            if (supplied != argument_nodes[i] && demand->argument_span.line == 0) {
                demand->argument_span = call->args[i].value->span;
            }
        }
        if (!scope_bind(state, function->parameters[i].name, function->parameters[i].name_length,
                        supplied, function->parameters[i].span)) {
            bound = 0;
            break;
        }
    }
    const int out = bound && lower_body_bindings(state, function) ? lower_expr(state, function->result)
                                                            : -1;

    state->depth--;
    state->scope_count = saved_scope_count;
    state->promise_scope = saved_promise;
    return out;
}

// wrap_opaque puts an OP_OPAQUE node over the lowered body of a call to a function
// declared `ungrounded`, returning its index or -1 after reporting a defect.
//
// The wrapper records that this call may depend on the outside world, even when its body
// reads only grounded values. A bare waiver on the call can vouch for it if its body also
// meets the grounding requirement. The ordinary join still derives the lane from the
// body. Keeping the waiver on a fresh wrapper prevents it from reaching an argument the
// callee returns directly.
//
// Takes state (resolve_state_t) which is the lowering in progress.
// Takes call (expr_t) which is the call site being resolved.
// Takes body (int) which is the node the callee's body lowered to.
// Returns the wrapper's index, or -1 after reporting a defect.
static int wrap_opaque(resolve_state_t *state, const expr_t *call, const int body) {
    dagnode_t node;
    dag_node_init(&node);
    node.op = OP_OPAQUE;
    node.op_class = OPC_NEUTRAL;
    node.in[0] = body;
    node.input_count = 1;
    node.lane = state->ambient;
    node.span = call->span;
    node.label = call->name;
    node.label_length = call->name_length;
    const int wrapper = dag_append(state->dag, &node);
    if (wrapper < 0) {
        diagnostic_error(state->diagnostics, call->span, "out of memory while building the graph");
        state->malformed = 1;
        return -1;
    }
    return wrapper;
}

// lower_call lowers a call of a builtin or of a declared function, returning the node
// index or -1 after a defect has been reported.
//
// Takes state (resolve_state_t) which is the lowering in progress.
// Takes call (expr_t) which is the call site being resolved.
int lower_call(resolve_state_t *state, expr_t *call) {
    if (call_names(call, "indices")) {
        return lower_indices(state, call, 0);
    }
    if (call_names(call, "integers")) {
        return lower_indices(state, call, 1);
    }
    if (call_names(call, "source")) {
        return lower_source(state, call);
    }
    if (call_names(call, "scan")) {
        return lower_scan(state, call);
    }
    if (call_names(call, "choose")) {
        return lower_choose(state, call);
    }
    if (call_names(call, "iterate")) {
        return lower_iterate(state, call);
    }
    if (call_names(call, "sum")) {
        return lower_sum(state, call, 0);
    }
    if (call_names(call, "isum")) {
        return lower_sum(state, call, 1);
    }

    func_t *function = find_function(state->program, call->name, call->name_length);
    if (!function) {
        diagnostic_error(state->diagnostics, call->span, "unknown function '%.*s'",
                   call->name_length, call->name);
        state->malformed = 1;
        return -1;
    }
    if (!call_is_well_formed(state, function, call)) {
        return -1;
    }

    int argument_nodes[MAX_CALL_ARGUMENTS];
    if (!lower_call_arguments(state, call, argument_nodes)) {
        return -1;
    }

    int out = inline_body(state, function, call, argument_nodes);
    if (out >= 0 && function->ungrounded) {
        out = wrap_opaque(state, call, out);
    }
    if (out >= 0 && function->lane != LANE_NONE) {
        out = declare_call_promise(state, out, function);
    }
    return out;
}
