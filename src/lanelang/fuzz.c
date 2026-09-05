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
#include "lattice.h"
#include "oracle.h"
#include "parse.h"
#include "resolve.h"
#include "status.h"

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SEED_STRIDE 0x9E3779B97F4A7C15ULL

#define MIN_GRAPH_NODES 8

#define GRAPH_NODE_SPAN 43

#define MAX_GENERATED_BINDINGS 64

#define FOLD_OPERAND_TRIES 8

#define DEFAULT_PATH_CAP 200000

#define DEFAULT_PROGRAM_COUNT 2000

#define DEFAULT_SEED 20260725ULL

#define EXIT_USAGE 3

typedef struct {
    long checked;

    long skipped;

    long mismatches;

    long permuted;

    long accepted_bad;

    long no_edge;

    long refused_wrongly;

    long max_paths;
} agree_counts_t;

typedef struct {
    long checked;

    long skipped;

    long mismatches;

    long max_paths;
} oracle_comparison_t;

typedef struct {
    long cases;

    long failures;
} metamorphic_counts_t;

typedef struct {
    long checked;

    long skipped;

    long mismatches;

    long malformed;

    long programs;

    long max_paths;

    long heals;

    long moved;
} roundtrip_counts_t;

typedef struct {
    char *text;

    size_t length;

    size_t capacity;
} source_buffer_t;

static uint64_t random_next(uint64_t *state) {
    uint64_t word = *state;
    word ^= word >> 12;
    word ^= word << 25;
    word ^= word >> 27;
    *state = word;
    return word * 0x2545F4914F6CDD1DULL;
}

static int random_below(uint64_t *state, const int bound) {
    return bound <= 0 ? 0 : (int) (random_next(state) % (uint64_t) bound);
}

static int graph_size_for(const uint64_t seed) {
    return MIN_GRAPH_NODES + (int) (seed % GRAPH_NODE_SPAN);
}

static void make_leaf(dagnode_t *node) {
    node->op = OP_INPUT;
    node->op_class = OPC_NEUTRAL;
}

static void make_elementwise(dagnode_t *node, uint64_t *state, const int index) {
    node->op = OP_ELEM;
    node->op_class = OPC_NEUTRAL;
    node->in[0] = random_below(state, index);
    node->in[1] = random_below(state, index);
    node->input_count = 2;
}

static const opclass_t FOLD_CLASSES[] = { OPC_FOLD_APPROX, OPC_FOLD_REPLAY, OPC_FOLD_EXACT };
static const lane_t LANE_DRAWS[] = { LANE_EXACT, LANE_REPLAY, LANE_APPROX };

static void make_fold(dagnode_t *node, uint64_t *state, const int index) {
    node->op = OP_FOLD;
    node->op_class = FOLD_CLASSES[random_below(state, 3)];
    node->in[0] = random_below(state, index);
    node->input_count = 1;
}

static void maybe_declare_demand(dagnode_t *node, uint64_t *state, const int index) {
    if (index <= 1 || random_below(state, 4) != 0) {
        return;
    }
    node->declared = LANE_DRAWS[random_below(state, 3)];
}

static void random_node(dagnode_t *node, uint64_t *state, const int index) {
    dag_node_init(node);
    node->span.line = index + 1;
    node->span.column = 1;
    node->span.length = 1;
    node->declared = LANE_NONE;

    const int kind = (index < 2) ? 0 : random_below(state, 10);
    if (kind < 2) {
        make_leaf(node);
    } else if (kind < 6) {
        make_elementwise(node, state, index);
    } else {
        make_fold(node, state, index);
    }
    maybe_declare_demand(node, state, index);
}

static dag_t *random_dag(const uint64_t seed, const int node_count, int *result_out) {
    uint64_t state = seed ? seed : 1;
    dag_t *dag = dag_new();
    if (!dag) {
        return NULL;
    }

    for (int i = 0; i < node_count; i++) {
        dagnode_t node;
        random_node(&node, &state, i);
        if (dag_append(dag, &node) < 0) {
            dag_free(dag); return NULL;
        }
    }
    *result_out = node_count - 1;
    dag_recount_uses(dag, *result_out);
    return dag;
}

static int find_edge_to_exchange(const dag_t *dag, int *producer_out, int *consumer_out) {
    const int count = dag_count(dag);
    for (int i = 0; i < count; i++) {
        const dagnode_t *node = dag_at_const(dag, i);
        for (int k = 0; k < node->input_count; k++) {
            if (node->in[k] >= 0 && node->in[k] < i) {
                *producer_out = node->in[k];
                *consumer_out = i;
                return 1;
            }
        }
    }
    return 0;
}

static dag_t *copy_with_positions_exchanged(
    const dag_t *dag,
    const int *new_index_of,
    const int *old_index_at,
    const int count
) {
    dag_t *out = dag_new();
    if (!out) {
        return NULL;
    }
    for (int i = 0; i < count; i++) {
        dagnode_t node = *dag_at_const(dag, old_index_at[i]);
        for (int k = 0; k < node.input_count; k++) {
            if (node.in[k] >= 0 && node.in[k] < count) {
                node.in[k] = new_index_of[node.in[k]];
            }
        }
        if (dag_append(out, &node) < 0) {
            dag_free(out); return NULL;
        }
    }
    return out;
}

static dag_t *permute_out_of_order(const dag_t *dag, const int result, int *result_out, int *oom_out) {
    int producer = -1;
    int consumer = -1;
    if (!find_edge_to_exchange(dag, &producer, &consumer)) {
        return NULL;
    }

    const int count = dag_count(dag);
    int *new_index_of = (int *) malloc((size_t) count * sizeof(int));
    int *old_index_at = (int *) malloc((size_t) count * sizeof(int));
    if (!new_index_of || !old_index_at) {
        free(new_index_of);
        free(old_index_at);
        *oom_out = 1;
        return NULL;
    }

    for (int i = 0; i < count; i++) {
        new_index_of[i] = i;
    }
    new_index_of[producer] = consumer;
    new_index_of[consumer] = producer;
    for (int i = 0; i < count; i++) {
        old_index_at[new_index_of[i]] = i;
    }

    dag_t *permuted = copy_with_positions_exchanged(dag, new_index_of, old_index_at, count);
    if (permuted) {
        *result_out = (result >= 0 && result < count) ? new_index_of[result] : result;
    } else {
        *oom_out = 1;
    }
    free(new_index_of);
    free(old_index_at);
    return permuted;
}

static void report_mismatch(
    const uint64_t seed,
    const int node,
    const lane_t swept,
    const lane_t wanted,
    const char *mode_flag
) {
    fprintf(stderr,
            "MISMATCH seed=%llu node=%d: sweep says %s, path oracle says %s\n"
            "  reproduce: lanefuzz %s 1 %llu\n",
            (unsigned long long) seed, node, lane_name(swept), lane_name(wanted),
            mode_flag, (unsigned long long) seed);
}

static oracle_comparison_t compare_against_oracle(
    const dag_t *dag,
    const lane_t *computed,
    const uint64_t seed,
    const long path_cap,
    const char *mode_flag
) {
    oracle_comparison_t out = { 0, 0, 0, 0 };
    const int count = dag_count(dag);
    for (int i = 0; i < count; i++) {
        long paths = 0;
        const lane_t wanted = oracle_lane(dag, i, path_cap, &paths);
        if (paths > out.max_paths) {
            out.max_paths = paths;
        }
        if (wanted == LANE_NONE) {
            out.skipped++; continue;
        }
        out.checked++;
        if (wanted != computed[i]) {
            out.mismatches++;
            report_mismatch(seed, i, computed[i], wanted, mode_flag);
        }
    }
    return out;
}

static int check_ordering_refusal(
    const dag_t *dag,
    const int result,
    const uint64_t seed,
    agree_counts_t *counts
) {
    int permuted_result = 0;
    int oom = 0;
    dag_t *permuted = permute_out_of_order(dag, result, &permuted_result, &oom);
    if (!permuted) {
        if (oom) {
            return 0;
        }
        counts->no_edge++;
        return 1;
    }

    const int count = dag_count(permuted);
    lane_t *computed = (lane_t *) calloc((size_t) count, sizeof(lane_t));
    if (!computed) {
        dag_free(permuted); return 0;
    }
    diagnostic_t *diagnostics = diagnostic_new("<fuzz-permuted>", "");
    const check_result_t swept = lattice_check(permuted, permuted_result, computed, diagnostics);
    counts->permuted++;
    if (swept.malformed < 0) {
        fprintf(stderr,
                "NOT REFUSED seed=%llu: an out-of-order graph was analysed anyway, so the "
                "sweep answered from a dropped dependency\n"
                "  reproduce: lanefuzz --agree 1 %llu\n",
                (unsigned long long) seed, (unsigned long long) seed);
        counts->accepted_bad++;
    }
    diagnostic_free(diagnostics);
    free(computed);
    dag_free(permuted);
    return 1;
}

static void report_wrong_refusal(const uint64_t seed, const int node) {
    fprintf(stderr, "seed=%llu: a generated graph was rejected as out of order at node %d, "
                    "which the generator cannot produce\n",
            (unsigned long long) seed, node);
}

static int check_one_agree_program(const uint64_t seed, const long path_cap, agree_counts_t *counts) {
    int result = 0;
    dag_t *dag = random_dag(seed, graph_size_for(seed), &result);
    if (!dag) {
        return 0;
    }

    const int count = dag_count(dag);
    lane_t *computed = (lane_t *) calloc((size_t) count, sizeof(lane_t));
    if (!computed) {
        dag_free(dag); return 0;
    }
    diagnostic_t *diagnostics = diagnostic_new("<fuzz>", "");
    const check_result_t swept = lattice_check(dag, result, computed, diagnostics);
    if (swept.malformed >= 0) {
        report_wrong_refusal(seed, swept.malformed);
        counts->refused_wrongly++;
    }

    const int ok = check_ordering_refusal(dag, result, seed, counts);

    const oracle_comparison_t compared = compare_against_oracle(dag, computed, seed, path_cap, "--agree");
    counts->checked += compared.checked;
    counts->skipped += compared.skipped;
    counts->mismatches += compared.mismatches;
    if (compared.max_paths > counts->max_paths) {
        counts->max_paths = compared.max_paths;
    }

    diagnostic_free(diagnostics);
    free(computed);
    dag_free(dag);
    return ok;
}

static void report_agree_totals(const agree_counts_t *counts, const long path_cap) {
    printf("differential oracle: %ld node answers compared against exhaustive path enumeration, "
           "%ld mismatches\n", counts->checked, counts->mismatches);
    printf("  (deepest node needed %ld path(s) against a cap of %ld; %ld skipped)\n",
           counts->max_paths, path_cap, counts->skipped);
    printf("ordering precondition: %ld out-of-order graph(s) put to the sweep, %ld analysed anyway"
           "%s\n", counts->permuted, counts->accepted_bad,
           counts->no_edge ? " (some programs were all leaves and had no edge to exchange)" : "");
}

static int agree_verdict(const agree_counts_t *counts) {
    const long skipped = counts->skipped;
    if (counts->checked == 0) {
        return rl_status("L7", "FAIL", 0, skipped, "nothing-compared");
    }
    if (skipped > counts->checked) {
        return rl_status("L7", "FAIL", counts->checked, skipped, "oracle-coverage-too-low");
    }
    if (counts->mismatches) {
        return rl_status("L7", "FAIL", counts->checked, skipped, "sweep-disagrees-with-oracle");
    }
    if (counts->refused_wrongly) {
        return rl_status("L7", "FAIL", counts->checked, skipped, "well-formed-graph-refused");
    }
    if (counts->accepted_bad) {
        return rl_status("L7", "FAIL", counts->checked, skipped, "out-of-order-graph-analysed");
    }
    return counts->permuted == 0
           ? rl_status("L7", "FAIL", counts->checked, skipped, "ordering-witness-not-materialised")
           : rl_status("L7", "PASS", counts->checked + counts->permuted, skipped, NULL);
}

static int mode_agree(const long program_count, const uint64_t first_seed, const long path_cap) {
    agree_counts_t counts = {0};

    for (long i = 0; i < program_count; i++) {
        const uint64_t seed = first_seed + (uint64_t) i * SEED_STRIDE;
        if (!check_one_agree_program(seed, path_cap, &counts)) {
            return rl_status("L7", "FAIL", counts.checked, counts.skipped, "oom");
        }
    }

    report_agree_totals(&counts, path_cap);
    return agree_verdict(&counts);
}

static int violations_of(dag_t *dag, const int result) {
    const int count = dag_count(dag);
    lane_t *computed = (lane_t *) calloc((size_t) count, sizeof(lane_t));
    if (!computed) {
        return -1;
    }
    diagnostic_t *diagnostics = diagnostic_new("<fuzz>", "");
    const check_result_t swept = lattice_check(dag, result, computed, diagnostics);
    diagnostic_free(diagnostics);
    free(computed);
    return swept.violations;
}

static int spans_differ(const span_t a, const span_t b) {
    return a.file != b.file || a.line != b.line || a.column != b.column || a.length != b.length;
}

static int dagnode_differs(const dagnode_t *a, const dagnode_t *b) {
    if (a->op != b->op || a->input_count != b->input_count || a->carried != b->carried
        || a->tick_of != b->tick_of) {
        return 1;
    }
    for (int k = 0; k < DAG_MAX_INPUTS; k++) {
        if (a->in[k] != b->in[k]) {
            return 1;
        }
    }
    if (a->op_class != b->op_class || a->declared != b->declared || a->lane != b->lane) {
        return 1;
    }
    if (spans_differ(a->span, b->span) || spans_differ(a->declared_span, b->declared_span)) {
        return 1;
    }
    if (memcmp(&a->constant, &b->constant, sizeof a->constant) != 0) {
        return 1;
    }
    if (a->binary_operator != b->binary_operator || a->bless != b->bless) {
        return 1;
    }
    return a->uses != b->uses || a->reached != b->reached
           ? 1
           : a->label != b->label || a->label_length != b->label_length;
}

static int graph_differs_from(dag_t *dag, const dagnode_t *snapshot, const int count) {
    for (int i = 0; i < count; i++) {
        if (dagnode_differs(&snapshot[i], dag_at(dag, i))) {
            return 1;
        }
    }
    return 0;
}

static void declare_all(dag_t *dag, const lane_t demand) {
    const int count = dag_count(dag);
    for (int i = 0; i < count; i++) {
        dagnode_t *node = dag_at(dag, i);
        if (node->declared != LANE_NONE) {
            node->declared = demand;
        }
    }
}

static int check_non_mutation(
    dag_t *dag,
    const int result,
    const uint64_t seed,
    metamorphic_counts_t *counts
) {
    const int count = dag_count(dag);
    dagnode_t *snapshot = (dagnode_t *) malloc(sizeof(dagnode_t) * (size_t) count);
    if (!snapshot) {
        return 0;
    }
    for (int i = 0; i < count; i++) {
        snapshot[i] = *dag_at(dag, i);
    }

    const int first = violations_of(dag, result);
    const int mutated = graph_differs_from(dag, snapshot, count);
    const int second = violations_of(dag, result);
    free(snapshot);
    if (first < 0 || second < 0) {
        return 0;
    }

    counts->cases++;
    if (mutated) {
        counts->failures++;
        fprintf(stderr, "M3 seed=%llu: the checker MUTATED the graph\n",
                (unsigned long long) seed);
    }
    counts->cases++;
    if (first != second) {
        counts->failures++;
        fprintf(stderr, "M3 seed=%llu: not idempotent (%d then %d violations)\n",
                (unsigned long long) seed, first, second);
    }
    return 1;
}

static int check_weakening(
    dag_t *dag,
    const int result,
    const uint64_t seed,
    metamorphic_counts_t *counts
) {
    declare_all(dag, LANE_APPROX);
    const int violations = violations_of(dag, result);
    if (violations < 0) {
        return -1;
    }
    counts->cases++;
    if (violations > 0) {
        counts->failures++;
        fprintf(stderr, "M1 seed=%llu: weakening every demand still left %d violation(s)\n",
                (unsigned long long) seed, violations);
    }
    return violations;
}

static int check_strengthening(
    dag_t *dag,
    const int result,
    const uint64_t seed,
    metamorphic_counts_t *counts
) {
    declare_all(dag, LANE_REPLAY);
    const int middle = violations_of(dag, result);
    declare_all(dag, LANE_EXACT);
    const int strong = violations_of(dag, result);
    if (middle < 0 || strong < 0) {
        return 0;
    }

    counts->cases++;
    if (strong < middle) {
        counts->failures++;
        fprintf(stderr,
                "M2 seed=%llu: replay -> exact REDUCED violations (%d -> %d); strengthening "
                "a demand made it easier to satisfy\n",
                (unsigned long long) seed, middle, strong);
    }
    return 1;
}

static int mode_metamorphic(const long program_count, const uint64_t first_seed) {
    metamorphic_counts_t counts = { 0, 0 };

    for (long i = 0; i < program_count; i++) {
        const uint64_t seed = first_seed + (uint64_t) i * SEED_STRIDE;
        int result = 0;
        dag_t *dag = random_dag(seed, graph_size_for(seed), &result);
        if (!dag) {
            return RL_FAIL("L8", counts.cases, "oom");
        }

        int ok = check_non_mutation(dag, result, seed, &counts);
        const int weak = ok ? check_weakening(dag, result, seed, &counts) : -1;
        if (weak >= 0) {
            ok = check_strengthening(dag, result, seed, &counts);
        }
        dag_free(dag);
        if (!ok || weak < 0) {
            return RL_FAIL("L8", counts.cases, "oom");
        }
    }

    printf("metamorphic properties: %ld assertions over %ld random programs, %ld failed\n",
           counts.cases, program_count, counts.failures);
    return counts.failures
           ? RL_FAIL("L8", counts.cases, "metamorphic-property-violated")
           : RL_PASS("L8", counts.cases);
}

static void source_append(source_buffer_t *buffer, const char *text) {
    const size_t added = strlen(text);
    if (buffer->length + added + 1 > buffer->capacity) {
        size_t capacity = buffer->capacity ? buffer->capacity * 2 : 1024;
        while (capacity < buffer->length + added + 1) {
            capacity *= 2;
        }
        char *grown = (char *) realloc(buffer->text, capacity);
        if (!grown) {
            return;
        }
        buffer->text = grown;
        buffer->capacity = capacity;
    }
    memcpy(buffer->text + buffer->length, text, added);
    buffer->length += added;
    buffer->text[buffer->length] = '\0';
}

static void source_append_format(source_buffer_t *buffer, const char *fmt, ...) {
    char line[256];
    va_list arguments;
    va_start(arguments, fmt);
    vsnprintf(line, sizeof line, fmt, arguments);
    va_end(arguments);
    source_append(buffer, line);
}

static int count_vector_bindings(const char *is_vector, const int count, int *first_out) {
    int available = 0;
    *first_out = 0;
    for (int k = 0; k < count; k++) {
        if (is_vector[k]) {
            if (!available) {
                *first_out = k;
            }
            available++;
        }
    }
    return available;
}

static int choose_binding_kind(uint64_t *state, const int index, const int is_last, const int vector_available) {
    int kind = (index < 2) ? 0 : random_below(state, 10);
    if (is_last && vector_available) {
        kind = 9;
    }
    if (kind >= 6 && !vector_available) {
        kind = 0;
    }
    return kind;
}

static void emit_leaf_binding(source_buffer_t *buffer, const int index) {
    source_append_format(buffer, "    v%d := indices(count: 1024.0)\n", index);
}

static int emit_elementwise_binding(
    source_buffer_t *buffer,
    uint64_t *state,
    const int index,
    const char *is_vector
) {
    static const char *const OPERATORS[] = { "+", "*", "-" };
    const int left = random_below(state, index);
    const int right = random_below(state, index);
    const char *operator_symbol = OPERATORS[random_below(state, 3)];
    source_append_format(buffer, "    v%d := v%d %s v%d\n", index, left, operator_symbol, right);
    return is_vector[left] || is_vector[right];
}

static const char *random_qualifier(uint64_t *state) {
    switch (random_below(state, 4)) {
    case 1:  return "replay ";
    case 2:  return "exact ";
    case 3:  return "approx ";
    default: return "";
    }
}

static void emit_fold_binding(
    source_buffer_t *buffer,
    uint64_t *state,
    const int index,
    const char *is_vector,
    const int first_vector
) {
    int operand = first_vector;
    for (int tries = 0; tries < FOLD_OPERAND_TRIES; tries++) {
        const int candidate = random_below(state, index);
        if (is_vector[candidate]) {
            operand = candidate; break;
        }
    }
    source_append_format(buffer, "    %sv%d := sum(v%d)\n", random_qualifier(state), index,
                         operand);
}

static char *random_source(const uint64_t seed, const int binding_count) {
    uint64_t state = seed ? seed : 1;
    source_buffer_t buffer = { NULL, 0, 0 };
    char is_vector[MAX_GENERATED_BINDINGS];
    const int bindings = binding_count > MAX_GENERATED_BINDINGS
                         ? MAX_GENERATED_BINDINGS
                         : binding_count;

    source_append(&buffer, "package app\n\nfunc main() Float {\n");
    for (int i = 0; i < bindings; i++) {
        int first_vector = 0;
        const int vector_available = count_vector_bindings(is_vector, i, &first_vector);
        const int kind = choose_binding_kind(&state, i, i == bindings - 1, vector_available);

        if (kind < 2) {
            emit_leaf_binding(&buffer, i);
            is_vector[i] = 1;
        } else if (kind < 6) {
            is_vector[i] = (char) emit_elementwise_binding(&buffer, &state, i, is_vector);
        } else {
            emit_fold_binding(&buffer, &state, i, is_vector, first_vector);
            is_vector[i] = 0;
        }
    }
    source_append_format(&buffer, "    return v%d\n}\n", bindings - 1);
    return buffer.text;
}

static void report_generator_defect(const char *what, const uint64_t seed, diagnostic_t *diagnostics) {
    fprintf(stderr, "GENERATOR EMITTED %s SOURCE seed=%llu\n", what, (unsigned long long) seed);
    diagnostic_render(diagnostics, stderr);
}

static int sweep_and_compare(
    const resolved_t *resolved,
    diagnostic_t *diagnostics,
    const uint64_t seed,
    const long path_cap,
    roundtrip_counts_t *counts
) {
    const int count = dag_count(resolved->dag);
    const size_t width = (size_t) (count > 0 ? count : 1);
    lane_t *computed = (lane_t *) calloc(width, sizeof(lane_t));
    lane_t *before = (lane_t *) calloc(width, sizeof(lane_t));
    if (!computed || !before) {
        free(computed);
        free(before);
        return 0;
    }
    lattice_compute_lanes(resolved->dag, count, before);
    counts->heals += heal_program(resolved->dag, resolved->result);
    lattice_check(resolved->dag, resolved->result, computed, diagnostics);
    for (int i = 0; i < count; i++) {
        if (before[i] != computed[i]) {
            counts->moved++;
        }
    }
    free(before);

    const oracle_comparison_t compared = compare_against_oracle(resolved->dag, computed, seed, path_cap,
                                                          "--roundtrip");
    counts->checked += compared.checked;
    counts->skipped += compared.skipped;
    counts->mismatches += compared.mismatches;
    if (compared.max_paths > counts->max_paths) {
        counts->max_paths = compared.max_paths;
    }
    free(computed);
    return 1;
}

static int check_one_roundtrip_program(
    const uint64_t seed,
    const long path_cap,
    roundtrip_counts_t *counts
) {
    char *source = random_source(seed, graph_size_for(seed));
    if (!source) {
        return 0;
    }

    diagnostic_t *diagnostics = diagnostic_new("<roundtrip>", source);
    program_t *program = parse_program("<roundtrip>", source, diagnostics);
    resolved_t resolved = { RESOLVE_MALFORMED, NULL, -1 };
    if (program) {
        resolved = resolve_program(program, "main", diagnostics);
    }

    int ok = 1;
    if (!program || resolved.status != RESOLVE_OK || !resolved.dag) {
        counts->malformed++;
        report_generator_defect(program ? "UNRESOLVABLE" : "UNPARSEABLE", seed, diagnostics);
    } else {
        counts->programs++;
        ok = sweep_and_compare(&resolved, diagnostics, seed, path_cap, counts);
    }

    dag_free(resolved.dag);
    ast_free(program);
    diagnostic_free(diagnostics);
    free(source);
    return ok;
}

static void report_roundtrip_totals(const roundtrip_counts_t *counts, const long path_cap) {
    printf("source round-trip: %ld program(s) parsed, resolved and healed; %ld node answers "
           "compared against exhaustive path enumeration, %ld mismatches; the healing "
           "strengthened %ld producer(s) and moved %ld node answer(s)\n",
           counts->programs, counts->checked, counts->mismatches, counts->heals, counts->moved);
    printf("  (deepest node needed %ld path(s) against a cap of %ld; %ld skipped)\n",
           counts->max_paths, path_cap, counts->skipped);
}

static int mode_roundtrip(const long program_count, const uint64_t first_seed, const long path_cap) {
    roundtrip_counts_t counts = {0};

    for (long i = 0; i < program_count; i++) {
        const uint64_t seed = first_seed + (uint64_t) i * SEED_STRIDE;
        if (!check_one_roundtrip_program(seed, path_cap, &counts)) {
            return rl_status("L6", "FAIL", counts.checked, counts.skipped, "oom");
        }
    }

    report_roundtrip_totals(&counts, path_cap);
    if (counts.malformed) {
        return rl_status("L6", "FAIL", counts.checked, counts.skipped, "generator-emitted-invalid-source");
    }
    if (counts.checked == 0) {
        return rl_status("L6", "FAIL", 0, counts.skipped, "nothing-compared");
    }
    if (counts.skipped > counts.checked) {
        return rl_status("L6", "FAIL", counts.checked, counts.skipped, "oracle-coverage-too-low");
    }
    if (counts.heals == 0) {
        return rl_status("L6", "FAIL", counts.checked + 2, counts.skipped, "healing-not-exercised");
    }
    if (counts.moved == 0) {
        return rl_status("L6", "FAIL", counts.checked + 2, counts.skipped, "healing-moved-no-lane");
    }
    return counts.mismatches
           ? rl_status("L6", "FAIL", counts.checked + 2, counts.skipped, "sweep-disagrees-with-oracle")
           : rl_status("L6", "PASS", counts.checked + 2, counts.skipped, NULL);
}

static long parse_count(const char *text, const long fallback) {
    char *after = NULL;
    errno = 0;
    const long value = strtol(text, &after, 10);
    if (errno != 0 || after == text || *after != '\0' || value <= 0) {
        fprintf(stderr, "lanefuzz: '%s' is not a positive count; using %ld\n", text, fallback);
        return fallback;
    }
    return value;
}

static uint64_t parse_seed(const char *text, const uint64_t fallback) {
    char *after = NULL;
    errno = 0;
    const unsigned long long value = strtoull(text, &after, 10);
    if (errno != 0 || after == text || *after != '\0') {
        fprintf(stderr, "lanefuzz: '%s' is not a seed; using %llu\n",
                text, (unsigned long long) fallback);
        return fallback;
    }
    return (uint64_t) value;
}

int main(const int argc, char **argv) {
    rl_build_stamp("lanefuzz");
    const char *mode = argc > 1 ? argv[1] : "--agree";
    const long program_count = argc > 2 ? parse_count(argv[2], DEFAULT_PROGRAM_COUNT)
                                        : DEFAULT_PROGRAM_COUNT;
    const uint64_t seed = argc > 3 ? parse_seed(argv[3], DEFAULT_SEED) : DEFAULT_SEED;

    if (strcmp(mode, "--agree") == 0) {
        return mode_agree(program_count, seed, DEFAULT_PATH_CAP);
    }
    if (strcmp(mode, "--metamorphic") == 0) {
        return mode_metamorphic(program_count, seed);
    }
    if (strcmp(mode, "--roundtrip") == 0) {
        return mode_roundtrip(program_count, seed, DEFAULT_PATH_CAP);
    }

    fprintf(stderr, "usage: lanefuzz (--agree|--metamorphic|--roundtrip) [N] [SEED]\n");
    return EXIT_USAGE;
}
