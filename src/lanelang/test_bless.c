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
#include "eval.h"
#include "generator.h"
#include "status.h"

#include <stdio.h>
#include <stdlib.h>

#define COLUMN 256

#define WORLD_BLESSED 1u
#define WORLD_OTHER   2u

static long cases = 0;

static long failures = 0;

static void check(const int ok, const char *what) {
    cases++;
    if (!ok) {
        failures++;
        fprintf(stderr, "  FAIL %s\n", what);
    }
}

static dag_t *build_source_fold(int *source_out) {
    dag_t *g = dag_new();
    if (!g) {
        return NULL;
    }
    dagnode_t source;
    dag_node_init(&source);
    source.op = OP_SOURCE;
    source.op_class = OPC_NEUTRAL;
    source.constant = (double) COLUMN;
    const int source_index = dag_append(g, &source);

    dagnode_t fold;
    dag_node_init(&fold);
    fold.op = OP_FOLD;
    fold.op_class = OPC_FOLD_REPLAY;
    fold.in[0] = source_index;
    fold.input_count = 1;
    const int fold_index = dag_append(g, &fold);

    if (source_index < 0 || fold_index < 0) {
        dag_free(g);
        return NULL;
    }
    *source_out = source_index;
    return g;
}

static int checksum_of_world(const unsigned world, unsigned *out) {
    double *column = (double *) malloc(sizeof *column * COLUMN);
    if (!column) {
        return 0;
    }
    generator_fill(world, column, COLUMN);
    *out = eval_bless_checksum(column, COLUMN);
    free(column);
    return 1;
}

static int evaluates(const dag_t *g, const int result, const unsigned world) {
    double value = 0.0;
    return eval_dag_in_world(g, result, EVAL_MATERIALISE, 0, 1, world, &value);
}

int main(void) {
    rl_build_stamp("test_bless");

    int source_index = -1;
    dag_t *g = build_source_fold(&source_index);
    if (!g) {
        return RL_FAIL("L5", 0, "bless-harness-out-of-memory");
    }
    const int result = dag_count(g) - 1;

    unsigned blessed = 0;
    unsigned other = 0;
    if (!checksum_of_world(WORLD_BLESSED, &blessed) || !checksum_of_world(WORLD_OTHER, &other)) {
        dag_free(g);
        return RL_FAIL("L5", 0, "bless-harness-out-of-memory");
    }

    check(blessed != other, "the two worlds hand back different data");

    dag_at(g, source_index)->bless = BLESS_PROMISE;
    check(evaluates(g, result, WORLD_BLESSED), "an unconditional bless evaluates in its own world");
    check(evaluates(g, result, WORLD_OTHER), "an unconditional bless evaluates in another world");

    dag_at(g, source_index)->bless = (long long) blessed + 1;
    check(evaluates(g, result, WORLD_BLESSED), "a matching blessed checksum evaluates");

    check(!evaluates(g, result, WORLD_OTHER), "a blessed checksum refuses another world's data");

    dag_free(g);
    printf("\n%ld assertions, %ld failed\n", cases, failures);
    return failures
           ? RL_FAIL("L5", cases, "blessed-checksum-not-enforced")
           : RL_PASS("L5", cases);
}
