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
#include "heal.h"
#include "parse.h"
#include "resolve.h"
#include "status.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COLUMN 256

#define WORLD_BLESSED 1u
#define WORLD_OTHER   2u

#define SOURCE_BYTES 256

static const char PROGRAM[] =
    "package app\n"
    "\n"
    "func main() Float {\n"
    "    %s x := source(count: 256.0)\n"
    "    replay energy := sum(x)\n"
    "    return energy\n"
    "}\n";

static long cases = 0;

static long failures = 0;

typedef struct {
    char source[SOURCE_BYTES];

    diagnostic_t *diagnostics;

    program_t *program;

    resolved_t resolved;
} built_program_t;

static void check(const int ok, const char *what) {
    cases++;
    if (!ok) {
        failures++;
        fprintf(stderr, "  FAIL %s\n", what);
    }
}

static void free_program(built_program_t *built) {
    dag_free(built->resolved.dag);
    built->resolved.dag = NULL;
    ast_free(built->program);
    built->program = NULL;
    diagnostic_free(built->diagnostics);
    built->diagnostics = NULL;
}

static int build_blessed(const char *blessing, built_program_t *built) {
    memset(built, 0, sizeof *built);
    snprintf(built->source, sizeof built->source, PROGRAM, blessing);

    built->diagnostics = diagnostic_new("<bless>", built->source);
    built->program = parse_program("<bless>", built->source, built->diagnostics);
    if (!built->program) {
        free_program(built);
        return 0;
    }
    built->resolved = resolve_program(built->program, "main", built->diagnostics);
    if (built->resolved.status != RESOLVE_OK || !built->resolved.dag) {
        free_program(built);
        return 0;
    }
    (void) heal_program(built->resolved.dag, built->resolved.result);

    const int count = dag_count(built->resolved.dag);
    lane_t *computed = (lane_t *) calloc((size_t) (count > 0 ? count : 1), sizeof(lane_t));
    if (!computed) {
        free_program(built);
        return 0;
    }
    const check_result_t outcome = lattice_check(built->resolved.dag, built->resolved.result,
                                                 computed, built->diagnostics);
    free(computed);
    if (check_rejected(&outcome) || check_malformed(&outcome)) {
        free_program(built);
        return 0;
    }
    return 1;
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

static int evaluates(const built_program_t *built, const unsigned world) {
    double value = 0.0;
    return eval_dag_in_world(built->resolved.dag, built->resolved.result, EVAL_MATERIALISE, 0, 1,
                             world, &value);
}

int main(void) {
    rl_build_stamp("test_bless");

    unsigned blessed = 0;
    unsigned other = 0;
    if (!checksum_of_world(WORLD_BLESSED, &blessed) || !checksum_of_world(WORLD_OTHER, &other)) {
        return RL_FAIL("L5", 0, "bless-harness-out-of-memory");
    }

    char checksum[32];
    snprintf(checksum, sizeof checksum, "@bless(0x%08x)", blessed);
    built_program_t promise;
    built_program_t receipt;
    if (!build_blessed("@bless", &promise)) {
        return RL_FAIL("L5", 0, "bless-program-did-not-build");
    }
    if (!build_blessed(checksum, &receipt)) {
        free_program(&promise);
        return RL_FAIL("L5", 0, "bless-program-did-not-build");
    }

    check(blessed != other, "the two worlds hand back different data");

    check(evaluates(&promise, WORLD_BLESSED), "an unconditional bless evaluates in its own world");
    check(evaluates(&promise, WORLD_OTHER), "an unconditional bless evaluates in another world");

    check(evaluates(&receipt, WORLD_BLESSED), "a matching blessed checksum evaluates");

    check(!evaluates(&receipt, WORLD_OTHER), "a blessed checksum refuses another world's data");

    free_program(&promise);
    free_program(&receipt);
    printf("\n%ld assertions, %ld failed\n", cases, failures);
    return failures
           ? RL_FAIL("L5", cases, "blessed-checksum-not-enforced")
           : RL_PASS("L5", cases);
}
