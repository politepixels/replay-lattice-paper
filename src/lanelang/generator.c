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

#include "generator.h"

// ldexp only, which is exact for an integer power of two and so gives the same bits on
// any host.
#include <math.h>

// TWO_TO_THE_52 is 2^52, the bias and the divisor that turn 53 random bits into a signed
// mantissa in [-1, 1). Exact in binary64, so the conversion adds no rounding of its own.
#define TWO_TO_THE_52 4503599627370496.0

// TWO_TO_THE_52_INT is the same 2^52 as an integer, used to centre the raw 53-bit draw on
// zero before it is scaled.
#define TWO_TO_THE_52_INT 4503599627370496LL

// MANTISSA_DISCARD_BITS is how many low bits are dropped to leave a double's 53-bit
// mantissa, which is 64 minus 53.
#define MANTISSA_DISCARD_BITS 11

// EXPONENT_SPAN is how many exponents the draw is scaled across. Forty binades of dynamic
// range make the summation ill-conditioned enough for the bracketing to reach the low
// bits.
#define EXPONENT_SPAN 41

// EXPONENT_OFFSET centres the drawn exponent on zero, giving [-20, 20].
#define EXPONENT_OFFSET 20

// CANCEL_RESIDUE_SHIFT is how far below a cancellation pair's magnitude its residue sits.
// Small enough that the true sum is dominated by the residues rather than by the
// summands. The low bits of the result are then an artefact of the bracketing.
#define CANCEL_RESIDUE_SHIFT 30

// CANCEL_TAIL_EXPONENT is the exponent of the uncancelled tail element written when the
// requested count is odd. Any small value will do; it is fixed so the column stays
// reproducible.
#define CANCEL_TAIL_EXPONENT (-10)

// XORSHIFT_SHIFT_A, XORSHIFT_SHIFT_B and XORSHIFT_SHIFT_C are the xorshift64* triple.
//
// Published constants of the algorithm rather than choices made here: changing one moves
// the stream, and every bit pattern the checks measure with it.
#define XORSHIFT_SHIFT_A 12

// XORSHIFT_SHIFT_B is the second shift of the xorshift64* triple.
#define XORSHIFT_SHIFT_B 25

// XORSHIFT_SHIFT_C is the third shift of the xorshift64* triple.
#define XORSHIFT_SHIFT_C 27

// XORSHIFT_MULTIPLIER is the output multiplier that makes xorshift64* a `*` variant.
#define XORSHIFT_MULTIPLIER 0x2545F4914F6CDD1DULL

// SEED_MIX is xor-folded into the caller's seed to pick the stream. The value is frozen
// because recorded divergence figures were measured over the stream it selects, so
// changing it changes the bit patterns those figures describe.
#define SEED_MIX (0x9E3779B97F4A7C15ULL * 3u)

// XORSHIFT_FALLBACK_STATE is the state used when the mixed seed lands on zero. Zero is
// the one state a xorshift stream cannot leave, so it is replaced; the particular value
// is arbitrary and only has to be non-zero.
#define XORSHIFT_FALLBACK_STATE 0x123456789ABCDEFULL

// xorshift_next advances a xorshift64* stream and returns the next word. The state must
// not be zero.
//
// Takes state (uint64_t) which is the xorshift state, advanced in place.
static uint64_t xorshift_next(uint64_t *state) {
    uint64_t word = *state;
    word ^= word >> XORSHIFT_SHIFT_A;
    word ^= word << XORSHIFT_SHIFT_B;
    word ^= word >> XORSHIFT_SHIFT_C;
    *state = word;
    return word * XORSHIFT_MULTIPLIER;
}

// mixed_mantissa draws a mantissa in [-1, 1) from 53 random bits.
//
// Takes state (uint64_t) which is the xorshift state, advanced in place.
static double mixed_mantissa(uint64_t *state) {
    const int64_t centred =
        (int64_t) (xorshift_next(state) >> MANTISSA_DISCARD_BITS) - TWO_TO_THE_52_INT;
    return (double) centred / TWO_TO_THE_52;
}

// mixed_exponent draws an exponent in [-20, 20].
//
// Takes state (uint64_t) which is the xorshift state, advanced in place.
static int mixed_exponent(uint64_t *state) {
    return (int) (xorshift_next(state) % EXPONENT_SPAN) - EXPONENT_OFFSET;
}

// generator_fill writes exact cancellation pairs (v, -v) plus a small residue on the
// second element of each pair, so the true sum is tiny relative to the summands and the
// low bits of the result are dominated by the bracketing. An odd count leaves a tail
// element uncancelled.
//
// Takes seed (uint64_t) which seeds the generator.
// Takes values (double) which receives the column.
// Takes count (size_t) which is how many values to write.
void generator_fill(const uint64_t seed, double *values, const size_t count) {
    const uint64_t mixed = seed ^ SEED_MIX;
    uint64_t state = mixed ? mixed : XORSHIFT_FALLBACK_STATE;

    for (size_t i = 0; i + 1 < count; i += 2) {
        const double mantissa = mixed_mantissa(&state);
        const int exponent = mixed_exponent(&state);
        const double value = ldexp(mantissa, exponent);
        values[i] = value;
        values[i + 1] = -value + ldexp(mantissa, exponent - CANCEL_RESIDUE_SHIFT);
    }
    if (count & 1) {
        values[count - 1] = ldexp(1.0, CANCEL_TAIL_EXPONENT);
    }
}
