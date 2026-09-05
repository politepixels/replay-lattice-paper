#!/usr/bin/env bash
# Copyright 2026 PolitePixels Limited
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

# shellcheck source=lib/receipts.sh
. ./scripts/lib/receipts.sh

# Where the development lives. Overridable so a must-fail control can point the gate at a
# deliberately broken copy without touching the shipped one.
PROOF_DIR="${RL_PROOF_DIR:-src/proofs}"

# The complete list of parameters the mechanised results may rest on: five of the sibling
# development's eight, and the two primitive float operations the reassociation witness
# evaluates. Anything beyond it fails the audit.
EXPECTED_AXIOMS="Gated add bits eqb eval schedule world"

# The scratch directory holding the build and audit logs, removed on a trap.
WORK=""

# How many assertions this gate has made so far.
cases=0

# say prints the machine-readable receipt and exits with the code the protocol gives it.
# Arguments:
#   $1 - PASS, SKIP or FAIL
#   $2 - How many cases the gate asserted
#   $3 - How many it declined
#   $4 - The reason token
# Outputs:
#   The RL-STATUS line on stdout
# Returns:
#   Exits 0 for PASS, 77 for SKIP and 1 otherwise
say() {
    printf 'RL-STATUS leg=L2 result=%s cases=%s skipped=%s reason=%s\n' "$1" "$2" "$3" "$4"
    case "$1" in
        PASS) exit 0 ;;
        SKIP) exit 77 ;;
        *)    exit 1 ;;
    esac
}

# require_coq skips the gate when no Coq compiler is installed, because a check that
# cannot run must say so and never report PASS.
# Outputs:
#   The SKIP receipt when coqc is absent
require_coq() {
    command -v coqc >/dev/null 2>&1 || say SKIP 0 1 no-coq
}

# compile_developments compiles the abstract development and then the evaluator
# development on top of it, failing the gate at the first that does not build.
# Globals:
#   WORK - Read
# Outputs:
#   The indented build log on stderr when compilation fails
compile_developments() {
    if ! coqc -Q . "" ReplayLattice.v >"$WORK/build.log" 2>&1; then
        sed 's/^/    /' "$WORK/build.log" >&2
        say FAIL 0 0 sibling-does-not-compile
    fi
    if ! coqc -Q . "" ReplayLatticeEval.v >>"$WORK/build.log" 2>&1; then
        sed 's/^/    /' "$WORK/build.log" >&2
        say FAIL 0 0 proof-does-not-compile
    fi
}

# write_assumptions_query writes the audit script: one Print Assumptions per row of the
# operation-class table, plus the witnesses that the model can tell schedules apart at
# all, listed deliberately because invariance rows with no way to fail assert nothing.
# Globals:
#   WORK - Read
write_assumptions_query() {
    cat >"$WORK/assumptions.v" <<'EOF'
Require Import ReplayLatticeEval.
Print Assumptions row_fixed_tree_reduction.
Print Assumptions row_data_movement.
Print Assumptions row_carried_recurrence.
Print Assumptions row_fixed_block_scan.
Print Assumptions row_oracle_order_matches_reference.
Print Assumptions row_elementwise.
Print Assumptions row_exact_integer.
Print Assumptions row_exact_integer_at_Z.
Print Assumptions row_external_source_at_fixed_world.
Print Assumptions approx_rows_are_vacuous.
Print Assumptions witness_scan_by_workers_moves.
Print Assumptions witness_free_reduction_moves.
Print Assumptions witness_float_reassociation_moves.
Print Assumptions witness_tree_by_workers_moves.
Print Assumptions witness_move_by_workers_moves.
Print Assumptions witness_recurrence_by_workers_moves.
Print Assumptions witness_source_by_workers_moves.
Print Assumptions witness_elementwise_by_workers_moves.
Print Assumptions demo_prog_wf.
Print Assumptions demo_assumption1.
Print Assumptions mgated_separates.
EOF
}

# run_assumptions_query asks Coq what every audited result rests on, failing the gate
# when the query itself does not run.
# Globals:
#   WORK - Read
#   cases - Read
# Outputs:
#   The indented query log on stderr when the query fails
run_assumptions_query() {
    if ! coqc -Q . "" "$WORK/assumptions.v" >"$WORK/assumptions.log" 2>&1; then
        sed 's/^/    /' "$WORK/assumptions.log" >&2
        say FAIL "$cases" 0 assumptions-query-failed
    fi
}

# check_axiom_set asserts that the set of axioms observed across every audited result is
# exactly the declared set. Qualified names are reduced to their last component, because
# a result here reports `ReplayLattice.eval` where its sibling reports `eval` and the two
# are the same parameter.
# Globals:
#   WORK, EXPECTED_AXIOMS - Read
#   cases - Incremented when the assertion holds
# Outputs:
#   The two sets on stderr when they differ
check_axiom_set() {
    local observed expected
    observed="$(awk '/^Axioms:/ {inblock=1; next}
                     /^Closed under/ {inblock=0}
                     inblock && /^[A-Za-z_][A-Za-z0-9_.'"'"']* :/ {
                         n = split($1, parts, "."); print parts[n]
                     }' "$WORK/assumptions.log" | sort -u | tr '\n' ' ')"
    observed="${observed% }"
    # shellcheck disable=SC2086
    expected="$(printf '%s\n' $EXPECTED_AXIOMS | sort -u | tr '\n' ' ')"
    expected="${expected% }"
    if [ "$observed" != "$expected" ]; then
        printf '    expected axioms: %s\n    observed axioms: %s\n' "$expected" "$observed" >&2
        say FAIL "$cases" 0 unexpected-axiom-set
    fi
    cases=$((cases + 1))
}

# check_axiom_free_count asserts, as an equality rather than a floor, how many audited
# results rest on nothing at all; raise the number in the same edit that closes a result.
# Globals:
#   WORK - Read
#   cases - Incremented when the assertion holds
# Outputs:
#   The observed count on stderr when it is not exactly 19
check_axiom_free_count() {
    local closed_count
    closed_count="$(grep -c '^Closed under the global context' "$WORK/assumptions.log")"
    if [ "$closed_count" -ne 19 ]; then
        printf '    expected exactly 19 axiom-free results, saw %s\n' "$closed_count" >&2
        say FAIL "$cases" 0 rows-not-axiom-free
    fi
    cases=$((cases + 1))
}

# main runs the audit in order: build both developments, query, then the two assertions.
# Globals:
#   WORK - Set
#   PROOF_DIR - Read
# Outputs:
#   The receipt on stdout
main() {
    require_coq
    WORK="$(rl_make_work_directory replay-lattice-proofs-eval)" || exit 1
    trap 'rm -rf "$WORK"' EXIT INT TERM
    cd "$PROOF_DIR" || say FAIL 0 0 no-proof-dir
    compile_developments
    write_assumptions_query
    run_assumptions_query
    check_axiom_set
    check_axiom_free_count
    say PASS "$cases" 0 -
}

main "$@"
