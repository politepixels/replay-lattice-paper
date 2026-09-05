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
#include <cub/device/device_reduce.cuh>
#include <cuda/__execution/require.h>

// The execution namespace holding `require`.
namespace ex  = cuda::execution;

// The namespace holding the determinism guarantees themselves.
namespace det = ex::determinism;

// compose reduces `in` into `mid` under no determinism guarantee, then reduces `mid` into `out`
// under a guarantee of run-to-run determinism.
void compose(float *in, float *mid, float *out) {
    // Produced under a schedule nobody pinned.
    (void) cub::DeviceReduce::Reduce(in, mid, 100, cuda::std::plus<float>{}, 0.0f,
                                     ex::require(det::not_guaranteed));
    // Consumed where run-to-run determinism is required. No diagnostic, here or anywhere.
    (void) cub::DeviceReduce::Reduce(mid, out, 1, cuda::std::plus<float>{}, 0.0f,
                                     ex::require(det::run_to_run));
}
