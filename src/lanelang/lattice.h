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

// check_result_t is what one run of the checker found.
typedef struct {
    // violations counts declarations refused because their computed lane is too weak.
    // Grounding refusals are counted separately.
    int violations;

    // checked is how many declarations were checked.
    int checked;

    // result_lane is the lane computed for the node the program returns, or LANE_EXACT
    // when there is none.
    lane_t result_lane;

    // malformed is -1 until a sweep fails. An invalid edge records its consumer's index.
    // Allocation failure and fixed-point exhaustion record zero. A failure leaves the
    // check without a verdict.
    int malformed;

    // malformed_reason is why the sweep refused, or MALFORMED_NONE when it did not. A
    // refusal is never counted in `violations`, so a caller reading only that field would
    // print an acceptance over a sweep that had refused to run.
    enum {
        // MALFORMED_NONE means no sweep failure has been recorded.
        MALFORMED_NONE = 0,

        // MALFORMED_FORWARD_INPUT means an ordinary operand does not name an earlier node
        // or a carried edge points beyond the graph.
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

    // ungrounded counts `exact` and `replay` declarations whose cone reaches a source or
    // opaque call without an effective waiver.
    int ungrounded;

    // ungrounded_leaf identifies the source or opaque call behind the first grounding
    // refusal. The search falls back to the demand on allocation failure. The field
    // starts at -1.
    int ungrounded_leaf;

    // ungrounded_nodes counts nodes the program does not fix, including those covered by
    // waivers. It is -1 until the grounding sweep finishes.
    int ungrounded_nodes;

    // passes counts the forward lane sweeps. A graph without a carried edge takes one
    // pass. With a recurrence, the checker repeats the sweep until no lane changes.
    int passes;

    // carried_depth is the greatest of the shortest carried-edge distances between
    // reachable pairs of nodes. A depth of d bounds the sweep count by d + 2, including
    // the final pass that observes no change. If the distance workspace cannot be
    // allocated, the node count supplies a conservative bound.
    int carried_depth;
} check_result_t;

// check_result_none is a result meaning "nothing has been checked", with every field at
// the value that says so.
//
// The node indices `malformed` and `ungrounded_leaf` start at -1 because zero is a valid
// index. The count `ungrounded_nodes` also starts at -1 to distinguish a sweep that never
// ran from one that found zero ungrounded nodes.
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

// check_rejected reports whether a declaration was refused on its lane or its grounding.
//
// Takes outcome (check_result_t) which holds the result to inspect.
static inline int check_rejected(const check_result_t *outcome) {
    return outcome->violations > 0 || outcome->ungrounded > 0;
}

// check_malformed reports whether a sweep failure prevented a verdict.
//
// Takes outcome (check_result_t) which holds the result to inspect.
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
// Returns the number written, or -1 if the walk could not allocate its workspace.
int lattice_blame(
    const dag_t *g,
    const lane_t *computed,
    int failing,
    lane_t bad,
    int *out,
    int cap
);

#endif  // LANELANG_LATTICE_H
