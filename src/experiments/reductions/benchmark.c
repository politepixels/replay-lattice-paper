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

// _POSIX_C_SOURCE exposes the monotonic clock used for elapsed time.
#define _POSIX_C_SOURCE 200809L

#include "eval.h"
#include "generator.h"
#include "heal.h"
#include "kernels.h"
#include "parse.h"
#include "resolve.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// ELEMENTS is the number of doubles in the generated column.
#define ELEMENTS 100000

// BATCHES is the odd number of timed batches used for each median.
#define BATCHES 25

// benchmark_t keeps the source alive while its graph and diagnostics refer to it.
typedef struct {
    // name is the qualifier written on the sum.
    const char *name;

    // lane selects the corresponding standalone kernel.
    lane_t lane;

    // source holds the program for this lane.
    char source[192];

    // diagnostics records any errors while preparing the program.
    diagnostic_t *diagnostics;

    // program owns the parsed syntax tree.
    program_t *program;

    // resolved holds the graph evaluated by the full evaluator.
    resolved_t resolved;

    // expected is the result both entry points must reproduce in each batch.
    double expected;

    // kernel_us holds the mean kernel time per call in each batch.
    double kernel_us[BATCHES];

    // evaluator_us holds the mean evaluation time per call in each batch.
    double evaluator_us[BATCHES];
} benchmark_t;

// prepare resolves, heals and checks the same source expression for each lane.
//
// Takes bench (benchmark_t) which receives the parsed and checked program.
// Returns one when the program was accepted.
static int prepare(benchmark_t *bench) {
    snprintf(bench->source, sizeof bench->source,
             "package app\nfunc main() Float {\nx := indices(count: %d.0)\n"
             "%s total := sum(x)\nreturn total\n}\n", ELEMENTS, bench->name);
    bench->diagnostics = diagnostic_new("<benchmark>", bench->source);
    if (!bench->diagnostics) {
        return 0;
    }
    bench->program = parse_program("<benchmark>", bench->source, bench->diagnostics);
    if (!bench->program) {
        return 0;
    }
    bench->resolved = resolve_program(bench->program, "main", bench->diagnostics);
    if (bench->resolved.status != RESOLVE_OK || !bench->resolved.dag) {
        return 0;
    }
    heal_program(bench->resolved.dag, bench->resolved.result);
    lane_t *lanes = calloc((size_t) dag_count(bench->resolved.dag), sizeof *lanes);
    if (!lanes) {
        return 0;
    }
    const check_result_t result =
        lattice_check(bench->resolved.dag, bench->resolved.result, lanes, bench->diagnostics);
    free(lanes);
    return !check_rejected(&result) && !check_malformed(&result);
}

// evaluate calls either the single-worker kernel or the full materialising evaluator.
//
// Takes bench (benchmark_t) which supplies the graph and the lane.
// Takes reader (eval_state_t) which supplies the pre-generated column to the kernel.
// Takes full (int) which selects the full evaluator when non-zero.
// Takes out (double) which receives the sum.
// Returns one when the requested calculation completed.
static int evaluate(const benchmark_t *bench, eval_state_t *reader, const int full, double *out) {
    if (full) {
        return eval_dag(bench->resolved.dag, bench->resolved.result, EVAL_MATERIALISE, 64, 1, out);
    }
    switch (bench->lane) {
    case LANE_EXACT:  return fold_exact(reader, 0, ELEMENTS, out);
    case LANE_REPLAY: return fold_replay(reader, 0, ELEMENTS, 1, out);
    default:          *out = fold_free(reader, 0, ELEMENTS, 1); return 1;
    }
}

// measure times repeated calls after a warm-up and checks the result by its bits.
//
// Takes bench (benchmark_t) which holds the expected result and receives the timing.
// Takes reader (eval_state_t) which supplies the pre-generated column to the kernel.
// Takes full (int) which selects the full evaluator when non-zero.
// Takes batch (size_t) which identifies the timing sample.
// Returns one when the clock and both the calculation and its result check succeeded.
static int measure(benchmark_t *bench, eval_state_t *reader, const int full, const size_t batch) {
    struct timespec start;
    struct timespec stop;
    const int repeats = full ? 12 : 128;
    double value;
    if (!evaluate(bench, reader, full, &value) || memcmp(&value, &bench->expected, sizeof value)
        || clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        return 0;
    }
    for (int i = 0; i < repeats; i++) {
        if (!evaluate(bench, reader, full, &value)) {
            return 0;
        }
    }
    if (clock_gettime(CLOCK_MONOTONIC, &stop) != 0 || memcmp(&value, &bench->expected, sizeof value)) {
        return 0;
    }
    const double elapsed_us = (double) (stop.tv_sec - start.tv_sec) * 1e6
                              + (double) (stop.tv_nsec - start.tv_nsec) * 1e-3;
    double *samples = full ? bench->evaluator_us : bench->kernel_us;
    samples[batch] = elapsed_us / repeats;
    return 1;
}

// compare_times orders two finite durations for the median.
//
// Takes left (void) which points to the first duration.
// Takes right (void) which points to the second duration.
// Returns their comparison in ascending order.
static int compare_times(const void *left, const void *right) {
    const double a = *(const double *) left;
    const double b = *(const double *) right;
    return (a > b) - (a < b);
}

// measure_lanes alternates the two measurements and rotates the order of the lanes.
//
// Takes cases (benchmark_t) which receives each batch of timings.
// Takes count (size_t) which is the number of lanes.
// Takes reader (eval_state_t) which supplies the pre-generated column to the kernels.
// Returns one when every batch completes with the expected bits.
static int measure_lanes(benchmark_t *cases, const size_t count, eval_state_t *reader) {
    for (size_t batch = 0; batch < BATCHES; batch++) {
        const int first = (int) (batch % 2);
        for (size_t i = 0; i < count; i++) {
            const size_t lane = (batch + (batch % 2 ? i : count - i)) % count;
            if (!measure(&cases[lane], reader, first, batch)
                || !measure(&cases[lane], reader, 1 - first, batch)) {
                return 0;
            }
        }
    }
    return 1;
}

// main times all three lanes in rotating order on the same generated input.
// Returns zero once both measurements have passed for every lane.
int main(void) {
    benchmark_t cases[] = {
        {.name = "approx", .lane = LANE_APPROX},
        {.name = "replay", .lane = LANE_REPLAY},
        {.name = "exact", .lane = LANE_EXACT},
    };
    const size_t count = sizeof cases / sizeof *cases;
    size_t length = ELEMENTS;
    double *column = malloc(length * sizeof *column);
    if (!column) {
        return EXIT_FAILURE;
    }
    generator_fill(1, column, length);
    eval_state_t reader = {.lengths = &length, .columns = &column};
    int ok = 1;
    for (size_t i = 0; i < count && ok; i++) {
        ok = prepare(&cases[i]) && evaluate(&cases[i], &reader, 0, &cases[i].expected);
    }
    ok = ok && measure_lanes(cases, count, &reader);
    if (ok) {
        printf("%d doubles, one worker, median of %d batches\n", ELEMENTS, BATCHES);
        puts("| Lane | Reduction (µs) | Full evaluator (µs) |");
        puts("|---|---:|---:|");
        for (size_t i = 0; i < count; i++) {
            qsort(cases[i].kernel_us, BATCHES, sizeof(double), compare_times);
            qsort(cases[i].evaluator_us, BATCHES, sizeof(double), compare_times);
            printf("| %s | %.2f | %.2f |\n", cases[i].name, cases[i].kernel_us[BATCHES / 2],
                   cases[i].evaluator_us[BATCHES / 2]);
        }
    } else {
        fputs("Benchmark build, evaluation, result comparison or timer failed.\n", stderr);
    }
    for (size_t i = 0; i < count; i++) {
        dag_free(cases[i].resolved.dag);
        ast_free(cases[i].program);
        diagnostic_free(cases[i].diagnostics);
    }
    free(column);
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
