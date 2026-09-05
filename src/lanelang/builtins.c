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

#include <math.h>
#include <string.h>

// fold_class returns the schedule class a fold takes under the current ambient. An
// `order: oracle` setting fixes the fold order to the oracle's and decides the class on
// its own.
//
// This is the op-class table for reductions; everything else in the language is neutral.
// An unqualified fold and an `approx` one both reassociate freely, which is why they
// share an arm.
//
// Takes ambient (lane_t) which is the lane in force at this point.
// Takes ordered (int) which is whether the fold carried the order dial.
static opclass_t fold_class(const lane_t ambient, const int ordered) {
    if (ordered) {
        return OPC_FOLD_EXACT;
    }
    switch (ambient) {
    case LANE_EXACT:  return OPC_FOLD_EXACT;
    case LANE_REPLAY: return OPC_FOLD_REPLAY;
    default:          return OPC_FOLD_APPROX;
    }
}

// scan_class returns the schedule class a scan takes under the current ambient.
//
// The same question as fold_class, answered the same way, which is the point of
// re-carving the table on kernels: a second family needs no second argument. The order
// dial has no scan realisation here, which is why refuse_order_argument declines it
// rather than letting the call fall quietly to the fixed-block kernel.
//
// Takes ambient (lane_t) which is the lane in force at this point.
static opclass_t scan_class(const lane_t ambient) {
    switch (ambient) {
    case LANE_EXACT:
    case LANE_REPLAY: return OPC_SCAN_REPLAY;
    default:          return OPC_SCAN_APPROX;
    }
}

// call_names reports whether a call names the builtin `name`.
//
// Takes call (expr_t) which is the call site being resolved.
// Takes name (char) which is the name being looked up.
int call_names(const expr_t *call, const char *name) {
    return names_equal(call->name, call->name_length, name, (int) strlen(name));
}

// INTEGER_EXACT_LIMIT is 2^53, the point above which every double is already an integer.
// Below it, integer addition in double is `exact` and so associative, which is what lets
// the free order and the serial order agree.
#define INTEGER_EXACT_LIMIT 9007199254740992.0

// count_is_whole reports whether a non-negative double is an integer. Doubles at 2^53 and
// above are all integers, and below that a round trip through long long is `exact`.
//
// Takes value (double) which is the value being tested.
int count_is_whole(const double value) {
    return value >= INTEGER_EXACT_LIMIT ? 1 : (double) (long long) value == value;
}

// generator_count reads the element count written at a generator call, writing it through
// `out`. The count must be a numeric literal, non-negative and whole; anything else is
// reported as a structural defect and 0 is returned.
//
// The label is checked rather than assumed, because reading argument 0 by position alone
// would accept any one argument as the count. A count that is not a literal is accepted
// and `out` is NaN, which is what lets a generator inside a declaration take a parameter
// as its length: the standalone pass lowers every function with its parameters as leaves,
// and a parameter has no value at that point. Such a call checks and cannot run, because
// eval_materialise_indices tests for NaN first and fails the evaluation; the lane
// judgement never reads the count, so nothing it decides depends on the NaN.
//
// Takes state (resolve_state_t) which is the lowering in progress.
// Takes call (expr_t) which is the call site being resolved.
// Takes name (char) which is the generator's name as it was written.
// Takes out (double) which receives the answer.
static int generator_count(resolve_state_t *state, const expr_t *call, const char *name,
                           double *out) {
    if (call->arg_count < 1 || !call->args[0].value) {
        diagnostic_error(state->diagnostics, call->span,
                   "%s expects a count, as in %s(count: 1024.0)", name, name);
        state->malformed = 1;
        return 0;
    }
    if (!call->args[0].label
        || !NAMES_EQUAL_LITERAL(call->args[0].label, call->args[0].label_length, "count")) {
        diagnostic_error(state->diagnostics, call->args[0].value->span,
                   "a generator's first argument is its length and is written `count:`, as in "
                   "%s(count: 1024.0)", name);
        state->malformed = 1;
        return 0;
    }
    if (call->arg_count != 1) {
        diagnostic_error(state->diagnostics, call->args[1].value->span,
                   "a generator takes exactly one argument, its `count:`, and this call passes %d. "
                   "An argument the builtin does not read would be evaluated and dropped",
                   call->arg_count);
        state->malformed = 1;
        return 0;
    }
    if (call->args[0].value->kind != EX_NUMBER) {
        *out = NAN;
        return 1;
    }

    const double count = call->args[0].value->number;
    if (!(count >= 0.0) || !count_is_whole(count)) {
        diagnostic_error(state->diagnostics, call->args[0].value->span,
                   "a %s count must be a non-negative whole number", name);
        state->malformed = 1;
        return 0;
    }
    *out = count;
    return 1;
}

// intern_or_report interns a node, reporting a failed allocation at `at`: an
// out-of-memory defect must be distinguishable from an undefined name, never a bare
// malformed verdict over an empty sink.
//
// It stamps the promise scope, and it is the one funnel every node passes through, so the
// mark cannot be forgotten by a node kind added later. The field is part of the interning
// key, so it has to be set before the node is offered for interning.
//
// Takes state (resolve_state_t) which is the lowering in progress.
// Takes node (dagnode_t) which is the node under examination.
// Takes at (span_t) which is where a failed allocation is reported.
int intern_or_report(resolve_state_t *state, const dagnode_t *node, const span_t at) {
    dagnode_t stamped = *node;
    stamped.promise = state->promise_scope;
    const int index = dag_intern(state->dag, &stamped);
    if (index < 0) {
        diagnostic_error(state->diagnostics, at, "out of memory while building the graph");
        state->malformed = 1;
    }
    return index;
}

// refuse_order_argument reports an `order:` setting written on an operation that has no
// oracle-order kernel to honour it with.
//
// The setting is refused rather than ignored, because written on the call it is a
// request, and the honest answer to a request that cannot be met is to refuse it. Every
// operation with no oracle-order kernel calls this, and the reason differs by operation,
// which is why the caller supplies it.
//
// Takes state (resolve_state_t) which is the lowering in progress.
// Takes call (expr_t) which is the call site being resolved.
// Takes reason (char) which says why this operation cannot honour a dial.
// Returns 1 when no dial was written, 0 after reporting one that cannot be honoured.
static int refuse_order_argument(resolve_state_t *state, expr_t *call, const char *reason) {
    for (int i = 0; i < call->arg_count; i++) {
        if (!call->args[i].label
            || !NAMES_EQUAL_LITERAL(call->args[i].label, call->args[i].label_length, "order")) {
            continue;
        }
        diagnostic_error(state->diagnostics,
                         call->args[i].value ? call->args[i].value->span : call->span,
                   "order: cannot be honoured here: %s", reason);
        state->malformed = 1;
        return 0;
    }
    return 1;
}

// refuse_stray_label reports a label on an argument the builtin reads bare.
//
// A label the builtin does not read is a claim nothing checks: `sum(banana: x)` would
// lower exactly as `sum(x)`, leaving the reader to trust a word the compiler never looked
// at. Refusing it is the rule the checker applies everywhere else: a name it does not
// recognise is an error, not a decoration.
//
// Takes state (resolve_state_t) which is the lowering in progress.
// Takes call (expr_t) which is the call site being resolved.
// Takes index (int) which is the argument that must be bare.
// Takes name (char) which is the builtin's name as it was written.
// Returns 1 when the argument is bare, 0 after reporting a label.
static int refuse_stray_label(resolve_state_t *state, const expr_t *call, const int index,
                              const char *name) {
    const arg_t *arg = &call->args[index];
    if (!arg->label) {
        return 1;
    }
    diagnostic_error(state->diagnostics, arg->value ? arg->value->span : call->span,
               "%s takes its column bare, and '%.*s:' names no argument it reads",
               name, arg->label_length, arg->label);
    state->malformed = 1;
    return 0;
}

// lower_indices builds the node for the indices() builtin and returns its index, or -1
// after reporting a defect.
//
// Takes state (resolve_state_t) which is the lowering in progress.
// Takes call (expr_t) which is the call site being resolved.
// Takes integers (int) which selects integers() over indices().
int lower_indices(resolve_state_t *state, expr_t *call, const int integers) {
    if (!refuse_order_argument(state, call,
                               "a generator computes a column rather than folding one, so there "
                               "is no order to choose")) {
        return -1;
    }
    double count;
    if (!generator_count(state, call, integers ? "integers" : "indices", &count)) {
        return -1;
    }
    dagnode_t node;
    dag_node_init(&node);
    node.op = integers ? OP_INTEGERS : OP_INDICES;
    node.op_class = OPC_NEUTRAL;
    node.lane = state->ambient;
    node.span = call->span;
    node.label = call->name;
    node.label_length = call->name_length;
    node.constant = count;
    return intern_or_report(state, &node, call->span);
}

// lower_scan builds the node for the scan() builtin and returns its index, or -1 after
// reporting a defect.
//
// A scan is the second operation family the middle class is reachable on.
//
// Takes state (resolve_state_t) which is the lowering in progress.
// Takes call (expr_t) which is the call site being resolved.
int lower_scan(resolve_state_t *state, expr_t *call) {
    if (!refuse_order_argument(state, call,
                               "a scan lowers to the fixed-block form whatever the dial says, and "
                               "the blocks are already serial")) {
        return -1;
    }
    if (call->arg_count != 1) {
        diagnostic_error(state->diagnostics, call->span, "scan expects exactly one argument");
        state->malformed = 1;
        return -1;
    }
    if (!refuse_stray_label(state, call, 0, "scan")) {
        return -1;
    }
    const int input = lower_expr(state, call->args[0].value);
    if (input < 0) {
        return -1;
    }

    dagnode_t node;
    dag_node_init(&node);
    node.op = OP_SCAN;
    node.op_class = scan_class(state->ambient);
    node.lane = state->ambient;
    node.span = call->span;
    node.label = call->name;
    node.label_length = call->name_length;
    node.in[0] = input;
    node.input_count = 1;
    return intern_or_report(state, &node, call->span);
}

// choose_argument returns the value of `call`'s argument at `position`, requiring it to
// carry `label`, or NULL after reporting a defect.
//
// The labels are enforced rather than decorative. choose's three arguments are all
// columns and none of them is interchangeable with another, so a positional call gives a
// reader nothing to check the order against, and swapping the arms inverts the program in
// silence.
//
// Takes state (resolve_state_t) which is the lowering in progress.
// Takes call (expr_t) which is the call site being resolved.
// Takes position (int) which is the argument index.
// Takes label (char) which is the label that argument must carry.
static expr_t *choose_argument(
    resolve_state_t *state,
    expr_t *call,
    const int position,
    const char *label
) {
    const arg_t *arg = &call->args[position];
    const int length = (int) strlen(label);
    if (!arg->label || arg->label_length != length
        || memcmp(arg->label, label, (size_t) length) != 0) {
        diagnostic_error(state->diagnostics, call->span,
                   "choose expects choose(when: mask, then: a, else: b), and argument %d "
                   "is not '%s:'", position + 1, label);
        state->malformed = 1;
        return NULL;
    }
    return arg->value;
}

// lower_choose builds the node for the choose() builtin and returns its index, or -1
// after reporting a defect.
//
// Three operands and no fourth thing: the mask goes in `in[0]` beside the two arms, and
// the join sweep joins every entry of `in` without asking what any of them means, so the
// condition's lane reaches the result through the rule that was already there. Both arms
// are lowered whatever the mask says, because both arms are in the graph.
//
// Takes state (resolve_state_t) which is the lowering in progress.
// Takes call (expr_t) which is the call site being resolved.
// Returns the node's index, or -1 after reporting a defect.
int lower_choose(resolve_state_t *state, expr_t *call) {
    if (call->arg_count != CHOOSE_ARGUMENT_COUNT) {
        diagnostic_error(state->diagnostics, call->span,
                   "choose expects exactly three arguments, as in "
                   "choose(when: mask, then: a, else: b)");
        state->malformed = 1;
        return -1;
    }
    expr_t *mask_expr = choose_argument(state, call, 0, "when");
    expr_t *then_expr = choose_argument(state, call, 1, "then");
    expr_t *else_expr = choose_argument(state, call, 2, "else");
    if (!mask_expr || !then_expr || !else_expr) {
        return -1;
    }

    const int mask = lower_expr(state, mask_expr);
    if (mask < 0) {
        return -1;
    }
    const int when_true = lower_expr(state, then_expr);
    if (when_true < 0) {
        return -1;
    }
    const int when_false = lower_expr(state, else_expr);
    if (when_false < 0) {
        return -1;
    }

    dagnode_t node;
    dag_node_init(&node);
    node.op = OP_CHOOSE;
    node.op_class = OPC_NEUTRAL;
    node.lane = state->ambient;
    node.span = call->span;
    node.label = call->name;
    node.label_length = call->name_length;
    node.in[0] = mask;
    node.in[1] = when_true;
    node.in[2] = when_false;
    node.input_count = CHOOSE_ARGUMENT_COUNT;
    return intern_or_report(state, &node, call->span);
}

// lower_source builds the node for the source() builtin and returns its index, or -1
// after reporting a defect.
//
// The node this builds is a column the program does not fix; a call to a function
// declared `ungrounded` is the other one. Its class is neutral like every other leaf's,
// because transparency is a claim about combination and a leaf combines nothing;
// `grounded` is what speaks to where a leaf's value came from. It is not interned: two
// source() calls are two values, because nothing says the world hands back the same
// column twice, and merging them would assume exactly the property the grounding
// judgement exists to refuse.
//
// Takes state (resolve_state_t) which is the lowering in progress.
// Takes call (expr_t) which is the call site being resolved.
// Returns the node's index, or -1 after reporting a defect.
int lower_source(resolve_state_t *state, expr_t *call) {
    if (!refuse_order_argument(state, call,
                               "a source hands back a column the world fixed, so there is no "
                               "order for this program to choose")) {
        return -1;
    }
    double count;
    if (!generator_count(state, call, "source", &count)) {
        return -1;
    }
    dagnode_t node;
    dag_node_init(&node);
    node.op = OP_SOURCE;
    node.op_class = OPC_NEUTRAL;
    node.lane = state->ambient;
    node.span = call->span;
    node.label = call->name;
    node.label_length = call->name_length;
    node.constant = count;
    const int index = dag_append(state->dag, &node);
    if (index < 0) {
        diagnostic_error(state->diagnostics, call->span, "out of memory while building the graph");
        state->malformed = 1;
        return -1;
    }
    return index;
}

// ORDER_ARGUMENT_UNUSABLE is read_order_argument's third answer, distinct from "no dial
// here" because the caller must stop rather than lower a fold whose setting was already
// refused.
#define ORDER_ARGUMENT_UNUSABLE (-2)

// read_order_argument reads a fold's `order:` setting, reporting a value it does not
// recognise, and reports which argument carried it.
//
// `order:` is a setting rather than a qualifier, so it sits beside `count:` in the
// argument list, and confusing a dial with a lane is unspellable: a lane is a qualifier
// in a declaration position and the dial is an argument inside a call. The value is
// matched here rather than lowered, because `oracle` names a fold order and not a value.
// It reads and must not remove: the syntax tree is shared, one declaration is lowered
// once on its own and again at each call site, and deleting the argument from
// `call->args` would mutate the tree every later lowering reads, so the dial would be
// honoured on the first lowering and silently absent on every one after it.
//
// Takes state (resolve_state_t) which is the lowering in progress.
// Takes call (expr_t) which is the call site being resolved.
// Takes ordered (int) which receives whether the oracle order was asked for.
// Returns the index of the argument carrying the dial, -1 when there is none, or
// ORDER_ARGUMENT_UNUSABLE after reporting a setting it will not honour.
static int read_order_argument(resolve_state_t *state, const expr_t *call, int *ordered) {
    *ordered = 0;
    for (int i = 0; i < call->arg_count; i++) {
        if (!call->args[i].label
            || !NAMES_EQUAL_LITERAL(call->args[i].label, call->args[i].label_length, "order")) {
            continue;
        }
        const expr_t *value = call->args[i].value;
        if (!value || value->kind != EX_IDENT
            || !NAMES_EQUAL_LITERAL(value->name, value->name_length, "oracle")) {
            diagnostic_error(state->diagnostics, value ? value->span : call->span,
                       "order: takes 'oracle', the serial fold order, and nothing else");
            state->malformed = 1;
            return ORDER_ARGUMENT_UNUSABLE;
        }
        *ordered = 1;
        return i;
    }
    return -1;
}

// CONTROL_DIAL_ALIASES_DEMAND is 1 under the control build that aliases the dial onto a
// demand and 0 in every other, so that the break reads as a value the two functions below
// test rather than as an `#ifdef` wrapped around their statements, since a directive
// splitting a statement is a construct C's grammar cannot express.
#ifdef LANELANG_ORDERED_ALIASES_EXACT
#define CONTROL_DIAL_ALIASES_DEMAND 1
#else
#define CONTROL_DIAL_ALIASES_DEMAND 0
#endif

// control_alias_dial_onto_ambient applies the first half of the aliasing control's break,
// reading the reorder dial as though it had stated an `exact` demand.
//
// The dial and an `exact` ambient reach the same op class for a reduction, so what the
// alias cannot fake is the absence of a demand: aliasing declares a demand the source
// never wrote, and healing then removes a free fold to satisfy it.
//
// Takes state (resolve_state_t) which is the lowering in progress.
// Takes asked_for_order (int) which is whether the fold carried `order: oracle`.
static void control_alias_dial_onto_ambient(resolve_state_t *state, const int asked_for_order) {
    if (CONTROL_DIAL_ALIASES_DEMAND && asked_for_order && state->ambient == LANE_NONE) {
        state->ambient = LANE_EXACT;
    }
}

// control_alias_dial_onto_result applies the second half of the aliasing control's break.
//
// The alias must declare the demand rather than merely pick the kernel, because healing
// is driven by what a node declares.
//
// Takes state (resolve_state_t) which is the lowering in progress.
// Takes out (int) which is the fold's node, or -1 when it could not be built.
// Takes asked_for_order (int) which is whether the fold carried `order: oracle`.
// Takes at (span_t) which is the call the demand would be blamed on.
static void control_alias_dial_onto_result(resolve_state_t *state, const int out,
                                           const int asked_for_order, const span_t at) {
    if (CONTROL_DIAL_ALIASES_DEMAND && out >= 0 && asked_for_order) {
        (void) declare_demand_meet(state, out, LANE_EXACT, at);
    }
}

// lower_sum builds the node for the sum() and isum() builtins and returns its index, or
// -1 after reporting a defect.
//
// `integer` selects the exact-integer discharge of an `exact` demand. Its class does not
// come from the ambient lane, unlike an ordinary fold's, because an integer fold is
// `exact` under every schedule by construction, so there is nothing for a qualifier to
// strengthen and nothing an `approx` ambient could weaken.
//
// Takes state (resolve_state_t) which is the lowering in progress.
// Takes call (expr_t) which is the call site being resolved.
// Takes integer (int) which selects isum() over sum().
// Returns the node's index, or -1 after reporting a defect.
int lower_sum(resolve_state_t *state, expr_t *call, const int integer) {
    const char *name = integer ? "isum" : "sum";
    if (integer && !refuse_order_argument(state, call,
                                          "an exact-integer fold is order-independent by "
                                          "construction, so the setting has nothing to do")) {
        return -1;
    }
    int asked_for_order = 0;
    const int dial = read_order_argument(state, call, &asked_for_order);
    if (dial == ORDER_ARGUMENT_UNUSABLE) {
        return -1;
    }
    const int folded = dial == 0 ? 1 : 0;
    if (call->arg_count != (asked_for_order ? 2 : 1)) {
        diagnostic_error(state->diagnostics, call->span,
                   "%s expects exactly one argument, and an optional order:", name);
        state->malformed = 1;
        return -1;
    }
    if (!refuse_stray_label(state, call, folded, name)) {
        return -1;
    }
    const int ordered = asked_for_order;
    control_alias_dial_onto_ambient(state, asked_for_order);
    const int input = lower_expr(state, call->args[folded].value);
    if (input < 0) {
        return -1;
    }

    dagnode_t node;
    dag_node_init(&node);
    node.op = OP_FOLD;
    node.op_class = integer ? OPC_FOLD_INTEGER : fold_class(state->ambient, ordered);
    node.lane = state->ambient;
    node.in[0] = input;
    node.input_count = 1;
    node.span = call->span;
    node.label = call->name;
    node.label_length = call->name_length;
    const int out = intern_or_report(state, &node, call->span);
    control_alias_dial_onto_result(state, out, asked_for_order, call->span);
    return out;
}
