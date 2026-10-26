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
#include "diagnostic.h"
#include "heal.h"
#include "lattice.h"
#include "lower.h"
#include "parse.h"
#include "resolve.h"
#include "status.h"

#include <ctype.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_FIXTURE_DIR "../tests/lanelang"

#define MAX_FIXTURES 256

#define EXPECTED_FIXTURE_COUNT 171

#define NAME_BYTES 256

#define PATH_BYTES 1024

#define LINE_BYTES 512

#define FIELD_BYTES 16

#define MESSAGE_FIELD_BYTES 160

#define MAX_EXPECTED_KERNELS 4

#define MAX_SEEN_KERNELS 8

#define KERNEL_NAME_BYTES 24

static long cases = 0;

static long failures = 0;

typedef enum {
    WANT_ACCEPT,

    WANT_REJECT,

    WANT_MALFORMED
} want_t;

typedef struct {
    want_t want;

    char bad_verdict[FIELD_BYTES];

    char bad_key[FIELD_BYTES];

    int dropped_directive;

    char computed[FIELD_BYTES];

    char demanded[FIELD_BYTES];

    char kernels[MAX_EXPECTED_KERNELS][KERNEL_NAME_BYTES];

    int kernel_count;

    char forbidden_kernels[MAX_EXPECTED_KERNELS][KERNEL_NAME_BYTES];

    int forbidden_kernel_count;

    char blame_primary[FIELD_BYTES];

    char blame_secondary[FIELD_BYTES];

    char blame_hops[FIELD_BYTES];

    char ungrounded_nodes[FIELD_BYTES];

    char diagnostic[MESSAGE_FIELD_BYTES];

    char forbidden_diagnostic[MESSAGE_FIELD_BYTES];
} expect_t;

typedef struct {
    want_t got;

    char computed[FIELD_BYTES];

    char demanded[FIELD_BYTES];

    char kernels[MAX_SEEN_KERNELS][KERNEL_NAME_BYTES];

    int kernel_count;

    int kernels_overflowed;

    int passes;

    int has_carried;

    int carried_depth;

    int lane_climbed;

    char ungrounded_nodes[FIELD_BYTES];
} outcome_t;

static const char *const WANT_NAMES[] = { "accept", "reject", "malformed" };

static void fail(const char *fixture, const char *fmt, ...) {
    va_list arguments;
    failures++;
    fprintf(stderr, "  FAIL %s: ", fixture);
    va_start(arguments, fmt);
    vfprintf(stderr, fmt, arguments);
    va_end(arguments);
    fputc('\n', stderr);
}

static char *read_file(const char *path) {
    FILE *stream = fopen(path, "rb");
    if (!stream) {
        return NULL;
    }
    if (fseek(stream, 0, SEEK_END) != 0) {
        (void) fclose(stream); return NULL;
    }
    const long size = ftell(stream);
    if (size < 0) {
        (void) fclose(stream); return NULL;
    }
    rewind(stream);
    char *text = (char *) malloc((size_t) size + 1);
    if (!text) {
        (void) fclose(stream); return NULL;
    }
    const size_t got = fread(text, 1, (size_t) size, stream);
    text[got] = '\0';
    (void) fclose(stream);
    return text;
}

static void read_field(const char *line, const char *key, char *out, const size_t capacity) {
    const char *at = strstr(line, key);
    out[0] = '\0';
    if (!at) {
        return;
    }
    at += strlen(key);
    size_t i = 0;
    while (*at && *at != ' ' && *at != '\n' && i + 1 < capacity) {
        out[i++] = *at++;
    }
    out[i] = '\0';
}

static int line_starts_with(const char *line, const char *prefix) {
    return strncmp(line, prefix, strlen(prefix)) == 0;
}

static const char *copy_line(const char *at, char *line, const size_t capacity) {
    const char *newline = strchr(at, '\n');
    size_t length = newline ? (size_t) (newline - at) : strlen(at);
    if (length > capacity - 1) {
        length = capacity - 1;
    }
    memcpy(line, at, length);
    line[length] = '\0';
    return newline ? newline + 1 : NULL;
}

static const char *const EXPECT_KEYS[] = {
    "computed=", "demanded=", "primary=", "secondary=", "hops=", "ungrounded-nodes=",
};

static void find_unknown_key(const char *line, char *out, const size_t capacity) {
    out[0] = '\0';
    for (const char *at = line; *at; at++) {
        if (*at != '=' || at == line) {
            continue;
        }
        const char *start = at;
        while (start > line && (isalnum((unsigned char) start[-1]) || start[-1] == '-')) {
            start--;
        }
        if (start == at) {
            continue;
        }
        const size_t length = (size_t) (at - start) + 1;
        int known = 0;
        for (size_t k = 0; k < sizeof EXPECT_KEYS / sizeof EXPECT_KEYS[0]; k++) {
            known |= strlen(EXPECT_KEYS[k]) == length && strncmp(start, EXPECT_KEYS[k], length) == 0;
        }
        if (!known) {
            const size_t copied = length < capacity - 1 ? length : capacity - 1;
            memcpy(out, start, copied);
            out[copied] = '\0';
            return;
        }
    }
}

static void parse_expect_verdict(const char *line, expect_t *expect) {
    if (strstr(line, "accept")) {
        expect->want = WANT_ACCEPT;
    }
    else if (strstr(line, "malformed")) {
        expect->want = WANT_MALFORMED;
    }
    else if (strstr(line, "reject")) {
        expect->want = WANT_REJECT;
    }
    else {
        read_field(line, "EXPECT: ", expect->bad_verdict, sizeof expect->bad_verdict);
        if (!expect->bad_verdict[0]) {
            snprintf(expect->bad_verdict, sizeof expect->bad_verdict, "(empty)");
        }
    }

    read_field(line, "computed=", expect->computed, sizeof expect->computed);
    read_field(line, "demanded=", expect->demanded, sizeof expect->demanded);
    read_field(line, "primary=", expect->blame_primary, sizeof expect->blame_primary);
    read_field(line, "secondary=", expect->blame_secondary, sizeof expect->blame_secondary);
    read_field(line, "hops=", expect->blame_hops, sizeof expect->blame_hops);
    read_field(line, "ungrounded-nodes=", expect->ungrounded_nodes, sizeof expect->ungrounded_nodes);
    find_unknown_key(line, expect->bad_key, sizeof expect->bad_key);
}

static void parse_expect_diagnostic(const char *line, const char *key, char *out) {
    const char *at = strstr(line, key);
    if (!at) {
        return;
    }
    at += strlen(key);
    while (*at == ' ') {
        at++;
    }
    size_t i = 0;
    while (*at && *at != '\n' && i + 1 < (size_t) MESSAGE_FIELD_BYTES) {
        out[i++] = *at++;
    }
    while (i > 0 && (out[i - 1] == ' ' || out[i - 1] == '\r')) {
        i--;
    }
    out[i] = '\0';
}

static void parse_expect_kernel(
    const char *line,
    const char *key,
    char list[][KERNEL_NAME_BYTES],
    int *count
) {
    read_field(line, key, list[*count], KERNEL_NAME_BYTES);
    if (list[*count][0]) {
        (*count)++;
    }
}

static int parse_expect(const char *source, expect_t *expect) {
    memset(expect, 0, sizeof *expect);
    int have = 0;
    const char *at = source;

    while (at && *at) {
        char line[LINE_BYTES];
        at = copy_line(at, line, sizeof line);

        if (line_starts_with(line, "// EXPECT:")) {
            parse_expect_verdict(line, expect);
            have = 1;
        } else if (line_starts_with(line, "// EXPECT-KERNEL:")) {
            if (expect->kernel_count >= MAX_EXPECTED_KERNELS) {
                expect->dropped_directive = 1;
            }
            else {
                parse_expect_kernel(line, "EXPECT-KERNEL: ", expect->kernels, &expect->kernel_count);
            }
        } else if (line_starts_with(line, "// EXPECT-NO-KERNEL:")) {
            if (expect->forbidden_kernel_count >= MAX_EXPECTED_KERNELS) {
                expect->dropped_directive = 1;
            }
            else {
                parse_expect_kernel(line, "EXPECT-NO-KERNEL: ", expect->forbidden_kernels,
                                    &expect->forbidden_kernel_count);
            }
        } else if (line_starts_with(line, "// EXPECT-DIAGNOSTIC:")) {
            parse_expect_diagnostic(line, "EXPECT-DIAGNOSTIC:", expect->diagnostic);
        } else if (line_starts_with(line, "// EXPECT-NO-DIAGNOSTIC:")) {
            parse_expect_diagnostic(line, "EXPECT-NO-DIAGNOSTIC:", expect->forbidden_diagnostic);
        }
    }
    return have;
}

static void record_first_violation(const dag_t *dag, const lane_t *computed, outcome_t *outcome) {
    const int count = dag_count(dag);
    for (int i = 0; i < count; i++) {
        const dagnode_t *node = dag_at_const(dag, i);
        if (node->declared != LANE_NONE && !lane_satisfies(computed[i], node->declared)) {
            snprintf(outcome->computed, sizeof outcome->computed, "%s", lane_name(computed[i]));
            snprintf(outcome->demanded, sizeof outcome->demanded, "%s", lane_name(node->declared));
            return;
        }
    }
}

static void collect_kernels(const dag_t *dag, outcome_t *outcome) {
    const int count = dag_count(dag);
    for (int i = 0; i < count; i++) {
        const char *kernel = lower_kernel_for(dag_at_const(dag, i));
        if (!kernel) {
            continue;
        }
        if (outcome->kernel_count >= MAX_SEEN_KERNELS) {
            outcome->kernels_overflowed = 1;
            return;
        }
        snprintf(outcome->kernels[outcome->kernel_count], KERNEL_NAME_BYTES, "%s", kernel);
        outcome->kernel_count++;
    }
}

static int check_resolved(const resolved_t *resolved, diagnostic_t *diagnostics, outcome_t *outcome) {
    heal_program(resolved->dag, resolved->result);

    const int count = dag_count(resolved->dag);
    lane_t *computed = (lane_t *) calloc((size_t) (count ? count : 1), sizeof(lane_t));
    if (!computed) {
        return 0;
    }
    check_result_t swept = lattice_check(resolved->dag, resolved->result, computed, diagnostics);

    outcome->passes = swept.passes;
    outcome->carried_depth = swept.carried_depth;
    snprintf(outcome->ungrounded_nodes, sizeof outcome->ungrounded_nodes, "%d", swept.ungrounded_nodes);
    outcome->has_carried = 0;
    outcome->lane_climbed = 0;
    for (int i = 0; i < count; i++) {
        if (dag_at_const(resolved->dag, i)->carried >= 0) {
            outcome->has_carried = 1;
        }
        if (computed[i] != LANE_EXACT) {
            outcome->lane_climbed = 1;
        }
    }

    if (check_malformed(&swept)) {
        outcome->got = WANT_MALFORMED;
    } else if (check_rejected(&swept)) {
        outcome->got = WANT_REJECT;
        record_first_violation(resolved->dag, computed, outcome);
    }
    collect_kernels(resolved->dag, outcome);
    free(computed);
    return 1;
}

static int run_checker(
    const char *path,
    const char *source,
    diagnostic_t *diagnostics,
    outcome_t *outcome
) {
    memset(outcome, 0, sizeof *outcome);
    outcome->got = WANT_ACCEPT;

    program_t *program = parse_program(path, source, diagnostics);
    if (!program) {
        outcome->got = WANT_MALFORMED;
        return 1;
    }

    int ok = 1;
    int declaration_malformed = 0;
    check_result_t declaration_found = check_result_none();
    const int declarations_hold = resolve_verify_declarations(program, "main", diagnostics,
                                                              &declaration_malformed,
                                                              &declaration_found);
    if (declaration_malformed) {
        outcome->got = WANT_MALFORMED;
        ast_free(program);
        return 1;
    }
    resolved_t resolved = resolve_program(program, "main", diagnostics);
    if (resolved.status != RESOLVE_OK) {
        outcome->got = WANT_MALFORMED;
    } else {
        ok = check_resolved(&resolved, diagnostics, outcome);
        if (!declarations_hold && outcome->got != WANT_MALFORMED) {
            outcome->got = WANT_REJECT;
        }
    }
    dag_free(resolved.dag);
    ast_free(program);
    return ok;
}

static int check_text_field(
    const char *fixture,
    const char *what,
    const char *expected,
    const char *actual
) {
    if (!expected[0]) {
        return 1;
    }
    cases++;
    if (strcmp(expected, actual) == 0) {
        return 1;
    }
    fail(fixture, "%s: want %s, got %s", what, expected, actual);
    return 0;
}

static int check_lanes(const char *fixture, const expect_t *expect, const outcome_t *outcome) {
    const int computed_ok = check_text_field(fixture, "computed lane", expect->computed,
                                       outcome->computed);
    const int demanded_ok = check_text_field(fixture, "demanded lane", expect->demanded,
                                       outcome->demanded);
    return computed_ok && demanded_ok;
}

static int check_passes(const char *fixture, const outcome_t *outcome) {
    int ok = 1;
    const int ceiling = outcome->carried_depth + LANE_CHAIN_HEIGHT - 1;

    cases++;
    if (outcome->passes < 1 || outcome->passes > ceiling) {
        fail(fixture, "lane passes: want 1 to %d at carried depth %d, got %d",
             ceiling, outcome->carried_depth, outcome->passes);
        ok = 0;
    }

    if (!outcome->has_carried) {
        cases++;
        if (outcome->passes != 1) {
            fail(fixture, "lane passes without a carried edge: want 1, got %d", outcome->passes);
            ok = 0;
        }
    } else if (!outcome->lane_climbed) {
        cases++;
        if (outcome->passes != 1) {
            fail(fixture, "lane passes: want 1 where no lane left the seed, got %d",
                 outcome->passes);
            ok = 0;
        }
    }

    return ok;
}

static int check_blame(const char *fixture, const expect_t *expect, const diagnostic_t *diagnostics) {
    if (!expect->blame_primary[0] && !expect->blame_secondary[0] && !expect->blame_hops[0]) {
        return 1;
    }

    span_t primary = SPAN_NONE;
    span_t secondary = SPAN_NONE;
    int hops = -1;
    if (!diagnostic_first_spans(diagnostics, &primary, &secondary, &hops)) {
        cases++;
        fail(fixture, "expected a two-span diagnostic; the checker reported none");
        return 0;
    }

    char primary_text[FIELD_BYTES];
    char secondary_text[FIELD_BYTES];
    char hops_text[FIELD_BYTES];
    snprintf(primary_text, sizeof primary_text, "%d:%d", primary.line, primary.column);
    snprintf(secondary_text, sizeof secondary_text, "%d:%d", secondary.line, secondary.column);
    snprintf(hops_text, sizeof hops_text, "%d", hops);

    const int primary_ok = check_text_field(fixture, "blame primary span", expect->blame_primary,
                                      primary_text);
    const int secondary_ok = check_text_field(fixture, "blame secondary span", expect->blame_secondary,
                                        secondary_text);
    const int hops_ok = check_text_field(fixture, "blame distance in hops", expect->blame_hops,
                                   hops_text);
    return primary_ok && secondary_ok && hops_ok;
}

static int kernel_seen(const outcome_t *outcome, const char *kernel) {
    for (int i = 0; i < outcome->kernel_count; i++) {
        if (strcmp(outcome->kernels[i], kernel) == 0) {
            return 1;
        }
    }
    return 0;
}

static int check_kernels(const char *fixture, const expect_t *expect, const outcome_t *outcome) {
    int ok = 1;
    if (outcome->kernels_overflowed && (expect->kernel_count > 0 || expect->forbidden_kernel_count > 0)) {
        cases++;
        fail(fixture, "the lowering picked more than %d kernels, so a kernel assertion here could "
                      "only pass; raise MAX_SEEN_KERNELS", MAX_SEEN_KERNELS);
        ok = 0;
    }
    for (int i = 0; i < expect->kernel_count; i++) {
        cases++;
        if (!kernel_seen(outcome, expect->kernels[i])) {
            fail(fixture, "expected kernel %s, not emitted", expect->kernels[i]);
            ok = 0;
        }
    }
    for (int i = 0; i < expect->forbidden_kernel_count; i++) {
        cases++;
        if (kernel_seen(outcome, expect->forbidden_kernels[i])) {
            fail(fixture, "kernel %s must NOT be emitted", expect->forbidden_kernels[i]);
            ok = 0;
        }
    }
    return ok;
}

static int check_expectation(
    const char *fixture,
    const expect_t *expect,
    const outcome_t *outcome,
    const diagnostic_t *diagnostics
) {
    int ok = 1;
    cases++;
    if (outcome->got != expect->want) {
        fail(fixture, "want %s, got %s", WANT_NAMES[expect->want], WANT_NAMES[outcome->got]);
        ok = 0;
    }
    if (expect->want == WANT_REJECT && outcome->got == WANT_REJECT) {
        if (!check_lanes(fixture, expect, outcome)) {
            ok = 0;
        }
        if (!check_blame(fixture, expect, diagnostics)) {
            ok = 0;
        }
    }
    if (expect->diagnostic[0]) {
        cases++;
        if (!diagnostic_message_contains(diagnostics, expect->diagnostic)) {
            fail(fixture, "no diagnostic held \"%s\"", expect->diagnostic);
            ok = 0;
        }
    }
    if (expect->forbidden_diagnostic[0]) {
        cases++;
        if (diagnostic_message_contains(diagnostics, expect->forbidden_diagnostic)) {
            fail(fixture, "a diagnostic held \"%s\", which this fixture forbids",
                 expect->forbidden_diagnostic);
            ok = 0;
        }
    }
    if (!check_kernels(fixture, expect, outcome)) {
        ok = 0;
    }
    if (outcome->got != WANT_MALFORMED) {
        if (!check_passes(fixture, outcome)) {
            ok = 0;
        }
        if (!check_text_field(fixture, "ungrounded nodes", expect->ungrounded_nodes,
                        outcome->ungrounded_nodes)) {
            ok = 0;
        }
    }
    return ok;
}

static void run_one(const char *dir, const char *name) {
    char path[PATH_BYTES];
    snprintf(path, sizeof path, "%.700s/%.255s", dir, name);
    char *source = read_file(path);
    if (!source) {
        fail(name, "cannot read");
        return;
    }

    expect_t expect;
    if (!parse_expect(source, &expect)) {
        fail(name, "no // EXPECT: line");
        free(source);
        return;
    }
    if (expect.bad_verdict[0]) {
        fail(name, "unrecognised verdict '%s' on the // EXPECT: line", expect.bad_verdict);
        free(source);
        return;
    }
    if (expect.dropped_directive) {
        cases++;
        fail(name, "more than %d kernel directives of one kind; the extra ones were read and not "
                   "stored, so they assert nothing. Raise MAX_EXPECTED_KERNELS", MAX_EXPECTED_KERNELS);
        free(source);
        return;
    }
    if (expect.bad_key[0]) {
        fail(name, "'%s' on the // EXPECT: line is not a field this suite reads, so it asserts "
                   "nothing; the kernel directives are EXPECT-KERNEL: and EXPECT-NO-KERNEL: on "
                   "their own lines", expect.bad_key);
        free(source);
        return;
    }

    diagnostic_t *diagnostics = diagnostic_new(path, source);
    outcome_t outcome;
    if (!run_checker(path, source, diagnostics, &outcome)) {
        fail(name, "out of memory");
        diagnostic_free(diagnostics);
        free(source);
        return;
    }

    const int ok = check_expectation(name, &expect, &outcome, diagnostics);
    printf("%-28s %-10s %s\n", name, WANT_NAMES[outcome.got], ok ? "ok" : "FAIL");

    diagnostic_free(diagnostics);
    free(source);
}

static int collect_fixture_names(const char *dir, char names[][NAME_BYTES]) {
    DIR *directory = opendir(dir);
    if (!directory) {
        fprintf(stderr, "cannot open fixture directory %s\n", dir);
        return -1;
    }

    int count = 0;
    int seen = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        const size_t length = strlen(entry->d_name);
        if (length > 5 && strcmp(entry->d_name + length - 5, ".lane") == 0) {
            seen++;
            if (count < MAX_FIXTURES) {
                snprintf(names[count], NAME_BYTES, "%s", entry->d_name);
                count++;
            }
        }
    }
    closedir(directory);
    if (seen > count) {
        fprintf(stderr,
                "the fixture directory holds %d .lane file(s) and this harness can run %d. "
                "The rest were not read and assert nothing. Raise MAX_FIXTURES\n",
                seen, MAX_FIXTURES);
        return -2;
    }
    return count;
}

static void sort_names(char names[][NAME_BYTES], const int count) {
    for (int i = 1; i < count; i++) {
        char held[NAME_BYTES];
        memmove(held, names[i], NAME_BYTES);
        int j = i - 1;
        while (j >= 0 && strcmp(names[j], held) > 0) {
            memmove(names[j + 1], names[j], NAME_BYTES);
            j--;
        }
        memmove(names[j + 1], held, NAME_BYTES);
    }
}

int main(const int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    rl_build_stamp("test_lanelang");
    const char *dir = argc > 1 ? argv[1] : DEFAULT_FIXTURE_DIR;

    char names[MAX_FIXTURES][NAME_BYTES];
    const int count = collect_fixture_names(dir, names);
    if (count == -2) {
        return RL_FAIL("L3", 0, "fixtures-over-capacity");
    }
    if (count < 0) {
        return RL_FAIL("L3", 0, "no-fixture-dir");
    }

    sort_names(names, count);
    for (int i = 0; i < count; i++) {
        run_one(dir, names[i]);
    }

    cases++;
    if (count != EXPECTED_FIXTURE_COUNT) {
        fail("(suite)", "fixture count: want %d, got %d", EXPECTED_FIXTURE_COUNT, count);
    }

    cases++;
    const int demand_free = lower_kernel_demand_free_cases();
    if (demand_free <= 0) {
        fail("(suite)", "kernel selection reads the demand: the discharge must follow the operation");
    } else {
        cases += demand_free;
    }

    printf("\n%d fixtures, %ld assertions, %ld failed\n", count, cases, failures);
    if (count == 0) {
        return RL_FAIL("L3", 0, "no-fixtures-found");
    }
    return failures
           ? RL_FAIL("L3", cases, "fixture-expectations-unmet")
           : RL_PASS("L3", cases);
}
