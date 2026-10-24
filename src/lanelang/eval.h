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

// eval_mode_t is where the elementwise work happens: the three traversals a
// lane-preserving compiler may choose between.
typedef enum {
    // EVAL_MATERIALISE writes every intermediate column to memory, which is the
    // unoptimised shape.
    EVAL_MATERIALISE = 0,

    // EVAL_FUSED computes elementwise producers on demand inside their consumer.
    EVAL_FUSED,

    // EVAL_TILED is the same, in tiles: computed a tile ahead, folded, discarded.
    EVAL_TILED
} eval_mode_t;

// eval_dag evaluates `g` to its `result` node and writes the scalar to `out`.
//
// Takes g (dag_t) which is the graph being walked.
// Takes result (int) which is the node the program returns.
// Takes mode (eval_mode_t) which is the traversal to evaluate under.
// Takes tile (size_t) which is the tile width in elements.
// Takes workers (int) which is how many workers the schedule uses.
// Takes out (double) which receives the answer.
int eval_dag(const dag_t *g, int result, eval_mode_t mode, size_t tile, int workers, double *out);

// eval_dag_in_world is the same, in a named world: `world` selects what every external
// source hands back.
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
// This is the axis that gives `exact` its content, and it is not reachable from eval_dag:
// the schedule space is (mode, tile, workers), none of the three can pick a kernel, so a
// check that varies only the schedule asks whether a value agrees with itself, and a
// kernel that is deterministic and consistently wrong passes it unremarked. No mode, tile
// or worker count, because the reference is not a deployment schedule: it is one extra
// point beside the deployment's space, taken where nothing has been chosen.
//
// Takes g (dag_t) which is the graph being walked.
// Takes result (int) which is the node the program returns.
// Takes out (double) which receives the answer.
int eval_dag_on_reference(const dag_t *g, int result, double *out);

// eval_last_footprint reports what the last eval_dag actually did, so that "the traversal
// changed" is a measurement.
//
// Takes columns (size_t) which receives the column footprint.
// Takes tiles (size_t) which receives the tile footprint.
void eval_last_footprint(size_t *columns, size_t *tiles);

// eval_perturbation is the perturbation this build carries: "-" for a shipped build, a
// token for a control build. The check prints it as a PERTURBED marker, so a -D which
// failed to reach the code is distinguishable from a check that failed to fire.
const char *eval_perturbation(void);

// eval_bless_checksum is FNV-1a over a column's bit patterns: what a `@bless(checksum:)`
// declares, and what the evaluator compares the loaded column against.
//
// Exposed so a check can compute the checksum a world produces rather than hard-code one;
// a check that had to write the number down would be asserting its own arithmetic.
//
// Takes data (double) which is the column to fingerprint.
// Takes count (size_t) which is how many elements it holds.
unsigned eval_bless_checksum(const double *data, size_t count);

#endif  // LANELANG_EVAL_H
