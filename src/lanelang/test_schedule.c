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

#include "eval.h"
#include "heal.h"
#include "parse.h"
#include "resolve.h"
#include "status.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NW ((int) (sizeof WORKERS / sizeof WORKERS[0]))

#define LEG "L4"

#define SOURCE_BYTES 512

#define TILE_PROBE_WIDTH 64

#define FOOTPRINT_EVALUATIONS 3

#define FOOTPRINT_ASSERTIONS 2

#define PINNED_COUNT 2

static const int WORKERS[] = { 1, 2, 3, 8 };

static const char *const PROGRAMS[] = {
    "package app\n"
    "func main() Float {\n"
    "    x := indices(count: 5000.0)\n"
    "    %stotal := sum(x)\n"
    "    return total\n"
    "}\n",

    "package app\n"
    "func main() Float {\n"
    "    x := indices(count: 5000.0)\n"
    "    %stotal := sum(x * 0.5)\n"
    "    return total\n"
    "}\n",

    "package app\n"
    "func main() Float {\n"
    "    x := indices(count: 4096.0)\n"
    "    y := x * 2.0\n"
    "    z := y - x\n"
    "    %stotal := sum(z * z)\n"
    "    return total\n"
    "}\n",

    "package app\n"
    "func main() Float {\n"
    "    x := indices(count: 999.0)\n"
    "    %stotal := sum((x + x) * (x - 0.25))\n"
    "    return total\n"
    "}\n",

    "package app\n"
    "func main() Float {\n"
    "    x := indices(count: 1500.0)\n"
    "    %stotal := sum(scan(x))\n"
    "    return total\n"
    "}\n",

    "package app\n"
    "func main() Float {\n"
    "    x := indices(count: 700.0)\n"
    "    p := scan(x * 0.5)\n"
    "    %stotal := sum(p - x)\n"
    "    return total\n"
    "}\n",

};

#define NP ((int) (sizeof PROGRAMS / sizeof PROGRAMS[0]))

static const char *const PINNED[] = { "replay ", "exact " };

#define PINNED_REFUSALS_EXPECTED 2

typedef struct {
    const char *name;

    eval_mode_t mode;

    size_t tile;
} traversal_t;

typedef struct {
    char source[SOURCE_BYTES];

    diagnostic_t *diagnostics;

    program_t *program;

    resolved_t resolved;
} built_program_t;

typedef struct {
    size_t materialised_columns;

    size_t materialised_tiles;

    size_t fused_columns;

    size_t fused_tiles;

    size_t tiled_columns;

    size_t tiled_tiles;

    int evaluated;
} footprint_t;

typedef struct {
    long cases;

    long failed;

    const char *why;

    int discriminating;

    int refused;
} run_state_t;

static const traversal_t TRAV[] = {
    { "materialised", EVAL_MATERIALISE, 0 },
    { "fused",        EVAL_FUSED,       0 },
    { "tiled(7)",     EVAL_TILED,       7 },
    { "tiled(64)",    EVAL_TILED,      64 },
    { "tiled(1000)",  EVAL_TILED,    1000 },
};

#define NT ((int) (sizeof TRAV / sizeof TRAV[0]))

static void note_failure(run_state_t *state, const char *why) {
    if (state->failed <= 1) {
        state->why = why;
    }
}

static uint64_t bits_of(double value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

static void free_program(built_program_t *built) {
    dag_free(built->resolved.dag);
    built->resolved.dag = NULL;
    ast_free(built->program);
    built->program = NULL;
    diagnostic_free(built->diagnostics);
    built->diagnostics = NULL;
}

typedef enum {
    BUILD_ACCEPTED,

    BUILD_REFUSED,

    BUILD_BROKEN
} build_outcome_t;

static build_outcome_t build_program(
    const char *template_text,
    const char *annotation,
    built_program_t *built
) {
    memset(built, 0, sizeof *built);
    snprintf(built->source, sizeof built->source, template_text, annotation);

    built->diagnostics = diagnostic_new("<schedule>", built->source);
    built->program = parse_program("<schedule>", built->source, built->diagnostics);
    if (!built->program) {
        free_program(built);
        return BUILD_BROKEN;
    }

    built->resolved = resolve_program(built->program, "main", built->diagnostics);
    if (built->resolved.status != RESOLVE_OK || !built->resolved.dag) {
        free_program(built);
        return BUILD_BROKEN;
    }

    (void) heal_program(built->resolved.dag, built->resolved.result);

    const int count = dag_count(built->resolved.dag);
    lane_t *computed = (lane_t *) calloc((size_t) (count > 0 ? count : 1), sizeof(lane_t));
    if (!computed) {
        free_program(built);
        return BUILD_BROKEN;
    }
    const check_result_t outcome =
        lattice_check(built->resolved.dag, built->resolved.result, computed, built->diagnostics);
    free(computed);
    if (check_rejected(&outcome)) {
        free_program(built);
        return BUILD_REFUSED;
    }
    if (check_malformed(&outcome)) {
        free_program(built);
        return BUILD_BROKEN;
    }
    return BUILD_ACCEPTED;
}

static int has_intermediate(const char *template_text) {
    built_program_t built;
    if (build_program(template_text, "", &built) != BUILD_ACCEPTED) {
        return 0;
    }

    int elementwise = 0;
    const int count = dag_count(built.resolved.dag);
    for (int i = 0; i < count; i++) {
        if (dag_at(built.resolved.dag, i)->op == OP_ELEM) {
            elementwise++;
        }
    }
    free_program(&built);
    return elementwise > 0;
}

typedef struct {
    eval_mode_t mode;

    size_t tile;

    int workers;
} schedule_point_t;

static int eval_one(
    const char *template_text,
    const char *annotation,
    const schedule_point_t schedule,
    double *out,
    size_t *columns,
    size_t *tiles
) {
    built_program_t built;
    if (build_program(template_text, annotation, &built) != BUILD_ACCEPTED) {
        return 0;
    }

    const int ok = eval_dag(built.resolved.dag, built.resolved.result, schedule.mode,
                            schedule.tile, schedule.workers, out);
    if (ok) {
        eval_last_footprint(columns, tiles);
    }
    free_program(&built);
    return ok;
}

static int remember_pattern(uint64_t *patterns, const int seen_count, const int capacity, const uint64_t bits) {
    for (int i = 0; i < seen_count; i++) {
        if (patterns[i] == bits) {
            return seen_count;
        }
    }
    if (seen_count >= capacity) {
        return seen_count;
    }
    patterns[seen_count] = bits;
    return seen_count + 1;
}

#define GRID_REFUSED (-1)

#define GRID_BUILD_BROKEN (-2)

#define GRID_EVAL_FAILED (-3)

static int evaluate_grid(const char *template_text, const char *annotation, uint64_t *cells) {
    built_program_t built;
    const build_outcome_t outcome = build_program(template_text, annotation, &built);
    if (outcome == BUILD_REFUSED) {
        return GRID_REFUSED;
    }
    if (outcome == BUILD_BROKEN) {
        return GRID_BUILD_BROKEN;
    }
    int evaluated = 0;
    for (int i = 0; i < NT; i++) {
        for (int j = 0; j < NW; j++) {
            double value = 0.0;
            if (!eval_dag(built.resolved.dag, built.resolved.result, TRAV[i].mode, TRAV[i].tile,
                          WORKERS[j], &value)) {
                free_program(&built);
                return GRID_EVAL_FAILED;
            }
            cells[i * NW + j] = bits_of(value);
            evaluated++;
        }
    }
    free_program(&built);
    return evaluated;
}

static int distinct_in(const uint64_t *cells, const int count, const int stride) {
    uint64_t seen[NT * NW];
    int seen_count = 0;
    for (int i = 0; i < count; i++) {
        seen_count = remember_pattern(seen, seen_count, NT * NW, cells[i * stride]);
    }
    return seen_count;
}

static int distinct_patterns(
    const char *template_text,
    const char *annotation,
    uint64_t *patterns,
    const int capacity,
    int *evaluations
) {
    uint64_t cells[NT * NW];
    const int evaluated = evaluate_grid(template_text, annotation, cells);
    *evaluations = evaluated < 0 ? 0 : evaluated;
    if (evaluated < 0) {
        return evaluated;
    }
    int seen_count = 0;
    for (int i = 0; i < evaluated; i++) {
        seen_count = remember_pattern(patterns, seen_count, capacity, cells[i]);
    }
    return seen_count;
}

static int check_unpinned(const char *template_text, const int index, run_state_t *state,
                         uint64_t *cells) {
    const int evaluated = evaluate_grid(template_text, "", cells);
    const int distinct = evaluated < 0 ? -1 : distinct_in(cells, NT * NW, 1);
    state->cases += (evaluated < 0 ? 0 : evaluated) + 1;

    if (distinct < 0) {
        printf("FAIL    unpinned program %d did not build or evaluate\n", index);
        state->failed++;
        note_failure(state, "schedule-eval-failed");
        return distinct;
    }
    if (distinct > 1) {
        state->discriminating++;
        printf("  ok    unpinned: %d distinct pattern(s) over %d traversal x worker cells\n",
               distinct, NT * NW);
    } else {
        printf("  --    unpinned: 1 pattern; the bracketing does not bite on this shape\n");
    }
    return distinct;
}

static void check_traversal_axis(const uint64_t *cells, run_state_t *state) {
    for (int i = 0; i < NW; i++) {
        const int distinct = distinct_in(cells + i, NT, NW);
        state->cases++;
        if (distinct != 1) {
            printf("FAIL    unpinned at workers=%d: %d pattern(s) across %d traversals; a "
                   "traversal changed the bits\n", WORKERS[i], distinct, NT);
            state->failed++;
            note_failure(state, "traversal-changed-bits");
        }
    }
}

static footprint_t measure_footprints(const char *template_text) {
    footprint_t out = {0};
    double value = 0.0;

    out.evaluated = eval_one(template_text, "", (schedule_point_t) {EVAL_MATERIALISE, 0, 1}, &value,
                             &out.materialised_columns, &out.materialised_tiles)
                  + eval_one(template_text, "", (schedule_point_t) {EVAL_FUSED, 0, 1}, &value,
                             &out.fused_columns, &out.fused_tiles)
                  + eval_one(template_text, "", (schedule_point_t) {EVAL_TILED, TILE_PROBE_WIDTH, 1}, &value,
                             &out.tiled_columns, &out.tiled_tiles);
    return out;
}

static void check_footprint(const char *template_text, run_state_t *state) {
    const footprint_t footprint = measure_footprints(template_text);
    const int fusable = has_intermediate(template_text);
    state->cases += FOOTPRINT_EVALUATIONS + FOOTPRINT_ASSERTIONS;

    if (footprint.evaluated != FOOTPRINT_EVALUATIONS) {
        printf("FAIL    footprint probe did not evaluate\n");
        state->failed++;
        note_failure(state, "schedule-eval-failed");
    } else if (footprint.materialised_tiles != 0 || footprint.fused_tiles != 0) {
        printf("FAIL    tiles: materialised=%zu fused=%zu; a traversal that is not tiled "
               "held tiles\n", footprint.materialised_tiles, footprint.fused_tiles);
        state->failed++;
        note_failure(state, "traversal-not-performed");
    } else if (footprint.tiled_columns != footprint.fused_columns) {
        printf("FAIL    columns: fused=%zu tiled=%zu; tiling is a cache over a fused "
               "traversal and must materialise the same columns\n",
               footprint.fused_columns, footprint.tiled_columns);
        state->failed++;
        note_failure(state, "traversal-not-performed");
    } else if (!fusable) {
        printf("  --    columns: %zu, no elementwise intermediate to fuse or tile\n",
               footprint.materialised_columns);
    } else if (footprint.materialised_columns <= footprint.fused_columns) {
        printf("FAIL    columns: materialised=%zu fused=%zu; fusion removed nothing\n",
               footprint.materialised_columns, footprint.fused_columns);
        state->failed++;
        note_failure(state, "traversal-not-performed");
    } else if (footprint.tiled_tiles == 0) {
        printf("FAIL    the tiled traversal held no tiles on a program with %zu fusable "
               "element(s)\n", footprint.materialised_columns - footprint.fused_columns);
        state->failed++;
        note_failure(state, "traversal-not-performed");
    } else {
        printf("  ok    columns: %zu materialised -> %zu fused (%zu elements never "
               "written), tiled holds %zu tile element(s)\n",
               footprint.materialised_columns, footprint.fused_columns,
               footprint.materialised_columns - footprint.fused_columns, footprint.tiled_tiles);
    }
}

static void check_pinned(const char *template_text, run_state_t *state) {
    uint64_t patterns[NT * NW];
    for (int i = 0; i < PINNED_COUNT; i++) {
        int evaluations = 0;
        const int distinct = distinct_patterns(template_text, PINNED[i], patterns, NT * NW,
                                         &evaluations);
        if (distinct == GRID_REFUSED) {
            printf("  --    %-8s the checker does not accept this program at this lane, so its "
                   "bits are outside the corollary\n", PINNED[i]);
            state->refused++;
            continue;
        }
        if (distinct == GRID_EVAL_FAILED || distinct == GRID_BUILD_BROKEN) {
            state->cases++;
            state->failed++;
            if (distinct == GRID_EVAL_FAILED) {
                printf("FAIL    %-8s the checker accepted this program and its evaluation "
                       "still failed\n", PINNED[i]);
                note_failure(state, "pinned-eval-failed");
            } else {
                printf("FAIL    %-8s the front end failed before the checker could answer at "
                       "this lane\n", PINNED[i]);
                note_failure(state, "pinned-build-broken");
            }
            continue;
        }
        state->cases += evaluations + 1;
        if (distinct == 1) {
            printf("  ok    %-8s 1 pattern over %d cells (0x%016llx)\n",
                   PINNED[i], NT * NW, (unsigned long long) patterns[0]);
        } else {
            printf("FAIL    %-8s %d pattern(s) over %d cells; a pinned value moved\n",
                   PINNED[i], distinct, NT * NW);
            state->failed++;
            note_failure(state, "lane-not-schedule-invariant");
        }
    }
}

static const char INTEGER_PROGRAM[] =
    "package app\n"
    "func main() Float {\n"
    "    x := integers(count: 4096.0)\n"
    "    exact total := isum(x)\n"
    "    return total\n"
    "}\n";

static const char EXACT_FLOAT_PROGRAM[] =
    "package app\n"
    "func main() Float {\n"
    "    x := indices(count: 4096.0)\n"
    "    exact total := sum(x * 0.1)\n"
    "    return total\n"
    "}\n";

static int eval_reference(const char *template_text, double *out) {
    built_program_t built;
    if (build_program(template_text, "", &built) != BUILD_ACCEPTED) {
        return 0;
    }
    const int ok = eval_dag_on_reference(built.resolved.dag, built.resolved.result, out);
    free_program(&built);
    return ok;
}

static const char INTEGER_OUT_OF_DOMAIN[] =
    "package app\n"
    "func main() Float {\n"
    "    x := integers(count: 4096.0)\n"
    "    exact total := isum(x * 6400000.0)\n"
    "    return total\n"
    "}\n";

static void check_integer_domain(run_state_t *state) {
    int refusals = 0;
    for (int i = 0; i < NT; i++) {
        for (int j = 0; j < NW; j++) {
            double value = 0.0;
            built_program_t built;
            if (build_program(INTEGER_OUT_OF_DOMAIN, "", &built) != BUILD_ACCEPTED) {
                continue;
            }
            if (!eval_dag(built.resolved.dag, built.resolved.result, TRAV[i].mode, TRAV[i].tile,
                          WORKERS[j], &value)) {
                refusals++;
            }
            free_program(&built);
        }
    }
    double reference = 0.0;
    const int reference_answered = eval_reference(INTEGER_OUT_OF_DOMAIN, &reference);

    state->cases += 2;
    if (refusals != NT * NW) {
        printf("FAIL    int/dom  %d of %d cell(s) refused; whether an out-of-domain exact-integer "
               "fold evaluates is a function of the schedule\n", refusals, NT * NW);
        state->failed++;
        note_failure(state, "integer-domain-follows-schedule");
    } else if (reference_answered) {
        printf("FAIL    int/dom  every schedule refused and the reference answered; the axis "
               "`exact` is defined against is the one path allowed to round\n");
        state->failed++;
        note_failure(state, "reference-answers-where-schedules-refuse");
    } else {
        printf("  ok    int/dom  refused at all %d cells and against the reference\n", NT * NW);
    }
}

static void check_reference(run_state_t *state) {
    static const struct { const char *name; const char *program; } CASES[] = {
        { "exact/int",   INTEGER_PROGRAM },
        { "exact/float", EXACT_FLOAT_PROGRAM }
    };

    for (size_t c = 0; c < sizeof CASES / sizeof CASES[0]; c++) {
        double reference = 0.0;
        state->cases++;
        if (!eval_reference(CASES[c].program, &reference)) {
            printf("FAIL    %-13s did not evaluate against the reference\n", CASES[c].name);
            state->failed++;
            note_failure(state, "schedule-eval-failed");
            continue;
        }

        int moved = 0;
        for (int t = 0; t < NT && !moved; t++) {
            for (int w = 0; w < NW && !moved; w++) {
                double got = 0.0;
                size_t columns = 0, tiles = 0;
                state->cases++;
                if (!eval_one(CASES[c].program, "",
                              (schedule_point_t) {TRAV[t].mode, TRAV[t].tile, WORKERS[w]},
                              &got, &columns, &tiles)) {
                    printf("FAIL    %-13s did not evaluate at traversal %d workers %d\n",
                           CASES[c].name, t, WORKERS[w]);
                    state->failed++;
                    note_failure(state, "schedule-eval-failed");
                    moved = 1;
                    break;
                }
                if (memcmp(&got, &reference, sizeof got) != 0) {
                    printf("FAIL    %-13s traversal %d workers %d gives 0x%016llx, the serial "
                           "reference gives 0x%016llx; an `exact` value is not the reference\n",
                           CASES[c].name, t, WORKERS[w],
                           (unsigned long long) bits_of(got),
                           (unsigned long long) bits_of(reference));
                    state->failed++;
                    note_failure(state, "exact-value-not-reference-equal");
                    moved = 1;
                }
            }
        }
        if (!moved) {
            printf("  ok    %-13s equals the serial reference 0x%016llx over all %d cells\n",
                   CASES[c].name, (unsigned long long) bits_of(reference), NT * NW);
        }
    }
}

static const char ITERATE_PROGRAM[] =
    "package app\n"
    "func main() exact Float {\n"
    "    x := integers(count: 8.0)\n"
    "    total := iterate(times: 4.0, from: 0.0, with: (acc, t) => {\n"
    "        exact part := isum(x)\n"
    "        return acc + part\n"
    "    })\n"
    "    return total\n"
    "}\n";

static void check_recurrence_runs(run_state_t *state) {
    uint64_t patterns[NT * NW];
    int evaluations = 0;
    const int distinct = distinct_patterns(ITERATE_PROGRAM, "", patterns, NT * NW, &evaluations);
    state->cases += evaluations + 1;
    if (distinct < 0) {
        printf("FAIL    %-8s did not build or evaluate\n", "iterate");
        state->failed++;
        note_failure(state, "schedule-eval-failed");
        return;
    }
    if (distinct != 1) {
        printf("FAIL    %-8s %d pattern(s) over %d cells; a sequential carry moved\n",
               "iterate", distinct, NT * NW);
        state->failed++;
        note_failure(state, "lane-not-schedule-invariant");
        return;
    }
    double got = 0.0;
    memcpy(&got, &patterns[0], sizeof got);
    state->cases++;
    if (got != -16336.0) {
        printf("FAIL    %-8s value %.1f, want -16336.0 (4 ticks x -4084)\n", "iterate", got);
        state->failed++;
        note_failure(state, "recurrence-tick-count-wrong");
        return;
    }
    printf("  ok    %-8s 1 pattern over %d cells, value %.1f = 4 x -4084\n",
           "iterate", NT * NW, got);
}

static const char TICK_PROGRAM[] =
    "package app\n"
    "func main() exact Float {\n"
    "    total := iterate(times: 4.0, from: 0.0, with: (acc, t) => acc + t)\n"
    "    return total\n"
    "}\n";

static const char NESTED_TICK_PROGRAM[] =
    "package app\n"
    "func main() exact Float {\n"
    "    total := iterate(times: 3.0, from: 0.0, with: (outer, t) =>\n"
    "        outer + t + iterate(times: 3.0, from: 0.0, with: (inner, u) => inner + u))\n"
    "    return total\n"
    "}\n";

static const char NESTED_CARRY_PROGRAM[] =
    "package app\n"
    "func main() exact Float {\n"
    "    total := iterate(times: 3.0, from: 0.0, with: (outer, t) =>\n"
    "        outer + iterate(times: 3.0, from: 1.0, with: (inner, u) => inner + inner))\n"
    "    return total\n"
    "}\n";

static const char CHOOSE_THEN_PROGRAM[] =
    "package app\n"
    "func main() Float {\n"
    "    x := integers(count: 4096.0)\n"
    "    picked := choose(when: 1.0, then: x * 0.0 + 3.0, else: x * 0.0 + 5.0)\n"
    "    exact total := isum(picked)\n"
    "    return total\n"
    "}\n";

static const char CHOOSE_ELSE_PROGRAM[] =
    "package app\n"
    "func main() Float {\n"
    "    x := integers(count: 4096.0)\n"
    "    picked := choose(when: 0.0, then: x * 0.0 + 3.0, else: x * 0.0 + 5.0)\n"
    "    exact total := isum(picked)\n"
    "    return total\n"
    "}\n";

static const char INCIDENT_FLIP_PROGRAM[] =
    "package app\n"
    "func main() Float {\n"
    "    x := indices(count: 100000.0)\n"
    "    approx s := sum(x * 0.5)\n"
    "    picked := choose(when: s - 0.0042511812704049134, then: x * 0.0 + 3.0, else: x * 0.0 + 5.0)\n"
    "    total := isum(picked)\n"
    "    return total\n"
    "}\n";

static const char INCIDENT_REPLAY_PROGRAM[] =
    "package app\n"
    "func main() Float {\n"
    "    x := indices(count: 100000.0)\n"
    "    replay s := sum(x * 0.5)\n"
    "    picked := choose(when: s - 0.0042511812704049134, then: x * 0.0 + 3.0, else: x * 0.0 + 5.0)\n"
    "    total := isum(picked)\n"
    "    return total\n"
    "}\n";

static const char INCIDENT_EXACT_PROGRAM[] =
    "package app\n"
    "func main() Float {\n"
    "    x := indices(count: 100000.0)\n"
    "    exact s := sum(x * 0.5)\n"
    "    picked := choose(when: s - 0.0042511812704049134, then: x * 0.0 + 3.0, else: x * 0.0 + 5.0)\n"
    "    total := isum(picked)\n"
    "    return total\n"
    "}\n";

static const char INCIDENT_DRIFT_PROGRAM[] =
    "package app\n"
    "func main() Float {\n"
    "    x := indices(count: 100000.0)\n"
    "    approx s := sum(x * 0.5)\n"
    "    return s\n"
    "}\n";

#define INCIDENT_THEN_TOTAL 300000.0
#define INCIDENT_ELSE_TOTAL 500000.0
#define INCIDENT_RECORDED_SUM 0.0042511812704049134

static int first_differing_digit(const double value, const double recorded) {
    char got[32];
    char want[32];
    snprintf(got, sizeof got, "%.16e", value);
    snprintf(want, sizeof want, "%.16e", recorded);
    if (strcmp(strchr(got, 'e'), strchr(want, 'e')) != 0) {
        return 1;
    }
    int digit = 0;
    for (int i = 0; got[i] && got[i] != 'e'; i++) {
        if (got[i] == '.') {
            continue;
        }
        digit++;
        if (got[i] != want[i]) {
            return digit;
        }
    }
    return 0;
}

static void report_incident_drift(void) {
    uint64_t cells[NT * NW];
    if (evaluate_grid(INCIDENT_DRIFT_PROGRAM, "", cells) < 0) {
        printf("        drift: the free sum did not evaluate, so nothing is reported\n");
        return;
    }
    const uint64_t recorded = bits_of(INCIDENT_RECORDED_SUM);
    int kept = 0;
    int earliest = 0;
    for (int j = 0; j < NW; j++) {
        int moved = 0;
        for (int i = 0; i < NT; i++) {
            const uint64_t cell = cells[i * NW + j];
            if (cell == recorded) {
                continue;
            }
            double value;
            memcpy(&value, &cell, sizeof value);
            const int digit = first_differing_digit(value, INCIDENT_RECORDED_SUM);
            if (earliest == 0 || (digit > 0 && digit < earliest)) {
                earliest = digit;
            }
            moved = 1;
        }
        kept += !moved;
    }
    printf("        drift: the free sum keeps the recorded bits at %d of %d worker counts; "
           "the rest move it first in significant digit %d\n", kept, NW, earliest);
}

static void check_incident_flip(run_state_t *state) {
    uint64_t cells[NT * NW];
    const int evaluated = evaluate_grid(INCIDENT_FLIP_PROGRAM, "", cells);
    state->cases += (evaluated < 0 ? 0 : evaluated) + 1;
    if (evaluated < 0) {
        printf("FAIL    incident: the flip program did not build or evaluate\n");
        state->failed++;
        note_failure(state, "incident-did-not-run");
        return;
    }
    const int distinct = distinct_in(cells, NT * NW, 1);
    if (distinct != 2) {
        printf("FAIL    incident: %d pattern(s) over %d cells where the decision must take "
               "both arms\n", distinct, NT * NW);
        state->failed++;
        note_failure(state, "incident-decision-did-not-flip");
        return;
    }
    const uint64_t then_bits = bits_of(INCIDENT_THEN_TOTAL);
    const uint64_t else_bits = bits_of(INCIDENT_ELSE_TOTAL);
    state->cases++;
    for (int i = 0; i < NT * NW; i++) {
        if (cells[i] != then_bits && cells[i] != else_bits) {
            printf("FAIL    incident: cell %d holds a value that is neither arm\n", i);
            state->failed++;
            note_failure(state, "incident-arm-unknown");
            return;
        }
    }
    printf("  ok    incident: both arms over %d cells; the schedule picked the answer\n",
           NT * NW);
    report_incident_drift();
}

static void check_incident_pinned(run_state_t *state, const char *program,
                                  const char *label) {
    uint64_t cells[NT * NW];
    const int evaluated = evaluate_grid(program, "", cells);
    state->cases += (evaluated < 0 ? 0 : evaluated) + 1;
    if (evaluated < 0) {
        printf("FAIL    %-8s the repaired program did not build or evaluate\n", label);
        state->failed++;
        note_failure(state, "incident-did-not-run");
        return;
    }
    const int distinct = distinct_in(cells, NT * NW, 1);
    if (distinct != 1) {
        printf("FAIL    %-8s %d pattern(s) over %d cells; the repair moved with the "
               "schedule\n", label, distinct, NT * NW);
        state->failed++;
        note_failure(state, "incident-repair-moved");
        return;
    }
    state->cases++;
    printf("  ok    %-8s 1 pattern over %d cells\n", label, NT * NW);
    if (cells[0] != bits_of(INCIDENT_ELSE_TOTAL)) {
        printf("FAIL    %-8s the repair's one value is not the arm the serial run takes\n", label);
        state->failed++;
        note_failure(state, "incident-repair-not-serial");
        return;
    }
    printf("  ok    %-8s and it is the arm the serial run takes\n", label);
}

static void check_value(run_state_t *state, const char *program, const double want,
                        const char *label, const char *subject) {
    uint64_t patterns[NT * NW];
    int evaluations = 0;
    const int distinct = distinct_patterns(program, "", patterns, NT * NW, &evaluations);
    state->cases += evaluations + 1;
    if (distinct < 0) {
        printf("FAIL    %-8s did not build or evaluate\n", label);
        state->failed++;
        note_failure(state, "schedule-eval-failed");
        return;
    }
    if (distinct != 1) {
        printf("FAIL    %-8s %d pattern(s) over %d cells; the %s moved with the schedule\n",
               label, distinct, NT * NW, subject);
        state->failed++;
        note_failure(state, "value-not-schedule-invariant");
        return;
    }
    double got = 0.0;
    memcpy(&got, &patterns[0], sizeof got);
    state->cases++;
    if (got != want) {
        printf("FAIL    %-8s value %.1f, want %.1f\n", label, got, want);
        state->failed++;
        note_failure(state, "value-wrong");
        return;
    }
    printf("  ok    %-8s 1 pattern over %d cells, value %.1f\n", label, NT * NW, got);
}

static void check_program(const int index, const char *template_text, run_state_t *state) {
    printf("program %d\n", index);

    uint64_t cells[NT * NW];
    if (check_unpinned(template_text, index, state, cells) < 0) {
        return;
    }

    check_traversal_axis(cells, state);
    check_footprint(template_text, state);
    check_pinned(template_text, state);
}

static const char SCAN_WITNESS_PROGRAM[] =
    "package app\n"
    "func main() Float {\n"
    "    x := indices(count: 1500.0)\n"
    "    total := sum(scan(x), order: oracle)\n"
    "    return total\n"
    "}\n";

static void check_scan_witness(run_state_t *state) {
    uint64_t patterns[NW];
    int seen = 0;
    for (int i = 0; i < NW; i++) {
        double value = 0.0;
        size_t columns = 0;
        size_t tiles = 0;
        state->cases++;
        if (!eval_one(SCAN_WITNESS_PROGRAM, "", (schedule_point_t) {EVAL_MATERIALISE, 0, WORKERS[i]},
                      &value, &columns, &tiles)) {
            printf("FAIL    %-8s did not build or evaluate\n", "scan");
            state->failed++;
            note_failure(state, "schedule-eval-failed");
            return;
        }
        seen = remember_pattern(patterns, seen, NW, bits_of(value));
    }
    state->cases++;
    if (seen > 1) {
        printf("  ok    %-8s %d pattern(s) over %d worker counts, the fold over it pinned\n",
               "scan", seen, NW);
    } else {
        printf("FAIL    %-8s 1 pattern over %d worker counts with the fold over it pinned; "
               "nothing witnesses that a free scan depends on the schedule\n", "scan", NW);
        state->failed++;
        note_failure(state, "free-scan-not-discriminating");
    }
}

static void check_discrimination_floor(run_state_t *state) {
    state->cases++;
    if (state->discriminating * 2 < NP) {
        printf("FAIL    only %d of %d programs discriminate; the invariances above are vacuous\n",
               state->discriminating, NP);
        state->failed++;
        note_failure(state, "schedule-witness-not-discriminating");
    }
}

int main(void) {
    rl_build_stamp("test_schedule");
    if (strcmp(eval_perturbation(), "-") != 0) {
        printf("PERTURBED %s\n", eval_perturbation());
    }

    run_state_t state = { 0, 0, "-", 0, 0 };
    for (int i = 0; i < NP; i++) {
        check_program(i, PROGRAMS[i], &state);
    }
    state.cases++;
    if (state.refused == PINNED_REFUSALS_EXPECTED) {
        printf("  ok    refused: %d program-lane pair(s), the expected exclusions\n",
               state.refused);
    } else {
        printf("FAIL    refused: %d program-lane pair(s) where %d are expected; the pinned "
               "coverage drifted\n", state.refused, PINNED_REFUSALS_EXPECTED);
        state.failed++;
        note_failure(&state, "pinned-coverage-drifted");
    }
    check_reference(&state);
    check_integer_domain(&state);
    check_recurrence_runs(&state);
    check_scan_witness(&state);
    check_value(&state, TICK_PROGRAM, 6.0, "tick", "tick index");
    check_value(&state, NESTED_TICK_PROGRAM, 12.0, "tick2", "tick index");
    check_value(&state, NESTED_CARRY_PROGRAM, 24.0, "nested", "carried value");
    check_value(&state, CHOOSE_THEN_PROGRAM, 12288.0, "choose+", "chosen arm");
    check_value(&state, CHOOSE_ELSE_PROGRAM, 20480.0, "choose-", "chosen arm");
    check_incident_flip(&state);
    check_value(&state, INCIDENT_REPLAY_PROGRAM, INCIDENT_THEN_TOTAL, "incident+", "chosen arm");
    check_incident_pinned(&state, INCIDENT_EXACT_PROGRAM, "incident=");
    check_discrimination_floor(&state);

    printf("schedule: %ld assertion(s); %d program(s) x %d traversal(s) x %d worker count(s), "
           "%d discriminating; fusion and tiling cannot reach a pinned value\n",
           state.cases, NP, NT, NW, state.discriminating);
    return state.failed > 0 ? RL_FAIL(LEG, state.cases, state.why) : RL_PASS(LEG, state.cases);
}
