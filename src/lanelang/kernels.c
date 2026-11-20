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

#include "column_sums.h"

#include <pthread.h>
#include <stdlib.h>

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

// fold_strict folds `length` elements of node `input` from left to right. When the
// materialised fast path produces a NaN, the reference loop supplies the result bits.
//
// Takes state (eval_state_t) which is the evaluation in progress.
// Takes input (int) which is the node whose column is read.
// Takes length (size_t) which is how many elements it holds.
double fold_strict(eval_state_t *state, const int input, const size_t length) {
    if (CONTROL_TILE_REBRACKETS && state->tiled) {
        return fold_tiles(state, input, length);
    }
    if (state->columns[input] && state->lengths[input] == length && !state->reference) {
        const double sum = column_sum(state->columns[input], length);
        if (!isnan(sum)) {
            return sum;
        }
    }
    double total = 0.0;
    for (size_t i = 0; i < length; i++) {
        total += eval_element_of(state, input, i);
    }
    return total;
}

// replay_job_t owns one worker's reader and its range of whole blocks.
typedef struct {
    // reader borrows immutable columns and owns the worker's private tiles.
    eval_state_t reader;

    // input identifies the fold operand.
    int input;

    // length is the total number of input elements.
    size_t length;

    // first is the index of the first assigned block.
    size_t first;

    // end is one past the final assigned block.
    size_t end;

    // totals holds one sum per block in input order.
    double *totals;

    // thread is the joinable worker.
    pthread_t thread;

    // parent identifies the thread which launched the worker.
    pthread_t parent;
} replay_job_t;

// replay_blocks sums each assigned block independently in input order.
//
// Takes state (eval_state_t) which supplies the input reader.
// Takes job (replay_job_t) which describes the range and output slots.
static void replay_blocks(eval_state_t *state, const replay_job_t *job) {
    const double *values = state->columns[job->input];
    if (values && state->lengths[job->input] == job->length) {
        replay_column_blocks(values, job->length, job->first, job->end, job->totals);
        return;
    }
    for (size_t b = job->first; b < job->end; b++) {
        const size_t low = b * EVAL_BLOCK;
        const size_t high = job->length - low < EVAL_BLOCK ? job->length : low + EVAL_BLOCK;
        double sum = 0.0;
        for (size_t i = low; i < high; i++) {
            sum += eval_element_of(state, job->input, i);
        }
        job->totals[b] = sum;
    }
}

// replay_worker publishes its block totals before the parent joins it.
//
// Takes argument (void) which points to its assigned job.
// Returns the job that completed on a worker thread.
static void *replay_worker(void *argument) {
    replay_job_t *job = argument;
    replay_blocks(&job->reader, job);
    return pthread_equal(pthread_self(), job->parent) ? NULL : job;
}

// replay_parallel joins every started worker before releasing its storage.
//
// Takes state (eval_state_t) which owns the input.
// Takes fold (replay_job_t) which describes the complete range.
// Takes workers (int) which is the number of non-empty ranges.
// Returns zero on allocation or launch failure.
//
// Panics if a started worker cannot be joined and its storage may still be in use.
//
// Concurrent workers write disjoint block totals and own their tile caches.
static int replay_parallel(eval_state_t *state, const replay_job_t *fold, const int workers) {
    replay_job_t *jobs = calloc((size_t) workers, sizeof *jobs);
    if (!jobs) {
        return 0;
    }
    size_t first = 0;
    int prepared = 0;
    for (; prepared < workers; prepared++) {
        replay_job_t *job = &jobs[prepared];
        *job = *fold;
        job->parent = pthread_self();
        job->first = first;
        first += fold->end / (size_t) workers + ((size_t) prepared < fold->end % (size_t) workers);
        job->end = first;
        if (!eval_worker_reader(&job->reader, state, job->input)) {
            break;
        }
    }
    int started = 0;
    while (prepared == workers && started < workers
           && pthread_create(&jobs[started].thread, NULL, replay_worker, &jobs[started]) == 0) {
        started++;
    }
    int completed = 0;
    for (int i = 0; i < started; i++) {
        void *result = NULL;
        if (pthread_join(jobs[i].thread, &result) != 0) {
            abort();
        }
        completed += result == &jobs[i];
        state->tile_elements += jobs[i].reader.tile_elements;
    }
    for (int i = 0; i < workers; i++) {
        eval_release_tiles(&jobs[i].reader);
    }
    free(jobs);
    if (started == workers && state->replay_workers < completed) {
        state->replay_workers = completed;
    }
    return started == workers;
}

// fold_replay folds blocks of up to EVAL_BLOCK elements from left to right. Workers may
// compute whole blocks in parallel. Their totals are added in block order and a NaN
// result is canonicalised.
//
// The fixed tree depends only on `length`. Allocation or thread creation failure returns
// zero. The pinned-eval-breaks control always fails this kernel. The serial reference
// uses a separate path.
//
// Takes state (eval_state_t) which is the evaluation in progress.
// Takes input (int) which is the node whose column is read.
// Takes length (size_t) which is how many elements it holds.
// Takes workers (int) which selects the requested team size.
// Takes out (double) which receives the answer.
int fold_replay(eval_state_t *state, const int input, const size_t length, const int workers, double *out) {
    if (CONTROL_PINNED_EVAL_BREAKS) {
        return 0;
    }
    if (CONTROL_TILE_REBRACKETS && state->tiled) {
        *out = fold_tiles(state, input, length);
        return 1;
    }
    const size_t block_count = length / EVAL_BLOCK + (length % EVAL_BLOCK != 0);
    if (block_count == 0) {
        *out = 0.0; return 1;
    }
    double *block_results = (double *) malloc(block_count * sizeof(double));
    if (!block_results) {
        return 0;
    }

    const replay_job_t fold = {.input = input, .length = length, .end = block_count, .totals = block_results};
    if (workers <= 1 || block_count == 1) {
        replay_blocks(state, &fold);
        if (state->replay_workers == 0) {
            state->replay_workers = 1;
        }
    } else {
        const int active = (size_t) workers > block_count ? (int) block_count : workers;
        if (!replay_parallel(state, &fold, active)) {
            free(block_results);
            return 0;
        }
    }
    *out = replay_column_total(block_results, block_count);
    free(block_results);
    return 1;
}

// fold_free divides the input into chunks by worker count and combines the partial sums
// in worker order. Each chunk uses the same striped grouping across traversals.
//
// Changing the worker count can change the bracketing and the result bits. If the partial
// sums cannot be allocated, fold_free falls back to fold_strict.
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
        partials[worker] = free_chunk_sum(state, input, low, high);
    }
    double total = 0.0;
    for (int worker = 0; worker < lanes; worker++) {
        total += partials[worker];
    }
    free(partials);
    return total;
}

// EVAL_SCAN_BLOCK is the frozen leaf-block width of the fixed-block scan, in elements.
// Reproducing the scan's bracketing requires the same width on both sides.
#define EVAL_SCAN_BLOCK 64

#ifdef LANELANG_SCAN_BLOCK_FOLLOWS_WORKERS
// SCAN_WIDTH_ELEMENTS_PER_WORKER sets the block width per worker in the control build.
// The test programs span several blocks, so a change in worker count can change the
// grouping.
#define SCAN_WIDTH_ELEMENTS_PER_WORKER 8u

// scan_leaf_width makes the block width depend on the worker count in the control build.
// The output ranges remain disjoint, but changing the grouping can change the bits.
//
// Takes workers (int) which is how many workers the schedule uses.
static size_t scan_leaf_width(const int workers) {
    return workers > 0 ? (size_t) workers * SCAN_WIDTH_ELEMENTS_PER_WORKER : EVAL_SCAN_BLOCK;
}
#else
// scan_leaf_width is the frozen width the shipped build uses.
//
// Takes workers (int) which is how many workers the schedule uses.
static size_t scan_leaf_width(const int workers) {
    (void) workers;
    return EVAL_SCAN_BLOCK;
}
#endif

// scan_fixed_block computes a blocked prefix into `out`, block by block.
//
// Each block computes its local prefixes from zero. Each output adds the totals of the
// preceding blocks to its local prefix, and the base advances once per block. With the
// shipped block width, these additions depend only on the input data and length.
// Accumulating straight through the boundary would erase the effect of changing the width
// in the control build.
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

// scan_free groups its prefix sums by worker count. Changing the chunk boundaries can
// change the low bits.
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

// EVAL_EXACT_INTEGER_LIMIT is the exclusive upper bound on the sum of absolute input
// values accepted by the integer fold. Below it, every possible partial sum is an exactly
// representable integer.
#define EVAL_EXACT_INTEGER_LIMIT 9007199254740992.0

// eval_is_exact_integer reports whether `x` is integral and has magnitude strictly below
// EVAL_EXACT_INTEGER_LIMIT.
//
// Takes x (double) which is the value tested.
static int eval_is_exact_integer(const double x) {
    return !(x > -EVAL_EXACT_INTEGER_LIMIT && x < EVAL_EXACT_INTEGER_LIMIT)
           ? 0
           : x == (double) (long long) x;
}

// fold_integer_chunk sums elements [low, high) of `input`, accumulating the sum of their
// magnitudes beside the sum itself and refusing as soon as an element leaves the range
// where double addition is `exact`.
//
// The sum of absolute values bounds every partial under every bracketing. Checking that
// bound for the chunks and then for their combined magnitudes makes acceptance depend
// only on the data. Checking the signed partials of one schedule could accept a column
// that another schedule refuses.
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
// Every input must be integral and the sum of absolute values must stay strictly below
// 2^53. Within this domain any grouping gives the serial result. The guard also rejects
// some columns whose sums happen to be exact beyond the bound. Because the bound is a
// property of the input, worker count can change neither the accepted result nor the
// domain verdict.
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

// fold_exact discharges an `exact` demand with the serial fold.
//
// The exact-lowers-to-tree control substitutes the fixed tree. Its results repeat across
// schedules, but for some floating-point inputs they differ from the serial result. The
// reference comparison detects this even when the schedule comparisons all agree.
//
// Takes state (eval_state_t) which is the evaluation in progress.
// Takes input (int) which is the node holding the column being folded.
// Takes length (size_t) which is how many elements that column has.
// Takes out (double) which receives the fold's value.
// Returns 1, or 0 when the kernel could not run.
int fold_exact(eval_state_t *state, const int input, const size_t length, double *out) {
    if (CONTROL_EXACT_LOWERS_TO_TREE) {
        return fold_replay(state, input, length, 1, out);
    }
    *out = fold_strict(state, input, length);
    return 1;
}
