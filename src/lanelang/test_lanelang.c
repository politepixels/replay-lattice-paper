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

    char computed[FIELD_BYTES];

    char demanded[FIELD_BYTES];

    const char *kernels[MAX_EXPECTED_KERNELS];

    int kernel_count;

    const char *forbidden_kernels[MAX_EXPECTED_KERNELS];

    int forbidden_kernel_count;

    char blame_primary[FIELD_BYTES];

    char blame_secondary[FIELD_BYTES];

    char blame_hops[FIELD_BYTES];

    char ungrounded_nodes[FIELD_BYTES];

    const char *diagnostic;

    const char *forbidden_diagnostic;
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

static int line_starts_with(const char *line, const char *prefix) {
    return strncmp(line, prefix, strlen(prefix)) == 0;
}

static int store_expect_field(expect_t *expect, const char *token, const size_t width) {
    const struct {
        const char *key;
        char *value;
    } fields[] = {
        {"computed=", expect->computed}, {"demanded=", expect->demanded},
        {"primary=", expect->blame_primary}, {"secondary=", expect->blame_secondary},
        {"hops=", expect->blame_hops}, {"ungrounded-nodes=", expect->ungrounded_nodes},
    };
    for (size_t i = 0; i < sizeof fields / sizeof fields[0]; i++) {
        const size_t key_length = strlen(fields[i].key);
        if (width <= key_length || memcmp(token, fields[i].key, key_length) != 0) {
            continue;
        }
        const char *value = token + key_length;
        const size_t length = width - key_length;
        if (expect->want == WANT_MALFORMED ||
            (expect->want == WANT_ACCEPT && fields[i].value != expect->ungrounded_nodes) ||
            fields[i].value[0] || length >= FIELD_BYTES || memchr(value, '=', length)) {
            return 0;
        }
        memcpy(fields[i].value, value, length);
        fields[i].value[length] = '\0';
        return 1;
    }
    return 0;
}

static int parse_expect_verdict(const char *line, expect_t *expect, const char *fixture) {
    const char *at = line + strspn(line, " \t\r");
    size_t width = strcspn(at, " \t\r\n");
    const size_t names = sizeof WANT_NAMES / sizeof WANT_NAMES[0];
    size_t i = 0;
    while (i < names && (strlen(WANT_NAMES[i]) != width || memcmp(at, WANT_NAMES[i], width) != 0)) {
        i++;
    }
    if (i == names) {
        const int shown = (int) (width > FIELD_BYTES ? FIELD_BYTES : width);
        fail(fixture, "unrecognised verdict '%.*s' on the // EXPECT: line", shown, at);
        return 0;
    }
    expect->want = (want_t) i;
    for (;;) {
        at += width;
        at += strspn(at, " \t\r");
        width = strcspn(at, " \t\r\n");
        if (!width) {
            return 1;
        }
        if (!store_expect_field(expect, at, width)) {
            const int shown = (int) (width > MESSAGE_FIELD_BYTES ? MESSAGE_FIELD_BYTES : width);
            fail(fixture, "invalid or repeated field '%.*s' on the // EXPECT: line", shown, at);
            return 0;
        }
    }
}

static char *trim_value(char *value) {
    value += strspn(value, " \t\r");
    size_t length = strlen(value);
    while (length && strchr(" \t\r", value[length - 1])) {
        value[--length] = '\0';
    }
    return value;
}

static int parse_assertion(char *key, expect_t *expect, const char *fixture) {
    char *colon = strchr(key, ':');
    if (!colon) {
        fail(fixture, "missing ':' after fixture directive '%s'", key);
        return 0;
    }
    *colon = '\0';
    char *value = trim_value(colon + 1);
    if (!*value) {
        fail(fixture, "empty EXPECT-%s directive", key);
        return 0;
    }

    const int forbidden = line_starts_with(key, "NO-");
    const char *kind = forbidden ? key + strlen("NO-") : key;
    if (strcmp(kind, "KERNEL") == 0) {
        int *count = forbidden ? &expect->forbidden_kernel_count : &expect->kernel_count;
        const char **names = forbidden ? expect->forbidden_kernels : expect->kernels;
        if (strpbrk(value, " \t\r") || *count == MAX_EXPECTED_KERNELS) {
            fail(fixture, "expected one kernel name and at most %d EXPECT-%s entries",
                 MAX_EXPECTED_KERNELS, key);
            return 0;
        }
        names[(*count)++] = value;
        return 1;
    }
    if (strcmp(kind, "DIAGNOSTIC") == 0) {
        const char **message = forbidden ? &expect->forbidden_diagnostic : &expect->diagnostic;
        if (*message) {
            fail(fixture, "repeated EXPECT-%s directive", key);
            return 0;
        }
        *message = value;
        return 1;
    }
    fail(fixture, "unrecognised EXPECT-%s directive", key);
    return 0;
}

static int parse_expect(char *source, expect_t *expect, const char *fixture) {
    memset(expect, 0, sizeof *expect);
    int have = 0;
    char *line = source;
    while (line) {
        char *next = strchr(line, '\n');
        if (next) {
            *next++ = '\0';
        }
        char *directive = line + strspn(line, " \t");
        line = next;
        if (!line_starts_with(directive, "//")) {
            continue;
        }
        directive += strlen("//");
        directive += strspn(directive, " \t");
        if (line_starts_with(directive, "EXPECT:")) {
            if (have) {
                fail(fixture, "more than one // EXPECT: line");
                return 0;
            }
            if (!parse_expect_verdict(directive + strlen("EXPECT:"), expect, fixture)) {
                return 0;
            }
            have = 1;
        } else if (line_starts_with(directive, "EXPECT-") &&
                   !parse_assertion(directive + strlen("EXPECT-"), expect, fixture)) {
            return 0;
        }
    }
    if (!have) {
        fail(fixture, "no // EXPECT: line");
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
    const size_t slots = count > 0 ? (size_t) count : 1;
    lane_t *computed = calloc(slots, sizeof *computed);
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

static int check_diagnostics(const char *fixture, const expect_t *expect, const diagnostic_t *diagnostics) {
    int ok = 1;
    if (expect->diagnostic) {
        cases++;
        if (!diagnostic_message_contains(diagnostics, expect->diagnostic)) {
            fail(fixture, "no diagnostic held \"%s\"", expect->diagnostic);
            ok = 0;
        }
    }
    if (expect->forbidden_diagnostic) {
        cases++;
        if (diagnostic_message_contains(diagnostics, expect->forbidden_diagnostic)) {
            fail(fixture, "a diagnostic held \"%s\", which this fixture forbids",
                 expect->forbidden_diagnostic);
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
    if (!check_diagnostics(fixture, expect, diagnostics)) {
        ok = 0;
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

static void check_fixture(const char *path, const char *name, const char *source, const expect_t *expect) {
    diagnostic_t *diagnostics = diagnostic_new(path, source);
    outcome_t outcome;
    if (!diagnostics || !run_checker(path, source, diagnostics, &outcome)) {
        fail(name, "out of memory");
        diagnostic_free(diagnostics);
        return;
    }

    const int ok = check_expectation(name, expect, &outcome, diagnostics);
    printf("%-28s %-10s %s\n", name, WANT_NAMES[outcome.got], ok ? "ok" : "FAIL");

    diagnostic_free(diagnostics);
}

static void run_one(const char *dir, const char *name) {
    char path[PATH_BYTES];
    snprintf(path, sizeof path, "%.700s/%.255s", dir, name);
    size_t nul_byte = 0;
    char *source = parse_read_source(path, &nul_byte);
    if (!source) {
        if (nul_byte) {
            fail(name, "NUL byte in fixture at byte %zu", nul_byte);
        } else {
            fail(name, "cannot read the whole fixture");
        }
        return;
    }

    const size_t bytes = strlen(source) + 1;
    char *directives = malloc(bytes);
    if (!directives) {
        fail(name, "out of memory");
    } else {
        memcpy(directives, source, bytes);
        expect_t expect;
        if (parse_expect(directives, &expect, name)) {
            check_fixture(path, name, source, &expect);
        }
        free(directives);
    }
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
