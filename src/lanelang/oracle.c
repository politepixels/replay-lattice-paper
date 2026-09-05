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

#include "oracle.h"

// walk_t is one enumeration in progress.
typedef struct {
    // graph is the graph being walked, borrowed from the caller.
    const dag_t *graph;

    // cap is the largest number of complete paths the walk may take before it gives up.
    long cap;

    // paths is how many complete paths have been walked so far.
    long paths;

    // worst is the weakest lane seen across the complete paths so far, seeded with
    // LANE_EXACT, the join identity.
    lane_t worst;

    // overflow is non-zero once `cap` was exceeded, after which no answer is claimed.
    int overflow;
} walk_t;

// oracle_walk walks backwards from node `at`, carrying in `accumulator` the weakest
// schedule class seen so far on this path.
//
// Every complete path, meaning one that reaches a node with no inputs, contributes its
// accumulated weakest class, and the answer is the weakest across all of them. The walk
// memoises nothing and shares no state between paths, which is what keeps its agreement
// with the sweep evidence rather than the same algorithm answering twice.
//
// Takes state (walk_t) which is the enumeration in progress.
// Takes at (int) which is the node to start from.
// Takes accumulator (lane_t) which is the running fold value.
static void oracle_walk(walk_t *state, const int at, const lane_t accumulator) {
    if (state->overflow) {
        return;
    }

    const dagnode_t *node = dag_at_const(state->graph, at);
    const lane_t here = lane_join(accumulator, opclass_lane(node->op_class));

    if (node->input_count == 0) {
        if (++state->paths > state->cap) {
            state->overflow = 1; return;
        }
        state->worst = lane_join(state->worst, here);
        return;
    }
    for (int k = 0; k < node->input_count; k++) {
        const int input = node->in[k];
        if (input < 0) {
            continue;
        }
        oracle_walk(state, input, here);
        if (state->overflow) {
            return;
        }
    }
}

// oracle_lane is the weakest schedule class on any complete path backwards from `node`,
// or LANE_NONE when the node is out of range or the enumeration exceeds `cap` paths.
//
// Takes g (dag_t) which is the graph being walked.
// Takes node (int) which is the node under examination.
// Takes cap (long) which bounds how much work the walk may do.
// Takes paths_out (long) which receives how many complete paths were walked.
lane_t oracle_lane(const dag_t *g, const int node, const long cap, long *paths_out) {
    walk_t state;
    state.graph = g;
    state.cap = cap;
    state.paths = 0;
    state.worst = LANE_EXACT;
    state.overflow = 0;

    if (node < 0 || node >= dag_count(g)) {
        return LANE_NONE;
    }
    oracle_walk(&state, node, LANE_EXACT);

    if (paths_out) {
        *paths_out = state.paths;
    }
    return state.overflow ? LANE_NONE : state.worst;
}
