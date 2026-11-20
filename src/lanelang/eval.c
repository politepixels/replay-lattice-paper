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

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// EVAL_MAX_INDICES bounds the number of elements in one source or generated column.
#define EVAL_MAX_INDICES 100000

// g_column_elements counts element slots allocated for materialised nodes in the calling
// thread's last run.
static _Thread_local size_t g_column_elements;

// g_tile_elements counts all tile writes in the calling thread's last run.
static _Thread_local size_t g_tile_elements;

// g_replay_workers records the largest completed replay team in the last run.
static _Thread_local int g_replay_workers;

// eval_last_replay_workers returns the largest completed replay team in the last run.
int eval_last_replay_workers(void) {
    return g_replay_workers;
}

// eval_last_footprint reports the last run's column and tile footprints. Either pointer
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

// eval_apply_binary_operator applies one elementwise operator. The resolver emits '+',
// '-', '*' and '/'. An unknown operator defaults to addition.
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

// eval_compute_element reads a materialised value or computes it without consulting this
// node's tile cache.
//
// Reads stop at a materialised node. In fused and tiled traversals, OP_CHOOSE reads its
// mask and the selected arm. OP_OPAQUE and OP_DEMAND forward their operand. The remaining
// unmaterialised nodes are binary elementwise operations.
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
    const size_t high = state->tiles[node_index].capacity > length - low
                        ? length : low + state->tiles[node_index].capacity;
    for (size_t t = low; t < high; t++) {
        state->tiles[node_index].values[t - low] = eval_compute_element(state, node_index, t);
    }
    state->tiles[node_index].low = low;
    state->tiles[node_index].high = high;
    state->tile_elements += high - low;
}

// eval_element_of broadcasts a scalar over any position. It reads materialised columns
// directly and uses the tile cache for deferred producers in a tiled traversal.
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
    if (k < state->tiles[node_index].low || k >= state->tiles[node_index].high) {
        eval_refill_tile(state, node_index, k);
    }
    return state->tiles[node_index].values[k - state->tiles[node_index].low];
}

// eval_perturbation names the tile-rebrackets or pinned-eval-breaks control when active.
// Other builds return "-".
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
// A recurrence can materialise the same node on each iteration. Freeing its previous
// column before allocating the next one lets successive iterations use different lengths.
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

// eval_materialise_leaf materialises a literal or a parameter as a one-element column.
//
// A parameter has no value in the source. Giving it a fixed value keeps its input
// identical across the schedules being compared.
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
// Both scan kernels read through eval_element_of so a deferred input can supply its
// values on demand. The reference calls scan_free with one worker, giving one
// left-to-right chunk.
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
// The checksum covers the bytes loaded, including the sign of zero and the payload of a
// NaN. Numeric equality cannot distinguish the two signs of zero or compare NaN payloads.
// FNV-1a detects accidental changes but gives no protection against a chosen collision.
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
// A mismatch stops the run before any arithmetic over the column. The diagnostic
// distinguishes the two claims. The fold order still satisfies the requested lane, but
// the loaded bytes no longer match the recorded checksum.
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

// eval_alloc_input allocates a source or generated column of the requested length.
//
// Takes state (eval_state_t) which is the evaluation in progress.
// Takes index (int) which is the node that will own the column.
// Takes node (dagnode_t) which holds the requested length.
// Returns zero if the length is unknown, fractional or outside the supported range, or if
// allocation fails.
static int eval_alloc_input(eval_state_t *state, const int index, const dagnode_t *node) {
    const double count = node->constant;
    if (count != count) {
        fprintf(stderr, "lanelang: column length is unknown at evaluation time\n");
        return 0;
    }
    if (count < 1 || count > EVAL_MAX_INDICES || count != (double) (size_t) count) {
        fprintf(stderr, "lanelang: requested column length %.17g (expected a whole number from 1 to %d)\n",
                count, EVAL_MAX_INDICES);
        return 0;
    }
    return eval_alloc_column(state, index, (size_t) count);
}

// eval_materialise_generated fills a source column from the current world or an indices
// column from world 1.
//
// Takes state (eval_state_t) which is the evaluation in progress.
// Takes index (int) which is the node that will own the column.
// Takes node (dagnode_t) which selects the generator and holds any source checksum.
// Returns zero if the column cannot be allocated at its requested length or the source
// fails its checksum.
static int eval_materialise_generated(eval_state_t *state, const int index, const dagnode_t *node) {
    if (!eval_alloc_input(state, index, node)) {
        return 0;
    }
    const size_t count = state->lengths[index];
    const unsigned seed = node->op == OP_SOURCE ? state->world : 1u;
    generator_fill(seed, state->columns[index], count);
    return node->op != OP_SOURCE || bless_checksum_holds(node, state->columns[index], count);
}

// eval_materialise_integers materialises a column of `exact` integers.
//
// The values alternate in sign and their magnitudes depend only on the index. At the
// supported column lengths, their absolute values sum to less than 2^53. Every partial
// sum is therefore an exactly representable integer, whatever the schedule.
//
// Takes state (eval_state_t) which is the evaluation in progress.
// Takes index (int) which is the node's index in the graph.
// Takes node (dagnode_t) which is the node under examination.
static int eval_materialise_integers(eval_state_t *state, const int index, const dagnode_t *node) {
    if (!eval_alloc_input(state, index, node)) {
        return 0;
    }
    for (size_t i = 0; i < state->lengths[index]; i++) {
        const double magnitude = (double) (long long) ((i % 997u) * 1021u + 1u);
        state->columns[index][i] = (i % 2u) ? -magnitude : magnitude;
    }
    return 1;
}

// eval_materialise_elementwise materialises an elementwise node, or leaves it fused.
//
// A scalar broadcasts to the other operand's length. Two longer columns must have the
// same length because indexing the shorter one modulo its length would silently repeat
// its elements.
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

// eval_materialise_choose computes choose(when:, then:, else:) elementwise. It returns
// zero if the shapes disagree or allocation fails.
//
// The mask and both arms contribute to the result shape. A zero mask element selects the
// corresponding element of the `else` arm, and any other value selects the `then` arm.
// The traversal can read the selected element on demand. The choice has neutral schedule
// class because it combines no values across positions.
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
// This sets the initial carry from the seed. After each step, eval_carry_back replaces it
// with the body's result for the next tick.
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
// The reference uses serial order for every fold. The integer fold also checks its domain
// in the reference run, using one worker. Its inputs must be integers whose absolute
// values sum to strictly less than 2^53.
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
    case OPC_FOLD_REPLAY: return fold_replay(state, input, length, workers, &state->columns[index][0]);
    default:              state->columns[index][0] = fold_free(state, input, length, workers);
    }
    return 1;
}

// eval_materialise_opaque forwards an opaque call or a demand wrapper to its input.
//
// An opaque call passes through its body's length and elements. The `ungrounded`
// declaration affects the grounding judgement. Reading through eval_element_of also works
// when a fused or tiled producer has no whole column in storage.
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
// The step reads its enclosing recurrence's current clock, starting at zero.
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
// Returns zero if the node cannot be evaluated.
static int eval_materialise_node(eval_state_t *state, const int index, const eval_mode_t mode,
                                 const int workers, const int *done) {
    const dagnode_t *node = dag_at_const(state->graph, index);
    switch (node->op) {
    case OP_CONST:
    case OP_INPUT:   return eval_materialise_leaf(state, index, node);
    case OP_SOURCE:
    case OP_INDICES: return eval_materialise_generated(state, index, node);
    case OP_SCAN:    return eval_materialise_scan(state, index, node, workers);
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

// eval_carry_back replaces a recurrence's carry with its completed step result. A fused
// step can still read the old carry while producing the new column, whose length may
// differ. Cached tiles can depend on the old carry or tick and expire at this point.
//
// Takes state (eval_state_t) which is the evaluation in progress.
// Takes owner (int) which is the recurrence node that owns the carry.
// Takes body (int) which is the node holding the step result.
// Returns zero if the new carry could not be allocated.
static int eval_carry_back(eval_state_t *state, const int owner, const int body) {
    const size_t length = state->lengths[body];
    double *next = malloc(length * sizeof *next);
    if (!next) {
        return 0;
    }
    for (size_t k = 0; k < length; k++) {
        next[k] = eval_element_of(state, body, k);
    }
    free(state->columns[owner]);
    state->columns[owner] = next;
    state->lengths[owner] = length;
    g_column_elements += length;
    if (state->tiled) {
        for (int i = 0; i < dag_count(state->graph); i++) {
            state->tiles[i].high = 0;
        }
    }
    return 1;
}

// eval_step_count checks the bounds of a recurrence and gives the number of step
// evaluations it needs. A step with an empty range reuses an existing value on every tick
// and needs just one copy.
//
// Takes node (dagnode_t) which is the recurrence to check.
// Takes index (int) which is the recurrence's node index.
// Takes end (int) which is one past the last node in the enclosing range.
// Takes steps (int) which receives the number of step evaluations on success.
// Returns zero if the step extends outside the range or its result is unavailable.
static int eval_step_count(const dagnode_t *node, const int index, const int end, int *steps) {
    if (node->step_end < index || node->step_end >= end ||
        node->carried < 0 || node->carried > node->step_end) {
        return 0;
    }
    *steps = node->step_end == index ? 1 : (int) node->constant;
    return 1;
}

// eval_prepare_tile allocates enough space for one tile of a deferred producer.
//
// Takes state (eval_state_t) which owns the cache.
// Takes node (int) which selects the producer.
// Returns zero on allocation failure.
static int eval_prepare_tile(eval_state_t *state, const int node) {
    const size_t length = state->lengths[node];
    const size_t width = state->tile_length < length ? state->tile_length : length;
    eval_tile_t *tile = &state->tiles[node];
    if (width <= tile->capacity) {
        return 1;
    }
    if (width > SIZE_MAX / sizeof *tile->values) {
        return 0;
    }
    double *values = realloc(tile->values, width * sizeof *values);
    if (!values) {
        return 0;
    }
    tile->values = values;
    tile->capacity = width;
    return 1;
}

// eval_prepare_inputs allocates tiles along the paths a worker can read.
//
// Takes state (eval_state_t) which owns the private cache.
// Takes input (int) which selects the operand.
// Returns zero on allocation failure.
static int eval_prepare_inputs(eval_state_t *state, const int input) {
    if (state->columns[input] || state->tiles[input].values) {
        return 1;
    }
    if (!eval_prepare_tile(state, input)) {
        return 0;
    }
    const dagnode_t *node = dag_at_const(state->graph, input);
    for (int i = 0; i < node->input_count; i++) {
        if (!eval_prepare_inputs(state, node->in[i])) {
            return 0;
        }
    }
    return 1;
}

// eval_worker_reader prepares a private reader over the parent's immutable fold inputs.
//
// Takes reader (eval_state_t) which receives the worker's view.
// Takes state (eval_state_t) which supplies the shared graph and columns.
// Takes input (int) which selects the fold operand.
// Returns zero on allocation failure.
int eval_worker_reader(eval_state_t *reader, const eval_state_t *state, const int input) {
    *reader = *state;
    reader->tiles = NULL;
    reader->tile_elements = 0;
    if (!state->tiled || state->columns[input]) {
        return 1;
    }
    reader->tiles = calloc((size_t) dag_count(state->graph), sizeof *reader->tiles);
    return reader->tiles && eval_prepare_inputs(reader, input);
}

// eval_release_tiles releases the buffers and bounds owned by one reader.
//
// Takes state (eval_state_t) which owns the cache.
void eval_release_tiles(eval_state_t *state) {
    if (state->tiles) {
        for (int i = 0; i < dag_count(state->graph); i++) {
            free(state->tiles[i].values);
        }
        free(state->tiles);
        state->tiles = NULL;
    }
}

// eval_materialise_range evaluates the needed nodes in a half-open range. Each nested
// recurrence starts at tick zero and finishes before its enclosing step reads the carry.
//
// An unused recurrence can contain a node shared by a live expression, so the walk checks
// each node for use.
//
// Takes state (eval_state_t) which is the evaluation in progress.
// Takes first (int) which is the first node to visit.
// Takes end (int) which is one past the last node to visit.
// Takes mode (eval_mode_t) which is the traversal for this run.
// Takes workers (int) which is the number of workers in the schedule.
// Takes ticks (int) which holds each recurrence's current tick.
// Returns zero if evaluation fails or a recurrence has invalid bounds or a missing carry.
static int eval_materialise_range(eval_state_t *state, const int first, const int end,
                                  const eval_mode_t mode, const int workers, int *ticks) {
    for (int i = first; i < end; i++) {
        if (!state->needed[i]) {
            continue;
        }
        if (!eval_materialise_node(state, i, mode, workers, ticks)
            || (state->tiled && !state->columns[i] && !eval_prepare_tile(state, i))) {
            return 0;
        }
        const dagnode_t *node = dag_at_const(state->graph, i);
        if (node->op != OP_RECUR) {
            continue;
        }
        int steps;
        if (!eval_step_count(node, i, end, &steps)) {
            return 0;
        }
        for (ticks[i] = 0; ticks[i] < steps; ticks[i]++) {
            if (!eval_materialise_range(state, i + 1, node->step_end + 1, mode, workers, ticks)
                || !eval_carry_back(state, i, node->carried)) {
                return 0;
            }
        }
        i = node->step_end;
    }
    return 1;
}

// eval_materialise_all evaluates the graph with fresh tick counters for this run.
//
// Takes state (eval_state_t) which is the evaluation in progress.
// Takes count (int) which is the number of nodes in the graph.
// Takes mode (eval_mode_t) which is the traversal for this run.
// Takes workers (int) which is the number of workers in the schedule.
// Returns zero if allocation or evaluation fails.
static int eval_materialise_all(eval_state_t *state, const int count, const eval_mode_t mode,
                                const int workers) {
    int *ticks = calloc((size_t) count, sizeof *ticks);
    if (!ticks) {
        return 0;
    }
    const int ok = eval_materialise_range(state, 0, count, mode, workers, ticks);
    free(ticks);
    return ok;
}

// eval_state_init allocates the per-node tables one run needs, for a graph of `count`
// nodes.
//
// Takes state (eval_state_t) which is the evaluation in progress.
// Takes g (dag_t) which is the graph being walked.
// Takes count (int) which is how many nodes the graph holds.
// Takes result (int) which is the node the caller wants to evaluate.
// Takes mode (eval_mode_t) which is the traversal to evaluate under.
// Takes tile (size_t) which is the tile width in elements.
// Returns 0 if any allocation fails, in which case the partially built state is still
// safe to free.
static int eval_state_init(
    eval_state_t *state,
    const dag_t *g,
    const int count,
    const int result,
    const eval_mode_t mode,
    const size_t tile
) {
    memset(state, 0, sizeof *state);
    state->graph = g;
    state->tiled = (mode == EVAL_TILED);
    state->tile_length = tile;
    state->needed       = calloc((size_t) count, sizeof *state->needed);
    state->columns      = (double **) calloc((size_t) count, sizeof(double *));
    state->lengths      = (size_t *)  calloc((size_t) count, sizeof(size_t));
    if (state->tiled) {
        state->tiles = calloc((size_t) count, sizeof *state->tiles);
    }
    if (!state->needed || !state->columns || !state->lengths || (state->tiled && !state->tiles)) {
        return 0;
    }
    dag_mark_cone(g, result, state->needed);
    for (int i = count - 1; i >= 0; i--) {
        const dagnode_t *node = dag_at_const(g, i);
        if (node->op == OP_SOURCE && node->bless > 0) {
            state->needed[i] = 1;
        }
        if (state->needed[i] && node->op == OP_TICK) {
            dag_mark_cone(g, node->tick_of, state->needed);
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
    }
    free(state->needed);
    free(state->columns);
    free(state->lengths);
    eval_release_tiles(state);
}

// eval_collect_result transfers a materialised column to the caller. For a fused or tiled
// result it reads the elements while their operands are still live.
//
// Takes state (eval_state_t) which owns the evaluated columns.
// Takes result (int) which is the node the program returns.
// Returns a column owned by the caller, or an empty result if allocation fails.
static eval_result_t eval_collect_result(eval_state_t *state, const int result) {
    const size_t length = state->lengths[result];
    double *values = state->columns[result];
    if (values) {
        state->columns[result] = NULL;
    } else {
        values = malloc(length * sizeof *values);
        if (!values) {
            return (eval_result_t) {0};
        }
        for (size_t k = 0; k < length; k++) {
            values[k] = eval_element_of(state, result, k);
        }
    }
    return (eval_result_t) { .values = values, .length = length };
}

// eval_graph evaluates the result under the given schedule, input world and kernel order.
//
// Takes g (dag_t) which is the graph to evaluate.
// Takes result (int) which is the node the program returns.
// Takes mode (eval_mode_t) which is the traversal for this run.
// Takes tile (size_t) which is the tile width in elements.
// Takes workers (int) which is the number of workers in the schedule.
// Takes world (unsigned) which selects the input data for external sources.
// Takes reference (int) which selects serial combining kernels when non-zero.
// Returns a column owned by the caller, or an empty result on failure.
static eval_result_t eval_graph(const dag_t *g, const int result, const eval_mode_t mode,
                                const size_t tile, const int workers, const unsigned world,
                                const int reference) {
    const int count = dag_count(g);
    eval_result_t output = {0};
    g_column_elements = 0;
    g_tile_elements = 0;
    g_replay_workers = 0;
    if (result < 0 || result >= count) {
        return output;
    }

    eval_state_t state;
    if (eval_state_init(&state, g, count, result, mode, tile < 1 ? 1 : tile)) {
        state.world = world;
        state.reference = reference;
        if (eval_materialise_all(&state, count, mode, workers)) {
            output = eval_collect_result(&state, result);
        }
    }
    g_tile_elements = state.tile_elements;
    g_replay_workers = state.replay_workers;
    eval_state_free(&state, count);
    return output;
}

// eval_run evaluates the whole result in world 1 with one worker and materialised
// columns.
//
// Takes g (dag_t) which is the graph to evaluate.
// Takes result (int) which is the node the program returns.
// Returns a column owned by the caller, or an empty result on failure.
eval_result_t eval_run(const dag_t *g, const int result) {
    return eval_graph(g, result, EVAL_MATERIALISE, 1, 1, 1u, 0);
}

// eval_take_scalar extracts a single value for a schedule or reference comparison.
//
// Takes result (eval_result_t) which owns the column and is consumed on every path.
// Takes out (double) which receives the value if the column has one element.
// Returns one if a value was written, zero otherwise.
static int eval_take_scalar(eval_result_t result, double *out) {
    if (result.length == 1) {
        *out = result.values[0];
    }
    free(result.values);
    return result.length == 1;
}

// eval_dag_in_world evaluates a scalar result under a given schedule and input world.
//
// Takes g (dag_t) which is the graph to evaluate.
// Takes result (int) which is the node the program returns.
// Takes mode (eval_mode_t) which is the traversal for this run.
// Takes tile (size_t) which is the tile width in elements.
// Takes workers (int) which is the number of workers in the schedule.
// Takes world (unsigned) which selects the input data for external sources.
// Takes out (double) which receives the scalar on success.
// Returns zero if evaluation fails or the result does not have exactly one element.
int eval_dag_in_world(const dag_t *g, const int result, const eval_mode_t mode, const size_t tile,
                      const int workers, const unsigned world, double *out) {
    return eval_take_scalar(eval_graph(g, result, mode, tile, workers, world, 0), out);
}

// eval_dag evaluates `g` in the shipped world.
//
// Schedule comparisons use the same input world on both sides. A check that varies the
// inputs calls eval_dag_in_world directly.
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
// The reference uses materialised columns, one worker and serial combining order.
//
// Takes g (dag_t) which is the graph being walked.
// Takes result (int) which is the node the program returns.
// Takes out (double) which receives the answer.
int eval_dag_on_reference(const dag_t *g, const int result, double *out) {
    return eval_take_scalar(eval_graph(g, result, EVAL_MATERIALISE, 1, 1, 1u, 1), out);
}
