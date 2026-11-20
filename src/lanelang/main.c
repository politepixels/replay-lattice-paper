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
#include "eval.h"
#include "heal.h"
#include "lattice.h"
#include "lower.h"
#include "parse.h"
#include "resolve.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ARGV_EXPECTED counts the program name, the mode and the source file path on the command
// line.
#define ARGV_EXPECTED 3

// EXIT_ACCEPT indicates a successful invocation.
#define EXIT_ACCEPT    0

// EXIT_REJECT indicates that a well-formed program violates a determinism demand.
// reproduce.sh reads this status as a lane verdict.
#define EXIT_REJECT    1

// EXIT_MALFORMED indicates a structural failure during parsing, resolution or checking.
// An undefined name or a call to an undeclared function produces this status.
#define EXIT_MALFORMED 2

// EXIT_USAGE indicates a command line error, an unreadable source file or a failure in
// the driver itself.
#define EXIT_USAGE     3

// EXIT_EVALUATION indicates that the evaluator could not execute a checked program.
#define EXIT_EVALUATION 4

// options_t is what one invocation was asked to do.
typedef struct {
    // mode holds the requested subcommand, one of "check", "run", "lower" or "dot".
    const char *mode;

    // path is the path to the source file to read.
    const char *path;
} options_t;

// report_t holds the resolved program, the computed lanes and the outcome of checking.
typedef struct {
    // resolved holds the program's DAG and the node its entry function returns.
    const resolved_t *resolved;

    // computed is the lane computed for each node, with one entry per node in the DAG.
    const lane_t *computed;

    // node_count is how many nodes the DAG holds, which is the length of `computed`.
    int node_count;

    // outcome holds the entry graph's verdict. With checking disabled, only its result
    // lane is computed.
    check_result_t outcome;

    // healed is how many producers the inference strengthened.
    int healed;

    // checking records whether the entry graph's demands were checked.
    int checking;

    // diagnostics is where diagnostics were recorded, for the modes that render them.
    diagnostic_t *diagnostics;
} report_t;

// usage writes the command line the driver accepts to stderr.
static void usage(void) {
    fprintf(stderr,
            "usage: lanelang (check|run|lower|dot) FILE\n");
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
    if (strcmp(argv[1], "check") != 0 && strcmp(argv[1], "run") != 0 &&
        strcmp(argv[1], "lower") != 0 && strcmp(argv[1], "dot") != 0) {
        return 0;
    }
    options->mode = argv[1];
    options->path = argv[2];
    return 1;
}

// checking_enabled reports whether to enforce lane and grounding demands. Execution
// always enforces them. The standalone pass checks declarations for structural errors in
// every mode.
//
// Takes options (options_t) which holds the requested command.
static int checking_enabled(const options_t *options) {
    const char *kill = getenv(LANELANG_KILL_ENV);
    return strcmp(options->mode, "run") == 0 || !(kill && strcmp(kill, "1") == 0);
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
// A producer outside the strongest demanded cone can weaken the lane of the final result
// even when every demand holds. The note explains where a stronger demand could be
// written on a binding or a producer.
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
// demand on it, while an ungrounded demand reaches an external source or opaque call
// without the waiver it needs.
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

// run_check renders the verdict for "check", reporting a refusal with its diagnostics and
// counts or printing an acceptance line.
//
// A malformed graph has no usable lane verdict. The driver checks for this before reading
// the result lane. It also renders notes on accepted programs, including declarations
// that reserve the right to read from the outside world.
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

// run_evaluate executes an accepted graph with one worker and materialised intermediates.
// It prints each result element on its own line after evaluation succeeds.
//
// Takes report (report_t) which is everything the modes read.
static int run_evaluate(const report_t *report) {
    if (check_malformed(&report->outcome) || check_rejected(&report->outcome)) {
        return run_check(report);
    }
    diagnostic_render(report->diagnostics, stderr);
    eval_result_t result = eval_run(report->resolved->dag, report->resolved->result);
    if (!result.values) {
        fprintf(stderr, "lanelang: evaluation failed\n");
        return EXIT_EVALUATION;
    }
    for (size_t i = 0; i < result.length; i++) {
        printf("%.17g\n", result.values[i]);
    }
    free(result.values);
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

// first_blame_path writes the first lane violation's traceable path into `blame`. It
// returns the number of nodes written, zero if no path was found, or -1 on allocation
// failure.
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
        if (length != 0) {
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
    int *blame = NULL;
    int blame_count = 0;
    if (report->outcome.violations > 0) {
        blame = malloc((size_t) report->node_count * sizeof *blame);
        blame_count = blame ? first_blame_path(report, blame, report->node_count) : -1;
        if (blame_count < 0) {
            free(blame);
            fputs("lanelang: out of memory tracing the blame path\n", stderr);
            return EXIT_USAGE;
        }
    }
    dag_dot(report->resolved->dag, report->computed, blame, blame_count,
            report->resolved->result, stdout);
    free(blame);
    return check_rejected(&report->outcome) ? EXIT_REJECT : EXIT_ACCEPT;
}

// dispatch_mode runs the subcommand named by `mode` using the resolved graph and its
// report. An unknown mode returns EXIT_USAGE.
//
// Takes mode (char) which is the subcommand's name.
// Takes report (report_t) which is everything the modes read.
static int dispatch_mode(const char *mode, const report_t *report) {
    if (strcmp(mode, "check") == 0) {
        return run_check(report);
    }
    if (strcmp(mode, "run") == 0) {
        return run_evaluate(report);
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
    report.checking = checking_enabled(options);
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
// A broken demand in a standalone declaration earns the same verdict as one in `main`.
// The pass checks every declaration except the entry, so it can report both lane
// violations and ungrounded demands in one run.
//
// Takes diagnostics (diagnostic_t) which is where diagnostics are collected.
// Takes found (check_result_t) which is what the pass found wrong.
static int report_declaration_refusal(diagnostic_t *diagnostics, const check_result_t *found) {
    diagnostic_render(diagnostics, stderr);
    if (found->violations > 0) {
        fprintf(stderr,
                "lanelang: %d determinism lane violation%s in a declaration checked on its own\n",
                found->violations, found->violations == 1 ? "" : "s");
    }
    if (found->ungrounded > 0) {
        fprintf(stderr, "lanelang: %d ungrounded demand%s in a declaration checked on its own\n",
                found->ungrounded, found->ungrounded == 1 ? "" : "s");
    }
    return EXIT_REJECT;
}

// run_program checks the non-entry declarations on their own, then resolves the entry.
// Structural failures take precedence. A standalone refusal stops the run before the
// entry's demands are checked when checking is enabled.
//
// Takes options (options_t) which is what this invocation was asked to do.
// Takes program (program_t) which is the parsed program.
// Takes diagnostics (diagnostic_t) which is where diagnostics are collected.
// Takes declaring (diagnostic_t) which is where the standalone pass reports,
// `diagnostics` itself when checking is enabled.
static int run_program(const options_t *options, program_t *program, diagnostic_t *diagnostics,
                       diagnostic_t *declaring) {
    int declaration_malformed = 0;
    check_result_t declaration_found = check_result_none();
    const int declarations_hold = resolve_verify_declarations(program, "main", declaring,
                                                              &declaration_malformed,
                                                              &declaration_found);
    if (declaration_malformed) {
        return report_malformed(declaring);
    }
    resolved_t resolved = resolve_program(program, "main", diagnostics);
    int rc;
    if (resolved.status != RESOLVE_OK) {
        rc = report_malformed(diagnostics);
    } else if (!declarations_hold && checking_enabled(options)) {
        rc = report_declaration_refusal(declaring, &declaration_found);
    } else {
        rc = run_resolved(options, &resolved, diagnostics);
    }
    dag_free(resolved.dag);
    return rc;
}

// run_lanelang reads, parses, resolves and then runs the requested subcommand.
//
// Takes options (options_t) which is what this invocation was asked to do.
static int run_lanelang(const options_t *options) {
    size_t nul_byte;
    char *source = parse_read_source(options->path, &nul_byte);
    if (!source) {
        if (nul_byte) {
            fprintf(stderr, "lanelang: %s: NUL byte in source at byte %zu\n", options->path, nul_byte);
            return EXIT_MALFORMED;
        }
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

    const int checking = checking_enabled(options);
    diagnostic_t *declaring = checking ? diagnostics : diagnostic_new(options->path, source);
    const int rc = run_program(options, program, diagnostics, declaring);
    if (declaring != diagnostics) {
        diagnostic_free(declaring);
    }
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
