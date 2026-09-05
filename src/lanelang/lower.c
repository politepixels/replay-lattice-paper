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
#include "lower.h"

#include <stdio.h>

// UNNAMED_FOLD_LABEL is what an unnamed fold is printed as: a fold with no binding still
// needs a name in the transcript a check reads back, and `sum` is what the source would
// have called it.
#define UNNAMED_FOLD_LABEL     "sum"

// lower_kernel_for is the name of the kernel a fold or scan node lowers to, or NULL for a
// node that is neither.
//
// The discharge is chosen by the operation, never by the demand: this function reads
// `op_class` and nothing else, not `declared`, not the lane the sweep computed, not the
// ambient. That is what makes a lane a function of the bits rather than the other way
// round, and it is the invariant lower_kernel_demand_free_cases asserts.
//
// Takes node (dagnode_t) which is the node under examination.
const char *lower_kernel_for(const dagnode_t *node) {
    if (node->op == OP_SCAN) {
        return node->op_class == OPC_SCAN_REPLAY ? "scan_replay" : "scan_free";
    }
    if (node->op != OP_FOLD) {
        return NULL;
    }
#ifdef LANELANG_KERNEL_FOLLOWS_DEMAND
    if (node->declared == LANE_EXACT) {
        return "fold_integer";
    }
    if (node->declared == LANE_REPLAY) {
        return "fold_replay";
    }
#endif
    switch (node->op_class) {
    case OPC_FOLD_REPLAY:  return "fold_replay";
    case OPC_FOLD_EXACT:   return "fold_strict";
    case OPC_FOLD_INTEGER: return "fold_integer";
    default:               return "fold_free";
    }
}

// kernel_is_demand_free sweeps every (demand x ambient) pair over one probe and reports
// how many agreed with `expected`, or 0 as soon as one did not.
//
// Takes probe (dagnode_t) which is the node whose class is under test; its lane fields
// are overwritten.
// Takes lanes (lane_t) which is the set of lanes to sweep, including LANE_NONE.
// Takes lane_count (size_t) which is how many entries `lanes` has.
// Takes expected (char) which is the kernel the class alone selects.
// Returns how many pairs agreed, or 0 when one disagreed.
static int kernel_is_demand_free(dagnode_t *probe, const lane_t *lanes, const size_t lane_count,
                                 const char *expected) {
    int cases = 0;
    for (size_t d = 0; d < lane_count; d++) {
        for (size_t a = 0; a < lane_count; a++) {
            probe->declared = lanes[d];
            probe->lane = lanes[a];
            const char *got = lower_kernel_for(probe);
            if (!got || strcmp(got, expected) != 0) {
                return 0;
            }
            cases++;
        }
    }
    return cases;
}

// lower_kernel_demand_free_cases checks that the kernel a fold lowers to does not depend
// on the demand written at it, and returns the number of (class, demand, ambient) triples
// checked, or 0 if the invariant is broken.
//
// It holds `op_class` fixed, sweeps `declared` and `lane` over every value the chain
// admits, and requires the selected kernel to be byte-identical throughout, so a build
// that lets either field reach the selection returns 0. The failure it catches is
// invisible to a verdict, because a demand-driven discharge accepts exactly the same
// programs and emits different bits. The op has to match the class, or a scan class would
// be asked through the fold arm and the sweep would compare a kernel neither family
// emits.
int lower_kernel_demand_free_cases(void) {
    static const opclass_t classes[] = {
        OPC_NEUTRAL, OPC_FOLD_EXACT, OPC_FOLD_INTEGER, OPC_FOLD_REPLAY, OPC_FOLD_APPROX,
        OPC_SCAN_REPLAY, OPC_SCAN_APPROX
    };
    static const lane_t lanes[] = { LANE_NONE, LANE_EXACT, LANE_REPLAY, LANE_APPROX };
    int cases = 0;
    for (size_t c = 0; c < sizeof classes / sizeof classes[0]; c++) {
        dagnode_t probe = {0};
        probe.op = (classes[c] == OPC_SCAN_REPLAY || classes[c] == OPC_SCAN_APPROX)
                 ? OP_SCAN : OP_FOLD;
        probe.op_class = classes[c];
        probe.declared = LANE_NONE;
        probe.lane = LANE_NONE;
        const char *expected = lower_kernel_for(&probe);
        if (!expected) {
            return 0;
        }
        const int checked = kernel_is_demand_free(&probe, lanes,
                                                  sizeof lanes / sizeof lanes[0], expected);
        if (checked == 0) {
            return 0;
        }
        cases += checked;
    }
    return cases;
}

// UNNAMED_FOLD_LABEL_LEN is UNNAMED_FOLD_LABEL's length, which the %.*s conversion needs.
#define UNNAMED_FOLD_LABEL_LEN 3

// lower_print prints the kernel mapping to `stream`, one line per kernel-bearing node, in
// node order.
//
// Takes g (dag_t) which is the graph being walked.
// Takes stream (void) which is where the output is written.
int lower_print(const dag_t *g, void *stream) {
    FILE *file = (FILE *) stream;
    const int count = dag_count(g);
    int fold_count = 0;
    for (int i = 0; i < count; i++) {
        const dagnode_t *node = dag_at_const(g, i);
        const char *kernel = lower_kernel_for(node);
        if (!kernel) {
            continue;
        }
        fold_count++;
        fprintf(file, "kernel n%d %.*s %s\n",
                i,
                node->label_length > 0 ? node->label_length : UNNAMED_FOLD_LABEL_LEN,
                node->label_length > 0 ? node->label : UNNAMED_FOLD_LABEL,
                kernel);
    }
    return fold_count;
}
