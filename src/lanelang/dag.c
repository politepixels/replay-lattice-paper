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

// DAG_INITIAL_CAPACITY is how many nodes a fresh graph makes room for. Growth doubles
// from here, so the value only trades one early reallocation against the memory a tiny
// program holds.
#define DAG_INITIAL_CAPACITY 64

// DAG_DOT_LABEL_MAX is the longest Graphviz node label built. A label that would exceed
// it is truncated by snprintf, which costs a figure some text and can never overrun the
// buffer.
#define DAG_DOT_LABEL_MAX 256

// DAG_DOT_TEXT_MAX is the longest node description built for a Graphviz label, which is a
// binding name and one word. Truncation has the same consequence as for the label itself.
#define DAG_DOT_TEXT_MAX 160

// dag is a growable array of nodes, kept in dependency order: every operand is appended
// before the node that reads it.
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

// dag_append appends a copy of `node`: the result is its index, or -1 when the array
// cannot grow.
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

// dag_arithmetic_equal reports whether two nodes agree on the arithmetic they perform:
// the literal or element count they carry, the elementwise operator they apply, and the
// name a literal was given.
//
// The constants are compared by bit pattern: == would merge -0.0 with +0.0, which an
// executor can tell apart, and would keep two identical NaN constants distinct.
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

// dag_contract_equal reports whether two nodes agree on their determinism contract: the
// lane each was built under, the demand stated on it, the schedule class its lowering
// will use, and the promise scope it was built inside.
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
    if (a->declared != b->declared) {
        return 0;
    }
    if (a->promise != b->promise) {
        return 0;
    }
    return a->op_class != b->op_class ? 0 : 1;
#endif
}

// dag_nodes_equal is structural equality for the interning: the operation, the operands,
// the determinism contract and the arithmetic.
//
// The carried edge is not compared, because nothing carrying one reaches here: dag_intern
// passes OP_RECUR straight through, so an interning candidate always has `carried` at -1.
// `tick_of` is compared and is load-bearing, because two tick nodes share an op, an
// operand count, a class and a lane, and dag_arithmetic_equal compares labels only for
// constants and generators, so `tick_of` is the whole of what separates an inner
// recurrence's counter from an outer one's.
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

// dag_intern interns a node, and passes leaves straight through: two parameters are two
// values however alike they look.
//
// OP_RECUR also goes straight through, because its body is built after it, so its carried
// edge is still -1 when it is added, and interning would merge two recurrences on a field
// neither has filled in yet. OP_SOURCE never interns because two source() calls are two
// values: nothing says the world hands back the same column twice. OP_OPAQUE never
// interns because `bless` is outside the interning key, and an opaque wrapper handed to a
// second owner would carry a promise nobody wrote about that owner's value.
//
// Takes g (dag_t) which is the graph being walked.
// Takes node (dagnode_t) which is the node under examination.
int dag_intern(dag_t *g, const dagnode_t *node) {
    if (node->op != OP_INPUT && node->op != OP_SOURCE && node->op != OP_RECUR
        && node->op != OP_OPAQUE) {
        for (int i = 0; i < g->count; i++) {
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
// from nothing.
//
// Takes g (dag_t) which is the graph being walked.
static void dag_clear_use_counts(dag_t *g) {
    for (int i = 0; i < g->count; i++) {
        g->nodes[i].uses = 0;
        g->nodes[i].reached = 0;
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
// A carried edge points forward, from a recurrence to a body built after it, so one
// backward sweep does not suffice: marking the recurrence would mark a body the sweep has
// already passed, and that body's own producers would never be reached. The sweep
// therefore repeats until nothing new is marked. Without this a producer reachable only
// through a carried edge stays outside the demanded cone, is never counted as used, and
// can never be healed, which silently costs every recurrence the inference that every
// other construct gets.
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
// Healing reads this count for one thing only: whether a consumer is live, and heal.c
// skips a dead one on `uses == 0`. resolve.c lowers every binding in source order whether
// or not it reaches the result, so counting over the whole array would give a dead
// binding a non-zero count and let it decide a verdict, which is dead-code elimination
// changing what a program means. The carried edge is a use like any other, and is
// counted: a recurrence really does read its body. Privacy is decided by heal.c's
// mark_escapes, never by this count.
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

// dag_lane_fill is the Graphviz fill colour for a lane: green for the strongest
// guarantee, blue for a pinned tree, red for a free reduction, grey for anything else.
// The hex values are byte-compared against the committed figures, so they are contract
// text.
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

// dag_node_text describes a node for a figure: its binding name where it has one, then
// one word for what it computes. The words are byte-compared against the committed
// figures, so they are contract text.
//
// Every op is named and there is no `default`, so -Wswitch under the build's -Werror
// makes the next op added fail to compile here rather than draw itself as a fold. A
// `default` answering "fold" would hand every later op that word, and no committed figure
// holds such a node, so no figure comparison could catch it.
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

// dag_dot_label builds the Graphviz label for one node. `with_lanes` adds the node's
// schedule class and any demand stated on it, which is what a figure of a checked program
// shows.
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
// blame path, which is what makes an arrow bold rather than merely its endpoints.
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
// The carried edge is the single cycle the language admits and the reason lattice_check
// has an iterative branch, so a figure that omitted it would show a graph that settles in
// one pass. It is dashed rather than solid because it runs backwards in the node order.
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

// dag_dot_preamble writes the graph attributes every figure shares: left-to-right
// ranking, which fits a two-column page, and the node and edge styles. The attribute
// order is byte-compared against the committed figures.
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
