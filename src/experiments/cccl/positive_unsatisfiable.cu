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

// unsatisfiable reduces `in` into `out` under multiplies over float, demanding a guarantee CCCL
// cannot give for that combination.
void unsatisfiable(float *in, float *out) {
    (void) cub::DeviceReduce::Reduce(in, out, 100, cuda::std::multiplies<float>{}, 1.0f,
                                     ex::require(det::gpu_to_gpu));
}
