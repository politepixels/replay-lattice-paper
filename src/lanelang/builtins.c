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
// An unqualified floating-point fold and an `approx` fold both allow reassociation. Scans
// have their own classes in scan_class.
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
// An `exact` or `replay` ambient selects the fixed-block scan. The scan has no
// oracle-order kernel, so refuse_order_argument diagnoses that setting before the class
// is chosen.
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

// INTEGER_EXACT_LIMIT is 2^53. At and above this threshold every finite binary64 value is
// a whole number.
#define INTEGER_EXACT_LIMIT 9007199254740992.0

// count_is_whole tests a finite, non-negative double for integrality. At and above 2^53,
// every such value is integral. Below it, a round trip through long long detects a
// fractional part.
//
// Takes value (double) which is the value being tested.
int count_is_whole(const double value) {
    return value >= INTEGER_EXACT_LIMIT ? 1 : (double) (long long) value == value;
}

// generator_count reads a generator's literal length. For a non-literal count it lowers
// the expression, leaving its demands in the graph for the checker, and records an
// unknown length as NaN. The count contributes no operand to the generator.
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
        if (lower_expr(state, call->args[0].value) < 0) {
            return 0;
        }
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

// intern_or_report interns a node and reports an allocation failure at `at`, so the
// diagnostic identifies where graph construction failed.
//
// It stamps the promise scope, and every interned node passes through it, so the mark
// cannot be forgotten by a node kind added later. The field is part of the interning key,
// so it has to be set before the node is offered for interning.
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
// Generators, sources, scans and integer folds each supply the reason they cannot honour
// the setting. The argument checks for `choose` and `iterate` enforce their own labels.
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
// For example, `sum(banana: x)` would otherwise lower exactly like `sum(x)`, even though
// the builtin has no argument named `banana`.
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

// lower_indices builds the node for indices() or integers() and returns its index, or -1
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
                               "there is no oracle-order scan kernel, and the fixed-block scan is "
                               "replay rather than the serial order")) {
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
// The labels identify the mask and the two arms. Swapping the arms in a positional call
// would silently reverse the choice.
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
// The mask occupies `in[0]` alongside the two arms. The ordinary join over the inputs
// carries the mask's lane to the result. Both arms enter the graph whatever the mask
// selects.
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
// The program does not fix a source column. Like any leaf, a source has a neutral
// schedule class because it combines no inputs. The grounding check accounts for its
// dependence on the outside world. Each call gets its own node because the world may
// return different columns to two calls.
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
// The `order:` setting belongs to a call, while a lane qualifier belongs to a
// declaration. The resolver recognises `oracle` here as a fold order. It leaves the
// argument in `call->args` because the standalone check and every inlined call read the
// same syntax tree.
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

// CONTROL_DIAL_ALIASES_DEMAND is 1 in the control build that treats the order dial as an
// exact demand. It is 0 in other builds.
#ifdef LANELANG_ORDERED_ALIASES_EXACT
#define CONTROL_DIAL_ALIASES_DEMAND 1
#else
#define CONTROL_DIAL_ALIASES_DEMAND 0
#endif

// control_alias_dial_onto_ambient applies the first half of the aliasing control's break,
// reading the reorder dial as though it had stated an `exact` demand.
//
// The dial and an `exact` ambient select the same reduction class. The control also
// declares a demand the source never wrote, which lets healing pin a free fold to satisfy
// it.
//
// Takes state (resolve_state_t) which is the lowering in progress.
// Takes asked_for_order (int) which is whether the fold carried `order: oracle`.
static void control_alias_dial_onto_ambient(resolve_state_t *state, const int asked_for_order) {
    if (CONTROL_DIAL_ALIASES_DEMAND && asked_for_order && state->ambient == LANE_NONE) {
        state->ambient = LANE_EXACT;
    }
}

// control_alias_dial_onto_result applies the second half of the aliasing control's break,
// and returns the node the call stands for.
//
// The alias must declare the demand as well as pick the kernel, because healing is driven
// by what a node declares, and the call must stand for the demand node, or the demand
// hangs off the graph and no walk from the result ever reaches it.
//
// Takes state (resolve_state_t) which is the lowering in progress.
// Takes out (int) which is the fold's node, or -1 when it could not be built.
// Takes asked_for_order (int) which is whether the fold carried `order: oracle`.
// Takes at (span_t) which is the call the demand would be blamed on.
// Returns the demand node under the alias, and `out` otherwise.
static int control_alias_dial_onto_result(resolve_state_t *state, const int out,
                                          const int asked_for_order, const span_t at) {
    if (CONTROL_DIAL_ALIASES_DEMAND && out >= 0 && asked_for_order) {
        return declare_demand(state, out, LANE_EXACT, at);
    }
    return out;
}

// lower_sum builds the node for the sum() and isum() builtins and returns its index, or
// -1 after reporting a defect.
//
// The integer fold has the exact-integer class in every ambient. Its evaluator checks
// that the input consists of integers whose absolute values sum to less than 2^53. Within
// that domain every allowed fold order is exact.
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
    return control_alias_dial_onto_result(state, out, asked_for_order, call->span);
}
