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

#include <stdlib.h>

// join_input_lanes joins a node's own schedule class with the computed lanes of its
// inputs.
//
// Takes node (dagnode_t) which is the node under examination.
// Takes index (int) which is the node's index in the graph.
// Takes computed (lane_t) which holds the lane computed for each node.
// Takes malformed_out (int) which receives the node the sweep refused at, or -1.
static lane_t join_input_lanes(
    const dagnode_t *node,
    const int index,
    const lane_t *computed,
    int *malformed_out
) {
#ifdef RL_LATTICE_ORDER_FAILOPEN
    (void) malformed_out;
#endif
    lane_t lane = opclass_lane(node->op_class);
    for (int k = 0; k < node->input_count; k++) {
        const int input = node->in[k];
#ifndef RL_LATTICE_ORDER_FAILOPEN
        if (input < 0 || input >= index) {
            *malformed_out = index;
            return LANE_NONE;
        }
#else
        if (input < 0 || input >= index) {
            continue;
        }
#endif
        lane = lane_join(lane, computed[input]);
    }
    return lane;
}

// any_input_tainted reports whether any operand of `node` is tainted.
//
// Takes node (dagnode_t) which is the node under examination.
// Takes index (int) which is the node's index in the graph.
// Takes tainted (char) which marks the nodes already blamed upstream.
static int any_input_tainted(const dagnode_t *node, const int index, const char *tainted) {
    for (int k = 0; k < node->input_count; k++) {
        const int input = node->in[k];
        if (input >= 0 && input < index && tainted[input]) {
            return 1;
        }
    }
    return 0;
}

// report_violation reports one violation: the demand that was not met, and the operation
// that lost the guarantee.
//
// Takes g (dag_t) which is the graph being walked.
// Takes computed (lane_t) which holds the lane computed for each node.
// Takes index (int) which is the node's index in the graph.
// Takes lane (lane_t) which is the lane to report.
// Takes d (diagnostic_t) which is where diagnostics are collected.
static void report_violation(
    const dag_t *g,
    const lane_t *computed,
    const int index,
    const lane_t lane,
    diagnostic_t *d
) {
    int blame[BLAME_PATH_MAX];
    const int blame_length = lattice_blame(g, computed, index, lane, blame, BLAME_PATH_MAX);
    const int introducer = blame_length > 0 ? blame[0] : index;
    const int hops = blame_length > 0 ? blame_length - 1 : 0;
    const int truncated = blame_length < 0;

    const dagnode_t *node = dag_at_const(g, index);
    const dagnode_t *source = dag_at_const(g, introducer);
    const span_t primary = node->declared_span.line ? node->declared_span : node->span;
    const span_t secondary = source->span;

    if (truncated) {
        diagnostic_error(d, primary, "determinism lane violation: this value computes %s where %s "
                   "is demanded, by a path longer than the %d nodes this checker walks",
                   lane_name(lane), lane_name(node->declared), BLAME_PATH_MAX);
        return;
    }
    diagnostic_error_with_cause(d, primary, secondary, hops,
                "determinism lane violation: this value computes %s where %s is demanded. "
                "An unpinned schedule cannot be recovered downstream: the rounding already "
                "happened. Pin the producer by naming it with a replay or exact qualifier, or weaken the demand",
                lane_name(lane), lane_name(node->declared));
    if (source->label_length > 0) {
        diagnostic_note(d, secondary, "'%.*s' is a %s, so it contributes %s to the join",
                  source->label_length, source->label, opclass_name(source->op_class),
                  lane_name(opclass_lane(source->op_class)));
    }
}

// leaf_is_grounded reports whether a leaf's value is a function of the program rather
// than of the world.
//
// A literal and a generated column are fixed by the program, and a parameter is the
// caller's obligation, so it is grounded by assumption from inside the callee. OP_SOURCE
// reaches out for a value nobody fixed, so it is the only leaf that may not carry a
// demand without a waiver; a call declared `ungrounded` is not a leaf and is decided in
// ground_of_node.
//
// Takes node (dagnode_t) which is the node under examination.
static int leaf_is_grounded(const dagnode_t *node) {
    return node->op != OP_SOURCE;
}

// GROUND_FIXED marks a node the program itself fixes: both bits, because anything the
// program fixes is also something a declaration may rest on.
#define GROUND_FIXED 3

// GROUND_BLESSED marks a node a declaration may rest on although the program does not fix
// it: the waived bit alone. A blessed source settles here and never at GROUND_FIXED.
#ifdef LANELANG_BLESS_FLIPS_GROUNDED
#define GROUND_BLESSED 3
#else
#define GROUND_BLESSED 2
#endif

// opaque_seed is what a node contributes to its own grounding before its inputs are
// conjoined: GROUND_FIXED for ordinary work, and for a call declared `ungrounded`, the
// waiver it carries and nothing else.
//
// An opaque call loses the fact and keeps the conjunction. Its author declared the value
// not a function of the program, so bit 0 goes whatever the body says, and dropping the
// fact can only refuse more programs. The conjunction still runs afterwards, so a
// `@bless` at the call site carries bit 1 only where everything the body reads already
// carries it, and a `source()` inside an unblessed body still refuses the demand above.
// The `#ifdef` spans whole statements rather than splitting one condition across a
// directive.
//
// Takes node (dagnode_t) which is the node under examination.
// Returns the grounding bits the node states on its own behalf.
static char opaque_seed(const dagnode_t *node) {
#ifdef LANELANG_OPAQUE_IS_GROUNDED
    (void) node;
    return GROUND_FIXED;
#else
    if (node->op != OP_OPAQUE) {
        return GROUND_FIXED;
    }
    return node->bless != 0 ? (char) GROUND_BLESSED : 0;
#endif
}

// ground_of_node returns node `index`'s grounding given what the sweep believes of its
// inputs.
//
// A leaf states its own grounding and everything else conjoins its inputs' bits, so a
// cone loses the fact as soon as one input lacks it, while a waiver survives only where
// every input carries one. The carried edge is a read like any other, because a
// recurrence whose body reaches a source is not grounded, and omitting that edge would
// hide it.
//
// Takes g (dag_t) which is the graph being walked.
// Takes count (int) which is how many nodes the graph holds.
// Takes grounded (char) which is the sweep's current belief about each node.
// Takes index (int) which is the node being recomputed.
// Returns the node's grounding bits.
static char ground_of_node(const dag_t *g, const int count, const char *grounded,
                           const int index) {
    const dagnode_t *node = dag_at_const(g, index);
    if (node->input_count == 0 && node->carried < 0) {
        const int fixed = leaf_is_grounded(node) ? GROUND_FIXED : 0;
        const int blessed = node->bless != 0 ? GROUND_BLESSED : 0;
        return (char) (fixed | blessed);
    }
    char all = opaque_seed(node);
    for (int k = 0; k < node->input_count; k++) {
        const int in = node->in[k];
        if (in >= 0 && in < count) {
            all = (char) (all & grounded[in]);
        }
    }
    if (node->carried >= 0 && node->carried < count) {
        all = (char) (all & grounded[node->carried]);
    }
    return all;
}

// ground_pass fills `grounded` for every node, two bits each.
//
// Bit 0 is the fact, the program fixes this value; bit 1 is the policy, a declaration
// over this value may be accepted. Both propagate as a conjunction over every read and
// differ only in what a leaf contributes; GROUND_FIXED has bit 1 set, so fixed implies
// acceptable by arithmetic, and the value 1 is unreachable. A bless must not make a value
// grounded: it makes a declaration acceptable while the value goes on reporting that the
// program does not fix it. The seed is the top, because the carried edge may point
// forward, so a recurrence reads a node the sweep has not visited, and seeding at
// not-grounded would make every recurrence ungrounded by accident of visit order.
// Settling is reported rather than assumed, because falling out of the loop at the bound
// is otherwise indistinguishable from settling.
//
// Takes g (dag_t) which is the graph being walked.
// Takes count (int) which is how many nodes the graph holds.
// Takes grounded (char) which marks the nodes whose value the program fixes.
// Returns 1 when it settled, 0 when the passes ran out with a bit still moving.
static int ground_pass(const dag_t *g, const int count, char *grounded) {
    for (int i = 0; i < count; i++) {
        grounded[i] = GROUND_FIXED;
    }
    for (int pass = 0; pass <= count; pass++) {
        int changed = 0;
        for (int i = 0; i < count; i++) {
            const char all = ground_of_node(g, count, grounded, i);
            if (all != grounded[i]) {
                grounded[i] = all;
                changed = 1;
            }
        }
        if (!changed) {
            return 1;
        }
    }
    return 0;
}

// report_over_declared_opaque notes every `ungrounded` call whose body the checker can
// see is grounded.
//
// Over-declaring is legal, because a library author may reserve the right to reach the
// world later without breaking callers, and forcing the answer false only ever refuses
// more programs; the note changes no verdict and names the reservation where it is
// written. The test against GROUND_FIXED is an equality and not a mask, because a body
// holding a blessed source carries the waiver bit alone and only a body the program
// itself fixes is an over-declaration.
//
// Takes g (dag_t) which is the graph being walked.
// Takes count (int) which is how many nodes the graph holds.
// Takes grounded (char) which is the sweep's settled answer for each node.
// Takes d (diagnostic_t) which is where diagnostics are collected.
static void report_over_declared_opaque(const dag_t *g, const int count,
                                        const char *grounded, diagnostic_t *d) {
    for (int i = 0; i < count; i++) {
        const dagnode_t *node = dag_at_const(g, i);
        if (node->op != OP_OPAQUE || node->input_count < 1) {
            continue;
        }
        const int body = node->in[0];
        if (body < 0 || body >= count || grounded[body] != GROUND_FIXED) {
            continue;
        }
        diagnostic_note(d, node->span,
                  "'%.*s' is declared 'ungrounded' and its body is a function of the program, so "
                  "the declaration is a reservation rather than a fact the checker needs. It still "
                  "holds: the value is treated as the world's whatever the body says",
                  node->label_length, node->label);
    }
}

// first_ungrounded_leaf_seen is the first ungrounded leaf reachable from `from`, or
// `from` itself when no leaf beneath it is ungrounded, or -1 when `from` is grounded,
// already visited, or out of range.
//
// It walks operands depth-first and stops at the first ungrounded leaf, which is where
// the value entered the program and the only place a reader can act. `seen` is a cycle
// guard rather than an optimisation, because the carried edge may point forward, so a
// recurrence whose body names the binding it defines would recurse until the stack ran
// out.
//
// Takes g (dag_t) which is the graph being walked.
// Takes count (int) which is how many nodes the graph holds.
// Takes grounded (char) which marks the nodes whose value the program fixes.
// Takes from (int) which is where the run begins.
// Takes seen (char) which marks the nodes already visited.
static int first_ungrounded_leaf_seen(
    const dag_t *g, const int count, const char *grounded, const int from, char *seen
) {
    if (from < 0 || from >= count || grounded[from] || seen[from]) {
        return -1;
    }
    seen[from] = 1;
    const dagnode_t *node = dag_at_const(g, from);
    if (node->input_count == 0 && node->carried < 0) {
        return from;
    }
    for (int k = 0; k < node->input_count; k++) {
        const int found = first_ungrounded_leaf_seen(g, count, grounded, node->in[k], seen);
        if (found >= 0) {
            return found;
        }
    }
    if (node->carried >= 0) {
        const int found = first_ungrounded_leaf_seen(g, count, grounded, node->carried, seen);
        if (found >= 0) {
            return found;
        }
    }
    return from;
}

// first_ungrounded_leaf is the node a refused declaration's diagnostic names: the first
// ungrounded leaf its cone reaches, or the start node itself when no leaf beneath it is
// ungrounded. -1 means the start node is grounded. On allocation failure it degrades to
// naming the start node.
//
// Takes g (dag_t) which is the graph being walked.
// Takes count (int) which is how many nodes the graph holds.
// Takes grounded (char) which marks the nodes whose value the program fixes.
// Takes from (int) which is the node to start from.
static int first_ungrounded_leaf(const dag_t *g, const int count, const char *grounded, const int from) {
    char *seen = (char *) calloc((size_t) (count > 0 ? count : 1), 1);
    if (!seen) {
        return from;
    }
    const int found = first_ungrounded_leaf_seen(g, count, grounded, from, seen);
    free(seen);
    return found;
}

// report_ungrounded reports a declaration refused because its cone reaches a leaf the
// program does not fix.
//
// It is deliberately not the two-span lane-violation shape, because no guarantee was ever
// available to lose, and a lost-guarantee reading invites the repair that does not work,
// which is pinning a producer; a source is ungrounded whatever qualifier it carries. The
// advice names the waiver that will be accepted: a source loads a column, so a checksum
// has something to be verified against, while a call declared `ungrounded` loads none and
// only the bare form is legal on it.
//
// Takes g (dag_t) which is the graph being walked.
// Takes count (int) which is how many nodes the graph holds.
// Takes grounded (char) which marks the nodes whose value the program fixes.
// Takes index (int) which is the node's index in the graph.
// Takes d (diagnostic_t) which is where diagnostics are collected.
static void report_ungrounded(
    const dag_t *g,
    const int count,
    const char *grounded,
    const int index,
    diagnostic_t *d
) {
    const dagnode_t *node = dag_at_const(g, index);
    const int leaf = first_ungrounded_leaf(g, count, grounded, index);
    const span_t primary = node->declared_span.line ? node->declared_span : node->span;
    const dagnode_t *source = dag_at_const(g, leaf >= 0 ? leaf : index);

    const char *const waiver = source->op == OP_OPAQUE ? "@bless" : "@bless(...)";
    diagnostic_error(d, primary,
                "%s demands a grounded cone, and this value is not grounded. The bits do not move "
                "with the schedule here, they move with the world: vouch for it with "
                "%s on the binding that names it, take the value as a parameter, which "
                "makes it the caller's obligation, or weaken the demand to %s",
                lane_name(node->declared), waiver, lane_name(LANE_APPROX));
    if (source->label_length > 0) {
        diagnostic_note(d, source->span,
                  source->op == OP_OPAQUE
                      ? "'%.*s' is declared 'ungrounded', so its value is not a function of the "
                        "program however its body is written"
                      : "'%.*s' is a leaf this program does not fix, so its value is not a "
                        "function of the program",
                  source->label_length, source->label);
    }
}

// dag_has_carried_edge reports whether the graph holds a carried edge, which is the only
// thing that can make one pass insufficient.
//
// Takes g (dag_t) which is the graph being walked.
// Takes count (int) which is how many nodes the graph holds.
static int dag_has_carried_edge(const dag_t *g, const int count) {
    for (int i = 0; i < count; i++) {
        if (dag_at_const(g, i)->carried >= 0) {
            return 1;
        }
    }
    return 0;
}

// carried_depth_at is the greatest number of carried edges on any one path through the
// graph from `at`: the recurrence nesting depth.
//
// Depth is what bounds the sweep. Within one pass every operand is already settled,
// because operands are strictly earlier; the carried edge is the only one that may point
// forward, so it is read stale and information travels backwards exactly one carried edge
// per pass. A recurrence nested d deep therefore needs d passes before its outermost lane
// reads anything true, so height bounds how far a lane moves and depth bounds how long
// the news takes to arrive. `seen` carries the depth already computed for a node, so the
// walk is linear over a graph that shares producers, and doubles as the cycle guard the
// carried edge makes necessary.
//
// Takes g (dag_t) which is the graph being walked.
// Takes count (int) which is how many nodes the graph holds.
// Takes at (int) which is the node to start from.
// Takes seen (int) which marks the nodes already visited.
static int carried_depth_at(const dag_t *g, const int count, const int at, int *seen) {
    if (at < 0 || at >= count) {
        return 0;
    }
    if (seen[at] >= 0) {
        return seen[at];
    }
    seen[at] = 0;

    const dagnode_t *node = dag_at_const(g, at);
    int deepest = 0;
    for (int k = 0; k < node->input_count; k++) {
        const int below = carried_depth_at(g, count, node->in[k], seen);
        if (below > deepest) {
            deepest = below;
        }
    }
    if (node->carried >= 0) {
        const int below = carried_depth_at(g, count, node->carried, seen);
        if (below + 1 > deepest) {
            deepest = below + 1;
        }
    }
    seen[at] = deepest;
    return deepest;
}

// dag_carried_depth is the recurrence nesting depth of the whole graph, or 0 when it
// holds no carried edge.
//
// Without the scratch array the depth cannot be computed, and a bound guessed from
// nothing is worse than a conservative one, so it falls back to `count`, which no acyclic
// graph can exceed.
//
// Takes g (dag_t) which is the graph being walked.
// Takes count (int) which is how many nodes the graph holds.
static int dag_carried_depth(const dag_t *g, const int count) {
    if (count <= 0) {
        return 0;
    }
    int *seen = (int *) malloc(sizeof(int) * (size_t) count);
    if (!seen) {
        return count;
    }
    for (int i = 0; i < count; i++) {
        seen[i] = -1;
    }

    int deepest = 0;
    for (int i = 0; i < count; i++) {
        const int here = carried_depth_at(g, count, i, seen);
        if (here > deepest) {
            deepest = here;
        }
    }
    free(seen);
    return deepest;
}

// lane_pass computes every node's lane by one forward pass, returning non-zero when a
// lane moved.
//
// The carried edge is joined here and nowhere else. It is read from `computed` rather
// than from `in`, so join_input_lanes never sees an index it would have to refuse, and
// the sweep's strictly-earlier precondition stays literally true.
//
// Takes g (dag_t) which is the graph being walked.
// Takes count (int) which is how many nodes the graph holds.
// Takes computed (lane_t) which holds the lane computed for each node.
// Takes malformed_out (int) which receives the node the sweep refused at, or -1.
static int lane_pass(const dag_t *g, const int count, lane_t *computed, int *malformed_out) {
    int changed = 0;
    for (int i = 0; i < count; i++) {
        const dagnode_t *node = dag_at_const(g, i);

        lane_t lane = join_input_lanes(node, i, computed, malformed_out);
        if (*malformed_out >= 0) {
            return 0;
        }

        if (node->carried >= 0) {
            if (node->carried >= count) {
                *malformed_out = i;
                return 0;
            }
            lane = lane_join(lane, computed[node->carried]);
        }

        if (lane != computed[i]) {
            computed[i] = lane;
            changed = 1;
        }
    }
    return changed;
}

// declaration_is_ungrounded reports whether a declaration demands more than its cone can
// ground.
//
// Grounding is checked before the lane, and separately from it. A demand for `exact` or
// `replay` is a claim that rerunning this artefact reproduces these bits, and a cone that
// reaches an external source cannot honour it however well pinned its folds are; `approx`
// demands nothing of the schedule and so demands nothing here either. The substitute
// below replaces the whole predicate rather than an `#ifdef` inside one condition.
//
// Takes node (dagnode_t) which is the node under examination.
// Takes grounded (char) which marks the nodes whose value the program fixes.
// Takes index (int) which is the node's index in the graph.
#ifdef LANELANG_NO_GROUNDING
// declaration_is_ungrounded always answers no, which drops the grounding judgement from
// this build.
//
// Takes node (dagnode_t) which is the node under examination.
// Takes grounded (char) which marks the nodes whose value the program fixes.
// Takes index (int) which is the node's index in the graph.
static int declaration_is_ungrounded(const dagnode_t *node, const char *grounded, const int index) {
    (void) node; (void) grounded; (void) index;
    return 0;
}
#else
// declaration_is_ungrounded is the shipped judgement.
//
// Takes node (dagnode_t) which is the node under examination.
// Takes grounded (char) which marks the nodes whose value the program fixes.
// Takes index (int) which is the node's index in the graph.
static int declaration_is_ungrounded(const dagnode_t *node, const char *grounded, const int index) {
    return grounded && !grounded[index] && node->declared != LANE_APPROX;
}
#endif

// declaration_check_t is everything the per-node check reads, gathered into one context.
typedef struct {
    // g is the graph being checked.
    const dag_t *g;

    // count is how many nodes the graph holds.
    int count;

    // computed is the lane the sweep computed for each node.
    const lane_t *computed;

    // grounded is the grounding sweep's result for each node.
    const char *grounded;

    // d is where diagnostics are collected.
    diagnostic_t *d;

    // tainted marks the nodes already implicated, so a consequence is not reported as a
    // cause. NULL when the allocation failed, which suppresses cascade collapsing and
    // nothing else.
    char *tainted;

    // outcome receives the counts and the first blameable leaf.
    check_result_t *outcome;
} declaration_check_t;

// check_one_declaration checks node `index` against the demand declared on it, recording
// the verdict and reporting it unless it is a consequence of one already reported.
//
// A node with no declaration is still visited, because the cascade is a property of the
// graph rather than of the declarations, so a value between two declarations carries the
// taint of the one that failed. Grounding is asked first and separately: a declaration
// over a value the program does not fix is refused whatever lane the sweep computed,
// because that lane is a statement about a value the checker cannot see the making of.
//
// Takes check (declaration_check_t) which is the context the whole sweep shares.
// Takes index (int) which is the node being checked.
static void check_one_declaration(const declaration_check_t *check, const int index) {
    const dagnode_t *node = dag_at_const(check->g, index);

    const int cascade = check->tainted && any_input_tainted(node, index, check->tainted);
    if (cascade) {
        check->tainted[index] = 1;
    }

    if (node->declared == LANE_NONE) {
        return;
    }
    check->outcome->checked++;

    if (declaration_is_ungrounded(node, check->grounded, index)) {
        if (check->tainted) {
            check->tainted[index] = 1;
        }
        check->outcome->ungrounded++;
        if (check->outcome->ungrounded_leaf < 0) {
            check->outcome->ungrounded_leaf =
                first_ungrounded_leaf(check->g, check->count, check->grounded, index);
        }
        if (!cascade) {
            report_ungrounded(check->g, check->count, check->grounded, index, check->d);
        }
        return;
    }

    if (lane_satisfies(check->computed[index], node->declared)) {
        return;
    }

    if (check->tainted) {
        check->tainted[index] = 1;
    }
    check->outcome->violations++;
    if (cascade) {
        return;
    }

    report_violation(check->g, check->computed, index, check->computed[index], check->d);
}

// check_declarations checks every declaration over settled lanes, reporting each
// violation not already explained upstream.
//
// It is separate from the lane computation because a recurrence runs that computation
// more than once, and a diagnostic must be emitted once however many passes it took to
// reach it.
//
// Takes g (dag_t) which is the graph being walked.
// Takes count (int) which is how many nodes the graph holds.
// Takes computed (lane_t) which holds the lane computed for each node.
// Takes grounded (char) which marks the nodes whose value the program fixes.
// Takes d (diagnostic_t) which is where diagnostics are collected.
// Takes outcome (check_result_t) which receives what the check found.
static void check_declarations(
    const dag_t *g,
    const int count,
    const lane_t *computed,
    const char *grounded,
    diagnostic_t *d,
    check_result_t *outcome
) {
    const declaration_check_t check = {
        .g = g,
        .count = count,
        .computed = computed,
        .grounded = grounded,
        .d = d,
        .tainted = (char *) calloc((size_t) (count > 0 ? count : 1), 1),
        .outcome = outcome,
    };

    for (int i = 0; i < count; i++) {
        check_one_declaration(&check, i);
    }

    free(check.tainted);
}

// report_malformed_sweep renders the refusal for a sweep that would not run, blaming the
// node it stopped at.
//
// It is deliberately not the lane-violation shape, because the failure is a property of
// the graph rather than of a path through it, so there is no cause span to walk to.
//
// Takes g (dag_t) which is the graph being walked.
// Takes count (int) which is how many nodes the graph holds.
// Takes outcome (check_result_t) which receives what the check found.
// Takes d (diagnostic_t) which is where diagnostics are collected.
static void report_malformed_sweep(
    const dag_t *g, const int count, const check_result_t *outcome, diagnostic_t *d
) {
    span_t at = SPAN_NONE;
    if (outcome->malformed >= 0 && outcome->malformed < count) {
        at = dag_at_const(g, outcome->malformed)->span;
    }
    if (outcome->malformed_reason == MALFORMED_OUT_OF_MEMORY) {
        diagnostic_error(d, at,
                   "out of memory while preparing the grounding judgement over %d node(s); "
                   "no verdict is reported, because the check could not run",
                   count);
    } else if (outcome->malformed_reason == MALFORMED_GROUNDING_NO_FIXED_POINT) {
        diagnostic_error(d, at,
                   "the grounding sweep did not settle in %d pass(es) over %d node(s); no "
                   "verdict is reported, because the judgement did not finish",
                   count + 1, count);
    } else if (outcome->malformed_reason == MALFORMED_NO_FIXED_POINT) {
        diagnostic_error(d, at,
                   "the lane sweep did not reach a fixed point in %d pass(es) over %d node(s); "
                   "no lane is reported, because none settled",
                   outcome->passes, count);
    } else if (outcome->malformed_reason == MALFORMED_FORWARD_INPUT) {
        diagnostic_error(d, at,
                   "an operand of this node is defined after it, so the lane sweep's "
                   "dependency order does not hold and no lane can be computed");
    } else {
        diagnostic_error(d, at,
                   "this graph was refused for a reason the reporter does not name, which "
                   "is a defect in the reporter rather than in the graph");
    }
}

// run_lane_sweep drives the join sweep over `g` until it settles, recording the pass
// count and any refusal in `outcome`.
//
// A graph with no carried edge settles in one pass by construction, because operands are
// emitted before their consumers. One with a carried edge is iterated to a fixed point,
// and exhaustion is recorded as a malformed graph rather than as a verdict.
//
// Takes g (dag_t) which is the graph being walked.
// Takes count (int) which is how many nodes the graph has.
// Takes computed (lane_t) which holds the lane computed for each node.
// Takes outcome (check_result_t) which receives the pass count and any refusal.
static void run_lane_sweep(dag_t *g, const int count, lane_t *computed,
                           check_result_t *outcome) {
    if (!dag_has_carried_edge(g, count)) {
        lane_pass(g, count, computed, &outcome->malformed);
        outcome->passes = 1;
        return;
    }
    outcome->carried_depth = dag_carried_depth(g, count);
    const int ceiling = count * (LANE_CHAIN_HEIGHT - 1) + 1;
    int settled = 0;
    while (outcome->passes < ceiling) {
        const int changed = lane_pass(g, count, computed, &outcome->malformed);
        outcome->passes++;
        if (outcome->malformed >= 0) {
            break;
        }
        if (!changed) {
            settled = 1;
            break;
        }
    }
    if (outcome->malformed < 0 && !settled) {
        outcome->malformed = 0;
        outcome->malformed_reason = MALFORMED_NO_FIXED_POINT;
    }
}

// lattice_compute_lanes runs the join sweep alone, for a build with the check disabled.
//
// Takes g (dag_t) which is the graph being walked.
// Takes count (int) which is how many nodes the graph holds.
// Takes computed (lane_t) which receives each node's lane.
void lattice_compute_lanes(dag_t *g, const int count, lane_t *computed) {
    check_result_t outcome = check_result_none();
    run_lane_sweep(g, count, computed, &outcome);
}

// count_ungrounded_nodes returns how many of `count` nodes the grounding sweep left
// ungrounded.
//
// It reads bit 0, the fact, and deliberately not the whole byte, because a blessed node
// settles at GROUND_BLESSED, which is non-zero, so counting truthiness would report a
// blessed cone as grounded.
//
// Takes grounded (char) which is the grounding sweep's per-node result.
// Takes count (int) which is how many nodes the graph has.
// Returns how many nodes are ungrounded in fact.
static int count_ungrounded_nodes(const char *grounded, const int count) {
    int ungrounded = 0;
    for (int i = 0; i < count; i++) {
        ungrounded += !(grounded[i] & 1);
    }
    return ungrounded;
}

// lattice_check computes every node's lane and checks every declaration.
//
// The sweep seeds at the join identity, because a carried edge is read before the node it
// points at has been visited, and starting at the identity makes the iteration climb
// towards the least fixed point. The ceiling is the loosest provable bound, count *
// (LANE_CHAIN_HEIGHT - 1) + 1 passes, deliberately looser than the tight bound the
// fixture check asserts, so a regression in the measurement stays a loud failure there
// rather than a wrong answer here. Exhaustion is recorded as a malformed graph and
// reported, because a refusal that renders as an acceptance is the failure mode this file
// exists to prevent. The grounding sweep runs to its own fixed point for the same reason,
// and failing to allocate its scratch buffer refuses rather than continuing, because
// passing NULL through to check_declarations would skip the grounding judgement for every
// declaration and then report the result as though it had run.
//
// Takes g (dag_t) which is the graph being walked.
// Takes result (int) which is the node the program returns.
// Takes computed (lane_t) which holds the lane computed for each node.
// Takes d (diagnostic_t) which is where diagnostics are collected.
check_result_t lattice_check(dag_t *g, const int result, lane_t *computed, diagnostic_t *d) {
    check_result_t outcome = check_result_none();

    const int count = dag_count(g);

    for (int i = 0; i < count; i++) {
        computed[i] = LANE_EXACT;
    }
    run_lane_sweep(g, count, computed, &outcome);

    if (outcome.malformed >= 0) {
        if (outcome.malformed_reason == MALFORMED_NONE) {
            outcome.malformed_reason = MALFORMED_FORWARD_INPUT;
        }
        report_malformed_sweep(g, count, &outcome, d);
        return outcome;
    }

    char *grounded = (char *) calloc((size_t) (count > 0 ? count : 1), 1);
    if (!grounded) {
        outcome.malformed = 0;
        outcome.malformed_reason = MALFORMED_OUT_OF_MEMORY;
        report_malformed_sweep(g, count, &outcome, d);
        return outcome;
    }
    if (!ground_pass(g, count, grounded)) {
        free(grounded);
        outcome.malformed = 0;
        outcome.malformed_reason = MALFORMED_GROUNDING_NO_FIXED_POINT;
        report_malformed_sweep(g, count, &outcome, d);
        return outcome;
    }
    outcome.ungrounded_nodes = count_ungrounded_nodes(grounded, count);
    report_over_declared_opaque(g, count, grounded, d);

    check_declarations(g, count, computed, grounded, d, &outcome);

    free(grounded);

    if (result >= 0 && result < count) {
        outcome.result_lane = computed[result];
    }
    return outcome;
}
