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

#ifndef LANELANG_LANE_H
#define LANELANG_LANE_H

// lane_t is a determinism demand, ordered by substitutability from the strongest
// guarantee to the weakest.
typedef enum {
    // LANE_NONE is no demand stated, never a computed class, only a missing declaration.
    LANE_NONE   = 0,

    // LANE_EXACT is the value equals the serial reference oracle's result under the fixed
    // build.
    LANE_EXACT  = 1,

    // LANE_REPLAY guarantees the same bits across schedules for fixed data and shape
    // under the fixed build.
    LANE_REPLAY = 2,

    // LANE_APPROX permits the result bits to vary with the schedule.
    LANE_APPROX = 3
} lane_t;

// LANE_CHAIN_HEIGHT counts the elements of the lane chain. A computed lane can climb at
// most LANE_CHAIN_HEIGHT - 1 times. LANE_NONE represents an absent demand.
#define LANE_CHAIN_HEIGHT 3

// lane_join is the join of two lanes, which is the weaker of the pair.
//
// Takes left (lane_t) which is the first operand.
// Takes right (lane_t) which is the second operand.
static inline lane_t lane_join(const lane_t left, const lane_t right) {
    return (lane_t) (left > right ? left : right);
}

// lane_satisfies reports whether a value of lane `have` may satisfy a demand for lane
// `want`, where a demand of LANE_NONE is no demand and anything satisfies it.
//
// Takes have (lane_t) which is the lane the value carries.
// Takes want (lane_t) which is the lane being demanded.
//
// Returns non-zero when the substitution is legal.
static inline int lane_satisfies(const lane_t have, const lane_t want) {
    return want == LANE_NONE || have <= want;
}

// lane_name is the source spelling of `lane`, as a diagnostic and a figure would print
// it.
//
// Takes lane (lane_t) which is the lane to report.
//
// Returns a string literal used in diagnostics and graph labels.
static inline const char *lane_name(const lane_t lane) {
    switch (lane) {
    case LANE_EXACT:  return "exact";
    case LANE_REPLAY: return "replay";
    case LANE_APPROX: return "approx";
    case LANE_NONE:   return "(none)";
    }
    return "(none)";
}

// opclass_t is the schedule class of an operation's own lowering contract.
typedef enum {
    // OPC_NEUTRAL covers elementwise arithmetic, data movement, literals and external
    // inputs, which perform no reassociable combination and so cannot introduce schedule
    // dependence.
    OPC_NEUTRAL,

    // OPC_FOLD_EXACT is an oracle-order fold, which preserves the input join and costs
    // all reduction parallelism.
    OPC_FOLD_EXACT,

    // OPC_FOLD_INTEGER permits regrouping within its checked domain. Integral inputs
    // whose absolute values sum to less than 2^53 give the serial result in any grouping.
    OPC_FOLD_INTEGER,

    // OPC_FOLD_REPLAY is a fixed-tree fold, which caps the result at LANE_REPLAY, and
    // whose leaf blocks a parallel backend may fold concurrently.
    OPC_FOLD_REPLAY,

    // OPC_FOLD_APPROX permits a free reduction to reassociate its inputs. The result bits
    // may vary with the schedule.
    OPC_FOLD_APPROX,

    // OPC_SCAN_REPLAY scans fixed-width blocks and advances the base in block order for
    // every worker count.
    OPC_SCAN_REPLAY,

    // OPC_SCAN_APPROX is a free scan, which chunks by worker count, so the partial
    // offsets combine in an order the deployment picks.
    OPC_SCAN_APPROX
} opclass_t;

// bitsdep_t describes what determines a kernel's output bits.
typedef enum {
    // BITS_NEUTRAL means the operation introduces no reassociation, so its result
    // preserves the agreement of its operands. Grounding separately describes a source's
    // dependence on the input world.
    BITS_NEUTRAL,

    // BITS_DATA_AND_REFERENCE means the result agrees with the serial reference under the
    // fixed build. An order-independent kernel can still differ from that reference, as a
    // correctly rounded sum can differ from a serial fold.
    BITS_DATA_AND_REFERENCE,

    // BITS_DATA_AND_SHAPE means that only (data, shape) determines the result bits. The
    // worker count, launch geometry and other members of a batch cannot change them.
    BITS_DATA_AND_SHAPE,

    // BITS_SCHEDULE allows the result bits to vary with the schedule.
    BITS_SCHEDULE
} bitsdep_t;

// bitsdep_lane is the lane a kernel whose bits depend on `dependency` contributes to the
// join.
//
// A kernel with a fixed result for its data and shape contributes LANE_REPLAY. Agreement
// with the serial reference strengthens this to LANE_EXACT. A kernel whose contract
// permits schedule dependence contributes LANE_APPROX. BITS_NEUTRAL contributes the join
// identity because the operation introduces no divergence of its own.
//
// Takes dependency (bitsdep_t) which is what the kernel's bits depend on.
static inline lane_t bitsdep_lane(const bitsdep_t dependency) {
    switch (dependency) {
    case BITS_SCHEDULE:           return LANE_APPROX;
    case BITS_DATA_AND_SHAPE:     return LANE_REPLAY;
    case BITS_DATA_AND_REFERENCE:
    case BITS_NEUTRAL:        return LANE_EXACT;
    }
    return LANE_EXACT;
}

// opclass_bits is what the bits of an operation of class `op_class` are a function of.
//
// Each kernel class has its own row, including classes with the same dependency. This
// makes each class's contribution to the join explicit.
//
// Takes op_class (opclass_t) which is the schedule class the kernel carries.
static inline bitsdep_t opclass_bits(const opclass_t op_class) {
    switch (op_class) {
    case OPC_FOLD_APPROX:  return BITS_SCHEDULE;
    case OPC_FOLD_REPLAY:  return BITS_DATA_AND_SHAPE;
    case OPC_FOLD_EXACT:
    case OPC_FOLD_INTEGER: return BITS_DATA_AND_REFERENCE;
    case OPC_SCAN_REPLAY:  return BITS_DATA_AND_SHAPE;
    case OPC_SCAN_APPROX:  return BITS_SCHEDULE;
    case OPC_NEUTRAL:  return BITS_NEUTRAL;
    }
    return BITS_NEUTRAL;
}

// opclass_lane is the lane an operation of class `op_class` contributes to the join,
// derived from what the kernel's bits depend on rather than asserted per operation.
//
// Takes op_class (opclass_t) which is the schedule class the kernel carries.
static inline lane_t opclass_lane(const opclass_t op_class) {
    return bitsdep_lane(opclass_bits(op_class));
}

// opclass_name gives the printed name of a schedule class for diagnostics and graph
// labels.
//
// Takes op_class (opclass_t) which is the schedule class the kernel carries.
//
// Returns a string literal owned by this function.
static inline const char *opclass_name(const opclass_t op_class) {
    switch (op_class) {
    case OPC_FOLD_APPROX:  return "fold/free";
    case OPC_FOLD_REPLAY:  return "fold/fixed-tree";
    case OPC_FOLD_EXACT:   return "fold/oracle-order";
    case OPC_FOLD_INTEGER: return "fold/exact-integer";
    case OPC_SCAN_REPLAY:  return "scan/fixed-block";
    case OPC_SCAN_APPROX:  return "scan/free";
    case OPC_NEUTRAL:  return "neutral";
    }
    return "neutral";
}

// BLESS_PROMISE encodes a bare `@bless` as a negative value. A positive value represents
// a checksum biased by one, so `> 0` is the test for "there is something to verify". The
// grounding sweep tracks the waiver separately from the lane.
#define BLESS_PROMISE (-1)

// LANELANG_KILL_ENV names the switch that lets `check`, `lower` and `dot` ignore
// declaration verdicts when set to "1". Standalone structural checks still run. The `run`
// command always enforces declaration verdicts.
#define LANELANG_KILL_ENV "LANELANG_NOCHECK"

#endif  // LANELANG_LANE_H
