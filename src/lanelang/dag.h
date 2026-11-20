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

    // OP_SOURCE represents `source(count: n)`. This leaf reads an external column whose
    // bits depend on the input world.
    OP_SOURCE,

    // OP_CONST is a literal.
    OP_CONST,

    // OP_INDICES represents `indices(count: n)`, a deterministic generated column.
    OP_INDICES,

    // OP_INTEGERS represents `integers(count: n)`, a deterministic column of exactly
    // representable integers.
    OP_INTEGERS,

    // OP_ELEM is elementwise addition, subtraction, multiplication or division, which is
    // lane-neutral.
    OP_ELEM,

    // OP_CHOOSE represents `choose(when: mask, then: a, else: b)`. Each position selects
    // `a` for a non-zero mask and `b` for zero. Any column can serve as the mask.
    //
    // The mask and both arms contribute to the ordinary lane join. Each result element
    // takes the selected arm's value unchanged. An `approx` mask can therefore make the
    // result `approx` even when the arms are `exact`.
    OP_CHOOSE,

    // OP_FOLD is a whole-column reduction, whose schedule class says which kernel it
    // lowers to.
    OP_FOLD,

    // OP_SCAN is scan(x), a running prefix, whose schedule class says which kernel it
    // lowers to.
    OP_SCAN,

    // OP_RECUR holds the value that `iterate(times: n, from: e, with: step)` carries
    // between ticks. Ticks run in index order, so the recurrence itself has a neutral
    // schedule class. Its carried edge can close a cycle, which is why lattice_check
    // iterates to a fixed point.
    OP_RECUR,

    // OP_TICK reads the zero-based index of the recurrence named by `tick_of`. It
    // supplies the second parameter of the step function. The program fixes this index,
    // so the leaf is grounded and `exact` and has no input or carried edge.
    OP_TICK,

    // OP_OPAQUE wraps the result of a call declared `ungrounded` and inherits its body's
    // lane. The checker treats the call as unfixed. A demand can rely on it if the call
    // has a waiver and the body is fixed or covered by waivers. Each call gets a fresh
    // wrapper.
    OP_OPAQUE,

    // OP_DEMAND records a written demand on its single input and inherits the input's
    // lane. The separate node keeps a demand on one binding from applying to the value's
    // other names. Equivalent demands may share a node, as in `replay a := sum(x)`
    // alongside `replay b := sum(x)`. The builder sets `declared` before interning
    // because it contributes to the key.
    OP_DEMAND
} dagop_t;

// dagnode_t is one value in the graph, holding everything a node needs to be checked,
// lowered and executed.
typedef struct {
    // op is what the node computes.
    dagop_t op;

    // in holds the indices of the `input_count` operands in source order. dag_node_init
    // clears the unused entries to zero, which is also a valid node index.
    int in[DAG_MAX_INPUTS];

    // input_count is how many entries of `in` are in use.
    int input_count;

    // carried is the body of an OP_RECUR node, or -1 for every other node. It is the only
    // edge in the graph that may point forward, because a recurrence's body is built
    // after the node it feeds, and it is deliberately not an entry of `in`, whose entries
    // must be strictly earlier than their consumer.
    int carried;

    // step_end is the last node emitted while lowering the recurrence's step. A nested
    // recurrence may appear earlier in this range than the nodes that compute its final
    // carry.
    int step_end;

    // tick_of identifies the OP_RECUR node whose counter an OP_TICK reads. Every other
    // node holds -1 here. Including the owner in the interning key keeps nested counters
    // distinct. The tick index is independent of the carried value, so the lane join only
    // follows `in` and `carried`.
    int tick_of;

    // op_class is the schedule class of this operation's own lowering contract.
    opclass_t op_class;

    // declared is the demand stated at this node, or LANE_NONE. The resolver sets it on
    // an OP_DEMAND wrapper before interning because it is part of the interning key.
    lane_t declared;

    // lane records the construction lane used for interning. When healing strengthens a
    // producer, it updates this field to the selected kernel's lane.
    lane_t lane;

    // promise identifies the call whose declared return lane encloses this node, or zero
    // outside such a call. The interning key includes this identifier so nodes can share
    // within a call while separate calls retain their own cones. The operation's class
    // still selects the kernel.
    int promise;

    // span is where the node's operation appears in the source.
    span_t span;

    // declared_span is where the node's demand appears in the source, or a span with line
    // 0 when it has none.
    span_t declared_span;

    // argument_span is where the argument a parameter's demand covers is written in the
    // call, or a span with line 0 when the demand is not on a parameter. A refusal of the
    // demand points here as well, so the blame reaches the call that supplied the value.
    span_t argument_span;

    // constant is the literal for OP_CONST, the element count for OP_INDICES, OP_INTEGERS
    // and OP_SOURCE, and the tick count for OP_RECUR.
    double constant;

    // binary_operator holds the OP_ELEM operator, one of '+', '-', '*' or '/'. Like
    // `constant`, it contributes to the interning key.
    int binary_operator;

    // bless records a waiver on a node. Zero means the waiver is absent, BLESS_PROMISE
    // records the bare form, and `checksum + 1` records a checksum for the evaluator to
    // verify. Only OP_SOURCE loads a column to verify. OP_OPAQUE accepts only a bare
    // waiver. Neither kind is interned, so the waiver applies to one source or call.
    long long bless;

    // stated_by_binding is 1 on a demand node a binding's qualifier wrote, and 0 on every
    // other node, a parameter's demand and a call's return promise among them. Healing
    // stops only at a demand of another lane that a binding wrote, so a lane the source
    // wrote on a binding bounds what healing may strengthen, while a parameter or a
    // promise does not keep a private producer from healing. The interning key keeps
    // these uses distinct even when they demand the same lane on the same value.
    int stated_by_binding;

    // uses counts each ordinary or carried edge read by a node that reaches the result,
    // plus one for the result itself. Healing uses a non-zero count to recognise a live
    // node. mark_escapes separately determines whether its value escapes the demanded
    // cone.
    int uses;

    // reached is non-zero once dag_recount_uses finds that this node reaches the result.
    int reached;

    // label is the name shown in diagnostics and figures, or NULL for an unnamed node. It
    // points to source text or to a string supplied by the builder.
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
    node->step_end = -1;
    node->tick_of = -1;
}

// dag_t stores the graph in a growable array of nodes in dependency order.
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
// Returns the matching node's index when sharing is legal, or the newly appended node's
// index. Returns -1 if allocation fails.
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

// dag_mark_cone marks a node and its transitive inputs, including recurrence carries.
//
// Takes g (dag_t) which is the graph being walked.
// Takes at (int) which is the node whose inputs to mark.
// Takes cone (char) which holds one byte per node, initially zero for an empty cone.
void dag_mark_cone(const dag_t *g, int at, char *cone);

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
