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

// eval_state_t is one run of the evaluator: the graph, the storage each node was given,
// and the tile cache.
typedef struct {
    // graph is the graph being executed, borrowed from the caller.
    const dag_t *graph;

    // columns is whole-column storage per node, non-NULL exactly when that node is
    // materialised; leaves always are, and elementwise nodes only when the traversal says
    // so.
    double **columns;

    // lengths is every node's logical length, whether or not it was materialised, because
    // a fused read still has to know when to broadcast.
    size_t *lengths;

    // tile_buffers is the tile cache, used by EVAL_TILED only: one live tile per node,
    // refilled on a miss. It is per node rather than one shared buffer, because filling a
    // tile of a fused node fills its operands' tiles, and one buffer would be overwritten
    // underneath the caller by its own recursion.
    double **tile_buffers;

    // tile_low is the first element index each node's live tile holds.
    size_t *tile_low;

    // tile_high is one past the last element index each node's live tile holds.
    size_t *tile_high;

    // tile_length is the tile width in elements, at least 1.
    size_t tile_length;

    // tiled is non-zero when the traversal is EVAL_TILED, so reads go through the tile
    // cache.
    int tiled;

    // world is which world an external source draws from, so two runs with different
    // values here differ in their inputs rather than in their schedule.
    unsigned world;

    // reference is non-zero under the reference evaluation, which replaces every
    // combining kernel with the serial order, whatever schedule class the node carries.
    int reference;
} eval_state_t;

// eval_element_of reads element `position` of node `node_index`, through the tile cache
// when the traversal is tiled.
//
// Declared here because the evaluation is mutually recursive: an elementwise node
// computes its own elements from its inputs' elements.
//
// Takes state (eval_state_t) which is the evaluation in progress.
// Takes node_index (int) which is the node's index in the graph.
// Takes position (size_t) which is the element to read.
double eval_element_of(eval_state_t *state, int node_index, size_t position);

// fold_strict is the oracle order: one serial left-to-right fold, which is the reference
// every `exact` demand is measured against.
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
// Takes out (double) which receives the fold's value.
// Returns 1, or 0 when the kernel could not run.
int fold_replay(eval_state_t *state, const int input, const size_t length, double *out);

// fold_free is the free fold, chunked by worker count, whose bits the schedule may move.
//
// Takes state (eval_state_t) which is the evaluation in progress.
// Takes input (int) which is the node whose column is folded.
// Takes length (size_t) which is how many elements it holds.
// Takes workers (int) which is how many workers the schedule uses.
// Returns the fold's value.
double fold_free(eval_state_t *state, const int input, const size_t length, const int workers);

// fold_exact discharges an `exact` demand with the serial fold; a control build makes it
// discharge with the fixed tree instead.
//
// Takes state (eval_state_t) which is the evaluation in progress.
// Takes input (int) which is the node whose column is folded.
// Takes length (size_t) which is how many elements it holds.
// Takes out (double) which receives the fold's value.
// Returns 1, or 0 when the kernel could not run.
int fold_exact(eval_state_t *state, const int input, const size_t length, double *out);

// fold_integer folds in worker order over `exact` integers, refusing as soon as a value
// or a running total leaves the range where double addition is `exact`.
//
// Takes state (eval_state_t) which is the evaluation in progress.
// Takes input (int) which is the node whose column is folded.
// Takes length (size_t) which is how many elements it holds.
// Takes workers (int) which is how many workers the schedule uses.
// Takes out (double) which receives the fold's value.
// Returns 1, or 0 when a value or a partial left the `exact` range.
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

#endif
