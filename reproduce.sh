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
#
# Usage:
#   ./reproduce.sh                          run every check and report skips
#   ./reproduce.sh --expect-skips=coq,cccl  allow only the named skips
#   ./reproduce.sh --list                   print the leg table and exit
#   ./reproduce.sh --self-check             the harness's own must-fail controls, then exit

set -uo pipefail
cd "$(dirname "$0")" || exit 1

# shellcheck source=scripts/lib/receipts.sh
. ./scripts/lib/receipts.sh

# The self-check and build checks run first. Buffering the rows keeps the final table in ID order.
RL_SORT_ROWS=1

# The scratch directory holding every leg's captured output, removed on a trap.
WORK=""

# Where the build puts every gate binary.
BIN=build/bin

# The comma-separated tags allowed to skip. An empty list permits any tagged leg to skip.
expect_skips=""

# 1 when --list was passed.
list_only=0

# 1 when --self-check was passed.
self_check=0

# How many legs passed.
pass=0

# How many legs skipped.
skip=0

# How many legs failed.
fail=0

# The IDs of the legs that skipped, each preceded by a space.
skipped_legs=""

# The IDs of the legs that failed, each preceded by a space.
failed_legs=""

# Every leg's outcome, one newline-terminated `id|result|cases|reason` record per leg.
rows=""

# The make invocation for the checker, assembled by build_make_commands.
lanelang_make=()

# How many of the harness's own controls the self-check ran, reported as leg L10's case count.
self_check_ran_count=0

# How many of the harness's own controls did not fire, which makes leg L10 fail.
self_check_dead_count=0

# print_usage prints the header comments immediately following the shebang.
# Outputs:
#   The header comment block on stdout.
print_usage() {
    awk 'NR == 1 { next } /^#/ { print; next } { exit }' "$0"
}

# parse_arguments reads the command line into the flag globals.
# Globals:
#   expect_skips, list_only, self_check - Set
# Arguments:
#   $@ - The command line
# Outputs:
#   The header on stdout for --help or an error on stderr for an unrecognised option
# Returns:
#   Exits 0 after --help and exits 2 on an unrecognised argument, otherwise 0
parse_arguments() {
    local argument
    for argument in "$@"; do
        case "$argument" in
            --expect-skips=*)  expect_skips="${argument#*=}" ;;
            --list)            list_only=1 ;;
            --self-check)      self_check=1 ;;
            -h|--help)         print_usage; exit 0 ;;
            *) echo "unknown argument: $argument" >&2; exit 2 ;;
        esac
    done
}

# Each registry row gives a leg's ID, skip tag and description. The --list output, skip checks
# and final tally all read this list.
LEGS=(
  "L1|cccl|the CCCL composition counterexample"
  "L2|coq|section 3.5's operation-class rows, proved of one concrete evaluator"
  "L3|lanelang|the checker's fixtures, with blame spans and kernel picks"
  "L4|lanelang|what the lane forbids the compiler: traversal cannot move a pinned value; CC14 stages the split"
  "L5|-|a blessed checksum refuses another world's data, and the unconditional form does not"
  "L6|lanelang|the same oracle over programs the front end actually parsed"
  "L7|lanelang|the differential oracle: linear join vs exhaustive path enumeration"
  "L8|lanelang|metamorphic properties of the checker"
  "L9|coq|the mechanised metatheory, and the axioms its theorems rest on"
  "L10|-|the harness checks receipts, counts, skip policy and clean-build failure"
  "L11|-|clean build"
  "L12|-|every gate binary is freshly built and carries a build stamp"
)

# leg_tag prints the skip tag the registry gives a leg, or "-" when the leg has none.
# Globals:
#   LEGS - Read
# Arguments:
#   $1 - The leg ID
# Outputs:
#   The tag on stdout
leg_tag() {
    local id="$1" row
    for row in "${LEGS[@]}"; do
        [ "${row%%|*}" = "$id" ] && { row="${row#*|}"; printf '%s' "${row%%|*}"; return; }
    done
    printf '%s' "-"
}

# all_tags prints every distinct skip tag the registry uses, comma separated.
# Globals:
#   LEGS - Read
# Outputs:
#   The tags on stdout
all_tags() {
    local row tag out=""
    for row in "${LEGS[@]}"; do
        row="${row#*|}"; tag="${row%%|*}"
        [ "$tag" = "-" ] && continue
        case ",$out," in *",$tag,"*) ;; *) out="${out:+$out,}$tag" ;; esac
    done
    printf '%s' "$out"
}

# print_leg_table prints the registry as a table, then the tags --expect-skips accepts.
# Globals:
#   LEGS - Read
# Outputs:
#   One header row, one row per leg, and two lines about the skip tags
print_leg_table() {
    local row id tag description
    printf '%-5s %-9s %s\n' CHECK TAG DESCRIPTION
    for row in "${LEGS[@]}"; do
        id="${row%%|*}"; row="${row#*|}"; tag="${row%%|*}"; description="${row#*|}"
        printf '%-5s %-9s %s\n' "$id" "$tag" "$description"
    done
    echo
    echo "skip tags that --expect-skips accepts: $(all_tags)"
    echo "a check whose tag is '-' may never skip; if it does, the run fails."
}

# record files one leg's outcome and moves the tallies.
# Globals:
#   rows - Appended with the leg's record
#   pass, skip, fail - One of them incremented
#   skipped_legs, failed_legs - Appended with the ID where it applies
# Arguments:
#   $1 - The leg ID
#   $2 - PASS, SKIP or anything else, which counts as a failure
#   $3 - The case count reported for the leg
#   $4 - The reason token
record() {
    local id="$1" result="$2" cases="$3" reason="$4"
    rows="${rows}${id}|${result}|${cases}|${reason}"$'\n'
    case "$result" in
        PASS) pass=$((pass + 1)) ;;
        SKIP) skip=$((skip + 1)); skipped_legs="$skipped_legs $id" ;;
        *)    fail=$((fail + 1)); failed_legs="$failed_legs $id" ;;
    esac
}

# leg_row adds a row to the run table and records the same reason in the tally.
# Globals:
#   rows and the tallies - Updated through record
# Arguments:
#   $1 - The leg ID
#   $2 - PASS, SKIP or anything else, which counts as a failure
#   $3 - The case count reported for the leg
#   $4 - The reason token
leg_row() {
    rl_report_row "$1" "$2" "$3" "$4"
    record "$1" "$2" "$3" "$4"
}

# result_for_exit_code maps a gate's exit code to the outcome it declares.
# Arguments:
#   $1 - The exit code
# Outputs:
#   PASS, SKIP or FAIL on stdout
result_for_exit_code() {
    case "$1" in
        0)  printf 'PASS' ;;
        77) printf 'SKIP' ;;
        *)  printf 'FAIL' ;;
    esac
}

# receipt_error prints a reason if the receipt cannot substantiate the recorded outcome.
# Arguments:
#   $1..$3 - The RL-STATUS lines, expected leg ID and outcome implied by the exit code
#   $4..$6 - The case count, declined count and reason from that line
receipt_error() {
    if [[ -z $1 ]]; then
        printf 'no-status-line'
    elif [[ $1 == *$'\n'* ]]; then
        printf 'multiple-status-lines'
    elif [[ $(rl_status_field leg <<<"$1") != "$2" ]]; then
        printf 'receipt-leg-mismatch'
    elif [[ $(rl_status_field result <<<"$1") != "$3" ]]; then
        printf 'receipt-result-mismatch'
    elif [[ ! $4 =~ ^[0-9]+$ || ! $5 =~ ^[0-9]+$ || -z $6 ]]; then
        printf 'malformed-receipt'
    elif [[ $3 == PASS && $4 =~ ^0+$ ]]; then
        printf 'zero-assertions'
    fi
}

# run_leg runs a gate command, checks its receipt against its exit code and records the outcome.
# Globals:
#   WORK - Read
#   rows and the tallies - Updated through record
# Arguments:
#   $1 - The leg ID
#   $2.. - The command and its arguments
# Outputs:
#   One run-table row, and up to eight lines of captured stderr when the leg failed
run_leg() {
    local id="$1"; shift
    local out="$WORK/$id.out" err="$WORK/$id.err"
    local status_code line result cases reason declined_count error
    "$@" >"$out" 2>"$err"
    status_code=$?
    line="$(grep -E '^RL-STATUS([[:space:]]|$)' "$out")"
    cases="$(rl_status_field cases <<<"$line")"
    declined_count="$(rl_status_field skipped <<<"$line")"
    reason="$(rl_status_field reason <<<"$line")"
    result="$(result_for_exit_code "$status_code")"
    error="$(receipt_error "$line" "$id" "$result" "$cases" "$declined_count" "$reason")"
    if [ -n "$error" ]; then
        result=FAIL
        reason="$error"
        cases=0
        declined_count=0
    fi
    if [[ ! $declined_count =~ ^0+$ ]]; then
        rl_report_row "$id" "$result" "$cases" "$reason ($declined_count case(s) declined)"
    else
        rl_report_row "$id" "$result" "$cases" "$reason"
    fi
    [ "$result" = FAIL ] && sed 's/^/      /' "$err" | head -8
    record "$id" "$result" "$cases" "$reason"
}

# control_report_drift compares the registry with the literal IDs in the script's report calls.
# Arguments:
#   $1 - The registry rows, one per line
#   $2 - Path to controls.sh
# Outputs:
#   One entry per control in one set and not the other, each preceded by a space
control_report_drift() {
    local defined called control out=""
    defined="$(printf '%s\n' "$1" | cut -d'|' -f1 | sort -u | tr '\n' ' ')"
    called="$(grep -oE 'report [A-Z]+[0-9]+' "$2" | grep -oE '[A-Z]+[0-9]+' | sort -u | tr '\n' ' ')"
    for control in $defined; do
        rl_in_set "$control" "$called" || out="$out $control(registry,never-reported)"
    done
    for control in $called; do
        rl_in_set "$control" "$defined" || out="$out $control(reported,not-in-registry)"
    done
    printf '%s' "$out"
}

# control_ids_in prints the control IDs a document's control-table rows name, space separated.
# Arguments:
#   $1 - Path to the document
# Outputs:
#   The IDs on stdout, sorted and separated by single spaces
control_ids_in() {
    grep -oE '^\| CC[0-9]+ ' "$1" 2>/dev/null | grep -oE 'CC[0-9]+' | sort -u | tr '\n' ' '
}

# control_map_drift checks that the registry and the reviewer's map list the same controls (CR2).
# Arguments:
#   $1 - The registry rows, one per line
#   $2 - Path to the reviewer's map
# Outputs:
#   One entry per control in one set and not the other, each preceded by a space
control_map_drift() {
    local defined rowed control out=""
    defined="$(printf '%s\n' "$1" | cut -d'|' -f1 | sort -u | tr '\n' ' ')"
    rowed="$(control_ids_in "$2")"
    for control in $defined; do
        rl_in_set "$control" "$rowed" || out="$out $control(defined,no-row)"
    done
    for control in $rowed; do
        rl_in_set "$control" "$defined" || out="$out $control(row,not-defined)"
    done
    printf '%s' "$out"
}

# control_roster_drift checks the registry's size and IDs against the report calls and the map
# when both files are present.
# Outputs:
#   One entry per disagreement on stdout, each preceded by a space
control_roster_drift() {
    local controls=controls.sh map=ARTIFACT.md block registry out="" quoted parsed malformed
    [ -f "$controls" ] && [ -f "$map" ] || return 0
    block="$(sed -n '/^CONTROLS=(/,/^)/p' "$controls")"
    registry="$(printf '%s\n' "$block" | sed -n 's/^ *"\([A-Z]\{1,2\}[0-9][^"]*\)".*$/\1/p')"
    parsed="$(printf '%s\n' "$registry" | grep -c .)"
    quoted="$(printf '%s\n' "$block" | grep -c '^ *"')"
    out="$(rl_floor_shortfall 'controls.sh registry rows' "$parsed" 19)"
    [ "$quoted" -eq "$parsed" ] \
        || out="$out
  the registry holds $quoted quoted row(s) and the id pattern read $parsed: some id has a shape the parser cannot see"
    malformed="$(printf '%s\n' "$registry" | cut -d'|' -f1 | grep -cvE '^CC[0-9]+$')"
    [ "$malformed" -eq 0 ] \
        || out="$out
  $malformed registry id(s) are not CCn"
    out="$out$(control_report_drift "$registry" "$controls")"
    out="$out$(control_map_drift "$registry" "$map")"
    printf '%s' "$out"
}

# count_outside_range reports whether an observed count falls outside a stated expectation.
# Arguments:
#   $1 - The expected count as an integer or a `low-high` range
#   $2 - The count the leg reported
# Returns:
#   0 when the count is outside the expectation, 1 when it satisfies it
count_outside_range() {
    local lowest highest
    case "$1" in
        *-*) lowest="${1%%-*}"; highest="${1##*-}" ;;
        *)   lowest="$1"; highest="$1" ;;
    esac
    [ "$2" -lt "$lowest" ] || [ "$2" -gt "$highest" ]
}

# ledger_expected_count prints the count ARTIFACT.md's row for one leg states.
# Arguments:
#   $1 - Path to the reviewer's map
#   $2 - The leg ID
# Outputs:
#   The stated count on stdout, or nothing when the row states none
ledger_expected_count() {
    grep -m1 "^| $2 |" "$1" | grep -oE 'cases=([0-9]+-[0-9]+|[0-9]+|varies)' | head -1 | cut -d= -f2
}

# ledger_rows_without_counts prints every leg row in the reviewer's map that states no count at all.
# Arguments:
#   $1 - Path to the reviewer's map
# Outputs:
#   One ID per row stating no count, each preceded by a space
ledger_rows_without_counts() {
    local id out=""
    for id in $(grep -oE '^\| L[0-9]+ ' "$1" | tr -d '| '); do
        [ -n "$(ledger_expected_count "$1" "$id")" ] || out="$out $id"
    done
    printf '%s' "$out"
}

# ledger_count_drift checks the map's row count, that the registry and the map name the same
# legs, and flags rows without a case count. For legs that passed, it compares the observed
# count with any stated number or range.
# Globals:
#   LEGS - Read
#   rows - Read
# Outputs:
#   Any row or count discrepancies on stdout when the map exists
ledger_count_drift() {
    local map=ARTIFACT.md id result cases reason want out="" row_count missing defined rowed leg
    [ -f "$map" ] || return 0
    row_count="$(grep -cE '^\| L[0-9]' "$map")"
    out="$(rl_floor_shortfall 'ARTIFACT.md leg rows' "$row_count" 12)"
    defined="$(printf '%s\n' "${LEGS[@]}" | cut -d'|' -f1 | tr '\n' ' ')"
    rowed="$(grep -oE '^\| L[0-9]+ ' "$map" | tr -d '| ' | tr '\n' ' ')"
    for leg in $defined; do
        rl_in_set "$leg" "$rowed" || out="$out $leg(defined,no-row)"
    done
    for leg in $rowed; do
        rl_in_set "$leg" "$defined" || out="$out $leg(row,not-defined)"
    done
    missing="$(ledger_rows_without_counts "$map")"
    [ -n "$missing" ] && out="$out${missing// / no-count:}"
    while IFS='|' read -r id result cases reason; do
        [ -n "$id" ] || continue
        [ "$result" = PASS ] || continue
        case "$cases" in ''|*[!0-9]*) continue ;; esac
        want="$(ledger_expected_count "$map" "$id")"
        [ -n "$want" ] || continue
        [ "$want" = varies ] && continue
        count_outside_range "$want" "$cases" && out="$out $id(map=$want,ran=$cases)"
    done <<< "$rows"
    printf '%s' "$out"
}

# undeclared_skips reports skipped legs marked as never skipping. With a non-empty list of
# expected tags, it also reports skipped legs whose tags are absent from the list.
# Globals:
#   skipped_legs, expect_skips - Read
# Outputs:
#   One `id(tag)` per undeclared skip on stdout, each preceded by a space
undeclared_skips() {
    local leg tag out=""
    for leg in $skipped_legs; do
        tag="$(leg_tag "$leg")"
        if [ "$tag" = "-" ]; then
            out="$out $leg(never-skips)"
            continue
        fi
        rl_in_set "$tag" "${expect_skips//,/ }" && continue
        [ -n "$expect_skips" ] && out="$out $leg($tag)"
    done
    printf '%s' "$out"
}

# write_self_check_stubs writes three synthetic gates and a make command that fails to clean.
# Globals:
#   WORK - Read
write_self_check_stubs() {
    cat >"$WORK/sc1" <<'STUB'
#!/bin/sh
echo "RL-STATUS leg=SC1 result=SKIP cases=0 skipped=0 reason=forced-skip"
exit 77
STUB
    cat >"$WORK/sc2" <<'STUB'
#!/bin/sh
echo "RL-STATUS leg=SC2 result=PASS cases=0 skipped=0 reason=-"
exit 0
STUB
    cat >"$WORK/sc3" <<'STUB'
#!/bin/sh
echo "n         bits                 verdict"
echo "1024      0xc22c83738e258f94   ok"
echo "4096      0xc24652312c70de62   ok"
echo ""
echo "89 assertions, 0 failed"
exit 0
STUB
    cat >"$WORK/make-stub" <<'STUB'
#!/bin/sh
[ "$1" = all ]
STUB
    chmod +x "$WORK/sc1" "$WORK/sc2" "$WORK/sc3" "$WORK/make-stub"
}

# register_self_check_stubs gives the stubs registry rows and a declaration that omits SC1's tag.
# Globals:
#   LEGS, expect_skips - Set
register_self_check_stubs() {
    LEGS+=("SC1|selfcheck|a gate that skips with an undeclared tag")
    LEGS+=("SC2|-|a gate that exits 0 having asserted nothing")
    LEGS+=("SC3|-|a stub that prints a convincing transcript and no status line")
    LEGS+=("SC5|-|a gate reporting another leg's receipt")
    LEGS+=("SC6|-|a gate reporting failure but exiting successfully")
    LEGS+=("SC7|-|a gate issuing two contradictory receipts")
    expect_skips="absent-tag"
}

# expect_leg_outcome asserts that one stub was recorded with the result and reason it should be.
# Globals:
#   rows - Read
#   self_check_ran_count, self_check_dead_count - Updated
# Arguments:
#   $1 - The stub's leg ID
#   $2 - The result it has to have been recorded with
#   $3 - A substring its recorded reason has to contain
# Outputs:
#   One row of the self-check table
expect_leg_outcome() {
    local row got_result got_reason
    self_check_ran_count=$((self_check_ran_count + 1))
    row="$(printf '%s' "$rows" | grep "^$1|" | tail -1)"
    got_result="$(printf '%s' "$row" | cut -d'|' -f2)"
    got_reason="$(printf '%s' "$row" | cut -d'|' -f4)"
    if [ "$got_result" = "$2" ] && case "$got_reason" in *"$3"*) true ;; *) false ;; esac; then
        printf '%-4s %-15s %-21s %s\n' "$1" "$2/$3" "as specified" "OK"
    else
        printf '%-4s %-15s %-21s %s\n' "$1" "$2/$3" "got $got_result/$got_reason" "CONTROL DID NOT FIRE"
        self_check_dead_count=$((self_check_dead_count + 1))
    fi
}

# check_stub_classification runs the receipt checks on synthetic gates and runs L11 with
# a make command whose clean target fails.
# Globals:
#   WORK - Read
# Outputs:
#   One row per stub
check_stub_classification() {
    local -a lanelang_make=("$WORK/make-stub")
    run_leg SC1 "$WORK/sc1" >/dev/null 2>&1
    run_leg SC2 "$WORK/sc2" >/dev/null 2>&1
    run_leg SC3 "$WORK/sc3" >/dev/null 2>&1
    run_leg SC5 printf '%s\n' 'RL-STATUS leg=L4 result=PASS cases=1 skipped=0 reason=-' >/dev/null
    run_leg SC6 printf '%s\n' 'RL-STATUS leg=SC6 result=FAIL cases=1 skipped=0 reason=-' >/dev/null
    run_leg SC7 printf '%s\n' 'RL-STATUS leg=SC7 result=FAIL cases=1 skipped=0 reason=broken' \
        'RL-STATUS leg=SC7 result=PASS cases=1 skipped=0 reason=-' >/dev/null
    expect_leg_outcome SC1 SKIP forced-skip
    expect_leg_outcome SC2 FAIL zero-assertions
    expect_leg_outcome SC3 FAIL no-status-line
    expect_leg_outcome SC5 FAIL receipt-leg-mismatch
    expect_leg_outcome SC6 FAIL receipt-result-mismatch
    expect_leg_outcome SC7 FAIL multiple-status-lines
    run_clean_build_leg >/dev/null
    expect_leg_outcome L11 FAIL clean-failed
}

# expect_drift_reported asserts that a drift function named the perturbation it was given.
# Globals:
#   self_check_ran_count, self_check_dead_count - Updated
# Arguments:
#   $1 - The control ID
#   $2 - What the control perturbed
#   $3 - The substring the function's output has to contain
#   $4 - What the function actually printed
# Outputs:
#   One row of the self-check table
expect_drift_reported() {
    self_check_ran_count=$((self_check_ran_count + 1))
    case "$4" in
        *"$3"*) printf '%-4s %-15s %-21s %s\n' "$1" "$2" "reported:$4" "OK" ;;
        *)      printf '%-4s %-15s %-21s %s\n' "$1" "$2" "reported '${4:-nothing}'" "CONTROL DID NOT FIRE"
                self_check_dead_count=$((self_check_dead_count + 1)) ;;
    esac
}

# check_count_drift_rule checks that ledger_count_drift names a leg whose count differs from
# the count in ARTIFACT.md (SC4).
# Globals:
#   rows - Read and restored
#   self_check_ran_count, self_check_dead_count - Updated
# Outputs:
#   One row of the self-check table
check_count_drift_rule() {
    local saved="$rows" probe
    rows="L3|PASS|999999|-"
    probe="$(ledger_count_drift)"
    rows="$saved"
    expect_drift_reported SC4 "ledger count" "L3(map=" "$probe"
}

# check_undeclared_skip_rule asserts that the rule turning SC1's skip into a red run fires.
# Globals:
#   self_check_ran_count, self_check_dead_count - Updated
# Outputs:
#   One row of the self-check table
check_undeclared_skip_rule() {
    local undeclared
    self_check_ran_count=$((self_check_ran_count + 1))
    undeclared="$(undeclared_skips)"
    case "$undeclared" in
        *SC1*) printf '%-4s %-15s %-21s %s\n' RULE "undeclared skip" "reported:$undeclared" "OK" ;;
        *)     printf '%-4s %-15s %-21s %s\n' RULE "undeclared skip" "reported nothing" "CONTROL DID NOT FIRE"
               self_check_dead_count=$((self_check_dead_count + 1)) ;;
    esac
}

# print_self_check_verdict prints leg L10's receipt and exits with its result.
# Globals:
#   self_check_ran_count, self_check_dead_count - Read
# Outputs:
#   The verdict and one RL-STATUS line on stdout
# Returns:
#   Exits 0 when at least nine controls ran and every one fired, 1 otherwise
print_self_check_verdict() {
    echo "------------------------------------------------------------"
    if [ "$self_check_ran_count" -lt 9 ]; then
        echo "FAILURE: only $self_check_ran_count harness control(s) ran, and there are at least 9. Something"
        echo "         stopped them being reached, so this leg is not evidence of anything."
        echo "RL-STATUS leg=L10 result=FAIL cases=$self_check_ran_count skipped=0 reason=harness-self-check-degenerate"
        exit 1
    fi
    if [ "$self_check_dead_count" -gt 0 ]; then
        echo "FAILURE: $self_check_dead_count harness control(s) did not fire. The harness cannot be trusted to"
        echo "         report a broken gate, so no result it prints means anything."
        echo "RL-STATUS leg=L10 result=FAIL cases=$self_check_ran_count skipped=0 reason=$self_check_dead_count-controls-dead"
        exit 1
    fi
    echo "THE HARNESS CATCHES ITS OWN BREAKS ($self_check_ran_count controls, all fired)"
    echo "RL-STATUS leg=L10 result=PASS cases=$self_check_ran_count skipped=0 reason=harness-self-checked"
    exit 0
}

# run_self_check runs the harness's own must-fail controls and exits.
# Globals:
#   WORK - Read
#   LEGS, expect_skips, rows and the tallies - Updated by the controls
# Returns:
#   Exits through print_self_check_verdict
run_self_check() {
    echo "The Replay Lattice: the harness's own must-fail controls"
    echo "ctl  gate            expected              actual"
    echo "------------------------------------------------------------"
    write_self_check_stubs
    register_self_check_stubs
    check_stub_classification
    check_count_drift_rule
    check_undeclared_skip_rule
    print_self_check_verdict
}

# build_make_commands assembles the make invocation, passing CC through where the caller set it.
# Globals:
#   lanelang_make - Set
build_make_commands() {
    lanelang_make=(make -C src/lanelang)
    [ "${CC+set}" = set ] && lanelang_make+=("CC=$CC")
    return 0
}

# run_clean_build_leg runs the checker's clean target before rebuilding it for L11.
# Globals:
#   WORK, lanelang_make - Read
#   rows and the tallies - Updated through record
# Outputs:
#   One run-table row and up to fifteen lines from the failed target's log
# Returns:
#   0 when both targets succeeded, 1 otherwise
run_clean_build_leg() {
    if ! "${lanelang_make[@]}" clean >"$WORK/build.log" 2>&1; then
        leg_row L11 FAIL 0 clean-failed
    elif ! "${lanelang_make[@]}" all >"$WORK/build.log" 2>&1; then
        leg_row L11 FAIL 0 build-failed
    else
        leg_row L11 PASS 1 built
        return 0
    fi
    tail -15 "$WORK/build.log" | sed 's/^/      /'
    return 1
}

# read_all_target_binaries prints the binaries a Makefile's `all:` target builds, one per line.
# Arguments:
#   $1 - Path to the Makefile
#   $2.. - Binaries to leave out, named rather than filtered implicitly
# Outputs:
#   One binary name per line on stdout
read_all_target_binaries() {
    local makefile="$1"; shift
    local excluded=" $* "
    sed -n '/^all:/,/^$/p' "$makefile" | tr ' \\\t' '\n\n\n' | sed -n 's|^\$(BIN)/||p' \
        | while read -r name; do
              [ -n "$name" ] || continue
              rl_in_set "$name" "$excluded" || printf '%s\n' "$name"
          done
}

# gate_binary_defect checks that the gate is executable and prints a compiler stamp and a clean
# control stamp under its own name.
# Globals:
#   WORK, BIN - Read
# Arguments:
#   $1 - The binary's name
# Outputs:
#   One space-prefixed complaint on stdout, or nothing
gate_binary_defect() {
    local name="$1" stamp="$WORK/$1.stamp"
    [ -x "$BIN/$name" ] || { printf ' %s(missing)' "$name"; return; }
    "$BIN/$name" >"$stamp" 2>/dev/null
    if ! grep -qE "^BUILD $name compiler=" "$stamp"; then
        printf ' %s(no-build-stamp)' "$name"
    elif ! grep -qE "^BUILD $name .* ctl=-$" "$stamp"; then
        printf ' %s(%s)' "$name" "$(sed -n "s/^BUILD $name .*ctl=/ctl=/p" "$stamp" | head -1)"
    fi
}

# make_targets_out_of_date names a tree whose `all:` target is not current after L11's build.
# Globals:
#   lanelang_make - Read
# Outputs:
#   One space-prefixed complaint on stdout, or nothing
make_targets_out_of_date() {
    "${lanelang_make[@]}" -q all >/dev/null 2>&1 || printf ' lanelang-out-of-date-after-build'
}

# report_build_stamp_leg adds L12 to the run table and records its outcome.
# Globals:
#   rows and the tallies - Updated through record
# Arguments:
#   $1 - Everything wrong with the built binaries, empty when nothing is
#   $2 - How many binaries carried a clean stamp
# Outputs:
#   One run-table row
report_build_stamp_leg() {
    if [ -n "$1" ]; then
        rl_report_row L12 FAIL 0 "stale-or-missing:$1"
        record L12 FAIL 0 "stale-binaries"
    else
        rl_report_row L12 PASS "$2" freshly-built
        record L12 PASS "$2" freshly-built
    fi
}

# surviving_control_binaries lists executables that print a control stamp without a clean stamp.
# Globals:
#   WORK, BIN - Read
# Outputs:
#   One space-prefixed complaint per surviving control binary
surviving_control_binaries() {
    local path name out=""
    for path in "$BIN"/*; do
        [ -x "$path" ] && [ -f "$path" ] || continue
        name="$(basename "$path")"
        "$path" >"$WORK/$name.scan" 2>/dev/null
        grep -qE "^BUILD .* ctl=-$" "$WORK/$name.scan" && continue
        grep -qE "^BUILD .* ctl=" "$WORK/$name.scan" \
            && out="$out $name($(sed -n 's/^BUILD .*ctl=/ctl=/p' "$WORK/$name.scan" | head -1))"
    done
    printf '%s' "$out"
}

# run_build_stamp_leg checks the gate binaries' stamps and looks for control stamps in the build
# directory. If those checks pass, it asks make whether the all target is up to date.
# Globals:
#   rows and the tallies - Updated through report_build_stamp_leg
# Outputs:
#   One run-table row
run_build_stamp_leg() {
    local checker_binaries binary defect
    local stale="" stamped=0 binary_count
    checker_binaries="$(read_all_target_binaries src/lanelang/Makefile lanelang)"
    binary_count="$(printf '%s\n' "$checker_binaries" | sort -u | grep -c .)"
    [ -n "$checker_binaries" ] \
        || stale=" could-not-read-all-target-from-Makefile"
    stale="$stale$(rl_floor_shortfall 'gate binaries named by the all: target' "$binary_count" 4)"
    for binary in $checker_binaries; do
        defect="$(gate_binary_defect "$binary")"
        if [ -n "$defect" ]; then stale="$stale$defect"; else stamped=$((stamped + 1)); fi
    done
    stale="$stale$(surviving_control_binaries)"
    [ -z "$stale" ] && stale="$(make_targets_out_of_date)"
    report_build_stamp_leg "$stale" "$stamped"
}

# run_build_legs runs the harness controls, clean build and build stamp checks in dependency order.
# Globals:
#   rows and the tallies - Updated through the legs
run_build_legs() {
    run_leg L10 ./reproduce.sh --self-check
    if run_clean_build_leg; then
        run_build_stamp_leg
    else
        leg_row L12 FAIL 0 clean-build-failed
    fi
}

# run_oracle_legs runs the round-trip, differential and metamorphic checks for L6, L7 and L8.
# Globals:
#   BIN - Read
#   rows and the tallies - Updated through the legs
run_oracle_legs() {
    if [ -x "$BIN/lanefuzz" ]; then
        run_leg L6 "$BIN/lanefuzz" --roundtrip
        run_leg L7 "$BIN/lanefuzz" --agree
        run_leg L8 "$BIN/lanefuzz" --metamorphic
    else
        leg_row L6 SKIP 0 not-built
        leg_row L7 SKIP 0 not-built
        leg_row L8 SKIP 0 not-built
    fi
}

# run_schedule_leg checks whether pinned values keep their bits across schedules for L4.
# Globals:
#   BIN - Read
run_schedule_leg() {
    run_leg L4 "$BIN/test_schedule"
}

# run_checker_legs runs the proof, fixture, schedule, blessing and oracle checks in leg order.
# Globals:
#   WORK, BIN, lanelang_make - Read
#   rows and the tallies - Updated through the legs
# Outputs:
#   A row for each leg run and up to ten lines of the build log if the checker failed to build
run_checker_legs() {
    run_leg L2 ./scripts/check-proofs-eval.sh
    if "${lanelang_make[@]}" all >"$WORK/ll.log" 2>&1; then
        run_leg L3 "$BIN/test_lanelang" src/tests/lanelang
        run_schedule_leg
        run_leg L5 "$BIN/test_bless"
    else
        leg_row L3 FAIL 0 lanelang-build-failed
        tail -10 "$WORK/ll.log" | sed 's/^/      /'
    fi
    run_oracle_legs
    run_leg L9 ./scripts/check-proofs.sh
}

# run_cccl_leg runs the CCCL composition counterexample for L1 when its script is available.
# Globals:
#   rows and the tallies - Updated through the leg
run_cccl_leg() {
    if [ -x src/experiments/cccl/run.sh ]; then
        run_leg L1 src/experiments/cccl/run.sh
    else
        leg_row L1 SKIP 0 not-built
    fi
}

# run_experiment_legs runs the CCCL experiment.
# Globals:
#   rows and the tallies - Updated through the leg
run_experiment_legs() {
    run_cccl_leg
}


# verdict_registry_complete asserts that every registered leg reported something.
# Globals:
#   LEGS, rows - Read
# Outputs:
#   Three lines naming the legs that never ran
# Returns:
#   0 when every registered leg reported, 1 otherwise
verdict_registry_complete() {
    local row id missing=""
    for row in "${LEGS[@]}"; do
        id="${row%%|*}"
        case "$rows" in
            *"${id}|"*) ;;
            *) missing="$missing $id" ;;
        esac
    done
    [ -n "$missing" ] || return 0
    echo "FAILURE: registered but never ran:$missing"
    echo "  (the registry declares these legs and nothing invoked them. A suite that reports on"
    echo "   fewer gates than it declares is reporting on a different suite.)"
    return 1
}

# verdict_control_roster asserts that controls.sh and the reviewer's map agree about which controls
# exist.
# Outputs:
#   A failure message describing any registry or map discrepancies
# Returns:
#   0 when the rosters agree, 1 otherwise
verdict_control_roster() {
    local drift
    drift="$(control_roster_drift)"
    [ -n "$drift" ] || return 0
    echo "FAILURE: controls.sh and ARTIFACT.md disagree about which controls exist:$drift"
    echo "  (a control with no row is one a reviewer cannot look up; a row with no control is a"
    echo "   promise nothing keeps. Add or remove the row in the same change as the control.)"
    return 1
}

# verdict_ledger_counts reports any map or count discrepancies found by ledger_count_drift.
# Outputs:
#   Four lines naming the drifted rows
# Returns:
#   0 when ledger_count_drift reported no discrepancies, 1 otherwise
verdict_ledger_counts() {
    local drift
    drift="$(ledger_count_drift)"
    [ -n "$drift" ] || return 0
    echo "FAILURE: ARTIFACT.md states a count the leg did not report:$drift"
    echo "  (the map a reviewer reads and the run they reproduce it with disagree. Update the row"
    echo "   in the same change as whatever moved the count, or state a range where the"
    echo "   environment legitimately moves it: cases=3-4, or cases=varies.)"
    return 1
}

# verdict_declared_skips fails the run when undeclared_skips reports a disallowed skip.
# Outputs:
#   Up to five lines naming the skips and what could be done about them
# Returns:
#   0 when every skip was allowed, 1 otherwise
verdict_declared_skips() {
    local unexpected
    unexpected="$(undeclared_skips)"
    [ -n "$unexpected" ] || return 0
    echo "FAILURE: skips that are not allowed:$unexpected"
    case "$unexpected" in
        *never-skips*)
            echo "  (a leg marked never-skips has no environmental precondition, so it cannot be"
            echo "   declared away: --expect-skips will not help. Either it is genuinely broken,"
            echo "   or the registry is wrong about it needing nothing.)" ;;
    esac
    if [ -n "$(printf '%s' "$unexpected" | tr ' ' '\n' | grep -v 'never-skips' | grep .)" ]; then
        echo "  (the rest: declare them with --expect-skips=, or fix the environment)"
    fi
    return 1
}

# print_green_line prints the final tally, naming the legs that skipped.
# Globals:
#   pass, skip, skipped_legs - Read
print_green_line() {
    if [ "$skip" -gt 0 ]; then
        echo "ALL CHECKS GREEN ($pass pass, $skip skipped:$skipped_legs)"
    else
        echo "ALL CHECKS GREEN ($pass pass, 0 skipped)"
    fi
}

# print_verdict checks the reported results against the registry, the documented counts and
# the declared skips before printing the final tally.
# Globals:
#   fail, failed_legs - Read
# Outputs:
#   The verdict on stdout
# Returns:
#   0 when the run was clean, 1 otherwise
print_verdict() {
    rl_flush_rows
    echo "-----------------------------"
    verdict_registry_complete || return 1
    verdict_control_roster || return 1
    verdict_ledger_counts || return 1
    verdict_declared_skips || return 1
    if [ "$fail" -gt 0 ]; then
        echo "FAILURE: $fail leg(s) failed:$failed_legs"
        return 1
    fi
    print_green_line
}

# main runs the whole suite, or whichever single mode the command line asked for.
# Globals:
#   WORK - Set, and removed on a trap
#   list_only, self_check - Read
# Arguments:
#   $@ - The command line
# Outputs:
#   The run table and the verdict on stdout
# Returns:
#   0 on success, including allowed skips, 1 on a failed check and 2 on a bad argument
main() {
    parse_arguments "$@"
    WORK="$(rl_make_work_directory replay-lattice)" || exit 1
    trap 'rm -rf "$WORK"' EXIT INT TERM
    if [ "$list_only" = 1 ]; then
        print_leg_table
        exit 0
    fi
    [ "$self_check" = 1 ] && run_self_check
    echo "The Replay Lattice: reproducing every receipt"
    echo "chk  res  cases  reason"
    echo "-----------------------------"
    build_make_commands
    run_build_legs
    run_experiment_legs
    run_checker_legs
    print_verdict
}

main "$@"
