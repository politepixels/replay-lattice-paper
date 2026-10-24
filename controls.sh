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

# Usage:
#   ./controls.sh          run every control and print the verdict
#   ./controls.sh --list   print the roster and exit
#   ./controls.sh --help   print this header and exit

set -uo pipefail
cd "$(dirname "$0")" || exit 1

# shellcheck source=scripts/lib/receipts.sh
. ./scripts/lib/receipts.sh

# The repository root, resolved once so a control can reach a shipped script from inside a copy.
ROOT="$PWD"

# The scratch directory holding every control's captured output, removed on a trap.
WORK=""

# 1 when --list was passed, which prints the roster instead of running anything.
list_only=0

# How many controls fired.
fired=0

# How many controls could not make their break materialise here.
weak=0

# How many controls could not run here.
skipped=0

# How many controls made their break materialise and watched the gate accept it.
dead=0

# The roster: one row per control, as id, the check it guards, whether it breaks a build, and the
# break in one line.
CONTROLS=(
  "CC1|L3|y|the grounding judgement removed, so a demand over an external source is accepted"
  "CC2|L2|y|the fixed tree's bracketing made to follow the worker count, so the row that says its bits are a function of (data, shape) stops being true"
  "CC3|L4|y|the fixed-block scan's frozen leaf width made to follow the worker count"
  "CC4|L4|y|@exact discharged by the fixed tree rather than the oracle order: one lane, two bit patterns, and every schedule-axis gate still green"
  "CC5|L3|y|the contract removed from the value-numbering key"
  "CC6|-|n|a malformed program, which must not read as a lane violation: a direct assertion on the shipped binary"
  "CC7|L4|y|the evaluator made to fail a pinned fold after acceptance, so a run that cannot answer must read as a failure under its own token and never as a checker refusal"
  "CC8|-|n|the checker's kill switch: a direct assertion on the shipped binary"
  "CC9|L3|y|the lane qualifiers left unreserved, so a demand with no name after it binds a variable and states nothing"
  "CC10|L3|y|a blessing that marks the leaf grounded rather than excusing the declaration, so the cone is repaired and no verdict moves"
  "CC11|L3|y|the reorder dial aliased onto the demand, so order: oracle states an exact demand the source did not write"
  "CC12|L3|y|a demand stamped onto the value instead of carried on a node of its own, which is what put it in the interning key after interning"
  "CC13|L3|y|lowering reads the kernel from the demand field instead of the class fixed at resolution, so one class emits two kernels"
  "CC14|L4|y|the tile width allowed to decide the bracketing, which is reduction factoring disguised as tiling"
  "CC15|L7|y|the sweep's ordering guard reverted to a fail-open skip"
  "CC16|L9|y|a lemma inside Theorem 1's dependency cone admitted rather than proved"
  "CC17|L3|y|healing disabled"
  "CC18|L3|y|the structural restriction on @bless dropped, so a waiver binds to a value the program computes"
  "CC19|L3|y|an ungrounded declaration made to state nothing, so a value the program does not fix reads as one it does"
)

# print_usage prints this file's own header block: every comment line after the shebang, stopping at
# the first line that is not a comment.
# Outputs:
#   The header comment block on stdout
print_usage() {
    awk 'NR == 1 { next } /^#/ { print; next } { exit }' "$0"
}

# parse_arguments reads the command line into the flag globals.
# Globals:
#   list_only - Set by --list
# Arguments:
#   $@ - The command line
# Outputs:
#   The header block on stdout for --help; a complaint on stderr for anything unrecognised
# Returns:
#   Exits 0 after --help and exits 2 on an unrecognised argument, otherwise 0
parse_arguments() {
    local argument
    for argument in "$@"; do
        case "$argument" in
            --list)    list_only=1 ;;
            -h|--help) print_usage; exit 0 ;;
            *) echo "unknown argument: $argument" >&2; exit 2 ;;
        esac
    done
}

# count_roster_rows prints how many roster rows carry one value in the column that says whether a
# control breaks a build.
# Globals:
#   CONTROLS - Read
# Arguments:
#   $1 - The value to count, y or n
# Outputs:
#   The count on stdout
count_roster_rows() {
    printf '%s\n' "${CONTROLS[@]}" | cut -d'|' -f3 | grep -c "^$1\$"
}

# print_roster prints the roster as a table, with the controls that break a build counted from it.
# Globals:
#   CONTROLS - Read
# Outputs:
#   One row per control, then a sentence counting the two kinds
print_roster() {
    local row control leg breaks summary
    printf '%-4s %-5s %-9s %s\n' CTL GUARDS TARGET BREAK
    for row in "${CONTROLS[@]}"; do
        IFS='|' read -r control leg breaks summary <<<"$row"
        [ "$breaks" = y ] && breaks=a-build || breaks=shipped
        printf '%-4s %-5s %-9s %s\n' "$control" "$leg" "$breaks" "$summary"
    done
    echo
    echo "$(count_roster_rows y) break a build of the checker, the evaluator, or the proof;" \
         "$(count_roster_rows n) assert on the shipped binary."
}

# lanelang_core_sources prints the checker's core translation units, read from the one place
# that list is maintained.
#
# CONVENTIONS.md section 0 freezes the `CORE :=` line's layout, because this function parses it.
CORE_SOURCE_FLOOR=9

lanelang_core_sources() {
    local list
    list="$(sed -n 's/^CORE[[:space:]]*:=[[:space:]]*//p' "$ROOT/src/lanelang/Makefile")"
    if [ "$(printf '%s' "$list" | wc -w)" -lt "$CORE_SOURCE_FLOOR" ]; then
        return 1
    fi
    printf '%s' "$list"
}

# report prints one control's outcome row and adds it to the tally.
# Globals:
#   fired, weak, skipped, dead - Incremented according to the outcome
# Arguments:
#   $1 - The control id
#   $2 - FIRED, WEAK, SKIP or DID-NOT-FIRE
#   $3 - The note explaining the outcome
# Outputs:
#   One fixed-width row on stdout
# Returns:
#   Exits 2 when the outcome is not one of the four
report() {
    CONTROL_ROWS="${CONTROL_ROWS:-}$(printf '%-4s %-13s %s' "$1" "$2" "$3")"$'\n'
    case "$2" in
        FIRED)        fired=$((fired + 1)) ;;
        WEAK)         weak=$((weak + 1)) ;;
        SKIP)         skipped=$((skipped + 1)) ;;
        DID-NOT-FIRE) dead=$((dead + 1)) ;;
        *)            printf 'controls.sh: %s reported the unknown outcome "%s"\n' "$1" "$2" >&2
                      exit 2 ;;
    esac
}


# control_cc1_grounding_removed requires the grounding judgement to decide verdicts.
#
# The break drops it, so a demand whose cone reaches an external source is accepted on its
# schedule history alone. Every lane in such a program is correct, so no lane assertion can see
# the break, and grounding has to be a judgement of its own beside the lane.
# Arguments:
#   $1 - The fixture directory
control_cc1_grounding_removed() {
    local fixtures="$1" broken="$ROOT/build/bin/test_lanelang_nogrounding" out status failures
    if [ ! -x "$broken" ]; then
        report CC1 SKIP "the ungrounded build is absent, so the judgement cannot be removed"
        return
    fi
    out="$("$broken" "$fixtures" 2>&1)"
    status="$(printf '%s\n' "$out" | sed -n 's/.*result=\([A-Z-]*\).*/\1/p' | tail -1)"
    failures="$(printf '%s\n' "$out" | grep -c 'want reject, got accept' || true)"
    if [ "$status" != "FAIL" ]; then
        report CC1 DID-NOT-FIRE "a build with no grounding judgement passed the fixture suite"
    elif [ "$failures" -lt 3 ]; then
        report CC1 WEAK "the leg rejected the build, but not by accepting the ungrounded demands"
    else
        report CC1 FIRED "without grounding, $failures demand(s) over an external source were accepted"
    fi
}

# control_cc2_schedule_dependent_tree requires L2 to reject a fixed-tree kernel that reads the
# schedule.
control_cc2_schedule_dependent_tree() {
    local status reason
    if ! command -v coqc >/dev/null 2>&1; then
        report CC2 SKIP "no coqc on this machine, so the development cannot be built at all"
        return
    fi
    mkdir -p "$WORK/proofs-eval"
    if ! cp "$ROOT/src/proofs/ReplayLattice.v" "$WORK/proofs-eval/ReplayLattice.v" ||
       ! cp "$ROOT/src/proofs/ReplayLatticeEval.v" "$WORK/proofs-eval/ReplayLatticeEval.v"; then
        report CC2 SKIP "could not copy the development"
        return
    fi
    # Anchored on the definition rather than on its body, for the reason CC16's substitution is.
    perl -0pi -e 's/(Definition kernel_fixed_tree \(s : msched\) \(l : list A\) : A :=)\s*blocked EVAL_BLOCK l\./$1 blocked (S (sworkers s)) l./s' \
        "$WORK/proofs-eval/ReplayLatticeEval.v"
    if ! grep -q 'kernel_fixed_tree (s : msched) (l : list A) : A := blocked (S (sworkers s)) l' \
            "$WORK/proofs-eval/ReplayLatticeEval.v"; then
        report CC2 WEAK "the substitution did not reach the file (the kernel still ignores the schedule)"
        return
    fi
    RL_PROOF_DIR="$WORK/proofs-eval" "$ROOT/scripts/check-proofs-eval.sh" >"$WORK/cc2.out" 2>&1
    status=$?
    reason="$(rl_status_field reason <"$WORK/cc2.out")"
    if [ "$status" -eq 77 ]; then
        report CC2 SKIP "the gate skipped, so it never ran against the break"
    elif [ "$status" -eq 0 ]; then
        report CC2 DID-NOT-FIRE "L2 still passed with the fixed tree reading the worker count"
    elif [ "$reason" = "proof-does-not-compile" ]; then
        report CC2 FIRED "the fixed tree made to read the worker count; the row is no longer provable ($reason)"
    else
        report CC2 WEAK "L2 rejected the broken development, but reported '$reason' instead"
    fi
}

# control_cc3_scan_width_follows_workers requires the blocked scan's leaf width to be frozen.
#
# The width is published ABI in the same sense the reduction tree's is: two implementations agree
# on a `replay` scan's bits only if they agree on it. The break lets the deployment pick it, the
# same defect as a tree whose shape follows the worker count.
#
# A kernel that accumulates across block boundaries computes a serial scan, so its width reaches no
# bit, the invariance leg passes under the break, and this control cannot fire. A control that
# cannot fire here points at that kernel.
control_cc3_scan_width_follows_workers() {
    local broken="$ROOT/build/bin/test_schedule_scanblock" status reason pinned_count unpinned_count
    if [ ! -x "$broken" ]; then
        report CC3 SKIP "the perturbed-width build is absent, so the width cannot be unfrozen"
        return
    fi
    "$broken" >"$WORK/cc3.out" 2>&1
    status=$?
    reason="$(rl_status_field reason <"$WORK/cc3.out")"
    pinned_count=$(grep -c 'a pinned value moved' "$WORK/cc3.out")
    unpinned_count=$(grep -c 'FAIL    unpinned' "$WORK/cc3.out")
    if ! grep -q 'LANELANG_SCAN_BLOCK_FOLLOWS_WORKERS' "$WORK/cc3.out"; then
        report CC3 WEAK "the -D did not reach the code (no control marker); the width was not unfrozen"
    elif [ "$status" -eq 0 ]; then
        report CC3 DID-NOT-FIRE "the leaf width reached no bit, so the published width decides nothing"
    elif [ "$unpinned_count" -ne 0 ]; then
        report CC3 WEAK "the unfrozen width broke the unpinned assertions too, so this is not a clean width move"
    elif [ "$reason" = "lane-not-schedule-invariant" ]; then
        report CC3 FIRED "letting the worker count pick the scan's leaf width moved $pinned_count pinned value(s)"
    else
        report CC3 WEAK "L4 rejected the unfrozen width, but reported '$reason' instead"
    fi
}

# control_cc4_exact_lowers_to_the_tree requires the reference comparison to catch `exact`
# discharged by the fixed tree instead of the oracle order.
#
# No other gate sees this break. The fixed tree is schedule-invariant, so the perturbed build still
# shows one bit pattern over the twenty traversal and worker cells for each pinned lane, and every
# invariance assertion passes. The fixed tree is not the serial reference, so a value declared
# `exact` has one meaning under this build and another under the shipped one: one lane, two bit
# patterns.
#
# If the leg fails for any reason other than reference inequality, the reference comparison has not
# been shown to be separate evidence, and the control reports WEAK. It therefore reads the leg's
# declared reason, not its exit status, and requires the schedule assertions to stay green: the
# count of `1 pattern over` lines must not fall.
control_cc4_exact_lowers_to_the_tree() {
    local broken="$ROOT/build/bin/test_schedule_exacttree" shipped="$ROOT/build/bin/test_schedule"
    local out reason invariant_before invariant_after
    if [ ! -x "$broken" ] || [ ! -x "$shipped" ]; then
        report CC4 SKIP "the tree-discharged build is absent, so @exact cannot be mislowered"
        return
    fi
    out="$("$broken" 2>&1)"
    reason="$(printf '%s\n' "$out" | sed -n 's/.*reason=\([a-z-]*\).*/\1/p' | tail -1)"
    invariant_after="$(printf '%s\n' "$out" | grep -c '1 pattern over' || true)"
    invariant_before="$("$shipped" 2>&1 | grep -c '1 pattern over' || true)"

    if [ "$reason" != "exact-value-not-reference-equal" ]; then
        report CC4 DID-NOT-FIRE "the tree discharged @exact and the leg did not notice (reason=$reason)"
    elif [ "$invariant_after" -lt "$invariant_before" ]; then
        report CC4 WEAK "the reference comparison fired, but $((invariant_before - invariant_after)) schedule-invariance assertion(s) fired too, so this break is visible without it"
    else
        report CC4 FIRED "the fixed tree discharged exact with all $invariant_after schedule-invariance assertion(s) still green; only the reference comparison saw it"
    fi
}

# control_cc5_contract_dropped_from_the_key requires the contract to be in the value-numbering key.
# Arguments:
#   $1 - The checker binary
#   $2 - The fixture directory
control_cc5_contract_dropped_from_the_key() {
    local checker="$1" fixtures="$2" broken="$ROOT/build/bin/lanelang_nolanecse"
    local with_contract without_contract
    if [ ! -x "$broken" ]; then
        report CC5 SKIP "the lane-blind build is absent, so the contract cannot be dropped from the key"
        return
    fi
    with_contract="$("$checker" lower "$fixtures/accept_cse_distinct.lane" 2>/dev/null | awk '{print $4}' | sort | tr '\n' ' ')"
    without_contract="$("$broken" lower "$fixtures/accept_cse_distinct.lane" 2>/dev/null | awk '{print $4}' | sort | tr '\n' ' ')"
    if [ -z "$(printf '%s' "$without_contract" | tr -d ' ')" ]; then
        report CC5 WEAK "the lane-blind build printed no kernels at all, so nothing was compared"
    elif [ "$with_contract" = "$without_contract" ]; then
        report CC5 DID-NOT-FIRE "the twins did not collapse; the lane is not in the key at all"
    elif [ "$(printf '%s' "$without_contract" | wc -w)" -lt "$(printf '%s' "$with_contract" | wc -w)" ]; then
        report CC5 FIRED "twins collapsed [$without_contract] where the contract keeps them distinct [$with_contract]"
    else
        report CC5 WEAK "the lowering changed but not by collapsing ($with_contract -> $without_contract)"
    fi
}

# control_cc6_malformed_is_not_a_lane_verdict requires a structural defect to be reported as such.
# Arguments:
#   $1 - The checker binary
#   $2 - The fixture directory
control_cc6_malformed_is_not_a_lane_verdict() {
    local checker="$1" fixtures="$2" status
    "$checker" check "$fixtures/malformed_forward.lane" >/dev/null 2>&1
    status=$?
    if [ "$status" -eq 2 ]; then
        report CC6 FIRED "forward reference reported malformed (exit 2), not as a lane violation"
    elif [ "$status" -eq 1 ]; then
        report CC6 DID-NOT-FIRE "a malformed program was reported as a lane violation"
    else
        report CC6 DID-NOT-FIRE "a malformed program was accepted (exit $status)"
    fi
}

# control_cc7_pinned_eval_breaks requires L4 to separate an accepted program the evaluator cannot
# answer from a program the checker refused.
control_cc7_pinned_eval_breaks() {
    local status reason broken_count unpinned_count
    local core
    if ! core="$(lanelang_core_sources)"; then
        report CC7 DID-NOT-FIRE \
            "src/lanelang/Makefile's CORE := line no longer parses, so the broken-evaluator variant could not be built"
        return
    fi
    # shellcheck disable=SC2086
    if ! ( cd "$ROOT/src/lanelang" && "${CC:-cc}" -O2 -std=c11 -DRL_PINNED_EVAL_BREAKS -I. \
            -o "$WORK/ts_pinnedeval" test_schedule.c $core ) >"$WORK/cc7.build" 2>&1; then
        report CC7 SKIP "could not build the broken-evaluator variant"
        return
    fi
    "$WORK/ts_pinnedeval" >"$WORK/cc7.out" 2>&1
    status=$?
    reason="$(rl_status_field reason <"$WORK/cc7.out")"
    broken_count=$(grep -c 'accepted this program and its evaluation still failed' "$WORK/cc7.out")
    unpinned_count=$(grep -c 'FAIL    unpinned' "$WORK/cc7.out")
    if ! grep -q '^PERTURBED pinned-eval-breaks' "$WORK/cc7.out"; then
        report CC7 WEAK "the -D did not reach the code (no PERTURBED marker); the evaluator was not broken"
    elif [ "$status" -eq 0 ]; then
        report CC7 DID-NOT-FIRE "L4 still passed with an accepted pinned program failing to evaluate"
    elif [ "$unpinned_count" -ne 0 ]; then
        report CC7 WEAK "the broken evaluator failed the unpinned assertions too, so this is not the post-acceptance break"
    elif [ "$reason" = "pinned-eval-failed" ]; then
        report CC7 FIRED "an accepted pinned program failed to evaluate at $broken_count lane(s); L4 reported it as a failure, not as a refusal"
    else
        report CC7 WEAK "L4 rejected the broken evaluator, but reported '$reason' instead"
    fi
}

# control_cc8_checker_kill_switch requires the kill switch to suppress every verdict, to leave a
# malformed program malformed, and to change no kernel pick.
# Arguments:
#   $1 - The checker binary
#   $2 - The fixture directory
control_cc8_checker_kill_switch() {
    local checker="$1" fixtures="$2" before after status declared structural
    before="$("$checker" lower "$fixtures/reject_poison.lane" 2>/dev/null | awk '{print $4}' | tr '\n' ' ')"
    LANELANG_NOCHECK=1 "$checker" check "$fixtures/reject_poison.lane" >/dev/null 2>&1
    status=$?
    LANELANG_NOCHECK=1 "$checker" check "$fixtures/reject_uncalled_body_violates.lane" >/dev/null 2>&1
    declared=$?
    LANELANG_NOCHECK=1 "$checker" check "$fixtures/malformed_beside_rejected_declaration.lane" >/dev/null 2>&1
    structural=$?
    after="$(LANELANG_NOCHECK=1 "$checker" lower "$fixtures/reject_poison.lane" 2>/dev/null | awk '{print $4}' | tr '\n' ' ')"
    if [ "$status" -ne 0 ]; then
        report CC8 DID-NOT-FIRE "the kill switch did not suppress the rejection"
    elif [ "$declared" -ne 0 ]; then
        report CC8 DID-NOT-FIRE "the kill switch left a declaration checked on its own refusing"
    elif [ "$structural" -ne 2 ]; then
        report CC8 DID-NOT-FIRE "the kill switch let a malformed declaration through beside a refused one"
    elif [ "$before" != "$after" ]; then
        report CC8 DID-NOT-FIRE "disabling the checker changed the lowering ($before vs $after)"
    else
        report CC8 FIRED "kill switch suppresses the verdict and changes no kernel pick"
    fi
}

# control_cc9_qualifiers_unreserved requires the lane qualifiers to be reserved words.
#
# The break makes them ordinary identifiers and decides the prefix by lookahead, so a qualifier
# with a name after it is still a qualifier and one without becomes a name. The second case is the
# defect: `replay := sum(x)` binds a variable called `replay`, the demand it plainly means is never
# stated, no diagnostic is printed, and the free kernel is selected.
#
# The break fails open on a demand: a demand the source plainly writes is dropped without a word.
#
# The assertion has two arms. The fixture must flip to accept, and the diagnostic must vanish with
# it; a build that still refused for another reason would prove nothing about this break.
# Arguments:
#   $1 - The fixture directory
control_cc9_qualifiers_unreserved() {
    local fixtures="$1" broken="$ROOT/build/bin/test_lanelang_unreserved" out status flipped quiet
    if [ ! -x "$broken" ]; then
        report CC9 SKIP "the unreserved build is absent, so the words cannot be freed"
        return
    fi
    out="$("$broken" "$fixtures" 2>&1)"
    status="$(printf '%s\n' "$out" | sed -n 's/.*result=\([A-Z-]*\).*/\1/p' | tail -1)"
    flipped="$(printf '%s\n' "$out" | grep -c 'want malformed, got accept' || true)"
    quiet="$(printf '%s\n' "$out" | grep -c 'no diagnostic held' || true)"
    if [ "$status" != "FAIL" ]; then
        report CC9 DID-NOT-FIRE "a build with the qualifiers unreserved passed the fixture suite"
    elif [ "$flipped" -lt 1 ] || [ "$quiet" -lt 1 ]; then
        report CC9 WEAK "the leg rejected the build, but not by both accepting a nameless demand ($flipped) and losing its diagnostic ($quiet)"
    else
        report CC9 FIRED "unreserved qualifiers bound $flipped nameless demand(s) as variables and lost $quiet expected diagnostic(s)"
    fi
}

# control_cc10_bless_flips_grounded requires a blessing to excuse a declaration without repairing
# the cone underneath it.
#
# The break marks a blessed leaf grounded instead. It changes no verdict: every fixture accepts and
# rejects as it does in a shipped build, because a declaration excused by the disjunct and one
# resting on a repaired cone are accepted by the same rule. What moves is the count of nodes the
# sweep left ungrounded, which the fixtures assert, so no accept-or-reject assertion can see this
# break.
#
# If the leg fails but no `ungrounded nodes` line appears, something else caught the break and the
# control reports WEAK; if any verdict moved, the break was not the one described.
# Arguments:
#   $1 - The fixture directory
control_cc10_bless_flips_grounded() {
    local fixtures="$1" broken="$ROOT/build/bin/test_lanelang_blessgrounds" out status counted moved
    if [ ! -x "$broken" ]; then
        report CC10 SKIP "the bless-grounds build is absent, so the blessing cannot be made to repair"
        return
    fi
    out="$("$broken" "$fixtures" 2>&1)"
    status="$(printf '%s\n' "$out" | sed -n 's/.*result=\([A-Z-]*\).*/\1/p' | tail -1)"
    counted="$(printf '%s\n' "$out" | grep -c 'ungrounded nodes:' || true)"
    moved="$(printf '%s\n' "$out" | grep -cE 'want (reject|accept|malformed), got' || true)"
    if [ "$status" != "FAIL" ]; then
        report CC10 DID-NOT-FIRE "a blessing that repaired its cone passed the fixture suite"
    elif [ "$counted" -lt 1 ]; then
        report CC10 WEAK "the leg rejected the build, but not by the ungrounded-node count"
    elif [ "$moved" -gt 0 ]; then
        report CC10 WEAK "$moved verdict(s) moved, so the break is not the invisible one described"
    else
        report CC10 FIRED "marking a blessed leaf grounded repaired $counted cone(s) with no accept-or-reject verdict changed"
    fi
}

# control_cc11_ordered_aliases_exact requires the cross-product fixtures to tell the order dial
# from an `exact` demand.
# Arguments:
#   $1 - The fixture directory
control_cc11_ordered_aliases_exact() {
    local fixtures="$1" binary="$ROOT/build/bin/test_lanelang_orderedalias" output failed survivors
    if [ ! -x "$binary" ]; then
        report CC11 SKIP "the alias build is not present; run make -C src/lanelang controls"
        return
    fi
    output="$("$binary" "$fixtures" 2>&1)"
    failed="$(printf '%s\n' "$output" | grep -c '^  FAIL xp_dial_only.lane: expected kernel fold_free, not emitted')"
    survivors="$(printf '%s\n' "$output" | grep -cE '^xp_(approx|exact|ordered|replay)\.lane .* ok$')"
    if [ "$failed" -ge 1 ] && [ "$survivors" -eq 4 ]; then
        report CC11 FIRED "aliasing the dial onto exact survived 4 of the 5 cross-product fixtures; xp_dial_only caught it, because the free kernel it requires was healed away by a demand the source never wrote"
    elif [ "$failed" -ge 1 ]; then
        report CC11 WEAK "the alias was caught, but only $survivors of the other 4 fixtures survived it, so this is not the subtle break the five cross-product fixtures were written to catch"
    else
        report CC11 DID-NOT-FIRE "aliasing the dial onto exact changed no verdict; the cross-product fixtures do not separate the two axes"
    fi
}

# control_cc12_demand_stamps_node requires a demand to be carried on a node of its own.
#
# The break stamps the demand onto the value instead. `declared` is in the interning key, so the
# stamp lands after the node has been offered for interning: which twins merge is then decided by
# the order two independent bindings were written in, and a demand reaches values the source
# demanded nothing about.
#
# The fixture that notices is the one about a promise read in a wider expression. Under the break
# the demand and the outside reader are the same node, so escape-marking refuses
# `return f(x) + 1.0`. Carried on its own node, the escaping reader reads a neutral wrapper and the
# producer beneath it never escaped, so a promise needs no exemption from the escape rule.
#
# The assertion has two arms. A verdict must move, or the node bought nothing; and the fold must
# lose its kernel, or something other than the stamping did the work.
# Arguments:
#   $1 - The fixture directory
control_cc12_demand_stamps_node() {
    local fixtures="$1" broken="$ROOT/build/bin/test_lanelang_stampdemand" out status flipped kernels
    if [ ! -x "$broken" ]; then
        report CC12 SKIP "the stamped-demand build is absent, so the node cannot be removed"
        return
    fi
    out="$("$broken" "$fixtures" 2>&1)"
    status="$(printf '%s\n' "$out" | sed -n 's/.*result=\([A-Z-]*\).*/\1/p' | tail -1)"
    flipped="$(printf '%s\n' "$out" | grep -c 'want accept, got reject' || true)"
    kernels="$(printf '%s\n' "$out" | grep -c 'expected kernel fold_replay, not emitted' || true)"
    if [ "$status" != "FAIL" ]; then
        report CC12 DID-NOT-FIRE "a build that stamps the demand onto the value passed the fixture suite"
    elif [ "$flipped" -lt 1 ] || [ "$kernels" -lt 1 ]; then
        report CC12 WEAK "the leg rejected the build, but not by both refusing a promised value ($flipped) and losing its kernel ($kernels)"
    else
        report CC12 FIRED "a stamped demand refused $flipped value(s) the shipped build accepts, a promise read in a wider expression among them, and lost $kernels pinned kernel(s)"
    fi
}

# control_cc13_demand_picks_the_kernel requires the discharge to follow the operation, never the
# demand.
#
# The break is the choice a production compiler made: let the lane written at a node select the
# kernel. Every fixture still reaches the outcome it expects, so no accept-or-reject assertion can
# see the break, and one computed class would emit two bit patterns. The invariant sweep over the
# (class x demand x ambient) product catches it, and no fixture fails under this build, so deleting
# the sweep would leave the break undetected.
#
# The receipt asserts the verdict half as well: a receipt saying no verdict moved, while measuring
# only that something rejected, would claim more than it checked.
# Arguments:
#   $1 - The fixture directory
control_cc13_demand_picks_the_kernel() {
    local fixtures="$1" broken="$ROOT/build/bin/test_lanelang_demandkernel" line status verdicts moved
    if [ ! -x "$broken" ]; then
        report CC13 SKIP "the demand-driven build is absent, so the invariant cannot be broken"
        return
    fi
    # 2>&1 keeps both streams: the harness prints its RL-STATUS line to stdout and every FAIL detail
    # to stderr, so a control that discards stderr can see the leg fail and not see what caught it.
    line="$("$broken" "$fixtures" 2>&1)"
    status="$(printf '%s\n' "$line" | sed -n 's/.*result=\([A-Z-]*\).*/\1/p' | tail -1)"
    verdicts="$(printf '%s\n' "$line" | grep -c 'kernel selection reads the demand' || true)"
    moved="$(printf '%s\n' "$line" | grep -c 'want reject, got accept\|want accept, got reject' || true)"
    if [ "$status" != "FAIL" ]; then
        report CC13 DID-NOT-FIRE "the leg accepted a build whose discharge follows the demand"
    elif [ "$verdicts" -lt 1 ]; then
        report CC13 WEAK "the leg rejected the build, but the invariant assertion was not what caught it"
    elif [ "$moved" -gt 0 ]; then
        report CC13 WEAK "$moved verdict(s) moved, so the break is not the one this control is about"
    else
        report CC13 FIRED "the demand-driven discharge was caught by the invariant sweep alone, with no verdict changed"
    fi
}

# control_cc14_tiling_that_rebrackets requires L4 to reject a tile width deciding the bracketing.
control_cc14_tiling_that_rebrackets() {
    local status reason pinned_count unpinned_count
    local core
    if ! core="$(lanelang_core_sources)"; then
        report CC14 DID-NOT-FIRE \
            "src/lanelang/Makefile's CORE := line no longer parses, so the rebracketing variant could not be built"
        return
    fi
    # shellcheck disable=SC2086
    if ! ( cd "$ROOT/src/lanelang" && "${CC:-cc}" -O2 -std=c11 -DRL_TILE_REBRACKETS -I. \
            -o "$WORK/ts_rebracket" test_schedule.c $core ) >"$WORK/cc14.build" 2>&1; then
        report CC14 SKIP "could not build the rebracketing variant"
        return
    fi
    "$WORK/ts_rebracket" >"$WORK/cc14.out" 2>&1
    status=$?
    reason="$(rl_status_field reason <"$WORK/cc14.out")"
    pinned_count=$(grep -c 'a pinned value moved' "$WORK/cc14.out")
    unpinned_count=$(grep -c 'FAIL    unpinned' "$WORK/cc14.out")
    if ! grep -q '^PERTURBED tile-rebrackets' "$WORK/cc14.out"; then
        report CC14 WEAK "the -D did not reach the code (no PERTURBED marker); nothing was rebracketed"
    elif [ "$status" -eq 0 ]; then
        report CC14 DID-NOT-FIRE "L4 still passed with the tile width deciding the bracketing"
    elif [ "$unpinned_count" -ne 0 ]; then
        report CC14 WEAK "the rebracketing broke the unpinned assertions too, so this is not a clean bracketing move"
    elif [ "$reason" = "lane-not-schedule-invariant" ]; then
        report CC14 FIRED "letting the tile width decide the bracketing moved $pinned_count pinned value(s) that must not move"
    else
        report CC14 WEAK "L4 rejected the rebracketing, but reported '$reason' instead"
    fi
}

# control_cc15_ordering_guard requires L7 to reject a sweep that answers what it cannot analyse.
control_cc15_ordering_guard() {
    local status reason answered_count mismatch_count
    local core
    if ! core="$(lanelang_core_sources)"; then
        report CC15 DID-NOT-FIRE \
            "src/lanelang/Makefile's CORE := line no longer parses, so the fail-open variant could not be built"
        return
    fi
    # shellcheck disable=SC2086
    if ! ( cd "$ROOT/src/lanelang" && "${CC:-cc}" -O2 -std=c11 -DRL_LATTICE_ORDER_FAILOPEN -I. \
            -o "$WORK/fuzz_failopen" fuzz.c $core oracle.c ) >"$WORK/cc15.build" 2>&1; then
        report CC15 SKIP "could not build the fail-open variant"
        return
    fi
    "$WORK/fuzz_failopen" --agree >"$WORK/cc15.out" 2>&1
    status=$?
    reason="$(rl_status_field reason <"$WORK/cc15.out")"
    answered_count=$(sed -n 's/^ordering precondition: [0-9]* out-of-order graph(s) put to the sweep, \([0-9]*\) analysed anyway.*/\1/p' "$WORK/cc15.out" | tail -1)
    mismatch_count=$(grep -c 'MISMATCH ' "$WORK/cc15.out")
    if ! grep -q '^BUILD lanefuzz .* ctl=RL_LATTICE_ORDER_FAILOPEN$' "$WORK/cc15.out"; then
        report CC15 WEAK "the -D did not reach the code (no ctl= marker in the build stamp)"
    elif [ "$status" -eq 0 ]; then
        report CC15 DID-NOT-FIRE "L7 still passed with the ordering guard skipping the edge it cannot handle"
    elif [ "$mismatch_count" -gt 0 ]; then
        report CC15 WEAK "the fail-open guard also moved $mismatch_count oracle comparison(s), so this is not a clean ordering break"
    elif [ "$reason" = "out-of-order-graph-analysed" ]; then
        report CC15 FIRED "the fail-open guard answered ${answered_count:-?} out-of-order graph(s) instead of refusing them"
    else
        report CC15 WEAK "L7 rejected the fail-open guard, but reported '$reason' instead"
    fi
}

# control_cc16_admitted_lemma requires L9 to reject a development with an admitted proof.
control_cc16_admitted_lemma() {
    local status reason
    if ! command -v coqc >/dev/null 2>&1; then
        report CC16 SKIP "no coqc on this machine, so the development cannot be built at all"
        return
    fi
    mkdir -p "$WORK/proofs"
    if ! cp "$ROOT/src/proofs/ReplayLattice.v" "$WORK/proofs/ReplayLattice.v"; then
        report CC16 SKIP "could not copy the development"
        return
    fi
    perl -0pi -e 's/(Lemma agrees_antitone :.*?)\nProof\..*?\nQed\./$1\nProof. Admitted./s' \
        "$WORK/proofs/ReplayLattice.v"
    if cmp -s "$ROOT/src/proofs/ReplayLattice.v" "$WORK/proofs/ReplayLattice.v"; then
        report CC16 WEAK "the substitution did not reach the file (the patched copy is unchanged)"
        return
    fi
    if ! perl -0ne 'exit(!(/Lemma agrees_antitone :.*?\nProof\. Admitted\./s))' "$WORK/proofs/ReplayLattice.v"; then
        report CC16 WEAK "the substitution landed somewhere other than agrees_antitone"
        return
    fi
    RL_PROOF_DIR="$WORK/proofs" "$ROOT/scripts/check-proofs.sh" >"$WORK/cc16.out" 2>&1
    status=$?
    reason="$(rl_status_field reason <"$WORK/cc16.out")"
    if [ "$status" -eq 77 ]; then
        report CC16 SKIP "the gate skipped, so it never ran against the break"
    elif [ "$status" -eq 0 ]; then
        report CC16 DID-NOT-FIRE "L9 still passed with a lemma in Theorem 1's cone admitted"
    elif [ "$reason" = "unexpected-axiom-set" ]; then
        report CC16 FIRED "the admitted lemma surfaced as an undeclared axiom ($reason)"
    else
        report CC16 WEAK "L9 rejected the admitted development, but reported '$reason' instead"
    fi
}

# control_cc17_healing_disabled requires healing to decide a verdict.
#
# Without it, a program the shipped checker accepts is rejected.
# Arguments:
#   $1 - The checker binary
#   $2 - The fixture directory
control_cc17_healing_disabled() {
    local checker="$1" fixtures="$2" healed unhealed
    "$checker" check "$fixtures/accept_healed.lane" >/dev/null 2>&1
    healed=$?
    "$ROOT/build/bin/lanelang_noheal" check "$fixtures/accept_healed.lane" >/dev/null 2>&1
    unhealed=$?
    if [ "$healed" -eq 0 ] && [ "$unhealed" -eq 1 ]; then
        report CC17 FIRED "accept_healed passes with healing, is rejected without it"
    elif [ "$healed" -ne 0 ]; then
        report CC17 DID-NOT-FIRE "accept_healed does not pass even with healing"
    else
        report CC17 DID-NOT-FIRE "disabling healing changed nothing; the pass is decorative"
    fi
}

# control_cc18_bless_on_any_node requires the restriction of @bless to a source or an `ungrounded`
# call to be enforced.
#
# The restriction is a structural side condition, not a production, and the break drops it. A
# waiver on a fold parses in every build: bless_is_legal_here asks the graph whether the value is a
# source or a call declared `ungrounded`, and refuses it with a written diagnostic when it is not.
# The question is about the graph, asked where the graph exists, so the rule lives in resolve.c
# and the break removes it there.
#
# A blessing says the bytes behind an external source will be the same next run, and a fold has no
# bytes behind it, so the fixture that must notice is a malformed one, not a rejection.
# Arguments:
#   $1 - The fixture directory
control_cc18_bless_on_any_node() {
    local fixtures="$1" broken="$ROOT/build/bin/test_lanelang_blessany" out status caught
    if [ ! -x "$broken" ]; then
        report CC18 SKIP "the bless-anywhere build is absent, so the restriction cannot be dropped"
        return
    fi
    out="$("$broken" "$fixtures" 2>&1)"
    status="$(printf '%s\n' "$out" | sed -n 's/.*result=\([A-Z-]*\).*/\1/p' | tail -1)"
    caught="$(printf '%s\n' "$out" | grep -c '^  FAIL malformed_bless_on_fold' || true)"
    if [ "$status" != "FAIL" ]; then
        report CC18 DID-NOT-FIRE "a waiver bound to a computed value and the fixture suite still passed"
    elif [ "$caught" -lt 1 ]; then
        report CC18 WEAK "the leg rejected the build, but not through the fold-blessing fixture"
    else
        report CC18 FIRED "with the restriction dropped, a waiver bound to a fold the program computes"
    fi
}

# control_cc19_opaque_is_grounded requires an `ungrounded` declaration to carry a grounding answer.
#
# The break makes the opaque wrapper state GROUND_FIXED like any other node, so the declaration
# still parses, still lowers and still builds the same graph, and says nothing. A body the program
# fixes then reads as grounded at the call site, which the qualifier exists to deny.
#
# The assertion has two arms. A verdict must move, or the declaration stated nothing already; and
# the ungrounded-node counts must fall, or something other than the grounding bit did the work. The
# wrapper is still built under the break, so a change in the graph's shape cannot be mistaken for
# the property under test.
# Arguments:
#   $1 - The fixture directory
control_cc19_opaque_is_grounded() {
    local fixtures="$1" broken="$ROOT/build/bin/test_lanelang_opaquegrounded" out status flipped counts
    if [ ! -x "$broken" ]; then
        report CC19 SKIP "the opaque-is-grounded build is absent, so the declaration cannot be emptied"
        return
    fi
    out="$("$broken" "$fixtures" 2>&1)"
    status="$(printf '%s\n' "$out" | sed -n 's/.*result=\([A-Z-]*\).*/\1/p' | tail -1)"
    flipped="$(printf '%s\n' "$out" | grep -c 'want reject, got accept' || true)"
    counts="$(printf '%s\n' "$out" | grep -c 'ungrounded nodes: want' || true)"
    if [ "$status" != "FAIL" ]; then
        report CC19 DID-NOT-FIRE "a build whose ungrounded declaration states nothing passed the fixture suite"
    elif [ "$flipped" -lt 1 ] || [ "$counts" -lt 1 ]; then
        report CC19 WEAK "the leg rejected the build, but not by both accepting a refused demand ($flipped) and losing the counts ($counts)"
    else
        report CC19 FIRED "an empty ungrounded declaration accepted $flipped refused demand(s) and dropped $counts ungrounded-node count(s)"
    fi
}


# build_checker builds the checker and every control variant of it.
# Globals:
#   ROOT, WORK - Read
# Returns:
#   0 when the checker and its variants built, 1 otherwise
build_checker() {
    make -C "$ROOT/src/lanelang" all controls >"$WORK/lanelang.build" 2>&1 \
        && [ -x "$ROOT/build/bin/lanelang" ]
}

# print_table_header prints the run's title and column headings.
print_table_header() {
    echo "The Replay Lattice: must-fail controls"
    echo "ctl  outcome       detail"
    echo "----------------------------------------------------------"
}

# run_every_control runs the whole roster in id order. Every control but CC2 and CC16 needs the
# checker, so when it does not build those seventeen report SKIP and the two proof controls run.
# Globals:
#   ROOT - Read
run_every_control() {
    local checker="$ROOT/build/bin/lanelang" fixtures="$ROOT/src/tests/lanelang" control
    if ! build_checker; then
        for control in CC1 CC3 CC4 CC5 CC6 CC7 CC8 CC9 CC10 CC11 CC12 CC13 CC14 CC15 CC17 CC18 CC19; do
            report "$control" SKIP "lanelang did not build"
        done
        control_cc2_schedule_dependent_tree
        control_cc16_admitted_lemma
        return
    fi
    control_cc1_grounding_removed "$fixtures"
    control_cc2_schedule_dependent_tree
    control_cc3_scan_width_follows_workers
    control_cc4_exact_lowers_to_the_tree
    control_cc5_contract_dropped_from_the_key "$checker" "$fixtures"
    control_cc6_malformed_is_not_a_lane_verdict "$checker" "$fixtures"
    control_cc7_pinned_eval_breaks
    control_cc8_checker_kill_switch "$checker" "$fixtures"
    control_cc9_qualifiers_unreserved "$fixtures"
    control_cc10_bless_flips_grounded "$fixtures"
    control_cc11_ordered_aliases_exact "$fixtures"
    control_cc12_demand_stamps_node "$fixtures"
    control_cc13_demand_picks_the_kernel "$fixtures"
    control_cc14_tiling_that_rebrackets
    control_cc15_ordering_guard
    control_cc16_admitted_lemma
    control_cc17_healing_disabled "$checker" "$fixtures"
    control_cc18_bless_on_any_node "$fixtures"
    control_cc19_opaque_is_grounded "$fixtures"
}

# print_verdict prints the tally and decides the run's exit status.
# Globals:
#   fired, weak, skipped, dead - Read
# Returns:
#   Exits 1 when any control did not fire, a skip included, otherwise exits 0
print_verdict() {
    local roster accounted
    roster=${#CONTROLS[@]}
    accounted=$((fired + weak + skipped + dead))

    printf '%s' "${CONTROL_ROWS:-}" | sort -k1.3n
    echo "----------------------------------------------------------"
    printf 'fired=%d weak=%d skipped=%d did-not-fire=%d accounted=%d/%d\n' \
        "$fired" "$weak" "$skipped" "$dead" "$accounted" "$roster"
    if [ "$dead" -gt 0 ]; then
        echo "FAILURE: $dead control(s) did not fire. The gate they guard is decorative."
        exit 1
    fi


    if [ "$accounted" -ne "$roster" ]; then
        echo "FAILURE: $accounted of $roster control(s) reported. The rest never ran and never said so."
        exit 1
    fi


    if [ "$fired" -eq 0 ]; then
        echo "FAILURE: no control fired. This run demonstrated nothing about any gate."
        exit 1
    fi


    if [ "$weak" -gt 0 ]; then
        echo "FAILURE: $weak control(s) reported WEAK. A break that did not materialise leaves its"
        echo "  gate unguarded, and this run cannot tell you whether that gate works."
        exit 1
    fi


    if [ "$skipped" -gt 0 ]; then
        echo "FAILURE: $skipped control(s) skipped. A control that does not fire fails the run, and"
        echo "  each SKIP line above names what it needed; supply that and run again."
        exit 1
    fi
    echo "ALL CONTROLS BEHAVED ($fired fired, $weak weak, $skipped skipped)"
    exit 0
}

# main parses the command line, runs the roster and prints the verdict.
# Globals:
#   WORK - Set to the scratch directory, removed on a trap
# Arguments:
#   $@ - The command line
main() {
    parse_arguments "$@"
    if [ "$list_only" -eq 1 ]; then
        print_roster
        exit 0
    fi
    WORK="$(rl_make_work_directory replay-lattice-ctl)" || exit 1
    trap 'rm -rf "$WORK"' EXIT INT TERM
    print_table_header
    run_every_control
    print_verdict
}

main "$@"
