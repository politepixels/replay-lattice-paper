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

#ifndef LANELANG_EVAL_H
#define LANELANG_EVAL_H

#include "dag.h"

#include <stddef.h>

// eval_mode_t selects how the evaluator traverses elementwise operations.
typedef enum {
    // EVAL_MATERIALISE stores each needed intermediate column in memory.
    EVAL_MATERIALISE = 0,

    // EVAL_FUSED computes elementwise producers on demand inside their consumer.
    EVAL_FUSED,

    // EVAL_TILED computes and caches one tile of a producer at a time.
    EVAL_TILED
} eval_mode_t;

// eval_result_t holds the result in element order. The caller owns the values on success.
// On failure the pointer is NULL and the length is zero.
typedef struct {
    // values holds the elements in result order and must be freed by the caller.
    double *values;

    // length counts the elements in `values`.
    size_t length;
} eval_result_t;

// eval_run evaluates the result in the default world with one worker and materialised
// columns.
//
// Takes g (dag_t) which is the graph being evaluated.
// Takes result (int) which is the node the program returns.
// Returns the values owned by the caller, or an empty result on failure.
eval_result_t eval_run(const dag_t *g, int result);

// eval_dag evaluates `g` to its `result` node and writes the scalar to `out`.
//
// Takes g (dag_t) which is the graph being walked.
// Takes result (int) which is the node the program returns.
// Takes mode (eval_mode_t) which is the traversal to evaluate under.
// Takes tile (size_t) which is the tile width in elements.
// Takes workers (int) which is how many workers the schedule uses.
// Takes out (double) which receives the answer.
int eval_dag(const dag_t *g, int result, eval_mode_t mode, size_t tile, int workers, double *out);

// eval_dag_in_world evaluates a scalar result using external source data from the
// selected world.
//
// Takes g (dag_t) which is the graph being walked.
// Takes result (int) which is the node the program returns.
// Takes mode (eval_mode_t) which is the traversal to evaluate under.
// Takes tile (size_t) which is the tile width in elements.
// Takes workers (int) which is how many workers the schedule uses.
// Takes world (unsigned) which selects what an external source hands back.
// Takes out (double) which receives the answer.
int eval_dag_in_world(
    const dag_t *g, int result, eval_mode_t mode, size_t tile, int workers, unsigned world,
    double *out
);

// eval_dag_on_reference evaluates `g` with every combining kernel replaced by the serial
// order, whatever schedule class its node carries.
//
// An `exact` result must agree with this reference. Varying (mode, tile, workers) at a
// fixed input can show that a result is reproducible, but even a consistently wrong
// kernel passes that comparison. The reference evaluates the serial order separately.
//
// Takes g (dag_t) which is the graph being walked.
// Takes result (int) which is the node the program returns.
// Takes out (double) which receives the answer.
int eval_dag_on_reference(const dag_t *g, int result, double *out);

// eval_last_replay_workers returns the largest completed replay team in the last run.
int eval_last_replay_workers(void);

// eval_last_footprint reports the total element slots allocated for materialised nodes
// and the number of elements written to tile caches in the last evaluation.
//
// Takes columns (size_t) which receives the column footprint.
// Takes tiles (size_t) which receives the tile footprint.
void eval_last_footprint(size_t *columns, size_t *tiles);

// eval_perturbation names the active tile-rebrackets or pinned-eval-breaks control for
// the schedule check's PERTURBED marker. Other builds return "-".
const char *eval_perturbation(void);

// eval_bless_checksum computes FNV-1a over the bytes of a column for `@bless(n)`. The
// evaluator checks the loaded column against the declared checksum. Tests can also use
// this function to obtain the checksum for a particular input world.
//
// Takes data (double) which is the column to fingerprint.
// Takes count (size_t) which is how many elements it holds.
unsigned eval_bless_checksum(const double *data, size_t count);

#endif  // LANELANG_EVAL_H
