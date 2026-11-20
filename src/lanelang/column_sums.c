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

#include "column_sums.h"

#include "kernels.h"

// replay_block_pair sums two complete blocks side by side. Each addition advances the
// same position in both blocks, preserving the serial order within each block.
//
// Takes values (double) which points to the beginning of the first block.
// Takes totals (double) which receives the two sums in block order.
void replay_block_pair(const double *values, double *totals) {
    double first = 0.0;
    double second = 0.0;
    for (size_t i = 0; i < EVAL_BLOCK; i++) {
        first += values[i];
        second += values[EVAL_BLOCK + i];
    }
    totals[0] = first;
    totals[1] = second;
}

// FREE_ACCUMULATORS counts the independent sums in a free fold.
#define FREE_ACCUMULATORS 16

// sum_deferred_chunk reads a chunk through the evaluator with the same grouping used for
// a materialised column.
//
// Takes state (eval_state_t) which supplies the input reader.
// Takes input (int) which identifies the column.
// Takes low (size_t) which is the first element.
// Takes high (size_t) which is one past the final element.
// Takes partials (double) which receives the independent sums.
// Returns the serial sum of the remaining elements.
static double sum_deferred_chunk(eval_state_t *state, const int input, const size_t low,
                                 const size_t high, double *partials) {
    size_t i = low;
    for (; high - i >= FREE_ACCUMULATORS; i += FREE_ACCUMULATORS) {
        for (size_t lane = 0; lane < FREE_ACCUMULATORS; lane++) {
            partials[lane] += eval_element_of(state, input, i + lane);
        }
    }
    double tail = 0.0;
    for (; i < high; i++) {
        tail += eval_element_of(state, input, i);
    }
    return tail;
}

// On x86-64 Linux with glibc, compilers supporting target_clones emit variants for the
// loader to select at run time. Other builds use the compiler's usual target.
#if defined(__x86_64__) && defined(__linux__) && defined(__GLIBC__) && defined(__has_attribute)
#if __has_attribute(target_clones)
__attribute__((target_clones("avx512f", "avx2", "default")))
#endif
#endif
// free_chunk_sum sums a free fold's worker chunk using independent accumulators.
//
// Takes state (eval_state_t) which supplies the input column.
// Takes input (int) which identifies the column.
// Takes low (size_t) which is the first element.
// Takes high (size_t) which is one past the final element.
// Returns the chunk total.
double free_chunk_sum(eval_state_t *state, const int input, const size_t low, const size_t high) {
    const double *values = state->columns[input];
    double partials[FREE_ACCUMULATORS] = {0.0};
    double total = 0.0;
    size_t i = low;
    if (values && high <= state->lengths[input]) {
        for (; high - i >= FREE_ACCUMULATORS; i += FREE_ACCUMULATORS) {
            for (size_t lane = 0; lane < FREE_ACCUMULATORS; lane++) {
                partials[lane] += values[i + lane];
            }
        }
        for (; i < high; i++) {
            total += values[i];
        }
    } else {
        total = sum_deferred_chunk(state, input, low, high, partials);
    }
    for (size_t lane = 0; lane < FREE_ACCUMULATORS; lane++) {
        total += partials[lane];
    }
    return total;
}
