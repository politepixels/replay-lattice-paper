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

# The binary under test, the first argument when one is given.
CHECKER="${1:-build/bin/lanelang}"

# The checked-in fixtures some cases run directly.
FIXTURES="src/tests/lanelang"

# The scratch directory holding each case's program and captured streams, removed on a trap.
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
    printf 'RL-STATUS leg=cli result=%s cases=%s skipped=%s reason=%s\n' "$1" "$2" "$3" "$4"
    case "$1" in
        PASS) exit 0 ;;
        SKIP) exit 77 ;;
        *)    exit 1 ;;
    esac
}

# require_checker fails the gate when the binary under test has not been built.
# Globals:
#   CHECKER - Read
# Outputs:
#   The FAIL receipt when the binary is missing
require_checker() {
    [ -x "$CHECKER" ] || say FAIL 0 0 no-checker
}

# fail reports a failed case with the last command's captured streams and stops the gate.
# Globals:
#   WORK, cases - Read
# Arguments:
#   $1 - The case name, which becomes the reason token
#   $2 - What was wrong
# Outputs:
#   The failure and both indented streams on stderr, then the FAIL receipt
fail() {
    printf '    %s: %s\n    stdout:\n' "$1" "$2" >&2
    sed 's/^/      /' "$WORK/stdout" >&2
    printf '    stderr:\n' >&2
    sed 's/^/      /' "$WORK/stderr" >&2
    say FAIL "$cases" 0 "$1"
}

# program writes the program on stdin to the case's own source file, so no case overwrites
# another's.
# Globals:
#   WORK - Read
# Arguments:
#   $1 - The file's name without its extension
program() {
    cat >"$WORK/$1.lane"
}

# expect runs a command and checks its exit status, its whole stdout and a substring of its
# stderr.
# Globals:
#   WORK - Read
#   cases - Incremented when the assertion holds
# Arguments:
#   $1 - The case name
#   $2 - The exit status wanted
#   $3 - The stdout wanted, or empty for none
#   $4 - A substring stderr must contain, or empty for no check
#   $5... - The command to run
# Outputs:
#   The failure on stderr when the command does not behave as wanted
expect() {
    local name=$1 want_status=$2 want_out=$3 want_error=$4 status=0
    shift 4
    "$@" >"$WORK/stdout" 2>"$WORK/stderr" || status=$?
    : >"$WORK/expected"
    if [ -n "$want_out" ]; then
        printf '%s\n' "$want_out" >"$WORK/expected"
    fi
    if [ "$status" -ne "$want_status" ] || ! cmp -s "$WORK/expected" "$WORK/stdout" ||
       { [ -n "$want_error" ] && ! grep -Fq -- "$want_error" "$WORK/stderr"; }; then
        fail "$name" "expected exit $want_status, got $status"
    fi
    cases=$((cases + 1))
}

# expect_stderr checks that the last case's stderr also contains a second substring.
# Globals:
#   WORK - Read
# Arguments:
#   $1 - The case name
#   $2 - The substring stderr must contain
# Outputs:
#   The failure on stderr when the substring is absent
expect_stderr() {
    grep -Fq -- "$2" "$WORK/stderr" || fail "$1" "stderr does not contain '$2'"
}

# expect_count checks how many lines of the last case's stderr contain a substring, which
# catches a diagnostic reported twice.
# Globals:
#   WORK - Read
# Arguments:
#   $1 - The case name
#   $2 - The substring counted
#   $3 - How many lines must contain it
# Outputs:
#   The failure on stderr when the count differs
expect_count() {
    local found
    found="$(grep -Fc -- "$2" "$WORK/stderr")"
    [ "$found" -eq "$3" ] || fail "$1" "stderr has $found lines with '$2', expected $3"
}

# kernel_names prints the kernel each fold or scan lowers to, in graph order.
# Globals:
#   CHECKER, WORK - Read
# Arguments:
#   $1 - The program to lower
# Outputs:
#   One kernel name per line
# Returns:
#   The exit status of `lower`
kernel_names() {
    local lower_status=0
    "$CHECKER" lower "$1" >"$WORK/lowering" || lower_status=$?
    awk '$1 == "kernel" { print $4 }' "$WORK/lowering"
    return "$lower_status"
}

# graph_edges prints the carried and blame edges of a program's Graphviz rendering, each
# named by its endpoints' labels.
# Globals:
#   CHECKER, WORK - Read
# Arguments:
#   $1 - The program to render
# Outputs:
#   A `graph` line, then one line per carried or blame edge
# Returns:
#   The exit status of `dot`
graph_edges() {
    local dot_status=0
    "$CHECKER" dot "$1" >"$WORK/graph" || dot_status=$?
    awk '/^digraph lanelang \{/ { print "graph" }
         /^  n[0-9]+ \[label="/ {
             split($0, quoted, /"/)
             split(quoted[2], label, /\\n/)
             name[$1] = label[1]
         }
         / -> .*label="carried"/ { print "carried", name[$1], "->", name[$3] }
         / -> .*penwidth=3/ { print "blame", name[$1], "->", name[$3] }' "$WORK/graph"
    return "$dot_status"
}

# check_lowering checks the kernels `lower` picks and the edges `dot` draws, for an
# accepted and a refused program each.
# Globals:
#   FIXTURES - Read
check_lowering() {
    expect lower-healed 0 'fold_replay
fold_replay' '' kernel_names "$FIXTURES/accept_healed.lane"

    expect lower-rejected 1 'fold_free
fold_replay' '' kernel_names "$FIXTURES/reject_poison.lane"

    expect dot-recurrence 0 'graph
carried * -> acc' '' graph_edges "$FIXTURES/accept_plain_recurrence.lane"

    expect dot-rejected 1 'graph
blame s -> *
blame * -> sum' '' graph_edges "$FIXTURES/reject_worked_escape.lane"
}

# check_results checks the values `run` prints for scalars, literals at the edges of binary64,
# sums, recurrences and columns.
# Globals:
#   CHECKER, WORK - Read
check_results() {
    program arithmetic <<'EOF'
package app
func main() Float {
    return (2.0 + 3.0) * 4.0
}
EOF
    expect arithmetic 0 20 '' "$CHECKER" run "$WORK/arithmetic.lane"

    program large-literal <<'EOF'
package app
func main() Float {
    return 1e100
}
EOF
    expect large-literal 0 1e+100 '' "$CHECKER" run "$WORK/large-literal.lane"

    program subnormal-literal <<'EOF'
package app
func main() Float {
    return 4.9e-324
}
EOF
    expect subnormal-literal 0 4.9406564584124654e-324 '' \
        "$CHECKER" run "$WORK/subnormal-literal.lane"

    program replay-sum <<'EOF'
package app
func main() Float {
    x := integers(count: 128.0)
    replay total := sum(x * 0.0 + 1.0)
    return total
}
EOF
    expect replay-sum 0 128 '' "$CHECKER" run "$WORK/replay-sum.lane"

    program invariant-recurrence <<'EOF'
package app
func main() exact Float {
    k := 5.0
    total := iterate(times: 3.0, from: 1.0, with: (acc, t) => k) + 100.0
    return total
}
EOF
    expect invariant-recurrence 0 105 '' "$CHECKER" run "$WORK/invariant-recurrence.lane"

    program column <<'EOF'
package app
func main() Column[Float] {
    return integers(count: 3.0)
}
EOF
    expect column 0 '1
-1022
2043' '' "$CHECKER" run "$WORK/column.lane"

    program carried-column <<'EOF'
package app
func main() Column[Float] {
    x := integers(count: 3.0)
    return iterate(times: 2.0, from: 1.0, with: (acc, t) => x + acc)
}
EOF
    expect carried-column 0 '3
-2043
4087' '' "$CHECKER" run "$WORK/carried-column.lane"
}

# check_column_lengths checks that every generator refuses an empty column and one past the
# evaluator's limit.
# Globals:
#   CHECKER, WORK - Read
check_column_lengths() {
    local generator length
    for generator in integers indices source; do
        for length in 0 100001; do
            program "$generator-length-$length" <<EOF
package app
func main() Float {
    x := $generator(count: $length.0)
    return isum(x * 0.0 + 1.0)
}
EOF
            expect "$generator-length-$length" 4 '' "requested column length $length" \
                "$CHECKER" run "$WORK/$generator-length-$length.lane"
        done
    done
}

# check_refusals checks that `run` refuses a lane violation, including one in a declaration
# nothing calls, and that the kill switch cannot turn the check off for `run`.
# Globals:
#   CHECKER, FIXTURES - Read
check_refusals() {
    expect rejected 1 '' 'determinism lane violation' \
        "$CHECKER" run "$FIXTURES/reject_poison.lane"

    expect rejected-declaration 1 '' 'determinism lane violation' \
        "$CHECKER" run "$FIXTURES/reject_uncalled_body_violates.lane"

    expect checks-required 1 '' 'determinism lane violation' \
        env LANELANG_NOCHECK=1 "$CHECKER" run "$FIXTURES/reject_poison.lane"

    expect declaration-checks-required 1 '' 'determinism lane violation' \
        env LANELANG_NOCHECK=1 "$CHECKER" run "$FIXTURES/reject_uncalled_body_violates.lane"

    expect malformed 2 '' '' "$CHECKER" run "$FIXTURES/malformed_self.lane"
}

# check_diagnostics checks that each structural defect is reported once, where it is
# written, and that blame reaches the call that supplied a value.
# Globals:
#   CHECKER, FIXTURES, WORK - Read
check_diagnostics() {
    local prefix message
    for prefix in x @; do
        program "invalid-token-after-$prefix" <<EOF
package app
func main() Float {
    x := 1.0
    return sum($prefix?)
}
EOF
        message=":4:17: error: unexpected character '?'"
        expect "invalid-token-after-$prefix" 2 '' "$message" \
            "$CHECKER" check "$WORK/invalid-token-after-$prefix.lane"
        expect_count "invalid-token-after-$prefix" "$message" 1
    done

    program recursion-once-per-call <<'EOF'
package app
func f(v Float) Float {
    return f(v) + f(v) + f(v)
}
func main() Float {
    return f(1.0)
}
EOF
    expect recursion-once-per-call 2 '' 'recursion is not supported' \
        "$CHECKER" check "$WORK/recursion-once-per-call.lane"
    expect_count recursion-once-per-call 'error:' 3

    program rebinding-refused <<'EOF'
package app
func main() Float {
    x := indices(count: 8.0)
    replay e := sum(x)
    e := sum(x * 2.0)
    return e
}
EOF
    expect rebinding-refused 2 '' "'e' is bound twice in this body" \
        "$CHECKER" check "$WORK/rebinding-refused.lane"

    program nested-step-names <<'EOF'
package app
func main() Float {
    a := 1.0
    return iterate(times: 2.0, from: a, with: (a, t) =>
        iterate(times: 2.0, from: a, with: (a, t) => a + t))
}
EOF
    expect nested-step-names 0 3 '' "$CHECKER" run "$WORK/nested-step-names.lane"

    expect parameter-blame-reaches-call 1 '' \
        ':25:18: note: the argument is passed to the demanding parameter here' \
        "$CHECKER" check "$FIXTURES/reject_param_lane.lane"
}

# check_command_line checks the usage errors, unreadable sources and NUL bytes, which are
# refused before any lane is computed.
# Globals:
#   CHECKER, WORK - Read
check_command_line() {
    expect missing-argument 3 '' 'usage:' "$CHECKER" run

    expect unknown-mode 3 '' 'usage:' "$CHECKER" unknown "$WORK/missing.lane"

    expect missing-file 3 '' 'cannot read' "$CHECKER" run "$WORK/missing.lane"

    expect directory-as-source 3 '' 'cannot read' "$CHECKER" check "$WORK"

    cp "$WORK/arithmetic.lane" "$WORK/nul-after-program.lane"
    printf '\000ignored\n' >>"$WORK/nul-after-program.lane"
    expect nul-after-program 2 '' 'NUL byte in source' \
        "$CHECKER" run "$WORK/nul-after-program.lane"

    cp "$WORK/arithmetic.lane" "$WORK/nul-in-comment.lane"
    printf '// comment\000\n' >>"$WORK/nul-in-comment.lane"
    expect nul-in-comment 2 '' 'NUL byte in source' \
        "$CHECKER" check "$WORK/nul-in-comment.lane"
}

# check_evaluation_failures checks the refusals that only evaluation can make, a checksum the
# data does not match, an integer sum outside its exact domain and a length no literal fixes.
# Globals:
#   CHECKER, WORK - Read
check_evaluation_failures() {
    program checksum-mismatch <<'EOF'
package app
func main() Float {
    @bless(0x1234abcd) x := source(count: 1000.0)
    replay total := sum(x)
    return total
}
EOF
    expect checksum-mismatch 4 '' 'blessed checksum does not match' \
        "$CHECKER" run "$WORK/checksum-mismatch.lane"

    program integer-domain <<'EOF'
package app
func main() Float {
    x := integers(count: 4096.0)
    exact total := isum(x * 6400000.0)
    return total
}
EOF
    expect integer-domain 4 '' 'evaluation failed' "$CHECKER" run "$WORK/integer-domain.lane"

    program unused-checksum <<'EOF'
package app
func main() exact Float {
    @bless(0x1234abcd) unused := source(count: 1000.0)
    return 7.0
}
EOF
    expect unused-checksum 4 '' 'blessed checksum does not match' \
        "$CHECKER" run "$WORK/unused-checksum.lane"

    program parameter-count <<'EOF'
package app
func make(n Float) Column[Float] { return integers(count: n) }
func main() Float { return isum(make(3.0)) }
EOF
    expect parameter-count 4 '' 'column length is unknown' \
        "$CHECKER" run "$WORK/parameter-count.lane"
}

# check_count_arguments checks a generator's count when it names something undefined or
# carries a demand of its own.
# Globals:
#   CHECKER, WORK - Read
check_count_arguments() {
    program undefined-count <<'EOF'
package app
func main() Float { return isum(integers(count: missing)) }
EOF
    expect undefined-count 2 '' "undefined name 'missing'" \
        "$CHECKER" check "$WORK/undefined-count.lane"

    program undefined-count-call <<'EOF'
package app
func main() Float { return sum(indices(count: absent(3.0))) }
EOF
    expect undefined-count-call 2 '' "unknown function 'absent'" \
        "$CHECKER" check "$WORK/undefined-count-call.lane"

    program unused-count-demand <<'EOF'
package app
func want(replay n Float) Float { return n }
func main() exact Float {
    approx n := sum(indices(count: 10.0))
    unused := source(count: want(n))
    return 7.0
}
EOF
    expect unused-count-demand 1 '' 'where replay is demanded' \
        "$CHECKER" run "$WORK/unused-count-demand.lane"
}

# check_unused_values checks that a value nothing reads neither fails evaluation nor carries a
# demand onto the value it aliases.
# Globals:
#   CHECKER, WORK - Read
check_unused_values() {
    program shared-step-value <<'EOF'
package app
func main() exact Float {
    unused := iterate(times: 2.0, from: 0.0, with: (acc, t) => {
        bad := integers(count: 0.0)
        return 3.0 + 4.0
    })
    return 3.0 + 4.0
}
EOF
    expect shared-step-value 0 7 '' "$CHECKER" run "$WORK/shared-step-value.lane"

    program unused-promise-alias <<'EOF'
package app
func total(x Column[Float]) replay Float { return sum(x) }
func main() exact Float {
    value := total(integers(count: 1.0))
    replay unused := value
    return value
}
EOF
    expect unused-promise-alias 0 1 '' "$CHECKER" run "$WORK/unused-promise-alias.lane"

    program unused-parameter-alias <<'EOF'
package app
func take(replay x Float) Float { return x }
func main() exact Float {
    value := sum(integers(count: 1.0))
    replay unused := value
    return take(value)
}
EOF
    expect unused-parameter-alias 0 1 '' "$CHECKER" run "$WORK/unused-parameter-alias.lane"
}

# check_grounding checks where a waiver may be written and that a source read inside a
# recurrence is blamed as the leaf the program does not fix.
# Globals:
#   CHECKER, WORK - Read
check_grounding() {
    program bless-inside-expression <<'EOF'
package app
func main() Float { return sum(@bless source(count: 8.0)) }
EOF
    expect bless-inside-expression 2 '' '@bless is written on the binding' \
        "$CHECKER" check "$WORK/bless-inside-expression.lane"

    program carried-source-origin <<'EOF'
package app
func main() replay Column[Float] {
    return iterate(times: 2.0, from: 0.0, with: (a,t) => (a + a) + source(count: 4.0))
}
EOF
    expect carried-source-origin 1 '' "'source' is a leaf this program does not fix" \
        "$CHECKER" check "$WORK/carried-source-origin.lane"
}

# check_long_programs checks blame across a long chain, healing after a long binding and a
# caret placed under a token past a wide run of tabs.
# Globals:
#   CHECKER, WORK - Read
check_long_programs() {
    awk 'BEGIN {
        print "package app\nfunc main() Float {\n    approx p := sum(indices(count: 8.0))"
        printf "    replay answer := p"
        for (i = 0; i < 70; i++) printf " + 1.0"
        print "\n    return answer\n}"
    }' | program deep-blame
    expect deep-blame 1 '' '70 steps upstream' "$CHECKER" check "$WORK/deep-blame.lane"
    expect_stderr deep-blame 'deep-blame.lane:3:17: note:'
    expect_stderr deep-blame 'deep-blame.lane:4:5: error:'

    awk 'BEGIN {
        print "package app\nfunc main() replay Float {"
        printf "    pad := 0.0"
        for (i = 1; i <= 260; i++) printf " + %d.0", i
        print "\n    return sum(integers(count: 1.0))\n}"
    }' | program late-fold-healed
    expect late-fold-healed 0 \
        'accepted: 1 declaration checked, 1 producer healed, result lane replay' '' \
        "$CHECKER" check "$WORK/late-fold-healed.lane"

    awk 'BEGIN {
        print "package app"
        for (i = 0; i < 10000; i++) print ""
        print "func main() Float {"
        printf "%550s\treturn\tmissing\n}\n", ""
    }' | program wide-tab-diagnostic
    expect wide-tab-diagnostic 2 '' "undefined name 'missing'" \
        "$CHECKER" check "$WORK/wide-tab-diagnostic.lane"
    expand "$WORK/stderr" | awk '
        /^ *10003 \|/ {
            token = index($0, "missing")
            getline
            aligned = token && index($0, "^^^^^^^") == token
        }
        END { exit !aligned }
    ' || fail wide-tab-diagnostic "the caret is not under 'missing' after tab expansion"
}

# main runs every group of cases against the binary under test.
# Globals:
#   WORK - Set
# Outputs:
#   The receipt on stdout
main() {
    require_checker
    WORK="$(rl_make_work_directory lanelang-cli)" || exit 1
    trap 'rm -rf "$WORK"' EXIT INT TERM
    check_lowering
    check_results
    check_column_lengths
    check_refusals
    check_diagnostics
    check_command_line
    check_evaluation_failures
    check_count_arguments
    check_unused_values
    check_grounding
    check_long_programs
    say PASS "$cases" 0 -
}

main "$@"
