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

# The fixture runner under test, the first argument when one is given.
RUNNER="${1:-build/bin/test_lanelang}"

# The fixtures the runner is pointed at, the second argument when one is given.
FIXTURES="${2:-src/tests/lanelang}"

# The fixtures given a wrong assertion, each of which the runner must name in its failures.
BROKEN_FIXTURES="accept_healed accept_plain_recurrence malformed_lex_bare_equals"

# The scratch directory holding the edited fixtures and the runner's output, removed on a trap.
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
    printf 'RL-STATUS leg=fixture-runner result=%s cases=%s skipped=%s reason=%s\n' \
        "$1" "$2" "$3" "$4"
    case "$1" in
        PASS) exit 0 ;;
        SKIP) exit 77 ;;
        *)    exit 1 ;;
    esac
}

# require_runner fails the gate when the fixture runner has not been built, because a
# runner that cannot start would otherwise look like one that caught every assertion.
# Globals:
#   RUNNER - Read
# Outputs:
#   The FAIL receipt when the runner is missing
require_runner() {
    [ -x "$RUNNER" ] || say FAIL 0 0 no-runner
}

# copy_fixtures copies every fixture into the scratch directory, so the edits below never
# touch the shipped ones.
# Globals:
#   WORK, FIXTURES - Read
# Outputs:
#   The FAIL receipt when the fixtures cannot be copied
copy_fixtures() {
    cp "$FIXTURES"/*.lane "$WORK/" || say FAIL 0 0 no-fixtures
}

# check_valid_assertion appends a true assertion written with a tab and a carriage return,
# and requires the runner to accept it, so that unusual spacing is parsed rather than skipped.
# Globals:
#   WORK, RUNNER - Read
#   cases - Incremented when the assertion holds
# Outputs:
#   The indented runner output on stderr when the runner refuses
check_valid_assertion() {
    printf '// EXPECT-KERNEL:\tfold_replay\r\n' >>"$WORK/accept_healed.lane"
    if ! "$RUNNER" "$WORK" >"$WORK/valid.log" 2>&1; then
        sed 's/^/    /' "$WORK/valid.log" >&2
        say FAIL "$cases" 0 valid-assertion-refused
    fi
    cases=$((cases + 1))
}

# write_invalid_assertions gives three fixtures an assertion that does not hold. One denies a
# kernel the program uses, one is empty and one names a diagnostic the program never prints.
# Globals:
#   WORK, FIXTURES - Read
write_invalid_assertions() {
    printf '  // EXPECT-NO-KERNEL:\tfold_replay\r\n' >>"$WORK/accept_healed.lane"
    printf '// EXPECT-DIAGNOSTIC:  \r\n' >>"$WORK/accept_plain_recurrence.lane"
    printf '// EXPECT-DIAGNOSTIC: absent message\n' >"$WORK/malformed_lex_bare_equals.lane"
    cat "$FIXTURES/malformed_lex_bare_equals.lane" >>"$WORK/malformed_lex_bare_equals.lane"
}

# check_invalid_assertions_fail requires the runner to exit 1 once the wrong assertions are
# in place.
# Globals:
#   WORK, RUNNER - Read
#   cases - Incremented when the assertion holds
# Outputs:
#   The indented runner output on stderr when the exit status is wrong
check_invalid_assertions_fail() {
    local status=0
    "$RUNNER" "$WORK" >"$WORK/invalid.log" 2>"$WORK/invalid.err" || status=$?
    if [ "$status" -ne 1 ]; then
        sed 's/^/    /' "$WORK/invalid.log" "$WORK/invalid.err" >&2
        say FAIL "$cases" 0 invalid-assertions-accepted
    fi
    cases=$((cases + 1))
}

# check_each_failure_named requires the runner to name every fixture given a wrong
# assertion, so that one caught failure cannot hide two that were missed.
# Globals:
#   WORK, BROKEN_FIXTURES - Read
#   cases - Incremented for each fixture named
# Outputs:
#   The missed fixture and the indented runner errors on stderr
check_each_failure_named() {
    local fixture
    for fixture in $BROKEN_FIXTURES; do
        if ! grep -q "^  FAIL ${fixture}\.lane: " "$WORK/invalid.err"; then
            printf '    the runner did not name %s.lane\n' "$fixture" >&2
            sed 's/^/    /' "$WORK/invalid.err" >&2
            say FAIL "$cases" 0 "missed-$fixture"
        fi
        cases=$((cases + 1))
    done
}

# main copies the fixtures, then checks that the runner accepts the true assertion and
# names each wrong one.
# Globals:
#   WORK - Set
# Outputs:
#   The receipt on stdout
main() {
    require_runner
    WORK="$(rl_make_work_directory lanelang-fixtures)" || exit 1
    trap 'rm -rf "$WORK"' EXIT INT TERM
    copy_fixtures
    check_valid_assertion
    write_invalid_assertions
    check_invalid_assertions_fail
    check_each_failure_named
    say PASS "$cases" 0 -
}

main "$@"
