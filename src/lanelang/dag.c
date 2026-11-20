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

#include "dag.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// DAG_INITIAL_CAPACITY is the number of slots allocated on the first append. Later growth
// doubles the capacity.
#define DAG_INITIAL_CAPACITY 64

// DAG_DOT_LABEL_MAX is the capacity in bytes of the node label buffer, including the
// terminating null. snprintf truncates longer labels.
#define DAG_DOT_LABEL_MAX 256

// DAG_DOT_TEXT_MAX is the capacity in bytes of the buffer for a node's name and
// operation, including the terminating null.
#define DAG_DOT_TEXT_MAX 160

// dag is a growable array of nodes in dependency order. Each operand precedes the node
// that reads it.
struct dag {
    // nodes is the nodes, or NULL before the first append.
    dagnode_t *nodes;

    // count is how many entries of `nodes` are in use.
    int count;

    // capacity is how many entries `nodes` has room for.
    int capacity;
};

// dag_new allocates an empty graph, or NULL when the allocation fails.
dag_t *dag_new(void) {
    dag_t *g = (dag_t *) calloc(1, sizeof *g);
    return g;
}

// dag_free frees the graph and every node it holds. Safe on NULL.
//
// Takes g (dag_t) which is the graph being walked.
void dag_free(dag_t *g) {
    if (!g) {
        return;
    }
    free(g->nodes);
    free(g);
}

// dag_grow makes room for one more node, doubling the array when it is full.
// Returns 0 if allocation fails, in which case the graph is unchanged.
//
// Takes g (dag_t) which is the graph being walked.
static int dag_grow(dag_t *g) {
    if (g->count < g->capacity) {
        return 1;
    }
    const int capacity = g->capacity ? g->capacity * 2 : DAG_INITIAL_CAPACITY;
    dagnode_t *nodes = (dagnode_t *) realloc(g->nodes, sizeof(dagnode_t) * (size_t) capacity);
    if (!nodes) {
        return 0;
    }
    g->nodes = nodes;
    g->capacity = capacity;
    return 1;
}

// dag_append appends a copy of `node` and returns its index, or -1 if the array cannot
// grow.
//
// Takes g (dag_t) which is the graph being walked.
// Takes node (dagnode_t) which is the node under examination.
int dag_append(dag_t *g, const dagnode_t *node) {
    if (!dag_grow(g)) {
        return -1;
    }
    g->nodes[g->count] = *node;
    return g->count++;
}

// dag_arithmetic_equal compares constants and generator lengths with their labels, and
// compares the operator for an elementwise node.
//
// Comparing the bits preserves the sign of zero and lets identical NaN bit patterns share
// a node. Numeric equality loses the sign and treats even identical NaNs as unequal.
//
// Takes a (dagnode_t) which is the first value.
// Takes b (dagnode_t) which is the second value.
static int dag_arithmetic_equal(const dagnode_t *a, const dagnode_t *b) {
    if (a->op == OP_CONST || a->op == OP_INDICES || a->op == OP_INTEGERS) {
        if (memcmp(&a->constant, &b->constant, sizeof a->constant) != 0) {
            return 0;
        }
    }
    if (a->op == OP_ELEM && a->binary_operator != b->binary_operator) {
        return 0;
    }
    if (a->op == OP_CONST || a->op == OP_INDICES || a->op == OP_INTEGERS) {
        if (a->label_length != b->label_length) {
            return 0;
        }
        if (a->label_length && memcmp(a->label, b->label, (size_t) a->label_length) != 0) {
            return 0;
        }
    }
    return 1;
}

// dag_contract_equal compares the construction lane, the stated demand and whether a
// binding supplied it, the schedule class and the enclosing promise scope.
//
// Takes a (dagnode_t) which is the first value.
// Takes b (dagnode_t) which is the second value.
static int dag_contract_equal(const dagnode_t *a, const dagnode_t *b) {
#ifdef LANELANG_CSE_IGNORES_LANE
    (void) a;
    (void) b;
    return 1;
#else
    if (a->lane != b->lane) {
        return 0;
    }
    if (a->declared != b->declared || a->stated_by_binding != b->stated_by_binding) {
        return 0;
    }
    if (a->promise != b->promise) {
        return 0;
    }
    return a->op_class != b->op_class ? 0 : 1;
#endif
}

// dag_nodes_equal checks whether the operation, operands, determinism contract and
// arithmetic allow two nodes to share an entry.
//
// dag_intern appends each OP_RECUR directly, so candidates here have no carried edge to
// compare. Tick nodes have no operands. Their `tick_of` fields distinguish an inner
// counter from an outer one.
//
// Takes a (dagnode_t) which is the first value.
// Takes b (dagnode_t) which is the second value.
static int dag_nodes_equal(const dagnode_t *a, const dagnode_t *b) {
    if (a->op != b->op || a->input_count != b->input_count) {
        return 0;
    }
    for (int i = 0; i < a->input_count; i++) {
        if (a->in[i] != b->in[i]) {
            return 0;
        }
    }
    if (a->tick_of != b->tick_of) {
        return 0;
    }
    return !dag_contract_equal(a, b) ? 0 : dag_arithmetic_equal(a, b);
}

// dag_intern reuses an equivalent node when it is safe to share. Each input parameter
// gets its own node.
//
// A new OP_RECUR has no carried edge until its body is built. A new OP_SOURCE may receive
// different data on each call. Each OP_OPAQUE also needs its own node because its owner's
// `bless` promise lies outside the interning key.
//
// A recurrence node holds the incoming carry inside its step and the final result after
// the step closes. An expression that reads the final result can only share a node built
// after the step.
//
// Takes g (dag_t) which is the graph being walked.
// Takes node (dagnode_t) which is the node under examination.
int dag_intern(dag_t *g, const dagnode_t *node) {
    if (node->op != OP_INPUT && node->op != OP_SOURCE && node->op != OP_RECUR
        && node->op != OP_OPAQUE) {
        int first = 0;
        for (int k = 0; k < node->input_count; k++) {
            const int input = node->in[k];
            if (input >= 0 && input < g->count && g->nodes[input].op == OP_RECUR
                && g->nodes[input].step_end >= first) {
                first = g->nodes[input].step_end + 1;
            }
        }
        for (int i = first; i < g->count; i++) {
            if (dag_nodes_equal(&g->nodes[i], node)) {
                return i;
            }
        }
    }
    return dag_append(g, node);
}

// dag_count is how many nodes the graph holds, 0 for a NULL graph.
//
// Takes g (dag_t) which is the graph being walked.
int dag_count(const dag_t *g) {
    return g ? g->count : 0;
}

// dag_at is the node at index `i`, mutable.
//
// Takes g (dag_t) which is the graph being walked.
// Takes i (int) which is the element being read.
dagnode_t *dag_at(dag_t *g, const int i) {
    return &g->nodes[i];
}

// dag_at_const is the node at index `i`, read-only.
//
// Takes g (dag_t) which is the graph being walked.
// Takes i (int) which is the element being read.
const dagnode_t *dag_at_const(const dag_t *g, const int i) {
    return &g->nodes[i];
}

// dag_clear_use_counts forgets every use count and reachability mark, so a recount starts
// from zero.
//
// Takes g (dag_t) which is the graph being walked.
static void dag_clear_use_counts(dag_t *g) {
    for (int i = 0; i < g->count; i++) {
        g->nodes[i].uses = 0;
        g->nodes[i].reached = 0;
    }
}

// dag_mark_cone marks a node and its transitive inputs, following carried edges too.
//
// Takes g (dag_t) which is the graph being walked.
// Takes at (int) which is the node whose inputs to mark.
// Takes cone (char) which holds one mark per node, initially zero for an empty cone.
void dag_mark_cone(const dag_t *g, const int at, char *cone) {
    if (at < 0 || at >= g->count || cone[at]) {
        return;
    }
    cone[at] = 1;
    const dagnode_t *node = dag_at_const(g, at);
    for (int k = 0; k < node->input_count; k++) {
        dag_mark_cone(g, node->in[k], cone);
    }
    if (node->carried >= 0) {
        dag_mark_cone(g, node->carried, cone);
    }
}

// dag_mark_producers_of marks every node that feeds node `index`, and reports whether
// that changed anything.
//
// The carried edge is walked beside the ordinary operands, because a producer reachable
// only through one is a producer all the same.
//
// Takes g (dag_t) which is the graph being walked.
// Takes index (int) which is the marked node whose producers are being reached.
// Returns 1 when a node was newly marked, 0 when all of them already were.
static int dag_mark_producers_of(dag_t *g, const int index) {
    int changed = 0;
    for (int k = 0; k < g->nodes[index].input_count; k++) {
        const int input = g->nodes[index].in[k];
        if (input >= 0 && input < g->count && !g->nodes[input].reached) {
            g->nodes[input].reached = 1;
            changed = 1;
        }
    }
    const int carried = g->nodes[index].carried;
    if (carried >= 0 && carried < g->count && !g->nodes[carried].reached) {
        g->nodes[carried].reached = 1;
        changed = 1;
    }
    return changed;
}

// dag_mark_nodes_reaching_result marks every node that reaches `result`, working
// backwards from it.
//
// A carried edge can lead from a recurrence to a body built later. By the time the
// backward sweep marks that body it may already have passed its producers. Repeating the
// sweep until the marks settle includes those producers in the use counts for healing.
//
// Takes g (dag_t) which is the graph being walked.
// Takes result (int) which is the node the program returns.
static void dag_mark_nodes_reaching_result(dag_t *g, const int result) {
    if (result >= 0 && result < g->count) {
        g->nodes[result].reached = 1;
    }
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int i = g->count - 1; i >= 0; i--) {
            if (g->nodes[i].reached) {
                changed |= dag_mark_producers_of(g, i);
            }
        }
    }
}

// dag_count_uses_from_reached counts one use for every edge that leaves a marked node, so
// a dead binding's edges count for nothing.
//
// Takes g (dag_t) which is the graph being walked.
static void dag_count_uses_from_reached(dag_t *g) {
    for (int i = 0; i < g->count; i++) {
        if (!g->nodes[i].reached) {
            continue;
        }
        for (int k = 0; k < g->nodes[i].input_count; k++) {
            const int input = g->nodes[i].in[k];
            if (input >= 0 && input < g->count) {
                g->nodes[input].uses++;
            }
        }
        const int carried = g->nodes[i].carried;
        if (carried >= 0 && carried < g->count) {
            g->nodes[carried].uses++;
        }
    }
}

// dag_recount_uses counts over the nodes that reach the result and no others.
//
// Healing uses these counts to identify live consumers. A binding outside the result's
// dependencies supplies no live read, even though the resolver lowered it. Each
// recurrence counts a read of its step result through the carried edge. The caller counts
// as a reader of the program's result. The separate escape walk decides privacy.
//
// Takes g (dag_t) which is the graph being walked.
// Takes result (int) which is the node the program returns.
void dag_recount_uses(dag_t *g, const int result) {
    dag_clear_use_counts(g);
    dag_mark_nodes_reaching_result(g, result);
    dag_count_uses_from_reached(g);
    if (result >= 0 && result < g->count) {
        g->nodes[result].uses++;
    }
}

// dag_lane_fill chooses green for `exact`, blue for `replay`, red for `approx` and grey
// for an unstated lane.
//
// Takes lane (lane_t) which is the lane to report.
static const char *dag_lane_fill(const lane_t lane) {
    switch (lane) {
    case LANE_EXACT:  return "#d7ecd9";
    case LANE_REPLAY: return "#dbe4f0";
    case LANE_APPROX: return "#f6dcdc";
    default:          return "#eeeeee";
    }
}

// dag_node_text describes a node with its label, if any, followed by the name of its
// operation.
//
// Every op is named. Omitting a default lets -Wswitch under the build's -Werror catch an
// operation missing from this switch.
//
// Takes node (dagnode_t) which is the node under examination.
// Takes text (char) which receives the description's bytes.
// Takes capacity (size_t) which bounds how many entries may be written.
static void dag_node_text(const dagnode_t *node, char *text, const size_t capacity) {
    const char *what = "fold";
    switch (node->op) {
    case OP_INPUT:   what = "input"; break;
    case OP_SOURCE:  what = "source"; break;
    case OP_CONST:   what = "const"; break;
    case OP_INDICES: what = "indices"; break;
    case OP_INTEGERS: what = "integers"; break;
    case OP_ELEM:    what = "elementwise"; break;
    case OP_CHOOSE:  what = "choose"; break;
    case OP_SCAN:    what = "scan"; break;
    case OP_OPAQUE:  what = "opaque"; break;
    case OP_FOLD:    what = "fold"; break;
    case OP_RECUR:   what = "recurrence"; break;
    case OP_TICK:    what = "tick"; break;
    case OP_DEMAND:  what = "demand";
    }
    if (node->label_length > 0) {
        snprintf(text, capacity, "%.*s\\n%s", node->label_length, node->label, what);
    } else {
        snprintf(text, capacity, "%s", what);
    }
}

// dag_dot_label builds the Graphviz label for one node. A figure of a checked program
// sets `with_lanes` to include the schedule class and any stated demand.
//
// Takes node (dagnode_t) which is the node under examination.
// Takes with_lanes (int) which selects the lane-annotated form.
// Takes label (char) which receives the label's bytes.
// Takes capacity (size_t) which bounds how many entries may be written.
static void dag_dot_label(const dagnode_t *node, const int with_lanes, char *label, const size_t capacity) {
    char text[DAG_DOT_TEXT_MAX];
    dag_node_text(node, text, sizeof text);
    if (!with_lanes) {
        snprintf(label, capacity, "%s", text);
        return;
    }
    snprintf(label, capacity, "%s\\n%s%s%s", text,
             opclass_name(node->op_class),
             node->declared != LANE_NONE ? "\\ndemands " : "",
             node->declared != LANE_NONE ? lane_name(node->declared) : "");
}

// dag_node_is_blamed reports whether node `index` lies on the blame path.
//
// Takes blame (int) which is the blame path, for highlighting.
// Takes blame_count (int) which is how many nodes the blame path holds.
// Takes index (int) which is the node's index in the graph.
static int dag_node_is_blamed(const int *blame, const int blame_count, const int index) {
    for (int b = 0; b < blame_count; b++) {
        if (blame[b] == index) {
            return 1;
        }
    }
    return 0;
}

// dag_edge_is_blamed reports whether the edge from `from` to `to` is a step along the
// blame path. An arrow is drawn bold for such a step, not whenever both its endpoints are
// blamed.
//
// Takes blame (int) which is the blame path, for highlighting.
// Takes blame_count (int) which is how many nodes the blame path holds.
// Takes from (int) which is the node the edge leaves.
// Takes to (int) which is the node the edge enters.
static int dag_edge_is_blamed(const int *blame, const int blame_count, const int from, const int to) {
    for (int b = 0; b + 1 < blame_count; b++) {
        if (blame[b] == from && blame[b + 1] == to) {
            return 1;
        }
    }
    return 0;
}

// dag_write_carried_edge draws node `index`'s carried edge, if it has one, dashed.
//
// The dashed line connects the step result to the recurrence that reads it. When the body
// depends on the carry, this edge closes a cycle that the lane sweep must settle.
//
// Takes file (FILE) which is the stream being written.
// Takes g (dag_t) which is the graph being drawn.
// Takes index (int) which is the node whose carried edge is drawn.
// Takes blame (int) which is the blamed path, for highlighting.
// Takes blame_count (int) which is how long the blamed path is.
static void dag_write_carried_edge(FILE *file, const dag_t *g, const int index,
                                   const int *blame, const int blame_count) {
    const int carried = g->nodes[index].carried;
    if (carried < 0) {
        return;
    }
    const int hot = dag_edge_is_blamed(blame, blame_count, carried, index);
    fprintf(file, "  n%d -> n%d [style=dashed constraint=false label=\"carried\"%s];\n",
            carried, index, hot ? " penwidth=3 color=\"#b03030\"" : "");
}

// dag_dot_preamble writes the shared node and edge styles and sets a left-to-right
// layout.
//
// Takes file (FILE) which is the stream being written.
static void dag_dot_preamble(FILE *file) {
    fprintf(file, "digraph lanelang {\n");
    fprintf(file, "  rankdir=LR;\n");
    fprintf(file, "  node [shape=box style=\"rounded,filled\" fontname=\"Helvetica\" fontsize=10];\n");
    fprintf(file, "  edge [fontname=\"Helvetica\" fontsize=9];\n");
}

// dag_dot_nodes writes one Graphviz node per graph node, coloured by lane, thickened
// where it is blamed, and double-bordered where it is the result.
//
// Takes g (dag_t) which is the graph being walked.
// Takes computed (lane_t) which holds the lane computed for each node.
// Takes blame (int) which is the blame path, for highlighting.
// Takes blame_count (int) which is how many nodes the blame path holds.
// Takes result (int) which is the node the program returns.
// Takes file (FILE) which is the stream being written.
static void dag_dot_nodes(
    const dag_t *g,
    const lane_t *computed,
    const int *blame,
    const int blame_count,
    const int result,
    FILE *file
) {
    for (int i = 0; i < g->count; i++) {
        const dagnode_t *node = &g->nodes[i];
        char label[DAG_DOT_LABEL_MAX];
        dag_dot_label(node, computed != NULL, label, sizeof label);
        const lane_t lane = computed ? computed[i] : node->lane;
        const int blamed = dag_node_is_blamed(blame, blame_count, i);
        fprintf(file, "  n%d [label=\"%s\\n= %s\" fillcolor=\"%s\"%s%s];\n",
                i, label, lane_name(lane), dag_lane_fill(lane),
                blamed ? " penwidth=3 color=\"#b03030\"" : "",
                i == result ? " peripheries=2" : "");
    }
}

// dag_dot_edges writes one Graphviz edge per operand, thickened where the edge is a step
// along the blame path.
//
// Takes g (dag_t) which is the graph being walked.
// Takes blame (int) which is the blame path, for highlighting.
// Takes blame_count (int) which is how many nodes the blame path holds.
// Takes file (FILE) which is the stream being written.
static void dag_dot_edges(const dag_t *g, const int *blame, const int blame_count, FILE *file) {
    for (int i = 0; i < g->count; i++) {
        for (int k = 0; k < g->nodes[i].input_count; k++) {
            const int input = g->nodes[i].in[k];
            if (input < 0) {
                continue;
            }
            const int hot = dag_edge_is_blamed(blame, blame_count, input, i);
            fprintf(file, "  n%d -> n%d%s;\n", input, i,
                    hot ? " [penwidth=3 color=\"#b03030\"]" : "");
        }
        dag_write_carried_edge(file, g, i, blame, blame_count);
    }
}

// dag_dot emits the graph as one Graphviz digraph on `stream`, coloured by lane, with the
// blame path bold.
//
// Takes g (dag_t) which is the graph being walked.
// Takes computed (lane_t) which holds the lane computed for each node.
// Takes blame (int) which is the blame path, for highlighting.
// Takes blame_count (int) which is how many nodes the blame path holds.
// Takes result (int) which is the node the program returns.
// Takes stream (void) which is where the output is written.
void dag_dot(
    const dag_t *g,
    const lane_t *computed,
    const int *blame,
    const int blame_count,
    const int result,
    void *stream
) {
    FILE *file = (FILE *) stream;
    dag_dot_preamble(file);
    dag_dot_nodes(g, computed, blame, blame_count, result, file);
    dag_dot_edges(g, blame, blame_count, file);
    fprintf(file, "}\n");
}
