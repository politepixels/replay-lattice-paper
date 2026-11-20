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

#include "heal.h"

#include "dag.h"

#include <stdlib.h>
#include <string.h>

// The healing control builds `lanelang_noheal` with -DLANELANG_NO_HEAL. Its build omits
// these helpers with their only caller so that -Wall -Wextra -Werror stays clean.
#ifndef LANELANG_NO_HEAL

// class_for selects the class that the operation family can offer for lane `want`.
//
// Folds can meet an `exact` demand with the serial kernel. The strongest scan kernel in
// this build is the fixed-block scan, whose lane is `replay`. Offering that class keeps
// the computed lane faithful to the kernel and leaves lattice_check to report the unmet
// `exact` demand.
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

// pin_node strengthens a producer's schedule class towards lane `want` when its operation
// family permits it.
//
// Neutral operations keep their class. For other operations the lane stored in the
// interning key comes from the selected kernel. For example, an `exact` demand on a scan
// can strengthen it only to `replay`. The caller checks privacy and binding qualifiers
// before reaching this point.
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

// mark_inputs_escaping propagates one consumer's escape to everything it reads, and
// reports whether that changed anything.
//
// A live consumer outside the cone makes its inputs escape. Inside the cone an escaping
// consumer passes that mark to its inputs. The demand itself cuts the path because the
// annotation licenses changes observed through the demanded value.
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
// A producer is private if an outside reader can observe it only through the demand.
// Suppose `p := sum(x*0.5)` feeds `a := p + 1.0` inside the cone and an outsider also
// reads `a`. Pinning `p` could change the value seen by the outsider. The escape
// therefore propagates through `a` to `p`. The demand itself cuts such paths. The marks
// run to a fixed point because a carried edge can refer to a later node.
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

// pin_cone walks the demanded cone rooted at `at` and strengthens eligible private
// producers towards lane `want`.
//
// A binding that explicitly states a different lane stops this walk. Otherwise it visits
// each operand and the carried edge, even when the current node escapes. Escape marks
// decide whether a producer itself may be pinned. A demand wrapper is neutral, so pinning
// the wrapper leaves its class unchanged.
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

// pin_one_demand runs the inference from the single demand stated at `at`.
//
// The caller is a reader of the result with no edge in the graph. Seeding `escapes` at
// `result` accounts for that reader. Without it, adding an unused demand could pin a
// producer whose bits the caller observes directly. The demand at the root still cuts
// escape propagation through its own value.
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
    dag_mark_cone(g, at, cone);
    if (result >= 0 && result < count) {
        escapes[result] = 1;
    }
    mark_escapes(g, count, at, cone, escapes);
    pin_cone(g, count, at, dag_at(g, at)->declared, escapes, seen);
}

// pin_every_demanded_cone runs the inference from every node that states a demand, over a
// graph of `count` nodes.
//
// Each demand gets its own cone mark and escape set. The same producer can be private to
// one demand while another demand shares it with an outside reader.
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

// heal_program strengthens eligible private producers in demanded cones and returns the
// number of nodes whose class changed. The healing control built with -DLANELANG_NO_HEAL
// returns zero.
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
    opclass_t *before = malloc((size_t) count * sizeof *before);
    if (!seen || !before) {
        free(before);
        free(seen);
        return 0;
    }
    for (int i = 0; i < count; i++) {
        before[i] = dag_at_const(g, i)->op_class;
    }
    pin_every_demanded_cone(g, count, result, seen);
    int changed = 0;
    for (int i = 0; i < count; i++) {
        changed += before[i] != dag_at_const(g, i)->op_class;
    }
    free(before);
    free(seen);
    return changed;
#endif
}
