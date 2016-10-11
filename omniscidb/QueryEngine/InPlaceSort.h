/*
 * @file    InPlaceSort.h
 * @author  Alex Suhan <alex@mapd.com>
 *
 * Copyright (c) 2015 MapD Technologies, Inc.  All rights reserved.
 */

#ifndef INPLACESORT_H
#define INPLACESORT_H

#include <cstdint>

class ThrustAllocator;

void sort_groups_gpu(int64_t* val_buff,
                     int32_t* key_buff,
                     const uint64_t entry_count,
                     const bool desc,
                     const uint32_t chosen_bytes,
                     ThrustAllocator& alloc);

void sort_groups_cpu(int64_t* val_buff,
                     int32_t* key_buff,
                     const uint64_t entry_count,
                     const bool desc,
                     const uint32_t chosen_bytes);

void apply_permutation_gpu(int64_t* val_buff,
                           int32_t* idx_buff,
                           const uint64_t entry_count,
                           const uint32_t chosen_bytes,
                           ThrustAllocator& alloc);

void apply_permutation_cpu(int64_t* val_buff,
                           int32_t* idx_buff,
                           const uint64_t entry_count,
                           int64_t* tmp_buff,
                           const uint32_t chosen_bytes);

#endif  // INPLACESORT_H
