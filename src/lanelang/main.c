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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ARGV_EXPECTED is the argument count of the one command line the driver accepts: argv[0]
// the program, argv[1] the mode, argv[2] the source file.
#define ARGV_EXPECTED 3

// EXIT_ACCEPT is the program is well formed and every demand it states was satisfied.
#define EXIT_ACCEPT    0

// EXIT_REJECT is the program is well formed and violates a determinism demand.
// reproduce.sh reads this as a lane verdict, and nothing else may produce it.
#define EXIT_REJECT    1

// EXIT_MALFORMED is the program never resolved: an undefined name, a self reference, a
// call to a function that does not exist, kept apart from EXIT_REJECT so a harness can
// tell a refusal from source that never parsed.
#define EXIT_MALFORMED 2

// EXIT_USAGE is the command line was wrong, the source could not be read, or the driver
// itself failed (out of memory), which is not a verdict about a program.
#define EXIT_USAGE     3

// options_t is what one invocation was asked to do.
typedef struct {
    // mode is the subcommand: "check", "lower" or "dot".
    const char *mode;

    // path is the path to the source file to read.
    const char *path;
} options_t;

// report_t is everything the modes read: the resolved program, the lanes computed for it,
// and what the check found.
typedef struct {
    // resolved is the resolved program: its DAG and the node its entry function returns.
    const resolved_t *resolved;

    // computed is the lane computed for each node, with one entry per node in the DAG.
    const lane_t *computed;

    // node_count is how many nodes the DAG holds, which is the length of `computed`.
    int node_count;

    // outcome is what the declaration check found, or the blank result check_result_none
    // gives when the check was disabled.
    check_result_t outcome;

    // healed is how many producers the inference strengthened.
    int healed;

    // checking is whether the declaration check ran, which the acceptance line reports.
    int checking;

    // diagnostics is where diagnostics were recorded, for the modes that render them.
    diagnostic_t *diagnostics;
} report_t;

// read_whole_file reads the whole of `path` into a NUL-terminated buffer the caller must
// free. Returns NULL when the file cannot be opened, sized or read.
//
// Takes path (char) which is the file being read.
static char *read_whole_file(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        (void) fclose(file); return NULL;
    }
    const long size = ftell(file);
    if (size < 0) {
        (void) fclose(file); return NULL;
    }
    rewind(file);
    char *buffer = (char *) malloc((size_t) size + 1);
    if (!buffer) {
        (void) fclose(file); return NULL;
    }
    const size_t read_count = fread(buffer, 1, (size_t) size, file);
    buffer[read_count] = '\0';
    (void) fclose(file);
    return buffer;
}

// usage writes the command line the driver accepts to stderr.
static void usage(void) {
    fprintf(stderr,
            "usage: lanelang (check|lower|dot) FILE\n");
}

// parse_options fills `options` from the command line. Returns 0 when the command line is
// not one this driver accepts, in which case `options` holds nothing worth reading.
//
// Takes argc (int) which is the argument count.
// Takes argv (char) which is the argument vector.
// Takes options (options_t) which is what this invocation was asked to do.
static int parse_options(const int argc, char **argv, options_t *options) {
    if (argc != ARGV_EXPECTED) {
        return 0;
    }
    options->mode = argv[1];
    options->path = argv[2];
    return 1;
}

// checking_enabled reports whether the declaration check should run. The kill switch is
// the environment variable named by LANELANG_KILL_ENV, and it disables the check when set
// to "1".
static int checking_enabled(void) {
    const char *kill = getenv(LANELANG_KILL_ENV);
    return !(kill && strcmp(kill, "1") == 0);
}

// strongest_demand is the strongest demand written anywhere in `g`, or LANE_NONE when the
// source stated none.
//
// Takes g (dag_t) which is the graph being walked.
static lane_t strongest_demand(const dag_t *g) {
    lane_t strongest = LANE_NONE;
    for (int i = 0; i < dag_count((dag_t *) g); i++) {
        const lane_t demand = dag_at_const(g, i)->declared;
        if (demand == LANE_NONE) {
            continue;
        }
        if (strongest == LANE_NONE || demand < strongest) {
            strongest = demand;
        }
    }
    return strongest;
}

// report_unmet_demand says so when the program was accepted but the result is weaker than
// the strongest demand written.
//
// Every demand the program wrote was met; the result is weaker because a producer outside
// every demanded cone contributed its own lane to the join. That is a note rather than a
// violation, and it names the two changes that lift the result: qualify the binding that
// names the whole expression, which pulls the producer into a cone, or write a demand
// where the producer is.
//
// Takes report (report_t) which is everything the modes read.
// Takes result_lane (lane_t) which is the lane the result carries.
static void report_unmet_demand(const report_t *report, const lane_t result_lane) {
    const lane_t demanded = strongest_demand(report->resolved->dag);
    if (demanded == LANE_NONE || lane_satisfies(result_lane, demanded)) {
        return;
    }
    printf("  note: the strongest demand written is %s, but the result is %s. A demand covers the\n"
           "        value it is written on and everything feeding it, so a producer no demand\n"
           "        reaches keeps its own lane. Qualify the binding that names the whole\n"
           "        expression, or name the producers still outside every cone.\n",
           lane_name(demanded), lane_name(result_lane));
}

// report_refusal renders the diagnostics for a refused program and the tally beneath
// them.
//
// The two tallies stay separate because a lane violation says a value is weaker than the
// demand on it, while an ungrounded demand says the checker cannot see how the value was
// made at all, and a single count would let one hide inside the other.
//
// Takes report (report_t) which is everything the modes read.
static void report_refusal(const report_t *report) {
    diagnostic_render(report->diagnostics, stderr);
    if (report->outcome.violations > 0) {
        fprintf(stderr, "lanelang: %d determinism lane violation%s\n",
                report->outcome.violations, report->outcome.violations == 1 ? "" : "s");
    }
    if (report->outcome.ungrounded > 0) {
        fprintf(stderr, "lanelang: %d ungrounded demand%s\n",
                report->outcome.ungrounded, report->outcome.ungrounded == 1 ? "" : "s");
    }
}

// run_check renders the verdict for "check": the diagnostics and a count when the program
// was rejected, and one acceptance line otherwise.
//
// Malformed is asked first because it is the answer that there is no answer: the sweep
// sets it when an operand pointed forward or when the iteration never settled, and in
// both cases `computed` holds nothing, so reading a lane out of it and printing an
// acceptance would be a silent wrong answer. The accept path still renders, because a
// program the checker accepts may carry notes, such as a declaration that reserves a fact
// rather than stating one.
//
// Takes report (report_t) which is everything the modes read.
static int run_check(const report_t *report) {
    if (check_malformed(&report->outcome)) {
        diagnostic_render(report->diagnostics, stderr);
        fprintf(stderr, "lanelang: malformed program (structural, not a lane verdict)\n");
        return EXIT_MALFORMED;
    }
    if (check_rejected(&report->outcome)) {
        report_refusal(report);
        return EXIT_REJECT;
    }
    diagnostic_render(report->diagnostics, stderr);
    const lane_t result_lane = report->outcome.result_lane;
    printf("accepted: %d declaration%s checked, %d producer%s healed, result lane %s%s\n",
           report->outcome.checked, report->outcome.checked == 1 ? "" : "s",
           report->healed, report->healed == 1 ? "" : "s",
           lane_name(result_lane),
           report->checking ? "" : " (CHECK DISABLED)");
    if (report->checking) {
        report_unmet_demand(report, result_lane);
    }
    return EXIT_ACCEPT;
}

// run_lower prints the kernel each fold and scan lowers to. The exit code still reports a
// rejection, so a lowering cannot be read as an acceptance.
//
// Takes report (report_t) which is everything the modes read.
static int run_lower(const report_t *report) {
    lower_print(report->resolved->dag, stdout);
    return check_rejected(&report->outcome) ? EXIT_REJECT : EXIT_ACCEPT;
}

// first_blame_path is the blame path for the first declaration that was not satisfied,
// written into `blame`, which has room for `cap` node indices. The path is empty when the
// program stated no violated demand.
//
// Takes report (report_t) which is everything the modes read.
// Takes blame (int) which receives the blame path.
// Takes cap (int) which bounds how many entries may be written to `blame`.
static int first_blame_path(const report_t *report, int *blame, const int cap) {
    if (report->outcome.violations == 0) {
        return 0;
    }
    for (int i = 0; i < report->node_count; i++) {
        const dagnode_t *node = dag_at_const(report->resolved->dag, i);
        if (node->declared == LANE_NONE) {
            continue;
        }
        if (lane_satisfies(report->computed[i], node->declared)) {
            continue;
        }
        const int length = lattice_blame(report->resolved->dag, report->computed, i,
                                   report->computed[i], blame, cap);
        if (length > 0) {
            return length;
        }
    }
    return 0;
}

// run_dot emits the graph as Graphviz, with the blame path drawn bold. The exit code
// still reports a rejection, so a figure cannot be read as an acceptance.
//
// Takes report (report_t) which is everything the modes read.
static int run_dot(const report_t *report) {
    int blame[BLAME_PATH_MAX];
    const int blame_count = first_blame_path(report, blame, BLAME_PATH_MAX);
    dag_dot(report->resolved->dag, report->computed, blame, blame_count,
            report->resolved->result, stdout);
    return check_rejected(&report->outcome) ? EXIT_REJECT : EXIT_ACCEPT;
}

// dispatch_mode runs the subcommand named by `mode` over an already-checked program,
// answering EXIT_USAGE for a subcommand this driver does not have.
//
// Takes mode (char) which is the subcommand's name.
// Takes report (report_t) which is everything the modes read.
static int dispatch_mode(const char *mode, const report_t *report) {
    if (strcmp(mode, "check") == 0) {
        return run_check(report);
    }
    if (strcmp(mode, "lower") == 0) {
        return run_lower(report);
    }
    if (strcmp(mode, "dot") == 0) {
        return run_dot(report);
    }
    usage();
    return EXIT_USAGE;
}

// run_resolved heals, checks and then runs the requested subcommand over a resolved
// program.
//
// Takes options (options_t) which is what this invocation was asked to do.
// Takes resolved (resolved_t) which is the resolved program and the node it returns.
// Takes diagnostics (diagnostic_t) which is where diagnostics are collected.
static int run_resolved(const options_t *options, const resolved_t *resolved, diagnostic_t *diagnostics) {
    report_t report;
    report.resolved = resolved;
    report.node_count = dag_count(resolved->dag);
    report.healed = heal_program(resolved->dag, resolved->result);
    report.diagnostics = diagnostics;
    report.checking = checking_enabled();
    report.outcome = check_result_none();

    const int slots = report.node_count > 0 ? report.node_count : 1;
    lane_t *computed = (lane_t *) calloc((size_t) slots, sizeof(lane_t));
    if (!computed) {
        fprintf(stderr, "lanelang: out of memory\n");
        return EXIT_USAGE;
    }
    report.computed = computed;
    if (report.checking) {
        report.outcome = lattice_check(resolved->dag, resolved->result, computed, diagnostics);
    } else {
        lattice_compute_lanes(resolved->dag, report.node_count, computed);
        report.outcome.result_lane =
            resolved->result >= 0 ? computed[resolved->result] : LANE_EXACT;
    }

    const int rc = dispatch_mode(options->mode, &report);
    free(computed);
    return rc;
}

// report_malformed renders the diagnostics for a program that did not resolve, and says
// that this is a structural answer rather than a lane verdict.
//
// Takes diagnostics (diagnostic_t) which is where diagnostics are collected.
static int report_malformed(diagnostic_t *diagnostics) {
    diagnostic_render(diagnostics, stderr);
    fprintf(stderr, "lanelang: malformed program (structural, not a lane verdict)\n");
    return EXIT_MALFORMED;
}

// report_declaration_refusal renders what the standalone declaration pass found and
// returns EXIT_REJECT.
//
// A demand broken in a function no call reaches is the same lane verdict as one broken in
// `main`, so this answers EXIT_REJECT; routing it through report_malformed would claim
// the program never resolved. The pass checks every declaration the entry does not reach,
// so both counts may be non-zero in one run, and a weaker value is counted apart from an
// ungrounded demand.
//
// Takes diagnostics (diagnostic_t) which is where diagnostics are collected.
// Takes found (check_result_t) which is what the pass found wrong.
static int report_declaration_refusal(diagnostic_t *diagnostics, const check_result_t *found) {
    diagnostic_render(diagnostics, stderr);
    if (found->violations > 0) {
        fprintf(stderr,
                "lanelang: %d determinism lane violation%s in a declaration no call reaches\n",
                found->violations, found->violations == 1 ? "" : "s");
    }
    if (found->ungrounded > 0) {
        fprintf(stderr, "lanelang: %d ungrounded demand%s in a declaration no call reaches\n",
                found->ungrounded, found->ungrounded == 1 ? "" : "s");
    }
    return EXIT_REJECT;
}

// run_lanelang reads, parses, resolves and then runs the requested subcommand.
//
// Takes options (options_t) which is what this invocation was asked to do.
static int run_lanelang(const options_t *options) {
    char *source = read_whole_file(options->path);
    if (!source) {
        fprintf(stderr, "lanelang: cannot read %s\n", options->path);
        return EXIT_USAGE;
    }

    diagnostic_t *diagnostics = diagnostic_new(options->path, source);
    program_t *program = parse_program(options->path, source, diagnostics);
    if (!program) {
        diagnostic_render(diagnostics, stderr);
        diagnostic_free(diagnostics);
        free(source);
        return EXIT_MALFORMED;
    }

    int declaration_rejected = 0;
    check_result_t declaration_found = check_result_none();
    if (!resolve_verify_declarations(program, "main", diagnostics, &declaration_rejected,
                                     &declaration_found)) {
        const int rc = declaration_rejected
                           ? report_declaration_refusal(diagnostics, &declaration_found)
                           : report_malformed(diagnostics);
        ast_free(program);
        diagnostic_free(diagnostics);
        free(source);
        return rc;
    }

    resolved_t resolved = resolve_program(program, "main", diagnostics);
    const int rc = resolved.status == RESOLVE_OK ? run_resolved(options, &resolved, diagnostics)
                                                 : report_malformed(diagnostics);
    dag_free(resolved.dag);
    ast_free(program);
    diagnostic_free(diagnostics);
    free(source);
    return rc;
}

// main parses the command line and runs one invocation of the driver.
//
// Takes argc (int) which is the argument count.
// Takes argv (char) which is the argument vector.
int main(const int argc, char **argv) {
    options_t options;
    if (!parse_options(argc, argv, &options)) {
        usage();
        return EXIT_USAGE;
    }
    return run_lanelang(&options);
}
