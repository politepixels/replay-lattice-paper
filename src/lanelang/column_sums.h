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

#ifndef LANELANG_COLUMN_SUMS_H
#define LANELANG_COLUMN_SUMS_H

#include <math.h>
#include <stddef.h>

// EVAL_BLOCK is the fixed leaf width of the `replay` tree. The final block may be
// shorter. Each block sums its elements in serial order.
#define EVAL_BLOCK 64

// column_sum adds the elements of a materialised column from left to right.
//
// Takes values (double) which supplies the elements.
// Takes length (size_t) which is the number of elements.
// Returns the serial sum.
static inline double column_sum(const double *values, const size_t length) {
    double sum = 0.0;
    for (size_t i = 0; i < length; i++) {
        sum += values[i];
    }
    return sum;
}

// replay_block_pair sums two complete blocks side by side. Each addition advances the
// same position in both blocks, preserving the serial order within each block.
//
// Takes values (double) which points to the beginning of the first block.
// Takes totals (double) which receives the two sums in block order.
void replay_block_pair(const double *values, double *totals);

// replay_column_blocks fills the assigned block totals from a materialised column.
//
// Takes values (double) which supplies the elements.
// Takes length (size_t) which is the number of elements.
// Takes first (size_t) which identifies the first assigned block.
// Takes end_block (size_t) which is one past the final assigned block.
// Takes totals (double) which receives the sums in input order.
static inline void replay_column_blocks(const double *values, const size_t length, const size_t first,
                                        const size_t end_block, double *totals) {
    size_t b = first;
    const size_t full = length / EVAL_BLOCK;
    const size_t end = end_block < full ? end_block : full;
    for (; b + 1 < end; b += 2) {
        replay_block_pair(values + b * EVAL_BLOCK, totals + b);
    }
    for (; b < end_block; b++) {
        const size_t low = b * EVAL_BLOCK;
        const size_t left = length - low;
        totals[b] = column_sum(values + low, left < EVAL_BLOCK ? left : EVAL_BLOCK);
    }
}

// replay_column_total combines block totals in order. A quiet NaN gives unordered sums
// one representation across scalar and vector instructions.
//
// Takes totals (double) which holds the block totals.
// Takes count (size_t) which is the number of blocks.
// Returns the sum of the blocks in input order.
static inline double replay_column_total(const double *totals, const size_t count) {
    const double sum = column_sum(totals, count);
    return isnan(sum) ? NAN : sum;
}

#endif  // LANELANG_COLUMN_SUMS_H
