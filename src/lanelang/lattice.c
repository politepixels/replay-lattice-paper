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

// report_violation reports the unmet demand and the operation that lost the guarantee.
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
    const int count = dag_count(g);
    int *blame = malloc((size_t) count * sizeof *blame);
    const int blame_length = blame ? lattice_blame(g, computed, index, lane, blame, count) : -1;
    const int introducer = blame_length > 0 ? blame[0] : index;
    const int hops = blame_length > 0 ? blame_length - 1 : 0;
    free(blame);

    const dagnode_t *node = dag_at_const(g, index);
    const dagnode_t *source = dag_at_const(g, introducer);
    const span_t primary = node->declared_span.line ? node->declared_span : node->span;
    const span_t secondary = source->span;

    if (blame_length < 0) {
        diagnostic_error(d, primary, "determinism lane violation: this value computes %s where %s "
                   "is demanded (out of memory tracing its origin)",
                   lane_name(lane), lane_name(node->declared));
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
    if (node->argument_span.line) {
        diagnostic_note(d, node->argument_span, "the argument is passed to the demanding "
                  "parameter here");
    }
}

// leaf_is_grounded reports whether a leaf's value is a function of the program rather
// than of the world.
//
// Literals and generated columns are fixed by the program. A callee assumes its
// parameters are grounded because their values are the caller's obligation. The entry has
// no caller and cannot declare a parameter. An external source needs a waiver to support
// an `exact` or `replay` demand. Opaque calls have an input and are handled by
// ground_of_node.
//
// Takes node (dagnode_t) which is the node under examination.
static int leaf_is_grounded(const dagnode_t *node) {
    return node->op != OP_SOURCE;
}

// GROUND_FIXED sets both bits for a value the program fixes. A declaration may rely on
// that value.
#define GROUND_FIXED 3

// GROUND_BLESSED sets the waiver bit alone. A declaration may rely on a blessed source
// even though the program does not fix its value.
#ifdef LANELANG_BLESS_FLIPS_GROUNDED
#define GROUND_BLESSED 3
#else
#define GROUND_BLESSED 2
#endif

// opaque_seed supplies the grounding bits to conjoin with a node's inputs. Ordinary
// operations start with GROUND_FIXED. An opaque call contributes only its own waiver, if
// present.
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
// A leaf states its own grounding. Other nodes conjoin the bits of their inputs,
// including the carried edge of a recurrence. For a demand to rely on the result, every
// input must be fixed or covered by a waiver. An opaque call also needs its own waiver.
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
// Bit 0 records whether the program fixes the value. Bit 1 records whether a declaration
// may rely on it. A fixed value sets both bits. A blessed source or an opaque call can
// carry just bit 1. The sweep conjoins the bits across reads, starting from both bits set
// so a forward carried edge can retain grounding until a dependency takes it away. A
// final pass with no change confirms the result has settled.
//
// Takes g (dag_t) which is the graph being walked.
// Takes count (int) which is how many nodes the graph holds.
// Takes grounded (char) which records which values are fixed and which a demand may use.
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
// A library author may declare a grounded body `ungrounded` to reserve the right to read
// external data later. The checker honours the declaration and adds a note at the call.
// Testing for GROUND_FIXED excludes bodies that rely on a waiver.
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

// first_ungrounded_leaf_seen finds the origin of an ungrounded value. It searches the
// operands before the carried edge and skips nodes already visited, so a cycle leaves
// other branches open. An unwaived opaque call is itself the origin if its body yields
// none.
//
// Takes g (dag_t) which is the graph being walked.
// Takes count (int) which is how many nodes the graph holds.
// Takes grounded (char) which records which values are fixed and which a demand may use.
// Takes from (int) which is where the run begins.
// Takes seen (char) which marks the nodes already visited.
// Returns the origin's node index, or -1 if this branch supplies none.
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
    return node->op == OP_OPAQUE && !node->bless ? from : -1;
}

// first_ungrounded_leaf finds the origin named by a grounding refusal. On allocation
// failure it falls back to the node where the search would start.
//
// Takes g (dag_t) which is the graph being walked.
// Takes count (int) which is how many nodes the graph holds.
// Takes grounded (char) which records which values are fixed and which a demand may use.
// Takes from (int) which is the node to start from.
static int first_ungrounded_leaf(const dag_t *g, const int count, const char *grounded, const int from) {
    char *seen = (char *) calloc(count > 0 ? (size_t) count : 1, 1);
    if (!seen) {
        return from;
    }
    const int found = first_ungrounded_leaf_seen(g, count, grounded, from, seen);
    free(seen);
    return found;
}

// report_ungrounded reports a declaration whose cone reaches an unwaived source or opaque
// call.
//
// Pinning a producer cannot fix its inputs. The diagnostic points to the unfixed source
// or opaque call and names the waiver that applies there. A source loads bytes that a
// checksum can verify. An opaque call accepts the bare waiver.
//
// Takes g (dag_t) which is the graph being walked.
// Takes count (int) which is how many nodes the graph holds.
// Takes grounded (char) which records which values are fixed and which a demand may use.
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
                "%s demands a cone the program fixes or a blessing vouches for, and this value "
                "is neither. The bits do not move with the schedule here, they move with the "
                "world: vouch for it with %s on the binding that names it, take the value as a "
                "parameter, which makes it the caller's obligation, or weaken the demand to %s",
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
    if (node->argument_span.line) {
        diagnostic_note(d, node->argument_span, "the argument is passed to the demanding "
                  "parameter here");
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

// seed_layer settles the carried targets the previous layer reached and stacks the ones
// not already settled, returning how many it stacked.
//
// Takes next (int) which holds the carried targets the previous layer reached.
// Takes pending (int) which is how many it holds.
// Takes layer (int) which is the count of carried edges crossed to reach them.
// Takes distance (int) which holds each node's count, -1 for a node not reached.
// Takes stack (int) which receives the nodes newly settled.
static int seed_layer(const int *next, const int pending, const int layer, int *distance,
                      int *stack) {
    int top = 0;
    for (int k = 0; k < pending; k++) {
        if (distance[next[k]] < 0) {
            distance[next[k]] = layer;
            stack[top++] = next[k];
        }
    }
    return top;
}

// close_layer settles everything the stacked nodes read through operands alone, at the
// same count, and collects the carried targets they read for the next layer.
//
// Takes g (dag_t) which is the graph being walked.
// Takes count (int) which is how many nodes the graph holds.
// Takes layer (int) which is the count every node settled here receives.
// Takes distance (int) which holds each node's count, -1 for a node not reached.
// Takes stack (int) which holds the nodes to expand, `stacked` of them.
// Takes stacked (int) which is how many the stack holds.
// Takes next (int) which receives the carried targets reached.
// Returns how many carried targets it collected.
static int close_layer(const dag_t *g, const int count, const int layer, int *distance,
                       int *stack, const int stacked, int *next) {
    int top = stacked;
    int pending = 0;
    while (top > 0) {
        const dagnode_t *node = dag_at_const(g, stack[--top]);
        for (int k = 0; k < node->input_count; k++) {
            const int in = node->in[k];
            if (in >= 0 && in < count && distance[in] < 0) {
                distance[in] = layer;
                stack[top++] = in;
            }
        }
        const int carried = node->carried;
        if (carried >= 0 && carried < count && distance[carried] < 0 && pending < count) {
            next[pending++] = carried;
        }
    }
    return pending;
}

// carried_distances_from records, for every node `from` reads, the fewest carried edges
// on any path from `from` back to it, and returns the greatest of those counts.
//
// Takes g (dag_t) which is the graph being walked.
// Takes count (int) which is how many nodes the graph holds.
// Takes from (int) which is the node the search starts at.
// Takes distance (int) which receives each node's count, -1 for a node not reached.
// Takes stack (int) which is scratch space for `count` nodes.
// Takes next (int) which is scratch space for `count` nodes.
// Returns the greatest count recorded.
static int carried_distances_from(const dag_t *g, const int count, const int from,
                                  int *distance, int *stack, int *next) {
    for (int i = 0; i < count; i++) {
        distance[i] = -1;
    }
    int deepest = 0;
    int pending = 1;
    next[0] = from;
    for (int layer = 0; pending > 0; layer++) {
        const int top = seed_layer(next, pending, layer, distance, stack);
        if (top > 0) {
            deepest = layer;
        }
        pending = close_layer(g, count, layer, distance, stack, top, next);
    }
    return deepest;
}

// dag_carried_depth is the most carried edges a change must cross to reach any node it
// reaches, counting the fewest along any path, or 0 when the graph holds no carried edge.
//
// Without the scratch arrays the count cannot be computed, and a guessed bound is worse
// than a conservative one, so it falls back to `count`, which no shortest path can
// exceed.
//
// Takes g (dag_t) which is the graph being walked.
// Takes count (int) which is how many nodes the graph holds.
static int dag_carried_depth(const dag_t *g, const int count) {
    if (count <= 0) {
        return 0;
    }
    int *distance = (int *) malloc(sizeof(int) * (size_t) count);
    int *stack = (int *) malloc(sizeof(int) * (size_t) count);
    int *next = (int *) malloc(sizeof(int) * (size_t) count);
    if (!distance || !stack || !next) {
        free(distance);
        free(stack);
        free(next);
        return count;
    }

    int deepest = 0;
    for (int i = 0; i < count; i++) {
        const int here = carried_distances_from(g, count, i, distance, stack, next);
        if (here > deepest) {
            deepest = here;
        }
    }
    free(distance);
    free(stack);
    free(next);
    return deepest;
}

// lane_pass computes every node's lane by one forward pass, returning non-zero when a
// lane moved.
//
// A carried edge may point forward, so its lane comes from the latest value in
// `computed`. Ordinary operands must precede their consumer.
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
// Grounding is checked before the lane. An `exact` or `replay` declaration requires its
// external inputs to be vouched for even when the fold order is pinned. An `approx`
// declaration needs no grounding guarantee.
//
// Takes node (dagnode_t) which is the node under examination.
// Takes grounded (char) which records which values are fixed and which a demand may use.
// Takes index (int) which is the node's index in the graph.
#ifdef LANELANG_NO_GROUNDING
// declaration_is_ungrounded always answers no, which drops the grounding judgement from
// this build.
//
// Takes node (dagnode_t) which is the node under examination.
// Takes grounded (char) which records which values are fixed and which a demand may use.
// Takes index (int) which is the node's index in the graph.
static int declaration_is_ungrounded(const dagnode_t *node, const char *grounded, const int index) {
    (void) node; (void) grounded; (void) index;
    return 0;
}
#else
// declaration_is_ungrounded is the shipped judgement.
//
// Takes node (dagnode_t) which is the node under examination.
// Takes grounded (char) which records which values are fixed and which a demand may use.
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
// A node with no declaration can still carry an upstream failure to an ordinary consumer.
// The grounding check runs first. An `exact` or `replay` declaration is refused when its
// cone lacks both grounding and an effective waiver, whatever lane the sweep computed.
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
// Takes grounded (char) which records which values are fixed and which a demand may use.
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
        .tainted = (char *) calloc(count > 0 ? (size_t) count : 1, 1),
        .outcome = outcome,
    };

    for (int i = 0; i < count; i++) {
        check_one_declaration(&check, i);
    }

    free(check.tainted);
}

// report_malformed_sweep reports why checking could not finish. Invalid edges use the
// consumer's span. Other failures use the first node's span when one exists.
//
// An incomplete sweep has no lane verdict or blame path.
//
// Takes g (dag_t) which is the graph being walked.
// Takes count (int) which is how many nodes the graph holds.
// Takes outcome (check_result_t) which identifies the failure to report.
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
// It reads bit 0, the fact, and not the whole byte, because a blessed node settles at
// GROUND_BLESSED, which is non-zero, so counting truthiness would report a blessed cone
// as grounded.
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
// towards the least fixed point. A graph with `count` nodes permits at most count *
// (LANE_CHAIN_HEIGHT - 1) lane increases. One additional pass confirms convergence. The
// fixture check tests the tighter carried-depth bound. Failure to settle, or to allocate
// the grounding workspace, stops the check before it can issue a verdict.
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

    char *grounded = (char *) calloc(count > 0 ? (size_t) count : 1, 1);
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
