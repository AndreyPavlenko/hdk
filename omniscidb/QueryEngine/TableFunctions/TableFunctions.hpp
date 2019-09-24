/*
 * Copyright 2019 OmniSci, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "../../Shared/funcannotations.h"

#define EXTENSION_INLINE extern "C" ALWAYS_INLINE DEVICE
#define EXTENSION_NOINLINE extern "C" NEVER_INLINE DEVICE

EXTENSION_NOINLINE int32_t row_copier(double* input_col,
                                      int* copy_multiplier,
                                      const int64_t* input_row_count_ptr,
                                      int64_t* output_buffer,
                                      int64_t* output_row_count) {
  // Copy the input buffer to the output, duplicating according to copy_multiplier
  const auto input_row_count = *input_row_count_ptr;

  auto output_buf_ptr = reinterpret_cast<double*>(output_buffer);
  for (auto i = 0; i < input_row_count; i++) {
    for (int c = 0; c < *copy_multiplier; c++) {
      output_buf_ptr[i + (c * input_row_count)] = input_col[i];
    }
  }

  *output_row_count = (*copy_multiplier) * input_row_count;
  return 0;
}
