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
#include "heal.h"

#include <stdlib.h>
#include <string.h>

// HEAL_SNAPSHOT_MAX is how many nodes have their schedule class remembered before the
// pass runs, so heal_program can report how many changed; a larger graph is still healed
// in full, and the reported count is then a lower bound.
#define HEAL_SNAPSHOT_MAX 512

// The helpers live inside the guard with their only caller. `lanelang_noheal`, the binary
// the healing control builds, compiles this file with -DLANELANG_NO_HEAL, which removes
// that caller; a helper left outside the guard would then be defined and unused, so the
// control build would fail under -Wall -Wextra -Werror while the ordinary build stayed
// clean.
#ifndef LANELANG_NO_HEAL

// class_for is the schedule class that discharges a demand for lane `want`, in `node`'s
// own operation family.
//
// A strengthened class has to be one the node's own lowering can emit, or the heal writes
// a lane the kernel never honours: a scan given a fold's class reads as pinned to the
// checker and still lowers to the free kernel, a silent wrong answer rather than a
// refusal. The offer is the strongest class the family has, not always the one asked for:
// this build routes no `exact` demand to a scan, so such a demand is offered
// the blocked class, whose lane is `replay`, and the refusal then comes from
// lattice_check comparing `replay` against the demand rather than from this function
// declining to pin. The blocked scan's bits are a function of (data, shape) alone, so the
// weaker offer is conservative.
//
// Takes node (dagnode_t) which is the node under examination.
// Takes want (lane_t) which is the lane being demanded.
static opclass_t class_for(const dagnode_t *node, const lane_t want) {
    if (node->op == OP_SCAN) {
        return want == LANE_APPROX ? OPC_SCAN_APPROX : OPC_SCAN_REPLAY;
    }
    switch (want) {
    case LANE_EXACT:  return OPC_FOLD_EXACT;
    case LANE_REPLAY: return OPC_FOLD_REPLAY;
    default:          return OPC_FOLD_APPROX;
    }
}

// pin_node strengthens `node` to discharge a demand for lane `want`, if it is unannotated
// and its class can be strengthened at all.
//
// A neutral node is left alone, a test written as a property of the class so that a
// second reduction family needs no change here. The lane written back is the class's
// rather than the demand's, because the two differ wherever the family has no discharge
// for the demand, an `exact` demand on a scan being offered the blocked class whose lane
// is `replay`; writing `want` there would record a node as built under a lane its own
// kernel does not deliver, and `lane` is in the interning key, so the discrepancy would
// decide which nodes are one node. Privacy is decided by mark_escapes, not here.
//
// Takes node (dagnode_t) which is the node under examination.
// Takes want (lane_t) which is the lane being demanded.
static void pin_node(dagnode_t *node, const lane_t want) {
    if (node->op_class == OPC_NEUTRAL) {
        return;
    }
    const opclass_t wanted = class_for(node, want);
    if (opclass_lane(wanted) >= opclass_lane(node->op_class)) {
        return;
    }
    node->op_class = wanted;
    node->lane = opclass_lane(wanted);
}

// mark_cone marks every node reachable from `at` by reading, which is the demanded cone.
//
// The carried edge is a read like any other, so a recurrence's body is inside the cone of
// a demand on the recurrence.
//
// Takes g (dag_t) which is the graph being walked.
// Takes count (int) which is how many nodes the graph holds.
// Takes at (int) which is the node to start from.
// Takes cone (char) which marks the nodes inside the demanded cone.
static void mark_cone(const dag_t *g, const int count, const int at, char *cone) {
    if (at < 0 || at >= count || cone[at]) {
        return;
    }
    cone[at] = 1;
    const dagnode_t *node = dag_at_const(g, at);
    for (int k = 0; k < node->input_count; k++) {
        mark_cone(g, count, node->in[k], cone);
    }
    if (node->carried >= 0) {
        mark_cone(g, count, node->carried, cone);
    }
}

// mark_inputs_escaping propagates one consumer's escape to everything it reads, and
// reports whether that changed anything.
//
// Outside the cone a consumer makes everything it reads escape; inside it a consumer
// passes on its own escape, unless it is the node stating the demand, where the path is
// cut, because a change that reaches an outsider only through the demanded value is a
// change the annotation licenses.
//
// Takes g (dag_t) which is the graph being walked.
// Takes at (int) which is the consumer whose reads are being marked.
// Takes root (int) which is the node stating the demand, where a path is cut.
// Takes cone (char) which marks the nodes inside the demanded cone.
// Takes escapes (char) which marks the nodes with a read-path out of the cone.
static int mark_inputs_escaping(const dag_t *g, const int at, const int root,
                                const char *cone, char *escapes) {
    const dagnode_t *consumer = dag_at_const(g, at);
    if (consumer->uses == 0) {
        return 0;
    }
    if (cone[at] && (at == root || !escapes[at])) {
        return 0;
    }
    int changed = 0;
    for (int k = 0; k < consumer->input_count; k++) {
        const int input = consumer->in[k];
        if (input >= 0 && !escapes[input]) {
            escapes[input] = 1;
            changed = 1;
        }
    }
    if (consumer->carried >= 0 && !escapes[consumer->carried]) {
        escapes[consumer->carried] = 1;
        changed = 1;
    }
    return changed;
}

// mark_escapes marks every node with a live read-path leaving the cone rooted at `root`.
//
// A producer is private to a demand when nothing outside that demand can observe a change
// to it, and the rule is a closure because one neutral binding can hide an escape: with
// `p := sum(x*0.5)` feeding only `a := p + 1.0` inside the cone, `p` would be pinned
// while `a` is also read outside it, so the bits an outsider observes move. The path is
// cut at the root, because without the cut every demand whose own result feeds anything
// else would mark its whole cone as escaping and heal nothing, and a change that reaches
// an outsider only through the demanded value is licensed by the annotation. The marking
// runs to a fixpoint rather than one sweep, because the carried edge may point forward,
// so escape can travel from a later index to an earlier one and back.
//
// Takes g (dag_t) which is the graph being walked.
// Takes count (int) which is how many nodes the graph holds.
// Takes root (int) which is the node stating the demand.
// Takes cone (char) which marks the nodes inside the demanded cone.
// Takes escapes (char) which marks the nodes with a read-path out of the cone.
static void mark_escapes(const dag_t *g, const int count, const int root, const char *cone,
                         char *escapes) {
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int i = count - 1; i >= 0; i--) {
            changed |= mark_inputs_escaping(g, i, root, cone, escapes);
        }
    }
}

// pin_cone walks the demanded cone rooted at `at` and pins every producer private to it
// to lane `want`.
//
// The walk descends unconditionally and tests each node on its own, because refusing to
// descend through a node with more than one operand edge would truncate the cone and
// leave a fold two levels further up unreached. A demand rides on an OP_DEMAND node of
// its own: the escaping reader reads that wrapper, which is neutral, so pinning it is a
// no-op, and the producer beneath it has the wrapper as its only consumer and never
// escapes.
//
// Takes g (dag_t) which is the graph being walked.
// Takes count (int) which is how many nodes the graph holds.
// Takes at (int) which is the node to start from.
// Takes want (lane_t) which is the lane being demanded.
// Takes escapes (char) which marks the nodes a change to which an outsider could observe.
// Takes seen (int) which marks the nodes already visited.
static void pin_cone(dag_t *g, const int count, const int at, const lane_t want, const char *escapes, int *seen) {
    if (at < 0 || at >= count || seen[at]) {
        return;
    }
    seen[at] = 1;
    dagnode_t *node = dag_at(g, at);
    if (node->declared != LANE_NONE && node->declared != want && node->stated_by_binding) {
        return;
    }

    if (!escapes[at]) {
        pin_node(node, want);
    }

    for (int k = 0; k < node->input_count; k++) {
        pin_cone(g, count, node->in[k], want, escapes, seen);
    }
    if (node->carried >= 0) {
        pin_cone(g, count, node->carried, want, escapes, seen);
    }
}

// snapshot_classes records the schedule class of the first `limit` nodes into `classes`,
// so a later pass can count what the healing changed.
//
// Takes g (dag_t) which is the graph being walked.
// Takes classes (int) which holds the class of each node.
// Takes limit (int) which bounds how many may be recorded.
static int snapshot_classes(dag_t *g, int *classes, const int limit) {
    const int recorded = dag_count(g) < limit ? dag_count(g) : limit;
    for (int i = 0; i < recorded; i++) {
        classes[i] = (int) dag_at(g, i)->op_class;
    }
    return recorded;
}

// count_changed counts how many of the `recorded` nodes now hold a schedule class other
// than the one `classes` remembered.
//
// Takes g (dag_t) which is the graph being walked.
// Takes classes (int) which holds the class of each node.
// Takes recorded (int) which is how many entries were written.
static int count_changed(dag_t *g, const int *classes, const int recorded) {
    int changed = 0;
    for (int i = 0; i < recorded; i++) {
        if (classes[i] != (int) dag_at(g, i)->op_class) {
            changed++;
        }
    }
    return changed;
}

// pin_one_demand runs the inference from the single demand stated at `at`.
//
// A value the program hands back has a reader the graph does not hold: every other
// consumer is a node the escape walk finds by following an edge, and the caller is not,
// so `escapes` is seeded at `result` before the walk. Left unseeded, a demand that
// reaches that value from the side would pin a producer whose bits the caller observes,
// and adding a binding nobody reads would change the number the program computed. The cut
// at the root still applies, so a demand on the value handed back licenses its own
// change.
//
// Takes g (dag_t) which is the graph being walked.
// Takes count (int) which is how many nodes the graph holds.
// Takes at (int) which is the node stating the demand.
// Takes result (int) which is the node the program returns.
// Takes cone (char) which marks the nodes inside the demanded cone.
// Takes escapes (char) which marks the nodes a change to which an outsider could observe.
// Takes seen (int) which marks the nodes already visited.
static void pin_one_demand(dag_t *g, const int count, const int at, const int result, char *cone,
                           char *escapes, int *seen) {
    memset(seen, 0, sizeof(int) * (size_t) count);
    memset(cone, 0, (size_t) count);
    memset(escapes, 0, (size_t) count);
    mark_cone(g, count, at, cone);
    if (result >= 0 && result < count) {
        escapes[result] = 1;
    }
    mark_escapes(g, count, at, cone, escapes);
    pin_cone(g, count, at, dag_at(g, at)->declared, escapes, seen);
}

// pin_every_demanded_cone runs the inference from every node that states a demand, over a
// graph of `count` nodes.
//
// Each demand gets its own cone mark and its own escape set, because privacy is relative
// to the demand being discharged: a producer may be private to one and shared out of
// another.
//
// Takes g (dag_t) which is the graph being walked.
// Takes count (int) which is how many nodes the graph holds.
// Takes result (int) which is the node the program returns.
// Takes seen (int) which marks the nodes already visited.
static void pin_every_demanded_cone(dag_t *g, const int count, const int result, int *seen) {
    char *cone = (char *) calloc((size_t) count, 1);
    char *escapes = (char *) calloc((size_t) count, 1);
    if (!cone || !escapes) {
        free(cone);
        free(escapes);
        return;
    }
    for (int i = 0; i < count; i++) {
        if (dag_at(g, i)->declared != LANE_NONE) {
            pin_one_demand(g, count, i, result, cone, escapes, seen);
        }
    }
    free(cone);
    free(escapes);
}

#endif  // LANELANG_NO_HEAL

// heal_program strengthens every private producer inside a demanded cone, and returns how
// many nodes changed class; under -DLANELANG_NO_HEAL, the healing control, it does
// nothing and reports nothing healed.
//
// The recount is done here rather than at the call sites, because mark_escapes reads
// `uses` to skip dead consumers and the counts must be current when the pass runs.
//
// Takes g (dag_t) which is the graph being walked.
// Takes result (int) which is the node the program returns.
int heal_program(dag_t *g, const int result) {
#ifdef LANELANG_NO_HEAL
    (void) g;
    (void) result;
    return 0;
#else
    const int count = dag_count(g);
    if (count <= 0) {
        return 0;
    }
    dag_recount_uses(g, result);
    int *seen = (int *) calloc((size_t) count, sizeof(int));
    if (!seen) {
        return 0;
    }

    int before[HEAL_SNAPSHOT_MAX];
    const int recorded = snapshot_classes(g, before, HEAL_SNAPSHOT_MAX);
    pin_every_demanded_cone(g, count, result, seen);
    const int changed = count_changed(g, before, recorded);

    free(seen);
    return changed;
#endif
}
