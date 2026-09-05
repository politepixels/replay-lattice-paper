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

#include "eval.h"

#include "generator.h"
#include "kernels.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// EVAL_MAX_INDICES is the largest column a source or generator materialises; a larger
// count is clamped, which keeps an enormous request from exhausting memory. A count below
// one is raised to one, because this evaluator has no representation for an empty column:
// eval_element_of broadcasts any column of length one or less by reading element zero,
// and a zero-length column would be a null pointer that nothing may read.
#define EVAL_MAX_INDICES 100000

// g_column_elements is how many elements the last run wrote to whole-column storage, kept
// at file scope because it is diagnostic and this evaluator is single-threaded.
static size_t g_column_elements;

// g_tile_elements is how many elements the last run wrote to tile storage.
static size_t g_tile_elements;

// eval_last_footprint reports the last run's column and tile footprints; either pointer
// may be NULL.
//
// Takes columns (size_t) which receives the column footprint.
// Takes tiles (size_t) which receives the tile footprint.
void eval_last_footprint(size_t *columns, size_t *tiles) {
    if (columns) {
        *columns = g_column_elements;
    }
    if (tiles) {
        *tiles   = g_tile_elements;
    }
}

// eval_apply_binary_operator applies one elementwise operator. An operator that is not
// '+', '-', '*' or '/' is treated as addition, which is what a graph built by the
// resolver never contains.
//
// Takes binary_operator (int) which is the operator character.
// Takes left (double) which is the first operand.
// Takes right (double) which is the second operand.
static double eval_apply_binary_operator(const int binary_operator, const double left, const double right) {
    switch (binary_operator) {
    case '-': return left - right;
    case '*': return left * right;
    case '/': return left / right;
    case '+':
    default:  return left + right;
    }
}

// eval_compute_element computes element `position` of node `node_index` without
// consulting the tile cache.
//
// The recursion terminates because a materialised node returns immediately and every leaf
// is materialised; a fold is a scalar and is always materialised by the time anything
// reads it, so only elementwise work reaches the fallback. OP_CHOOSE, OP_OPAQUE and
// OP_DEMAND materialise only under EVAL_MATERIALISE, so without the guards a fused or
// tiled read of a one-input wrapper would fall into the binary fallback and read `in[1]`,
// which is -1 on a one-input node, and a fused or tiled read of a choose would never look
// at its third operand.
//
// Takes state (eval_state_t) which is the evaluation in progress.
// Takes node_index (int) which is the node's index in the graph.
// Takes position (size_t) which is the element to read.
static double eval_compute_element(eval_state_t *state, const int node_index,
                                   const size_t position) {
    const dagnode_t *node = dag_at_const(state->graph, node_index);
    const size_t length = state->lengths[node_index];
    const size_t k = (length <= 1) ? 0 : position % length;
    if (!state->columns[node_index] && node->op == OP_CHOOSE) {
        return eval_element_of(state, node->in[0], k) != 0.0
               ? eval_element_of(state, node->in[1], k)
               : eval_element_of(state, node->in[2], k);
    }
    if (!state->columns[node_index] && (node->op == OP_OPAQUE || node->op == OP_DEMAND)) {
        return eval_element_of(state, node->in[0], k);
    }
    return state->columns[node_index]
           ? state->columns[node_index][k]
           : eval_apply_binary_operator(node->binary_operator,
                                        eval_element_of(state, node->in[0], k),
                                        eval_element_of(state, node->in[1], k));
}

// eval_refill_tile recomputes the tile of node `node_index` that contains element
// `position`, starting at the tile boundary below it.
//
// Takes state (eval_state_t) which is the evaluation in progress.
// Takes node_index (int) which is the node's index in the graph.
// Takes position (size_t) which is the element to read.
static void eval_refill_tile(eval_state_t *state, const int node_index, const size_t position) {
    const size_t length = state->lengths[node_index];
    const size_t low = (position / state->tile_length) * state->tile_length;
    size_t high = low + state->tile_length;
    if (high > length) {
        high = length;
    }
    for (size_t t = low; t < high; t++) {
        state->tile_buffers[node_index][t - low] = eval_compute_element(state, node_index, t);
    }
    state->tile_low[node_index] = low;
    state->tile_high[node_index] = high;
    g_tile_elements += high - low;
}

// eval_element_of reads element `position` of node `node_index`, broadcasting a scalar
// over any position and going through the tile cache when the traversal is tiled.
//
// Columns longer than one are always read in range, because the materialise guards refuse
// mismatched lengths before any read happens.
//
// Takes state (eval_state_t) which is the evaluation in progress.
// Takes node_index (int) which is the node's index in the graph.
// Takes position (size_t) which is the element to read.
double eval_element_of(eval_state_t *state, const int node_index, const size_t position) {
    if (state->columns[node_index]) {
        const size_t length = state->lengths[node_index];
        return state->columns[node_index][(length <= 1) ? 0 : position % length];
    }
    if (!state->tiled) {
        return eval_compute_element(state, node_index, position);
    }

    const size_t length = state->lengths[node_index];
    const size_t k = (length <= 1) ? 0 : position % length;
    if (k < state->tile_low[node_index] || k >= state->tile_high[node_index]) {
        eval_refill_tile(state, node_index, k);
    }
    return state->tile_buffers[node_index][k - state->tile_low[node_index]];
}

// eval_perturbation is the name of the schedule perturbation compiled into this build,
// "-" when there is none.
const char *eval_perturbation(void) {
#ifdef RL_TILE_REBRACKETS
    return "tile-rebrackets";
#elif defined(RL_PINNED_EVAL_BREAKS)
    return "pinned-eval-breaks";
#else
    return "-";
#endif
}

// eval_alloc_column gives node `index` whole-column storage of `length` elements and
// counts it against the column footprint.
//
// Freeing first makes this re-entrant, which a recurrence needs: the nodes between a
// recurrence and its body are materialised once per iteration, and without it each pass
// would leak the previous one's storage; for every other node the free is a no-op.
//
// Takes state (eval_state_t) which is the evaluation in progress.
// Takes index (int) which is the node's index in the graph.
// Takes length (size_t) which is how many elements the column holds.
// Returns 0 if allocation fails.
static int eval_alloc_column(eval_state_t *state, const int index, const size_t length) {
    free(state->columns[index]);
    state->columns[index] = NULL;
    state->lengths[index] = length;
    state->columns[index] = (double *) malloc(length * sizeof(double));
    if (!state->columns[index]) {
        return 0;
    }
    g_column_elements += length;
    return 1;
}

// eval_materialise_leaf materialises a literal or an external input as a one-element
// column.
//
// An input has no value in the source, so it is given a fixed one: this evaluator
// compares a graph against itself under different schedules, and any value that is the
// same on both sides serves.
//
// Takes state (eval_state_t) which is the evaluation in progress.
// Takes index (int) which is the node's index in the graph.
// Takes node (dagnode_t) which is the node under examination.
static int eval_materialise_leaf(eval_state_t *state, const int index, const dagnode_t *node) {
    if (!eval_alloc_column(state, index, 1)) {
        return 0;
    }
    state->columns[index][0] = (node->op == OP_CONST) ? node->constant : 1.0;
    return 1;
}

// eval_materialise_scan materialises a scan as a whole column.
//
// Elements are read through eval_element_of, never from `columns[input]` directly,
// because under a fused or tiled traversal an elementwise producer has no materialised
// column and reading one would be a null dereference. A scan's serial reference is one
// running total left to right, which is what scan_free computes at one worker, so the
// reference path calls it with workers=1 rather than adding a fourth kernel.
//
// Takes state (eval_state_t) which is the evaluation in progress.
// Takes index (int) which is the node's index in the graph.
// Takes node (dagnode_t) which is the node under examination.
// Takes workers (int) which is how many workers the schedule uses.
static int eval_materialise_scan(
    eval_state_t *state,
    const int index,
    const dagnode_t *node,
    const int workers
) {
    const int input = node->in[0];
    const size_t length = state->lengths[input];
    if (!eval_alloc_column(state, index, length)) {
        return 0;
    }
    if (state->reference) {
        scan_free(state, input, state->columns[index], length, 1);
    } else if (node->op_class == OPC_SCAN_REPLAY) {
        scan_fixed_block(state, input, state->columns[index], length, workers);
    } else {
        scan_free(state, input, state->columns[index], length, workers);
    }
    return 1;
}

// FNV_OFFSET_BASIS is FNV-1a's published 32-bit offset basis.
#define FNV_OFFSET_BASIS 2166136261u

// FNV_PRIME is FNV-1a's published 32-bit prime.
#define FNV_PRIME        16777619u

// eval_bless_checksum is FNV-1a over a column's bit patterns.
//
// Bit patterns rather than values, because the bless is a claim about the bytes that
// entered the computation: a signed zero and a NaN payload are values a comparison would
// call equal and a rerun would not reproduce. It is a checksum rather than a hash; it
// detects a file that changed, not an adversary who changed it, and nothing here resists
// a chosen collision.
//
// Takes data (double) which is the column to fingerprint.
// Takes count (size_t) which is how many elements it holds.
unsigned eval_bless_checksum(const double *data, const size_t count) {
    const unsigned char *bytes = (const unsigned char *) data;
    unsigned hash = FNV_OFFSET_BASIS;
    for (size_t i = 0; i < count * sizeof *data; i++) {
        hash = (hash ^ bytes[i]) * FNV_PRIME;
    }
    return hash;
}

// bless_checksum_holds verifies a blessed source against the column just loaded,
// returning 0 when a declared checksum does not match it.
//
// The check runs before any arithmetic and fails closed, because a run that carried on
// would produce numbers a downstream reader believes are reproducible with nothing
// anywhere saying otherwise. The diagnostic says which claim broke: the lane still holds
// and the fold order is unchanged, while what is false is the developer's premise that
// the bytes would be the same next run. `count` is the clamped count, which is the column
// that actually exists; a checksum taken over what was asked for rather than over what
// was loaded would never match.
//
// Takes node (dagnode_t) which is the source node under examination.
// Takes data (double) which is the column just loaded.
// Takes count (size_t) which is how many elements it holds.
static int bless_checksum_holds(const dagnode_t *node, const double *data, const size_t count) {
    if (node->bless <= 0) {
        return 1;
    }
    const unsigned actual = eval_bless_checksum(data, count);
    if ((long long) actual + 1 == node->bless) {
        return 1;
    }
    fprintf(stderr, "lanelang: blessed checksum does not match the data loaded\n");
    fprintf(stderr, "  blessed to accept 0x%08llx, loaded 0x%08x over %zu element(s)\n",
            (unsigned long long) (node->bless - 1), actual, count);
    fprintf(stderr, "  the lane still holds: the fold order is unchanged. What no longer holds\n");
    fprintf(stderr, "  is that rerunning this program reproduces these bits\n");
    return 0;
}

// eval_materialise_source materialises an external source: a column the program does not
// fix.
//
// The value depends on `state->world`, which the caller varies between runs; every other
// leaf is a fixed function of the program, so this op is the one place a world change can
// reach the arithmetic.
//
// Takes state (eval_state_t) which is the evaluation in progress.
// Takes index (int) which is the node's index in the graph.
// Takes node (dagnode_t) which is the node under examination.
static int eval_materialise_source(eval_state_t *state, const int index, const dagnode_t *node) {
    double wanted = node->constant;
    if (wanted != wanted) {
        return 0;
    }
    if (!(wanted >= 1.0)) {
        wanted = 1.0;
    }
    if (wanted > (double) EVAL_MAX_INDICES) {
        wanted = (double) EVAL_MAX_INDICES;
    }
    const size_t count = (size_t) wanted;
    if (!eval_alloc_column(state, index, count)) {
        return 0;
    }
    generator_fill(state->world, state->columns[index], count);
    return bless_checksum_holds(node, state->columns[index], count);
}

// eval_materialise_indices materialises a generated column, drawn from the generator.
//
// The count is clamped while still a double, because converting a double outside size_t's
// range is undefined behaviour (C11 6.3.1.4); the first test also catches NaN.
//
// Takes state (eval_state_t) which is the evaluation in progress.
// Takes index (int) which is the node's index in the graph.
// Takes node (dagnode_t) which is the node under examination.
static int eval_materialise_indices(eval_state_t *state, const int index, const dagnode_t *node) {
    double wanted = node->constant;
    if (wanted != wanted) {
        return 0;
    }
    if (!(wanted >= 1.0)) {
        wanted = 1.0;
    }
    if (wanted > (double) EVAL_MAX_INDICES) {
        wanted = (double) EVAL_MAX_INDICES;
    }
    const size_t count = (size_t) wanted;
    if (!eval_alloc_column(state, index, count)) {
        return 0;
    }
    generator_fill(1u, state->columns[index], count);
    return 1;
}

// eval_materialise_integers materialises a column of `exact` integers.
//
// The values alternate sign and span three orders of magnitude, so a wrong fold order
// would be visible in the bits if the arithmetic could round; every element and every
// reachable partial stays well inside 2^53, so no schedule can move a bit. The pattern is
// a pure function of the index, so the column is identical on every machine and the bit
// patterns a check freezes are portable.
//
// Takes state (eval_state_t) which is the evaluation in progress.
// Takes index (int) which is the node's index in the graph.
// Takes node (dagnode_t) which is the node under examination.
static int eval_materialise_integers(eval_state_t *state, const int index, const dagnode_t *node) {
    double wanted = node->constant;
    if (wanted != wanted) {
        return 0;
    }
    if (!(wanted >= 1.0)) {
        wanted = 1.0;
    }
    if (wanted > (double) EVAL_MAX_INDICES) {
        wanted = (double) EVAL_MAX_INDICES;
    }
    const size_t count = (size_t) wanted;
    if (!eval_alloc_column(state, index, count)) {
        return 0;
    }
    for (size_t i = 0; i < count; i++) {
        const double magnitude = (double) (long long) ((i % 997u) * 1021u + 1u);
        state->columns[index][i] = (i % 2u) ? -magnitude : magnitude;
    }
    return 1;
}

// eval_materialise_elementwise materialises an elementwise node, or leaves it fused.
//
// Operand columns of different lengths are refused unless one is a scalar, which
// broadcasts; without the guard the shorter column would be read modulo its length,
// wrapping around in silence.
//
// Takes state (eval_state_t) which is the evaluation in progress.
// Takes index (int) which is the node's index in the graph.
// Takes node (dagnode_t) which is the node under examination.
// Takes mode (eval_mode_t) which is the traversal to evaluate under.
static int eval_materialise_elementwise(
    eval_state_t *state,
    const int index,
    const dagnode_t *node,
    const eval_mode_t mode
) {
    const size_t left = state->lengths[node->in[0]];
    const size_t right = state->lengths[node->in[1]];
    if (left != right && left > 1 && right > 1) {
        fprintf(stderr, "lanelang: elementwise operands disagree on length\n"
                        "  %zu elements against %zu, and neither is a scalar\n",
                left, right);
        return 0;
    }
    const size_t length = left > right ? left : right;
    state->lengths[index] = length;
    if (mode != EVAL_MATERIALISE) {
        return 1;
    }

    if (!eval_alloc_column(state, index, length)) {
        return 0;
    }
    for (size_t k = 0; k < length; k++) {
        state->columns[index][k] = eval_apply_binary_operator(
            node->binary_operator,
            eval_element_of(state, node->in[0], k),
            eval_element_of(state, node->in[1], k)
        );
    }
    return 1;
}

// eval_materialise_choose computes choose(when:, then:, else:) elementwise and returns 1,
// or 0 if a column could not be allocated.
//
// Both arms are materialised whatever the mask holds, and that is the semantics rather
// than a missed optimisation: predication means the result is a blend and not a jump, so
// there is no unevaluated arm, and the loop combines nothing across positions, so its
// class is neutral. A zero mask element takes the `else` arm and any other value takes
// the `then` arm; this language has no comparison operators, so masks are not constrained
// to {0.0, 1.0} and the wider reading is used.
//
// Takes state (eval_state_t) which is the evaluation in progress.
// Takes index (int) which is the node being materialised.
// Takes node (dagnode_t) which is that node.
// Takes mode (eval_mode_t) which says whether columns are being filled or only measured.
static int eval_materialise_choose(
    eval_state_t *state,
    const int index,
    const dagnode_t *node,
    const eval_mode_t mode
) {
    size_t length = state->lengths[node->in[0]];
    for (int operand = 1; operand < node->input_count; operand++) {
        const size_t candidate = state->lengths[node->in[operand]];
        if (candidate != length && candidate > 1 && length > 1) {
            fprintf(stderr, "lanelang: choose operands disagree on length\n"
                            "  %zu elements against %zu, and neither is a scalar\n",
                    candidate, length);
            return 0;
        }
        if (candidate > length) {
            length = candidate;
        }
    }
    state->lengths[index] = length;
    if (mode != EVAL_MATERIALISE) {
        return 1;
    }

    if (!eval_alloc_column(state, index, length)) {
        return 0;
    }
    for (size_t k = 0; k < length; k++) {
        const double mask = eval_element_of(state, node->in[0], k);
        state->columns[index][k] = mask != 0.0
            ? eval_element_of(state, node->in[1], k)
            : eval_element_of(state, node->in[2], k);
    }
    return 1;
}

// eval_materialise_recur materialises a recurrence's current value.
//
// On the first pass this is the seed; each later pass overwrites it with the body's value
// before the nodes between them are run again, so this only ever establishes the starting
// point.
//
// Takes state (eval_state_t) which is the evaluation in progress.
// Takes index (int) which is the node's index in the graph.
// Takes node (dagnode_t) which is the node under examination.
static int eval_materialise_recur(eval_state_t *state, const int index, const dagnode_t *node) {
    const int init = node->in[0];
    const size_t length = state->lengths[init];
    if (!eval_alloc_column(state, index, length)) {
        return 0;
    }
    for (size_t k = 0; k < length; k++) {
        state->columns[index][k] = eval_element_of(state, init, k);
    }
    return 1;
}

// eval_materialise_fold materialises a reduction as a one-element column, using the
// kernel its schedule class names.
//
// Under the reference evaluation every fold is the serial one, whatever class it carries;
// the test sits before the switch so that a kernel added later cannot forget to be
// covered, and no control perturbs the reference path, so the reference stays the
// reference. The domain guard is not a schedule, so the reference owes it too: an
// exact-integer fold refuses a column whose magnitudes sum past 2^53, and a reference
// that answered where every schedule refused would be returning the rounded number
// `exact` is defined as equality with.
//
// Takes state (eval_state_t) which is the evaluation in progress.
// Takes index (int) which is the node's index in the graph.
// Takes node (dagnode_t) which is the node under examination.
// Takes workers (int) which is how many workers the schedule uses.
static int eval_materialise_fold(
    eval_state_t *state,
    const int index,
    const dagnode_t *node,
    const int workers
) {
    const int input = node->in[0];
    const size_t length = state->lengths[input];
    if (!eval_alloc_column(state, index, 1)) {
        return 0;
    }
    if (state->reference) {
        if (node->op_class == OPC_FOLD_INTEGER) {
            return fold_integer(state, input, length, 1, &state->columns[index][0]);
        }
        state->columns[index][0] = fold_strict(state, input, length);
        return 1;
    }
    switch (node->op_class) {
    case OPC_FOLD_EXACT:  return fold_exact(state, input, length, &state->columns[index][0]);
    case OPC_FOLD_INTEGER: return fold_integer(state, input, length, workers,
                                               &state->columns[index][0]);
    case OPC_FOLD_REPLAY: return fold_replay(state, input, length, &state->columns[index][0]);
    default:              state->columns[index][0] = fold_free(state, input, length, workers);
    }
    return 1;
}

// eval_materialise_opaque materialises a call declared `ungrounded` as its body's value.
//
// The wrapper computes nothing, because `ungrounded` is a claim about where a value comes
// from rather than about what it is: the node carries a grounding answer and passes the
// arithmetic straight through, with its input's length and its input's elements. Elements
// are read through eval_element_of rather than from the input's column, because under a
// fused or tiled traversal a producer has no materialised column and reading one would be
// a null dereference.
//
// Takes state (eval_state_t) which is the evaluation in progress.
// Takes index (int) which is the node's index in the graph.
// Takes node (dagnode_t) which is the node under examination.
// Takes mode (eval_mode_t) which is the traversal to evaluate under.
// Returns 0 if allocation fails.
static int eval_materialise_opaque(
    eval_state_t *state,
    const int index,
    const dagnode_t *node,
    const eval_mode_t mode
) {
    const size_t length = state->lengths[node->in[0]];
    state->lengths[index] = length;
    if (mode != EVAL_MATERIALISE) {
        return 1;
    }
    if (!eval_alloc_column(state, index, length)) {
        return 0;
    }
    for (size_t k = 0; k < length; k++) {
        state->columns[index][k] = eval_element_of(state, node->in[0], k);
    }
    return 1;
}

// eval_materialise_tick gives node `index` a one-element column holding the current tick
// of the recurrence it counts.
//
// The counter lives in eval_materialise_all's `done` array, because it is the state of a
// run and not a property of the graph; a tick node sits after its recurrence and before
// that recurrence's body, so the rewind re-materialises it once per tick and it reads 0,
// 1, ... n-1 in turn.
//
// Takes state (eval_state_t) which is the evaluation in progress.
// Takes index (int) which is the node's index in the graph.
// Takes node (dagnode_t) which is the node being materialised.
// Takes done (int) which is how many ticks each recurrence has completed.
// Returns 0 if allocation fails.
static int eval_materialise_tick(eval_state_t *state, const int index, const dagnode_t *node,
                                 const int *done) {
    if (!eval_alloc_column(state, index, 1)) {
        return 0;
    }
    state->columns[index][0] = (double) done[node->tick_of];
    return 1;
}

// eval_materialise_node gives node `index` whatever storage its operation calls for.
//
// Takes state (eval_state_t) which is the evaluation in progress.
// Takes index (int) which is the node's index in the graph.
// Takes mode (eval_mode_t) which is the traversal to evaluate under.
// Takes workers (int) which is how many workers the schedule uses.
// Takes done (int) which is how many ticks each recurrence has completed.
// Returns 0 if allocation fails.
static int eval_materialise_node(eval_state_t *state, const int index, const eval_mode_t mode,
                                 const int workers, const int *done) {
    const dagnode_t *node = dag_at_const(state->graph, index);
    switch (node->op) {
    case OP_CONST:
    case OP_INPUT:   return eval_materialise_leaf(state, index, node);
    case OP_SOURCE:  return eval_materialise_source(state, index, node);
    case OP_SCAN:    return eval_materialise_scan(state, index, node, workers);
    case OP_INDICES: return eval_materialise_indices(state, index, node);
    case OP_INTEGERS: return eval_materialise_integers(state, index, node);
    case OP_ELEM:    return eval_materialise_elementwise(state, index, node, mode);
    case OP_CHOOSE:  return eval_materialise_choose(state, index, node, mode);
    case OP_FOLD:    return eval_materialise_fold(state, index, node, workers);
    case OP_RECUR:  return eval_materialise_recur(state, index, node);
    case OP_TICK:   return eval_materialise_tick(state, index, node, done);
    case OP_OPAQUE:
    case OP_DEMAND: return eval_materialise_opaque(state, index, node, mode);
    }
    return 1;
}

// eval_recur_owning_body finds the recurrence whose body is node `body`, returning its
// index, or -1 when no recurrence reads it.
//
// Takes g (dag_t) which is the graph being walked.
// Takes body (int) which is the recurrence body.
static int eval_recur_owning_body(const dag_t *g, const int body) {
    const int count = dag_count(g);
    for (int i = 0; i < count; i++) {
        if (dag_at_const(g, i)->carried == body) {
            return i;
        }
    }
    return -1;
}

// eval_restart_nested puts every recurrence between `owner` and its body back to zero
// ticks, so that a nested recurrence restarts with the tick that contains it.
//
// The rewind re-materialises every node in that range and eval_materialise_recur puts
// each recurrence there back to its seed; its tick count has to go back with it, or the
// inner recurrence sits at its seed for every outer tick after the first and contributes
// a value it never computed.
//
// Takes done (int) which is how many ticks each recurrence has completed.
// Takes owner (int) which is the recurrence the walk is about to rewind to.
// Takes body (int) which is that recurrence's body, the last node the rewind re-runs.
static void eval_restart_nested(int *done, const int owner, const int body) {
    for (int nested = owner + 1; nested <= body; nested++) {
        done[nested] = 0;
    }
}

// eval_materialise_all materialises every node, iterating each recurrence for the number
// of steps it declares.
//
// The shape is a loop with a rewind: nodes run in index order, and when the node just
// materialised is some recurrence's body, its value is carried back through
// eval_element_of, because the body may be fused and that read must happen before the
// tile cache is dropped. Every live tile is then dropped, because the cache holds values
// computed from the previous tick's carried value and a tiled traversal would otherwise
// answer from a stale tile. The rewind happens only while ticks remain, while the
// carry-back happens on every tick including the last, because the block's value is the
// carried name after the final tick; committing one fewer time returns the second-to-last
// value. A recurrence declares its own step count, so the number of rewinds is fixed
// before the run starts and the evaluation is total.
//
// Takes state (eval_state_t) which is the evaluation in progress.
// Takes count (int) which is how many nodes the graph holds.
// Takes mode (eval_mode_t) which is the traversal to evaluate under.
// Takes workers (int) which is how many workers the schedule uses.
static int eval_materialise_all(eval_state_t *state, const int count, const eval_mode_t mode, const int workers) {
    int *done = (int *) calloc((size_t) (count > 0 ? count : 1), sizeof(int));
    if (!done) {
        return 0;
    }

    for (int i = 0; i < count; i++) {
        if (!eval_materialise_node(state, i, mode, workers, done)) {
            free(done); return 0;
        }

        const int owner = eval_recur_owning_body(state->graph, i);
        if (owner < 0) {
            continue;
        }

        const int steps = (int) dag_at_const(state->graph, owner)->constant;
        if (done[owner] >= steps) {
            continue;
        }

        const size_t length = state->lengths[owner];
        for (size_t k = 0; k < length; k++) {
            state->columns[owner][k] = eval_element_of(state, i, k);
        }

        for (int n = 0; n < count; n++) {
            state->tile_low[n] = 0;
            state->tile_high[n] = 0;
        }

        done[owner]++;

        if (done[owner] < steps) {
            eval_restart_nested(done, owner, i);
            i = owner;
        }
    }

    free(done);
    return 1;
}

// eval_state_init allocates the per-node tables one run needs, for a graph of `count`
// nodes.
//
// Takes state (eval_state_t) which is the evaluation in progress.
// Takes g (dag_t) which is the graph being walked.
// Takes count (int) which is how many nodes the graph holds.
// Takes mode (eval_mode_t) which is the traversal to evaluate under.
// Takes tile (size_t) which is the tile width in elements.
// Returns 0 if any allocation fails, in which case the partially built state is still
// safe to free.
static int eval_state_init(
    eval_state_t *state,
    const dag_t *g,
    const int count,
    const eval_mode_t mode,
    const size_t tile
) {
    memset(state, 0, sizeof *state);
    state->graph = g;
    state->tiled = (mode == EVAL_TILED);
    state->tile_length = tile;
    state->columns      = (double **) calloc((size_t) count, sizeof(double *));
    state->lengths      = (size_t *)  calloc((size_t) count, sizeof(size_t));
    state->tile_buffers = (double **) calloc((size_t) count, sizeof(double *));
    state->tile_low     = (size_t *)  calloc((size_t) count, sizeof(size_t));
    state->tile_high    = (size_t *)  calloc((size_t) count, sizeof(size_t));
    if (!state->columns || !state->lengths || !state->tile_buffers ||
        !state->tile_low || !state->tile_high) {
        return 0;
    }
    if (!state->tiled) {
        return 1;
    }
    for (int i = 0; i < count; i++) {
        state->tile_buffers[i] = (double *) malloc(tile * sizeof(double));
        if (!state->tile_buffers[i]) {
            return 0;
        }
    }
    return 1;
}

// eval_state_free releases everything eval_state_init and the materialisers allocated,
// for a graph of `count` nodes. Safe on a state whose initialisation failed part way.
//
// Takes state (eval_state_t) which is the evaluation in progress.
// Takes count (int) which is how many nodes the graph holds.
static void eval_state_free(eval_state_t *state, const int count) {
    for (int i = 0; i < count; i++) {
        if (state->columns) {
            free(state->columns[i]);
        }
        if (state->tile_buffers) {
            free(state->tile_buffers[i]);
        }
    }
    free(state->columns);
    free(state->lengths);
    free(state->tile_buffers);
    free(state->tile_low);
    free(state->tile_high);
}

// eval_dag_in_world executes the graph under one traversal, tile width and worker count,
// writing the result's value through `out`.
//
// The answer is read through eval_element_of rather than off the result's column, because
// a node that materialises only under EVAL_MATERIALISE has no column under the other two
// traversals, and the one-input wrappers OP_OPAQUE and OP_DEMAND are exactly that shape;
// requiring a column here would make a program whose result is a wrapper fail to evaluate
// under fusion and tiling.
//
// Takes g (dag_t) which is the graph being walked.
// Takes result (int) which is the node the program returns.
// Takes mode (eval_mode_t) which is the traversal to evaluate under.
// Takes tile (size_t) which is the tile width in elements.
// Takes workers (int) which is how many workers the schedule uses.
// Takes world (unsigned) which selects what an external source hands back.
// Takes out (double) which receives the answer.
// Returns 0 when the run could not produce it.
int eval_dag_in_world(
    const dag_t *g, const int result, const eval_mode_t mode, const size_t tile,
    const int workers, const unsigned world, double *out
) {
    const int count = dag_count(g);
    if (count <= 0 || result < 0 || result >= count) {
        return 0;
    }
    const size_t tile_width = tile < 1 ? 1 : tile;

    g_column_elements = 0;
    g_tile_elements = 0;

    eval_state_t state;
    int ok = eval_state_init(&state, g, count, mode, tile_width);
    if (ok) {
        state.world = world;
    }
    if (ok) {
        ok = eval_materialise_all(&state, count, mode, workers);
    }
    if (ok) {
        *out = eval_element_of(&state, result, 0);
    }
    eval_state_free(&state, count);
    return ok;
}

// eval_dag evaluates `g` in the shipped world.
//
// Every schedule this evaluator varies leaves the world alone, so a lane can be a claim
// about pairs of runs with the inputs held equal; a check that wants to vary the world
// instead calls eval_dag_in_world directly, and what it measures is the other axis.
//
// Takes g (dag_t) which is the graph being walked.
// Takes result (int) which is the node the program returns.
// Takes mode (eval_mode_t) which is the traversal to evaluate under.
// Takes tile (size_t) which is the tile width in elements.
// Takes workers (int) which is how many workers the schedule uses.
// Takes out (double) which receives the answer.
int eval_dag(const dag_t *g, const int result, const eval_mode_t mode, const size_t tile,
             const int workers, double *out) {
    return eval_dag_in_world(g, result, mode, tile, workers, 1u, out);
}

// eval_dag_on_reference evaluates `g` with every combining kernel replaced by the serial
// order, and writes the result scalar to `out`.
//
// Materialised, one worker, no tiling: the reference is not a schedule, so it is
// evaluated at the one point of the schedule space where nothing has been chosen.
//
// Takes g (dag_t) which is the graph being walked.
// Takes result (int) which is the node the program returns.
// Takes out (double) which receives the answer.
int eval_dag_on_reference(const dag_t *g, const int result, double *out) {
    const int count = dag_count(g);
    if (count <= 0 || result < 0 || result >= count) {
        return 0;
    }

    g_column_elements = 0;
    g_tile_elements = 0;

    eval_state_t state;
    int ok = eval_state_init(&state, g, count, EVAL_MATERIALISE, 1);
    if (ok) {
        state.world = 1u; state.reference = 1;
    }
    if (ok) {
        ok = eval_materialise_all(&state, count, EVAL_MATERIALISE, 1);
    }
    if (ok && state.columns[result]) {
        memcpy(out, state.columns[result], sizeof(double));
    }
    else {
        ok = 0;
    }
    eval_state_free(&state, count);
    return ok;
}
