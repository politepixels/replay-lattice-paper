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

#ifndef RL_STATUS_H
#define RL_STATUS_H

#include <stdio.h>
#include <string.h>

// RL_EXIT_PASS is the exit code of a gate that passed. Read by reproduce.sh as "this leg
// asserted and held".
#define RL_EXIT_PASS 0

// RL_EXIT_FAIL is the exit code of a gate that failed. Any other non-zero code is
// reported as an exit the harness has no reading for.
#define RL_EXIT_FAIL 1

// RL_EXIT_SKIP is the exit code of a gate that could not run here. 77 is the autotools
// convention for a skipped test, so CI output reads the same way it does for any other
// project.
#define RL_EXIT_SKIP 77

// rl_status prints the one status line a gate owes and returns the exit code for main().
//
// Takes leg (char) which names the leg the receipt belongs to.
// Takes result (char) which is PASS, SKIP or FAIL.
// Takes cases (long) which is how many assertions the gate made.
// Takes skipped (long) which is how many it declined.
// Takes reason (char) which is the declared reason token.
static inline int rl_status(
    const char *leg,
    const char *result,
    const long cases,
    const long skipped,
    const char *reason
) {
    printf("RL-STATUS leg=%s result=%s cases=%ld skipped=%ld reason=%s\n",
           leg, result, cases, skipped, reason ? reason : "-");
    (void) fflush(stdout);
    if (result[0] == 'P') {
        return RL_EXIT_PASS;
    }
    return result[0] == 'S' ? RL_EXIT_SKIP : RL_EXIT_FAIL;
}

// RL_PASS reports leg `leg` as having made `cases` assertions, all of which held.
#define RL_PASS(leg, cases)          rl_status((leg), "PASS", (cases), 0, "-")

// RL_FAIL reports leg `leg` as broken after `cases` assertions, naming a declared FAIL
// token.
#define RL_FAIL(leg, cases, reason)  rl_status((leg), "FAIL", (cases), 0, (reason))

// RL_CTL_MARK appends one control macro name to a comma-separated list. The buffer must
// be a character array, not a pointer, because its capacity is taken with sizeof, and it
// must already be NUL terminated.
#define RL_CTL_MARK(buffer, name)                                                     \
    do {                                                                              \
        if ((buffer)[0]) {                                                            \
            strncat((buffer), ",", sizeof (buffer) - strlen(buffer) - 1);             \
        }                                                                             \
        strncat((buffer), (name), sizeof (buffer) - strlen(buffer) - 1);              \
    } while (0)

// rl_control_macros returns the control macros this binary was built with, comma
// separated, or "-" for a clean build. The storage is static, so the result is valid
// until the next call.
static inline const char *rl_control_macros(void) {
    static char buffer[512];
    buffer[0] = '\0';
#ifdef LANELANG_NO_HEAL
    RL_CTL_MARK(buffer, "LANELANG_NO_HEAL");
#endif
#ifdef LANELANG_CSE_IGNORES_LANE
    RL_CTL_MARK(buffer, "LANELANG_CSE_IGNORES_LANE");
#endif
#ifdef LANELANG_KERNEL_FOLLOWS_DEMAND
    RL_CTL_MARK(buffer, "LANELANG_KERNEL_FOLLOWS_DEMAND");
#endif
#ifdef LANELANG_SCAN_BLOCK_FOLLOWS_WORKERS
    RL_CTL_MARK(buffer, "LANELANG_SCAN_BLOCK_FOLLOWS_WORKERS");
#endif
#ifdef LANELANG_NO_GROUNDING
    RL_CTL_MARK(buffer, "LANELANG_NO_GROUNDING");
#endif
#ifdef LANELANG_EXACT_LOWERS_TO_TREE
    RL_CTL_MARK(buffer, "LANELANG_EXACT_LOWERS_TO_TREE");
#endif
#ifdef LANELANG_ORDERED_ALIASES_EXACT
    RL_CTL_MARK(buffer, "LANELANG_ORDERED_ALIASES_EXACT");
#endif
#ifdef LANELANG_BLESS_FLIPS_GROUNDED
    RL_CTL_MARK(buffer, "LANELANG_BLESS_FLIPS_GROUNDED");
#endif
#ifdef LANELANG_BLESS_ON_ANY_NODE
    RL_CTL_MARK(buffer, "LANELANG_BLESS_ON_ANY_NODE");
#endif
#ifdef LANELANG_OPAQUE_IS_GROUNDED
    RL_CTL_MARK(buffer, "LANELANG_OPAQUE_IS_GROUNDED");
#endif
#ifdef LANELANG_DEMAND_STAMPS_NODE
    RL_CTL_MARK(buffer, "LANELANG_DEMAND_STAMPS_NODE");
#endif
#ifdef LANELANG_QUALIFIERS_UNRESERVED
    RL_CTL_MARK(buffer, "LANELANG_QUALIFIERS_UNRESERVED");
#endif
#ifdef RL_TILE_REBRACKETS
    RL_CTL_MARK(buffer, "RL_TILE_REBRACKETS");
#endif
#ifdef RL_PINNED_EVAL_BREAKS
    RL_CTL_MARK(buffer, "RL_PINNED_EVAL_BREAKS");
#endif
#ifdef RL_LATTICE_ORDER_FAILOPEN
    RL_CTL_MARK(buffer, "RL_LATTICE_ORDER_FAILOPEN");
#endif
    return buffer[0] ? buffer : "-";
}

// rl_build_stamp prints the build fingerprint every check binary owes before its
// assertions.
//
// Takes who (char) which names the binary printing the line.
static inline void rl_build_stamp(const char *who) {
    printf("BUILD %s compiler=%s std=%ld double=%zu opt=%s ctl=%s\n",
           who,
#if defined(__VERSION__)
           __VERSION__,
#else
           "unknown",
#endif
           (long) __STDC_VERSION__,
           sizeof(double),
#if defined(__OPTIMIZE__)
           "yes",
#else
           "no",
#endif
           rl_control_macros()
    );
}

#endif  // RL_STATUS_H
