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

#ifndef LANELANG_RESOLVE_H
#define LANELANG_RESOLVE_H

#include "ast.h"
#include "dag.h"
#include "diagnostic.h"
#include "lattice.h"

// resolve_status_t is whether a program lowered, and if not, what kind of failure it was.
typedef enum {
    // RESOLVE_OK is the program lowered, the graph and the result index both usable.
    RESOLVE_OK = 0,

    // RESOLVE_MALFORMED is a structural defect, an undefined name, a self reference or a
    // call to a function that does not exist, kept strictly separate from a lane verdict
    // because "your program is not well formed" and "your program violates a determinism
    // demand" are different answers.
    RESOLVE_MALFORMED = 1
} resolve_status_t;

// resolved_t is what one lowering produced.
typedef struct {
    // status is whether the program lowered.
    resolve_status_t status;

    // dag is the lowered graph, owned by the caller and released with dag_free, or NULL
    // when it could not be allocated.
    dag_t *dag;

    // result is the node index of the entry function's returned value, or -1 when there
    // is none.
    int result;
} resolved_t;

// resolve_program lowers `entry` and everything it calls into one DAG.
//
// Takes program (program_t) which is the parsed program.
// Takes entry (char) which names the function to lower.
// Takes diagnostics (diagnostic_t) which is where diagnostics are collected.
resolved_t resolve_program(program_t *program, const char *entry, diagnostic_t *diagnostics);

// resolve_verify_declarations lowers, heals and lane-checks every function the entry path
// would not reach, so a published return lane and every demand written inside a body are
// verified whether or not anything calls them.
//
// Takes program (program_t) which is the parsed program.
// Takes entry (char) which is the entry function's name, skipped because it is lowered
// anyway.
// Takes diagnostics (diagnostic_t) which is where a refusal is reported.
// Takes rejected (int) which is set when a body violated a demand rather than failing to
// resolve, so the caller reports a lane verdict rather than a structural one.
// Takes found (check_result_t) which receives what was wrong, the two kinds of refusal
// counted apart.
// Returns 1 when each one checks, 0 when one did not.
int resolve_verify_declarations(program_t *program, const char *entry, diagnostic_t *diagnostics,
                                int *rejected, check_result_t *found);

#endif  // LANELANG_RESOLVE_H
