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

#ifndef RL_GENERATOR_H
#define RL_GENERATOR_H

#include <stddef.h>
#include <stdint.h>

// generator_fill writes values[0..count) deterministically.
//
// A seed of zero is folded to a usable value rather than degenerating. The bit patterns
// are identical on any IEEE-754 binary64 host.
//
// Takes seed (uint64_t) which seeds the generator.
// Takes values (double) which is the column being folded.
// Takes count (size_t) which is how many nodes the graph holds.
void generator_fill(uint64_t seed, double *values, size_t count);

#endif  // RL_GENERATOR_H
