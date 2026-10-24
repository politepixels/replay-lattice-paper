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

#ifndef LANELANG_DAG_H
#define LANELANG_DAG_H

#include "diagnostic.h"
#include "lane.h"

#include <stddef.h>
#include <string.h>

// DAG_MAX_INPUTS is the most operands any node may have, one above the three-operand
// `choose`, the widest construct in the language. A resolver that built a wider node
// would overrun the `in` array.
#define DAG_MAX_INPUTS 4

// dagop_t is what a node computes. The schedule class is carried separately, because the
// same operation can lower to more than one kernel.
typedef enum {
    // OP_INPUT is a parameter, which is a leaf. It is grounded, because what it holds is
    // the caller's obligation, and that obligation is what a lane on a parameter would
    // state.
    OP_INPUT,

    // OP_SOURCE is source(count: n), an external source, which is a leaf and is not
    // grounded, because its bits move with the world rather than with the schedule.
    OP_SOURCE,

    // OP_CONST is a literal.
    OP_CONST,

    // OP_INDICES is indices(count: n), a generated column, deterministic by construction.
    OP_INDICES,

    // OP_INTEGERS is integers(count: n), a generated column of `exact` integers, each
    // small enough that addition below 2^53 cannot round.
    OP_INTEGERS,

    // OP_ELEM is elementwise addition, subtraction, multiplication or division, which is
    // lane-neutral.
    OP_ELEM,

    // OP_CHOOSE is choose(when: mask, then: a, else: b), the conditional, taken
    // elementwise: per position the result is `a` where the mask is non-zero and `b`
    // where it is zero. Any column may be a mask, because the language has no comparison
    // operators.
    //
    // Both arms are operands of this node, so both contribute to the join at every
    // position whichever the mask selects, and each position takes one arm's value
    // unchanged, a select rather than a blend. The evaluator computes only the selected
    // arm per position, but the lane is read off `in` before anything is evaluated, so a
    // mask pinned at `approx` carries `approx` into the result with blame on the mask,
    // and no rule mentions this op.
    OP_CHOOSE,

    // OP_FOLD is a whole-column reduction, whose schedule class says which kernel it
    // lowers to.
    OP_FOLD,

    // OP_SCAN is scan(x), a running prefix, whose schedule class says which kernel it
    // lowers to.
    OP_SCAN,

    // OP_RECUR is recur(init: e, body: e'), a carried-state recurrence, where `body` may
    // name the binding it defines. It is evaluated strictly in index order, so it has
    // exactly one schedule and a neutral class; its carried edge is the only cycle the
    // language admits, and the reason lattice_check iterates to a fixed point.
    OP_RECUR,

    // OP_TICK is the 0-based tick index of the recurrence named by `tick_of`, which is
    // the second parameter of that recurrence's step function. It is a leaf: a tick index
    // is a function of the program rather than of any value, so it takes no operand and
    // no carried edge, it is `exact`, it is grounded, and no carried edge leads from it.
    OP_TICK,

    // OP_OPAQUE wraps the result of a call to a function declared `ungrounded`. It has
    // exactly one input, the body's lowered result, and its class is neutral: it computes
    // nothing and its lane is its input's. It exists to record the grounding answer, so
    // `ground_of_node` answers false for it whatever its input says. It never interns, so
    // `bless` can stay out of the interning key.
    OP_OPAQUE,

    // OP_DEMAND carries a demand the source wrote about a value. It has exactly one
    // input, the value demanded, and its class is neutral: it computes nothing and its
    // lane is its input's. It is its own node so that a demand on a shared value cannot
    // reach the value's other names, and unlike OP_OPAQUE it interns: `replay a :=
    // sum(x)` beside `replay b := sum(x)` is one node rather than two. Its `declared`
    // field is part of the interning key, so it is set before the node is offered to
    // dag_intern.
    OP_DEMAND
} dagop_t;

// dagnode_t is one value in the graph, holding everything a node needs to be checked,
// lowered and executed.
typedef struct {
    // op is what the node computes.
    dagop_t op;

    // in holds the indices of the operand nodes, in operand order; only the first
    // `input_count` entries mean anything. dag_node_init clears the node to zero bytes,
    // so an unused entry holds 0, which is a real node index rather than a sentinel, and
    // a reader that trusts the array past `input_count` is reading node 0 as an operand.
    int in[DAG_MAX_INPUTS];

    // input_count is how many entries of `in` are in use.
    int input_count;

    // carried is the body of an OP_RECUR node, or -1 for every other node. It is the only
    // edge in the graph that may point forward, because a recurrence's body is built
    // after the node it feeds, and it is deliberately not an entry of `in`, whose entries
    // must be strictly earlier than their consumer.
    int carried;

    // tick_of is the OP_RECUR node this OP_TICK counts, or -1 for every other node. It is
    // part of the interning key, because two tick nodes belonging to different
    // recurrences are two different values; without it a nested recurrence would intern
    // its inner tick onto the outer one and read the wrong counter. It is not an entry of
    // `in`, because the tick index does not read the carried value, and joining the
    // recurrence's lane into it would weaken every tick index.
    int tick_of;

    // op_class is the schedule class of this operation's own lowering contract.
    opclass_t op_class;

    // declared is the demand stated at this node, or LANE_NONE when the source stated
    // none. It is non-zero only on OP_DEMAND and is set before that node is interned,
    // because it is part of the interning key, and a key field written after a node may
    // have been shared changes the node's identity under whoever already holds it.
    lane_t declared;

    // lane is the ambient lane the node was built under, which is part of its identity.
    lane_t lane;

    // promise identifies the call whose declared return lane this node was built under,
    // or 0 for a node built anywhere else. It is identity rather than a contract: it
    // separates the cones of two calls without touching which kernel either lowers to, so
    // within one call the body's nodes share and across two calls they do not. It is part
    // of the interning key and is set before the node is interned.
    int promise;

    // span is where the node's operation appears in the source.
    span_t span;

    // declared_span is where the node's demand appears in the source, or a span with line
    // 0 when it has none.
    span_t declared_span;

    // constant is the literal for OP_CONST, the element count for OP_INDICES, OP_INTEGERS
    // and OP_SOURCE, and the tick count for OP_RECUR.
    double constant;

    // binary_operator is the operator for OP_ELEM, one of '+', '-', '*' or '/'. Part of
    // the interning key for the same reason `constant` is.
    int binary_operator;

    // bless is what a `@bless` on this node promises: 0 for none, BLESS_PROMISE for the
    // bare form, `checksum + 1` for the conditional one. It is non-zero only on OP_SOURCE
    // and OP_OPAQUE, the two kinds a waiver may bind to, and only the bare form on
    // OP_OPAQUE, because a checksum is verified against the column a source loads and
    // there is no such column behind a call. It is not part of the interning key, and the
    // waiver's kinds must stay the set of kinds dag_intern never interns, because a
    // waiver on a node the interner may hand to a second owner is a promise about a value
    // its writer never saw. The absent value is 0 because the checksum is stored biased
    // by one.
    long long bless;

    // stated_by_binding is 1 on a demand node a binding's qualifier wrote, and 0 on every
    // other node, a parameter's demand and a call's return promise among them. Healing
    // stops only at a demand of another lane that a binding wrote, so a lane the source
    // wrote on a binding bounds what healing may strengthen, while a parameter or a
    // promise does not keep a private producer from healing. It is not part of the
    // interning key, and a binding that reuses a node sets it there.
    int stated_by_binding;

    // uses is how many times nodes that reach the function result read this node, counted
    // per operand edge rather than per distinct consumer, plus one for the result itself.
    // It is a liveness flag: heal.c compares it to zero and to nothing else, and privacy
    // is decided by mark_escapes' read-path closure rather than by this count, so a
    // consumer naming this node twice counts twice and carries no verdict.
    int uses;

    // reached is scratch for dag_recount_uses: non-zero once this node is known to reach
    // the function result.
    int reached;

    // label is the source name of the binding this node came from, borrowed from the
    // source text, or NULL when the node has no name. Used by diagnostics and by the
    // figures.
    const char *label;

    // label_length is the length of `label` in bytes, or 0 when there is none.
    int label_length;
} dagnode_t;

// dag_node_init clears `node` to the state every builder starts from.
//
// Zeroing a node would leave the carried edge pointing at node 0, which is a real index
// and usually a leaf, so a builder that forgot to set the edge would create a silent
// cycle rather than an obvious crash. Every site that builds a node calls this rather
// than a bare memset.
//
// Takes node (dagnode_t) which is the node under examination.
static inline void dag_node_init(dagnode_t *node) {
    memset(node, 0, sizeof *node);
    node->carried = -1;
    node->tick_of = -1;
}

// dag_t is the graph itself: a growable array of nodes in dependency order.
typedef struct dag dag_t;

// dag_new creates an empty graph.
// Returns NULL if allocation fails.
dag_t *dag_new(void);

// dag_free releases a graph created by dag_new. Passing NULL is allowed and does nothing.
//
// Takes g (dag_t) which is the graph being walked.
void dag_free(dag_t *g);

// dag_intern interns a node.
//
// Takes g (dag_t) which is the graph being walked.
// Takes node (dagnode_t) which is the node under examination.
//
// Returns the index of an existing node that is structurally and contractually identical
// to `node` when one exists, otherwise appends `node` and returns its index.
// Returns -1 if allocation fails.
int dag_intern(dag_t *g, const dagnode_t *node);

// dag_append appends `node` without interning, for nodes that must stay distinct however
// alike they look.
// Returns the new node's index, or -1 if allocation fails.
//
// Takes g (dag_t) which is the graph being walked.
// Takes node (dagnode_t) which is the node under examination.
int dag_append(dag_t *g, const dagnode_t *node);

// dag_count is how many nodes the graph holds. A NULL graph holds none.
//
// Takes g (dag_t) which is the graph being walked.
int dag_count(const dag_t *g);

// dag_at is the node at index `i`, for a caller that will modify it. The index must be in
// range.
//
// Takes g (dag_t) which is the graph being walked.
// Takes i (int) which is the element being read.
dagnode_t *dag_at(dag_t *g, int i);

// dag_at_const is the node at index `i`, for a caller that will only read it. The index
// must be in range.
//
// Takes g (dag_t) which is the graph being walked.
// Takes i (int) which is the element being read.
const dagnode_t *dag_at_const(const dag_t *g, int i);

// dag_recount_uses recomputes `uses` across the graph.
//
// `result` counts as one additional use, so that a value the program returns is live even
// when no node reads it. heal.c calls this itself, before its pass, because its
// live-consumer test reads these counts and a stale count gives a wrong answer.
//
// Takes g (dag_t) which is the graph being walked.
// Takes result (int) which is the node the program returns.
void dag_recount_uses(dag_t *g, int result);

// dag_dot emits the graph as Graphviz.
//
// Nodes are labelled and coloured by computed lane when lanes are supplied, blame nodes
// are drawn bold, and the result node is drawn with a double border.
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
    int blame_count,
    int result,
    void *stream
);

#endif  // LANELANG_DAG_H
