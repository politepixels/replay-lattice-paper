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

    // LANE_REPLAY is the value was computed under a fixed tree whose shape depends on the
    // input shape alone.
    LANE_REPLAY = 2,

    // LANE_APPROX is no constraint: the bits may move with the schedule.
    LANE_APPROX = 3
} lane_t;

// LANE_CHAIN_HEIGHT is how many elements the chain has, so a computed lane can climb at
// most LANE_CHAIN_HEIGHT - 1 times; LANE_NONE is not on the chain and is not counted.
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
// Returns a string literal owned by this function. It is byte-compared by the fixture
// runner and by the committed figures, so its spelling is contract text.
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

    // OPC_FOLD_INTEGER is an exact-integer fold, which preserves the input join at no
    // parallelism cost, because integer addition below 2^53 is `exact` in double and so
    // associative there, and the operation enforces its own domain.
    OPC_FOLD_INTEGER,

    // OPC_FOLD_REPLAY is a fixed-tree fold, which caps the result at LANE_REPLAY and
    // folds its leaves concurrently.
    OPC_FOLD_REPLAY,

    // OPC_FOLD_APPROX is a free reduction, which reassociates as the deployment pleases,
    // so its result is schedule-dependent whatever it was given.
    OPC_FOLD_APPROX,

    // OPC_SCAN_REPLAY is a fixed-block scan, whose blocks write disjoint output ranges
    // and whose offset pass runs serially whatever the worker count, so the bits are a
    // function of (data, shape) alone.
    OPC_SCAN_REPLAY,

    // OPC_SCAN_APPROX is a free scan, which chunks by worker count, so the partial
    // offsets combine in an order the deployment picks.
    OPC_SCAN_APPROX
} opclass_t;

// bitsdep_t is what a kernel's output bits are a function of, the one question every row
// of the op-class table answers.
typedef enum {
    // BITS_NEUTRAL is the kernel performs no reassociable combination of its own, so
    // whatever agreement its operands had, its result keeps. It is a closure property,
    // not a claim about a source, and a leaf has no operands for it to be about.
    BITS_NEUTRAL,

    // BITS_DATA_AND_REFERENCE is the bits are a function of the data alone, and equal the
    // serial reference oracle's result under the fixed build. Order-independence alone
    // does not earn `exact`, because a correctly-rounded sum is order-free and is not the
    // serial fold's answer.
    BITS_DATA_AND_REFERENCE,

    // BITS_DATA_AND_SHAPE is the bits are a function of (data, shape) alone: never of the
    // worker count, the launch geometry, or whatever shares the batch.
    BITS_DATA_AND_SHAPE,

    // BITS_SCHEDULE is the bits move with the schedule.
    BITS_SCHEDULE
} bitsdep_t;

// bitsdep_lane is the lane a kernel whose bits depend on `dependency` contributes to the
// join.
//
// The rule is `replay` iff the bits are a function of (data, shape) alone, `exact` iff
// additionally equal to the serial reference, and `approx` otherwise. BITS_NEUTRAL
// contributes LANE_EXACT, the join identity, because a neutral operation introduces no
// divergence of its own and so cannot weaken what it is given.
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
// This is the table itself: one row per kernel, and nothing else in the language decides
// a class. Arms that repeat an earlier answer are deliberate and stay separate, because
// the repetition is what shows the class follows from what the bits depend on.
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

// opclass_name is the printed name of `op_class`, as the dump and the figures show it.
//
// Takes op_class (opclass_t) which is the schedule class the kernel carries.
//
// Returns a string literal owned by this function. The committed figures are
// byte-compared against it, so its spelling is contract text.
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

// BLESS_PROMISE is what a `@bless` carrying no checksum stores, negative because a
// checksum plus one is always positive, so a single comparison separates the promise from
// the receipt and `> 0` is the test for "there is something to verify". It flips no
// verdict: a blessed leaf is still ungrounded, and the bless is consulted only where a
// declaration is accepted or refused.
#define BLESS_PROMISE (-1)

// LANELANG_KILL_ENV is the environment variable that disables the declaration check, so
// the checker's cost and its verdicts can be compared against a build without it.
#define LANELANG_KILL_ENV "LANELANG_NOCHECK"

#endif  // LANELANG_LANE_H
