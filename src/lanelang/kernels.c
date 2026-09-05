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

#include "kernels.h"

#include <stdlib.h>

// EVAL_BLOCK is the fixed leaf width of the `replay` tree, small so a test program can
// straddle it. The width is frozen ABI: two implementations agree on a `replay` fold's
// bits only if they agree on it. A column shorter than one block folds as a single block,
// which is the serial order.
#define EVAL_BLOCK 64

// CONTROL_TILE_REBRACKETS is 1 under the tile-rebrackets control build and 0 in every
// other, so the two kernels below test a value rather than carrying an `#ifdef` around a
// statement.
#ifdef RL_TILE_REBRACKETS
#define CONTROL_TILE_REBRACKETS 1
#else
#define CONTROL_TILE_REBRACKETS 0
#endif

// CONTROL_PINNED_EVAL_BREAKS is 1 under the pinned-eval-breaks control build and 0 in
// every other, so fold_replay tests a value rather than carrying an `#ifdef` around a
// statement.
#ifdef RL_PINNED_EVAL_BREAKS
#define CONTROL_PINNED_EVAL_BREAKS 1
#else
#define CONTROL_PINNED_EVAL_BREAKS 0
#endif

// fold_tiles folds `length` elements of node `input` a tile at a time, accumulating a
// running total per tile and adding the tile totals up, so the tile width decides the
// bracketing.
//
// It is compiled into every build and reached only from the control build, which keeps
// the two call sites below free of directives. A tile is a traversal detail that must not
// reach the arithmetic, so a build in which it does is one where a `replay` demand is
// discharged by a kernel whose bits follow the tiling.
//
// Takes state (eval_state_t) which is the evaluation in progress.
// Takes input (int) which is the node whose column is read.
// Takes length (size_t) which is how many elements it holds.
// Returns the fold's value.
static double fold_tiles(eval_state_t *state, const int input, const size_t length) {
    double total = 0.0;
    for (size_t low = 0; low < length; low += state->tile_length) {
        size_t high = low + state->tile_length;
        if (high > length) {
            high = length;
        }
        double tile_total = 0.0;
        for (size_t i = low; i < high; i++) {
            tile_total += eval_element_of(state, input, i);
        }
        total += tile_total;
    }
    return total;
}

// fold_strict is the oracle order: one serial left-to-right fold over `length` elements
// of node `input`.
//
// Takes state (eval_state_t) which is the evaluation in progress.
// Takes input (int) which is the node whose column is read.
// Takes length (size_t) which is how many elements it holds.
double fold_strict(eval_state_t *state, const int input, const size_t length) {
    if (CONTROL_TILE_REBRACKETS && state->tiled) {
        return fold_tiles(state, input, length);
    }
    double total = 0.0;
    for (size_t i = 0; i < length; i++) {
        total += eval_element_of(state, input, i);
    }
    return total;
}

// fold_replay is the fixed tree: blocks of EVAL_BLOCK elements by position, each folded
// strict left to right, and the block results then folded the same way.
//
// The shape is a function of `length` alone, so the bits do not move with the deployment.
// It writes the total through `out` and returns 0 when the block array cannot be got,
// because a serial fallback would silently change the bracketing this kernel exists to
// pin; the control build makes it report failure unconditionally, a kernel lost after the
// checker has accepted, and the reference path never calls it, so the reference stays the
// reference.
//
// Takes state (eval_state_t) which is the evaluation in progress.
// Takes input (int) which is the node whose column is read.
// Takes length (size_t) which is how many elements it holds.
// Takes out (double) which receives the answer.
int fold_replay(eval_state_t *state, const int input, const size_t length, double *out) {
    if (CONTROL_PINNED_EVAL_BREAKS) {
        return 0;
    }
    if (CONTROL_TILE_REBRACKETS && state->tiled) {
        *out = fold_tiles(state, input, length);
        return 1;
    }
    const size_t block_count = (length + EVAL_BLOCK - 1) / EVAL_BLOCK;
    if (block_count == 0) {
        *out = 0.0; return 1;
    }
    double *block_results = (double *) malloc(block_count * sizeof(double));
    if (!block_results) {
        return 0;
    }

    for (size_t block = 0; block < block_count; block++) {
        const size_t low = block * EVAL_BLOCK;
        size_t high = low + EVAL_BLOCK;
        if (high > length) {
            high = length;
        }
        double block_total = 0.0;
        for (size_t i = low; i < high; i++) {
            block_total += eval_element_of(state, input, i);
        }
        block_results[block] = block_total;
    }
    double total = 0.0;
    for (size_t block = 0; block < block_count; block++) {
        total += block_results[block];
    }
    free(block_results);
    *out = total;
    return 1;
}

// fold_free is the free fold: chunk by worker count rather than by a fixed width, and
// combine the partials in worker order.
//
// That is the shape a reduction takes when nobody pinned it, and it is why its bits move
// when the deployment does. It falls back to fold_strict if the partials cannot be got.
//
// Takes state (eval_state_t) which is the evaluation in progress.
// Takes input (int) which is the node whose column is read.
// Takes length (size_t) which is how many elements it holds.
// Takes workers (int) which is how many workers the schedule uses.
double fold_free(eval_state_t *state, const int input, const size_t length,
                        const int workers) {
    const int lanes = workers < 1 ? 1 : workers;
    double *partials = (double *) calloc((size_t) lanes, sizeof(double));
    if (!partials) {
        return fold_strict(state, input, length);
    }

    size_t chunk = (length + (size_t) lanes - 1) / (size_t) lanes;
    if (chunk == 0) {
        chunk = 1;
    }
    for (int worker = 0; worker < lanes; worker++) {
        size_t low = (size_t) worker * chunk;
        size_t high = low + chunk;
        if (low > length) {
            low = length;
        }
        if (high > length) {
            high = length;
        }
        double partial = 0.0;
        for (size_t i = low; i < high; i++) {
            partial += eval_element_of(state, input, i);
        }
        partials[worker] = partial;
    }
    double total = 0.0;
    for (int worker = 0; worker < lanes; worker++) {
        total += partials[worker];
    }
    free(partials);
    return total;
}

// EVAL_SCAN_BLOCK is the frozen leaf-block width of the fixed-block scan, in elements.
// The width is frozen ABI rather than a tuning knob: two implementations agree on a
// `replay` scan's bits only if they agree on it.
#define EVAL_SCAN_BLOCK 64

#ifdef LANELANG_SCAN_BLOCK_FOLLOWS_WORKERS
// CC10_ELEMENTS_PER_WORKER is the perturbed width's scaling, small enough that the test
// programs straddle several blocks; any value that makes the width follow the worker
// count breaks the claim equally.
#define CC10_ELEMENTS_PER_WORKER 8u

// scan_leaf_width lets the deployment pick the width: the kernel still looks blocked and
// still writes disjoint output ranges, only the width moves, and that is enough to move
// the bits.
//
// Takes workers (int) which is how many workers the schedule uses.
static size_t scan_leaf_width(const int workers) {
    return workers > 0 ? (size_t) workers * CC10_ELEMENTS_PER_WORKER : EVAL_SCAN_BLOCK;
}
#else
// scan_leaf_width is the frozen width, which is what ships.
//
// Takes workers (int) which is how many workers the schedule uses.
static size_t scan_leaf_width(const int workers) {
    (void) workers;
    return EVAL_SCAN_BLOCK;
}
#endif

// scan_fixed_block computes a blocked prefix into `out`, block by block.
//
// The bits are a function of the data and the length alone: blocks write disjoint output
// ranges and the base pass runs in one order whatever the worker count, so nothing about
// the deployment reaches the arithmetic. Each block's prefix is computed from zero and
// the block's offset is added afterwards; accumulating straight through the boundary
// would form every partial sum in the same order whatever the width, and the width would
// reach no bit.
//
// Takes state (eval_state_t) which is the evaluation in progress.
// Takes input (int) which is the node whose column is read.
// Takes out (double) which receives the answer.
// Takes length (size_t) which is how many elements the column holds.
// Takes workers (int) which is how many workers the schedule uses.
void scan_fixed_block(eval_state_t *state, const int input, double *out, const size_t length,
                             const int workers) {
    size_t block = scan_leaf_width(workers);
    if (block == 0) {
        block = 1;
    }
    double base = 0.0;
    for (size_t start = 0; start < length; start += block) {
        size_t stop = start + block;
        if (stop > length) {
            stop = length;
        }
        double local = 0.0;
        for (size_t i = start; i < stop; i++) {
            local += eval_element_of(state, input, i);
            out[i] = base + local;
        }
        base += local;
    }
}

// scan_free is a free scan: chunked by worker count, so the partial bases combine in an
// order the deployment picks and the low bits move with it.
//
// Takes state (eval_state_t) which is the evaluation in progress.
// Takes input (int) which is the node whose column is read.
// Takes out (double) which receives the answer.
// Takes length (size_t) which is how many elements it holds.
// Takes workers (int) which is how many workers the schedule uses.
void scan_free(eval_state_t *state, const int input, double *out, const size_t length, const int workers) {
    size_t chunk = workers > 0 ? (length + (size_t) workers - 1) / (size_t) workers : length;
    if (chunk == 0) {
        chunk = 1;
    }
    double base = 0.0;
    for (size_t start = 0; start < length; start += chunk) {
        size_t stop = start + chunk;
        if (stop > length) {
            stop = length;
        }
        double running = 0.0;
        for (size_t i = start; i < stop; i++) {
            running += eval_element_of(state, input, i);
            out[i] = running + base;
        }
        base += running;
    }
}

// EVAL_EXACT_INTEGER_LIMIT is the largest magnitude at which every integer is exactly
// representable in a double. At or below it, integer addition is `exact` and associative,
// which is the entire reason an exact-integer fold may be scheduled freely and still earn
// an `exact` demand.
#define EVAL_EXACT_INTEGER_LIMIT 9007199254740992.0

// eval_is_exact_integer reports whether `x` is an integer this fold may accept: finite,
// integral, and within the range where addition cannot round.
//
// Takes x (double) which is the value to convert.
static int eval_is_exact_integer(const double x) {
    return !(x > -EVAL_EXACT_INTEGER_LIMIT && x < EVAL_EXACT_INTEGER_LIMIT)
           ? 0
           : x == (double) (long long) x;
}

// fold_integer_chunk sums elements [low, high) of `input`, accumulating the sum of their
// magnitudes beside the sum itself and refusing as soon as an element leaves the range
// where double addition is `exact`.
//
// The guard is on the sum of absolute values, which makes it a property of the data
// rather than of the schedule: bounding the partial of the chunking this run happened to
// pick would let the same column refuse at one worker count and answer at another, while
// the sum of magnitudes is the same number under every bracketing and dominates every
// partial of every one of them.
//
// Takes state (eval_state_t) which is the evaluation in progress.
// Takes input (int) which is the node holding the column being folded.
// Takes low (size_t) which is the first element of the chunk.
// Takes high (size_t) which is one past its last.
// Takes out (double) which receives the chunk's sum.
// Takes magnitude (double) which receives the sum of the chunk's absolute values.
// Returns 1, or 0 when an element or the magnitude left the `exact` range.
static int fold_integer_chunk(eval_state_t *state, const int input, const size_t low,
                              const size_t high, double *out, double *magnitude) {
    double partial = 0.0;
    double weight = 0.0;
    for (size_t i = low; i < high; i++) {
        const double element = eval_element_of(state, input, i);
        if (!eval_is_exact_integer(element)) {
            return 0;
        }
        partial += element;
        weight += element < 0.0 ? -element : element;
        if (!eval_is_exact_integer(weight)) {
            return 0;
        }
    }
    *out = partial;
    *magnitude = weight;
    return 1;
}

// fold_integer folds in worker order over `exact` integers, checking the domain as it
// goes.
//
// The claim `exact` is true while the sum of the column's absolute values stays below
// 2^53 and false beyond it, so the fold enforces the property its class asserts rather
// than trusting a caller to supply it: a value out of domain, or a column whose
// magnitudes sum past the range, fails the evaluation rather than returning a rounded
// answer. The worker count, the chunking and the order partials combine are not pinned,
// because none of them can move a bit or move the domain answer; a refusal that moved
// with the worker count would make evaluability a fact about the schedule.
//
// Takes state (eval_state_t) which is the evaluation in progress.
// Takes input (int) which is the node whose column is read.
// Takes length (size_t) which is how many elements it holds.
// Takes workers (int) which is how many workers the schedule uses.
// Takes out (double) which receives the answer.
int fold_integer(eval_state_t *state, const int input, const size_t length,
                        const int workers, double *out) {
    const int lanes = workers < 1 ? 1 : workers;
    double *partials = (double *) calloc((size_t) lanes * 2, sizeof(double));
    double *weights = partials ? partials + lanes : NULL;
    if (!partials) {
        return 0;
    }

    size_t chunk = (length + (size_t) lanes - 1) / (size_t) lanes;
    if (chunk == 0) {
        chunk = 1;
    }
    for (int worker = 0; worker < lanes; worker++) {
        size_t low = (size_t) worker * chunk;
        size_t high = low + chunk;
        if (low > length) {
            low = length;
        }
        if (high > length) {
            high = length;
        }
        if (!fold_integer_chunk(state, input, low, high, &partials[worker], &weights[worker])) {
            free(partials);
            return 0;
        }
    }
    double total = 0.0;
    double magnitude = 0.0;
    for (int worker = 0; worker < lanes; worker++) {
        total += partials[worker];
        magnitude += weights[worker];
        if (!eval_is_exact_integer(magnitude)) {
            free(partials); return 0;
        }
    }
    free(partials);
    *out = total;
    return 1;
}

// CONTROL_EXACT_LOWERS_TO_TREE is 1 under the exact-lowers-to-tree control build and 0 in
// every other, so fold_exact tests a value rather than carrying an `#ifdef` inside a
// statement.
#ifdef LANELANG_EXACT_LOWERS_TO_TREE
#define CONTROL_EXACT_LOWERS_TO_TREE 1
#else
#define CONTROL_EXACT_LOWERS_TO_TREE 0
#endif

// fold_exact discharges an `exact` demand, which is the serial fold and nothing else.
//
// The control build discharges it with the fixed tree instead. The fixed tree is a
// perfectly good `replay` kernel and is schedule-invariant, and it is not the serial
// reference for floating point, so that build is one in which a single lane has two bit
// patterns depending on which kernel discharged it; every schedule-axis assertion still
// passes under it, and only the reference comparison notices.
//
// Takes state (eval_state_t) which is the evaluation in progress.
// Takes input (int) which is the node holding the column being folded.
// Takes length (size_t) which is how many elements that column has.
// Takes out (double) which receives the fold's value.
// Returns 1, or 0 when the kernel could not run.
int fold_exact(eval_state_t *state, const int input, const size_t length, double *out) {
    if (CONTROL_EXACT_LOWERS_TO_TREE) {
        return fold_replay(state, input, length, out);
    }
    *out = fold_strict(state, input, length);
    return 1;
}
