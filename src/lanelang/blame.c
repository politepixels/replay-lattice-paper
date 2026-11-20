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

#include "lattice.h"

#include "dag.h"

#include <stdlib.h>

// blame_walk_stops reports whether the blame walk should stop at `node`.
//
// Takes node (dagnode_t) which is the node under examination.
// Takes bad (lane_t) which is the lane the blame walk is following.
static int blame_walk_stops(const dagnode_t *node, const lane_t bad) {
    return opclass_lane(node->op_class) == bad ? 1 : node->input_count == 0;
}

// skip_demands walks past any OP_DEMAND wrapper to the value it is about.
//
// An OP_DEMAND node carries what the source asked for and computes nothing, so recording
// it would add a hop to every distance this file reports without naming a value the
// reader can go and look at.
//
// Takes g (dag_t) which is the graph being walked.
// Takes node (int) which is where to start.
// Returns the first node that is not a demand, or -1 when there is none.
static int skip_demands(const dag_t *g, const int node) {
    int at = node;
    while (at >= 0 && dag_at_const(g, at)->op == OP_DEMAND) {
        at = dag_at_const(g, at)->in[0];
    }
    return at;
}

// blame_candidate returns the k-th edge to inspect at `node`. Operands come first in
// source order, followed by the carried edge.
//
// A carried edge is stored separately from the operands. Following it lets the walk
// explain a failing lane introduced in a recurrence body. If several edges carry that
// lane, the walk follows the first path in this order that reaches an origin.
//
// Takes node (dagnode_t) which is the node under examination.
// Takes k (int) which is the edge to return, counting from zero.
// Returns the node the edge points at, or -1 where that edge is absent.
static int blame_candidate(const dagnode_t *node, const int k) {
    if (k < node->input_count) {
        return node->in[k];
    }
    return k == node->input_count ? node->carried : -1;
}

// copy_reversed copies the path in source-to-sink order. If it exceeds the output
// capacity, the copy starts at the source and ends when the buffer fills.
//
// Takes path (int) which holds the walk's nodes, sink first.
// Takes length (int) which is how many entries of `path` are in use.
// Takes out (int) which receives the reversed path.
// Takes cap (int) which bounds how many entries may be written.
// Returns how many were written.
static int copy_reversed(const int *path, const int length, int *out, const int cap) {
    const int written = length < cap ? length : cap;
    for (int i = 0; i < written; i++) {
        out[i] = path[length - 1 - i];
    }
    return written;
}

// blame_walk follows the failing lane through operands in source order, then through the
// carried edge. Each node remembers its next edge when the walk backtracks.
//
// Takes g (dag_t) which is the graph being walked.
// Takes computed (lane_t) which holds the settled lane for each node.
// Takes bad (lane_t) which is the lane to follow.
// Takes path (int) which starts with the failing node and holds the path to its origin.
// Takes next_edge (int) which holds one cursor per node, initially zero.
// Returns the path length, or zero if no origin was found.
static int blame_walk(const dag_t *g, const lane_t *computed, const lane_t bad,
                      int *path, int *next_edge) {
    int length = 1;
    while (length > 0) {
        const int at = path[length - 1];
        const dagnode_t *node = dag_at_const(g, at);
        if (blame_walk_stops(node, bad)) {
            return length;
        }
        if (next_edge[at] > node->input_count) {
            length--;
            continue;
        }
        const int next = skip_demands(g, blame_candidate(node, next_edge[at]++));
        if (next >= 0 && computed[next] == bad && next_edge[next] == 0) {
            path[length++] = next;
        }
    }
    return 0;
}

// lattice_blame writes the blame path in source-to-sink order, skipping demand wrappers.
//
// Takes g (dag_t) which is the graph being walked.
// Takes computed (lane_t) which holds the settled lane for each node.
// Takes failing (int) which is the node whose declaration failed.
// Takes bad (lane_t) which is the lane to follow.
// Takes out (int) which receives the path.
// Takes cap (int) which bounds the number of entries written to `out`.
// Returns the number written, or -1 if the walk could not allocate its workspace.
int lattice_blame(
    const dag_t *g,
    const lane_t *computed,
    const int failing,
    const lane_t bad,
    int *out,
    const int cap
) {
    const int start = skip_demands(g, failing);
    if (start < 0) {
        return 0;
    }
    const size_t count = (size_t) dag_count(g);
    int *path = malloc(count * sizeof *path);
    int *next_edge = calloc(count, sizeof *next_edge);
    int written = -1;
    if (path && next_edge) {
        path[0] = start;
        const int length = blame_walk(g, computed, bad, path, next_edge);
        written = copy_reversed(path, length, out, cap);
    }
    free(next_edge);
    free(path);
    return written;
}
