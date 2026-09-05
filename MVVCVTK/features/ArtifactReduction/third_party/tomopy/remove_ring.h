// Copyright (c) 2015, UChicago Argonne, LLC. All rights reserved.

// Copyright 2015. UChicago Argonne, LLC. This software was produced
// under U.S. Government contract DE-AC02-06CH11357 for Argonne National
// Laboratory (ANL), which is operated by UChicago Argonne, LLC for the
// U.S. Department of Energy. The U.S. Government has rights to use,
// reproduce, and distribute this software.  NEITHER THE GOVERNMENT NOR
// UChicago Argonne, LLC MAKES ANY WARRANTY, EXPRESS OR IMPLIED, OR
// ASSUMES ANY LIABILITY FOR THE USE OF THIS SOFTWARE.  If software is
// modified to produce derivative works, such modified software should
// be clearly marked, so as not to confuse it with the version available
// from ANL.

// Additionally, redistribution and use in source and binary forms, with
// or without modification, are permitted provided that the following
// conditions are met:

//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.

//     * Redistributions in binary form must reproduce the above copyright
//       notice, this list of conditions and the following disclaimer in
//       the documentation and/or other materials provided with the
//       distribution.

//     * Neither the name of UChicago Argonne, LLC, Argonne National
//       Laboratory, ANL, the U.S. Government, nor the names of its
//       contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.

// THIS SOFTWARE IS PROVIDED BY UChicago Argonne, LLC AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
// FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL UChicago
// Argonne, LLC OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
// INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
// BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
// LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
// ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

// Modified for MVVCVTK; upstream 3377fed68103959e37a849309a096a9919b0bc97, patch mvvcvtk-1.
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { MVVCVTK_TOMOPY_OK = 0, MVVCVTK_TOMOPY_INVALID = 1,
       MVVCVTK_TOMOPY_ALLOCATION_FAILED = 2, MVVCVTK_TOMOPY_NUMERICAL_FAILED = 3 };

typedef struct MvvcvtkTomoPyAllocator {
    void* context;
    void* (*allocate)(void* context, size_t count, size_t size);
    void (*release)(void* context, void* allocation);
} MvvcvtkTomoPyAllocator;

typedef struct MvvcvtkTomoPyLayout {
    int polar_width;
    int polar_height;
    size_t workspace_bytes;
} MvvcvtkTomoPyLayout;

int mvvcvtk_tomopy_get_layout(int width, int height, float center_x, float center_y,
    int angular_min, int ring_width, int int_mode, MvvcvtkTomoPyLayout* layout);
// allocator.allocate must provide zero-initialized storage; NULL uses calloc/free.
// A failed call leaves data unchanged. All helper symbols remain file-private.
int mvvcvtk_tomopy_remove_ring(float* data, int width, int height,
    float center_x, float center_y, float thresh_max, float thresh_min, float threshold,
    int angular_min, int ring_width, int int_mode, const MvvcvtkTomoPyAllocator* allocator);
#ifdef __cplusplus
}
#endif
