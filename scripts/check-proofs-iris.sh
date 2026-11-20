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

# The files the concurrent proof needs, in dependency order.
PROOFS="ReplayLattice ReplayLatticeEval ConcurrentReplay"

# The scratch directory holding the copied development, the queries and their logs,
# removed on a trap.
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
    printf 'RL-STATUS leg=iris result=%s cases=%s skipped=%s reason=%s\n' "$1" "$2" "$3" "$4"
    case "$1" in
        PASS) exit 0 ;;
        SKIP) exit 77 ;;
        *)    exit 1 ;;
    esac
}

# require_rocq skips the gate when no Rocq compiler is installed, because a check that
# cannot run must say so and never report PASS.
# Outputs:
#   The SKIP receipt when rocq is absent
require_rocq() {
    command -v rocq >/dev/null 2>&1 || say SKIP 0 1 no-rocq
}

# copy_developments copies the proof files into the scratch directory, because the build
# writes its outputs beside them.
# Globals:
#   WORK, PROOF_DIR, PROOFS - Read
# Outputs:
#   The FAIL receipt when a file is missing
copy_developments() {
    local proof
    for proof in $PROOFS; do
        cp "$PROOF_DIR/$proof.v" "$WORK/" || say FAIL 0 0 no-proof-dir
    done
}

# compile_developments builds the lattice, the evaluator model and the concurrent proof in
# dependency order.
# Globals:
#   WORK, PROOFS - Read
# Outputs:
#   The indented build log on stderr when compilation fails
compile_developments() {
    local proof
    for proof in $PROOFS; do
        if ! rocq compile -Q . "" "$proof.v" >>"$WORK/build.log" 2>&1; then
            sed 's/^/    /' "$WORK/build.log" >&2
            say FAIL 0 0 proof-does-not-compile
        fi
    done
}

# write_audit_query states the four claims about the concurrent replay sum as one
# definition, so that its type records them, and asks for the assumptions it rests on.
# Globals:
#   WORK - Read
write_audit_query() {
    cat >"$WORK/Audit.v" <<'EOF'
From Stdlib Require Import Floats.
From iris.program_logic Require Import adequacy.
Require Import ReplayLattice ReplayLatticeEval ConcurrentReplay.
Import List.ListNotations.

Definition audited :
  (forall workers xs m, exists children, advance (replay_sum workers xs) m =
     Some (Join 0 workers 0%float, m, children) /\ length children = workers) /\
  (forall workers xs s, 0 < workers ->
     adequate NotStuck (replay_sum workers xs) empty_slots
       (fun v _ => v = kernel_fixed_tree PrimFloat.float PrimFloat.add 0%float s xs)) /\
  (forall s xs, mgated s -> 0 < sworkers s ->
     adequate NotStuck (replay_sum (sworkers s) xs) empty_slots
       (fun v _ => v = combine64 Replay s xs)) /\
  (forall workers xs s, 0 < workers -> exists tail m,
     rtc (@erased_step replay_language) ([replay_sum workers xs], empty_slots)
       (Return (kernel_fixed_tree PrimFloat.float PrimFloat.add 0%float s xs) :: tail, m)) :=
  conj replay_sum_forks_workers
    (conj concurrent_replay_sum (conj concurrent_replay_combine64 concurrent_replay_can_return)).

Print Assumptions audited.
EOF
}

# write_baseline_query asks for the assumptions of binary64 addition alone, which are the
# only ones the audited claims may rest on.
# Globals:
#   WORK - Read
write_baseline_query() {
    cat >"$WORK/Primitives.v" <<'EOF'
From Stdlib Require Import Floats.
From iris.program_logic Require Import adequacy.
Definition primitive_add (x y : PrimFloat.float) : PrimFloat.float := PrimFloat.add x y.
Print Assumptions primitive_add.
EOF
}

# check_claims_stated asserts that the four claims type-check against the proofs, which is
# what compiling the audit query shows.
# Globals:
#   WORK - Read
#   cases - Incremented when the assertion holds
# Outputs:
#   The indented query log on stderr when the claims do not type-check
check_claims_stated() {
    if ! rocq compile -Q . "" Audit.v >"$WORK/observed" 2>"$WORK/audit.log"; then
        sed 's/^/    /' "$WORK/audit.log" >&2
        say FAIL "$cases" 0 claims-do-not-type-check
    fi
    cases=$((cases + 1))
}

# check_assumptions_match_baseline asserts that the claims rest on exactly the assumptions
# of binary64 addition, so that no admitted lemma or extra axiom supports them.
# Globals:
#   WORK - Read
#   cases - Incremented when the assertion holds
# Outputs:
#   The indented difference on stderr when the assumptions differ
check_assumptions_match_baseline() {
    if ! rocq compile -Q . "" Primitives.v >"$WORK/expected" 2>"$WORK/baseline.log"; then
        sed 's/^/    /' "$WORK/baseline.log" >&2
        say FAIL "$cases" 0 baseline-query-failed
    fi
    if ! diff -u "$WORK/expected" "$WORK/observed" >"$WORK/assumptions.diff"; then
        sed 's/^/    /' "$WORK/assumptions.diff" >&2
        say FAIL "$cases" 0 unexpected-assumptions
    fi
    cases=$((cases + 1))
}

# main copies and builds the development, then checks that the claims hold and what they
# rest on.
# Globals:
#   WORK - Set
# Outputs:
#   The receipt on stdout
main() {
    require_rocq
    WORK="$(rl_make_work_directory replay-lattice-iris)" || exit 1
    trap 'rm -rf "$WORK"' EXIT INT TERM
    copy_developments
    cd "$WORK" || say FAIL 0 0 no-work-dir
    compile_developments
    write_audit_query
    write_baseline_query
    check_claims_stated
    check_assumptions_match_baseline
    say PASS "$cases" 0 -
}

main "$@"
