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

#ifndef LANELANG_KERNELS_H
#define LANELANG_KERNELS_H

#include "dag.h"

#include <stddef.h>

// eval_tile_t owns one reader's cached interval for a single producer.
typedef struct {
    // values holds the cached elements.
    double *values;

    // capacity is the allocated element count.
    size_t capacity;

    // low is the first cached position.
    size_t low;

    // high is one past the last cached position.
    size_t high;
} eval_tile_t;

// eval_state_t holds the graph, the per-node storage and the tile cache for one
// evaluation.
typedef struct {
    // graph is the graph being executed, borrowed from the caller.
    const dag_t *graph;

    // needed marks nodes required for the result or an explicit source checksum.
    char *needed;

    // columns holds whole-column storage for each materialised node. Needed leaves are
    // always materialised. The traversal determines whether elementwise operations also
    // store their results here.
    double **columns;

    // lengths records the logical length of each evaluated node, including deferred
    // producers whose scalars may be broadcast.
    size_t *lengths;

    // tiles holds the private cache for this reader.
    eval_tile_t *tiles;

    // tile_elements counts writes to this reader's cache.
    size_t tile_elements;

    // replay_workers is the most workers that completed blocks in one fold.
    int replay_workers;

    // tile_length is the tile width in elements, at least 1.
    size_t tile_length;

    // tiled is non-zero when reads of deferred producers use the tile cache.
    int tiled;

    // world selects the seed for external source data independently of the schedule.
    unsigned world;

    // reference is non-zero under the reference evaluation, which replaces every
    // combining kernel with the serial order, whatever schedule class the node carries.
    int reference;
} eval_state_t;

// eval_worker_reader borrows the graph and columns and prepares private tile storage.
//
// Takes reader (eval_state_t) which receives the worker's view.
// Takes state (eval_state_t) which owns the evaluation.
// Takes input (int) which identifies the root of the fold input.
// Returns zero on allocation failure. The reader can still be released.
int eval_worker_reader(eval_state_t *reader, const eval_state_t *state, int input);

// eval_release_tiles releases the private cache of one reader.
//
// Takes state (eval_state_t) which owns the cache.
void eval_release_tiles(eval_state_t *state);

// eval_element_of reads element `position` of node `node_index`. A tiled traversal uses
// the cache for deferred producers.
//
// Kernels call this function to read their inputs. Evaluating a fused element may in turn
// read elements from its operands.
//
// Takes state (eval_state_t) which is the evaluation in progress.
// Takes node_index (int) which is the node's index in the graph.
// Takes position (size_t) which is the element to read.
double eval_element_of(eval_state_t *state, int node_index, size_t position);

// fold_strict evaluates the serial left-to-right fold that defines the reference result.
//
// Takes state (eval_state_t) which is the evaluation in progress.
// Takes input (int) which is the node whose column is folded.
// Takes length (size_t) which is how many elements it holds.
// Returns the fold's value.
double fold_strict(eval_state_t *state, const int input, const size_t length);

// fold_replay is the fixed tree, whose bits depend on the data and the shape alone.
//
// Takes state (eval_state_t) which is the evaluation in progress.
// Takes input (int) which is the node whose column is folded.
// Takes length (size_t) which is how many elements it holds.
// Takes workers (int) which selects the requested number of workers.
// Takes out (double) which receives the fold's value.
// Returns 1, or 0 when the kernel could not run.
int fold_replay(eval_state_t *state, int input, size_t length, int workers, double *out);

// free_chunk_sum sums a free fold's worker chunk using independent accumulators.
//
// Takes state (eval_state_t) which supplies the input column.
// Takes input (int) which identifies the column.
// Takes low (size_t) which is the first element.
// Takes high (size_t) which is one past the final element.
// Returns the chunk total.
double free_chunk_sum(eval_state_t *state, int input, size_t low, size_t high);

// fold_free is the free fold, chunked by worker count, whose bits the schedule may move.
//
// Takes state (eval_state_t) which is the evaluation in progress.
// Takes input (int) which is the node whose column is folded.
// Takes length (size_t) which is how many elements it holds.
// Takes workers (int) which is how many workers the schedule uses.
// Returns the fold's value.
double fold_free(eval_state_t *state, const int input, const size_t length, const int workers);

// fold_exact uses the serial fold for an `exact` demand. A control build substitutes the
// fixed tree.
//
// Takes state (eval_state_t) which is the evaluation in progress.
// Takes input (int) which is the node whose column is folded.
// Takes length (size_t) which is how many elements it holds.
// Takes out (double) which receives the fold's value.
// Returns 1, or 0 when the kernel could not run.
int fold_exact(eval_state_t *state, const int input, const size_t length, double *out);

// fold_integer folds in worker order over integral values. It checks that their absolute
// values sum to less than 2^53, so every partial sum is exactly representable.
//
// Takes state (eval_state_t) which is the evaluation in progress.
// Takes input (int) which is the node whose column is folded.
// Takes length (size_t) which is how many elements it holds.
// Takes workers (int) which is how many workers the schedule uses.
// Takes out (double) which receives the fold's value.
// Returns 1 on success, or 0 if allocation fails or the inputs fall outside the checked
// domain.
int fold_integer(eval_state_t *state, const int input, const size_t length, const int workers,
                 double *out);

// scan_fixed_block computes a blocked prefix into `out`, block by block. Its leaf width
// is frozen ABI, and a control build lets the deployment pick it instead.
//
// Takes state (eval_state_t) which is the evaluation in progress.
// Takes input (int) which is the node whose column is scanned.
// Takes out (double) which receives the prefix.
// Takes length (size_t) which is how many elements the column holds.
// Takes workers (int) which is how many workers the schedule uses.
void scan_fixed_block(eval_state_t *state, const int input, double *out, const size_t length,
                      const int workers);

// scan_free computes a prefix in whatever order the worker count implies.
//
// Takes state (eval_state_t) which is the evaluation in progress.
// Takes input (int) which is the node whose column is scanned.
// Takes out (double) which receives the prefix.
// Takes length (size_t) which is how many elements the column holds.
// Takes workers (int) which is how many workers the schedule uses.
void scan_free(eval_state_t *state, const int input, double *out, const size_t length,
               const int workers);

#endif  // LANELANG_KERNELS_H
