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

// blame_walk_stops reports whether the blame walk should stop at `node`.
//
// Takes node (dagnode_t) which is the node under examination.
// Takes bad (lane_t) which is the lane the blame walk is following.
static int blame_walk_stops(const dagnode_t *node, const lane_t bad) {
    return opclass_lane(node->op_class) == bad ? 1 : node->input_count == 0;
}

// already_walked reports whether `node` is one of the `length` nodes the walk has
// recorded so far. It is the cycle guard: a recurrence makes the graph cyclic, and a walk
// that took an edge back to a node it had reported would circle until it exhausted
// BLAME_PATH_MAX.
//
// Takes path (int) which holds the nodes the walk has recorded, most recent last.
// Takes length (int) which is how many entries of `path` are in use.
// Takes node (int) which is the node being looked for.
// Returns 1 when the node is already on the path.
static int already_walked(const int *path, const int length, const int node) {
    for (int i = 0; i < length; i++) {
        if (path[i] == node) {
            return 1;
        }
    }
    return 0;
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

// blame_candidate is the k-th edge the blame walk may follow out of `node`: its operands
// in order, then the carried edge.
//
// The carried edge is considered last, and has to be considered at all: it is not an
// entry of `in`, but blame walks the same edges the join travelled and the join does
// travel this one, and omitting it makes a recurrence blame itself, the walk finding no
// bad operand and reporting a node whose own class is the join identity. Taking it last
// keeps a genuine forward operand preferred, so the reported path is the shorter one
// where both exist.
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

// copy_reversed copies `length` node indices from `path` into `out` back to front, so the
// path reads source to sink, which is the direction a reader thinks in.
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

// blame_walk extends the path in `path` until it reaches a node that introduced `bad`,
// trying each edge in turn and backtracking where a branch dead-ends.
//
// Takes g (dag_t) which is the graph being walked.
// Takes computed (lane_t) which holds the lane computed for each node.
// Takes bad (lane_t) which is the lane the blame walk is following.
// Takes path (int) which holds the chain being walked, sink first.
// Takes length (int) which is how many entries of `path` are in use.
// Returns the length of a path ending at an introducer, 0 when no branch from here
// reaches one, or -1 when the walk hit BLAME_PATH_MAX.
static int blame_walk(
    const dag_t *g,
    const lane_t *computed,
    const lane_t bad,
    int *path,
    const int length
) {
    const dagnode_t *node = dag_at_const(g, path[length - 1]);

    if (blame_walk_stops(node, bad)) {
        return length;
    }
    if (length >= BLAME_PATH_MAX) {
        return -1;
    }
    for (int k = 0; k <= node->input_count; k++) {
        const int next = skip_demands(g, blame_candidate(node, k));
        if (next < 0 || computed[next] != bad || already_walked(path, length, next)) {
            continue;
        }
        path[length] = next;
        const int found = blame_walk(g, computed, bad, path, length + 1);
        if (found != 0) {
            return found;
        }
    }
    return 0;
}

// lattice_blame traces `bad` from the node `failing` back to the node that introduced it,
// writing the path into `out`, source to sink, at most `cap` entries.
//
// A recurrence makes the graph cyclic, so the walk never takes an edge back to a node it
// has already reported; unguarded, it would circle until it exhausted BLAME_PATH_MAX and
// report a hop count that measures the cap rather than the distance. A path that hits
// BLAME_PATH_MAX stops short of the introducer, so the walk hands back a negative length
// when it truncates and a caller cannot mistake a stopping point for an origin. The
// search backtracks, because a greedy walk stops wherever its chosen operand has no bad
// input left to follow and reports a node whose own class is the join identity; the bad
// lane arrived by some path, so a search that backtracks finds a node that introduced it.
//
// Takes g (dag_t) which is the graph being walked.
// Takes computed (lane_t) which holds the lane computed for each node.
// Takes failing (int) which is the node whose declaration failed.
// Takes bad (lane_t) which is the lane the blame walk is following.
// Takes out (int) which receives the answer.
// Takes cap (int) which bounds how many entries may be written to `out`; BLAME_PATH_MAX
// bounds the walk itself.
int lattice_blame(
    const dag_t *g,
    const lane_t *computed,
    const int failing,
    const lane_t bad,
    int *out,
    const int cap
) {
    int path[BLAME_PATH_MAX];
    const int start = skip_demands(g, failing);
    int length;

    if (start < 0) {
        return 0;
    }
    path[0] = start;
    length = blame_walk(g, computed, bad, path, 1);
    return length <= 0 ? length : copy_reversed(path, length, out, cap);
}
