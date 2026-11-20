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

typedef struct {
    const dag_t *graph;

    long cap;

    long paths;

    lane_t worst;

    int overflow;
} walk_t;

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
