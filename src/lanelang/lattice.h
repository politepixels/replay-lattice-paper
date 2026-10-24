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

#ifndef LANELANG_LATTICE_H
#define LANELANG_LATTICE_H

#include "dag.h"
#include "diagnostic.h"
#include "lane.h"

// BLAME_PATH_MAX is the longest blame path the checker reports. A path longer than this is
// truncated at the source end, which costs a diagnostic some of its hop count but never
// changes a verdict.
#define BLAME_PATH_MAX 64

// check_result_t is what one run of the checker found.
typedef struct {
    // violations is how many declarations were not satisfied.
    int violations;

    // checked is how many declarations were checked.
    int checked;

    // result_lane is the lane computed for the node the program returns, or LANE_EXACT
    // when there is none.
    lane_t result_lane;

    // malformed is the index of the node the sweep refused to compute over, or -1 when it
    // ran to completion. No lane in `computed` is meaningful when this is set.
    int malformed;

    // malformed_reason is why the sweep refused, or MALFORMED_NONE when it did not. A
    // refusal is never counted in `violations`, so a caller reading only that field would
    // print an acceptance over a sweep that had refused to run.
    enum {
        // MALFORMED_NONE is the sweep ran to a fixed point.
        MALFORMED_NONE = 0,

        // MALFORMED_FORWARD_INPUT is an input pointed forward, so the single
        // left-to-right pass is not valid on this graph.
        MALFORMED_FORWARD_INPUT,

        // MALFORMED_NO_FIXED_POINT is the iteration hit its ceiling with a lane still
        // moving, which is unreachable while lanes are monotone on a finite chain, so
        // reaching it indicts the checker rather than the program.
        MALFORMED_NO_FIXED_POINT,

        // MALFORMED_GROUNDING_NO_FIXED_POINT is the same exhaustion in the grounding
        // sweep, reported so that falling out at the ceiling stays distinguishable from
        // settling.
        MALFORMED_GROUNDING_NO_FIXED_POINT,

        // MALFORMED_OUT_OF_MEMORY is the grounding judgement's scratch array could not be
        // allocated. It refuses rather than continuing, because a grounding check that
        // silently does not run would accept every demand it should refuse.
        MALFORMED_OUT_OF_MEMORY
    } malformed_reason;

    // ungrounded is how many declarations of `exact` or `replay` were refused because
    // their cone reaches a leaf whose value is not a function of the program.
    int ungrounded;

    // ungrounded_leaf is the ungrounded leaf a refused declaration reaches, or -1 when
    // there is none.
    int ungrounded_leaf;

    // ungrounded_nodes is how many nodes the grounding sweep settled at false, refused or
    // not, or -1 when the sweep never ran. It counts nodes where `ungrounded` counts
    // refused declarations, its absent value is -1 because zero is a real answer, and
    // `check_rejected` never reads it, so it is diagnostic rather than a verdict input.
    int ungrounded_nodes;

    // passes is how many forward passes the lane computation took to settle. A graph with
    // no carried edge takes exactly one; with recurrence the cost is set by
    // `carried_depth` below rather than by the height of the chain.
    int passes;

    // carried_depth is the most carried edges a change must cross to reach any node it
    // reaches, counting the fewest along any path, and the quantity `passes` is a function
    // of: a change crosses exactly one carried edge per sweep, nested or chained, so a
    // graph at depth d settles in d + 2 passes.
    int carried_depth;
} check_result_t;

// check_result_none is a result meaning "nothing has been checked", with every field at
// the value that says so.
//
// Three fields do not say it with a zero: `malformed` and `ungrounded_leaf` are node
// indices whose absent value is -1, and `ungrounded_nodes` keeps -1 because zero is a
// real answer there, so a caller that cleared the struct to zero bytes would declare node
// 0 both malformed and an ungrounded leaf.
static inline check_result_t check_result_none(void) {
    check_result_t outcome;
    outcome.violations = 0;
    outcome.checked = 0;
    outcome.result_lane = LANE_EXACT;
    outcome.malformed = -1;
    outcome.malformed_reason = MALFORMED_NONE;
    outcome.ungrounded = 0;
    outcome.ungrounded_leaf = -1;
    outcome.ungrounded_nodes = -1;
    outcome.passes = 0;
    outcome.carried_depth = 0;
    return outcome;
}

// check_rejected reports whether the checker refused the program, for any reason a
// declaration can be refused for.
//
// Takes outcome (check_result_t) which receives what the check found.
static inline int check_rejected(const check_result_t *outcome) {
    return outcome->violations > 0 || outcome->ungrounded > 0;
}

// check_malformed reports whether the sweep refused to produce lanes at all, which is not
// a verdict about the program's determinism and must not be printed as one.
//
// Takes outcome (check_result_t) which receives what the check found.
static inline int check_malformed(const check_result_t *outcome) {
    return outcome->malformed >= 0;
}

// lattice_check computes every node's lane and checks every declaration.
//
// Takes g (dag_t) which is the graph being walked.
// Takes result (int) which is the node the program returns.
// Takes computed (lane_t) which holds the lane computed for each node.
// Takes d (diagnostic_t) which is where diagnostics are collected.
check_result_t lattice_check(dag_t *g, int result, lane_t *computed, diagnostic_t *d);

// lattice_compute_lanes runs the join sweep alone, filling `computed` without checking
// any declaration or reporting anything.
//
// It is the checker's own sweep called without the check, because a build with the check
// disabled still has to pick kernels, and a private second sweep that joined operands in
// one pass would ignore the carried edge, so a recurrence's lane under the kill switch
// would be whatever one pass reached.
//
// Takes g (dag_t) which is the graph being walked.
// Takes count (int) which is how many nodes the graph holds.
// Takes computed (lane_t) which is `count` cleared entries, and receives each node's
// lane.
void lattice_compute_lanes(dag_t *g, int count, lane_t *computed);

// lattice_blame is the blame path for one violation, written source-to-sink.
//
// Takes g (dag_t) which is the graph being walked.
// Takes computed (lane_t) which holds the lane computed for each node.
// Takes failing (int) which is the node whose declaration failed.
// Takes bad (lane_t) which is the lane the blame walk is following.
// Takes out (int) which receives the answer.
// Takes cap (int) which bounds how many entries may be written to `out`.
int lattice_blame(
    const dag_t *g,
    const lane_t *computed,
    int failing,
    lane_t bad,
    int *out,
    int cap
);

#endif  // LANELANG_LATTICE_H
