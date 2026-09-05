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

// Original author: Justin Blair
// Modified for MVVCVTK: pinned upstream 3377fed68103959e37a849309a096a9919b0bc97.
// Patch mvvcvtk-1: checked dimensions/allocations/indices, status and single-slice API,
// private helpers and injectable allocator. Normal-domain numerical operations retained.

#include <math.h>
#include <stdlib.h>

#include "remove_ring.h"
#include <limits.h>
#include <stdint.h>

#define INT_MODE_WRAP 0
#define INT_MODE_REFLECT 1

#define PI 3.14159265359


typedef struct RingContext {
    const MvvcvtkTomoPyAllocator* allocator;
    int status;
} RingContext;
static void* ring_calloc(RingContext* context, size_t count, size_t size)
{
    void* result;
    if(count == 0 || size == 0 || count > SIZE_MAX / size) {
        context->status = MVVCVTK_TOMOPY_INVALID;
        return NULL;
    }
    result = context->allocator
        ? context->allocator->allocate(context->allocator->context, count, size)
        : calloc(count, size);
    if(!result) context->status = MVVCVTK_TOMOPY_ALLOCATION_FAILED;
    return result;
}
static void ring_free(RingContext* context, void* pointer)
{
    if(!pointer) return;
    if(context->allocator) context->allocator->release(context->allocator->context, pointer);
    else free(pointer);
}

static int
min_distance_to_edge(float center_x, float center_y, int width, int height)
{
    int dist[4];
    dist[0]   = center_x + 1;
    dist[1]   = center_y + 1;
    dist[2]   = width - center_x;
    dist[3]   = height - center_y;
    int min   = dist[0];
    for(int i = 1; i < 4; i++)
    {
        if(min > dist[i])
        {
            min = dist[i];
        }
    }
    return min;
}

static int
iroundf(float x)
{
    return (x != 0.0) ? floor(x + 0.5) : 0;
}

static float**
polar_transform(RingContext* context, float** image, float center_x, float center_y, int width, int height,
                int* p_pol_width, int* p_pol_height, float thresh_max, float thresh_min,
                int r_scale, int ang_scale, int overhang)
{
    int max_r      = min_distance_to_edge(center_x, center_y, width, height);
    int pol_width  = r_scale * max_r;
    int pol_height = iroundf((float) ang_scale * 2.0 * PI * (float) max_r);
    *p_pol_width   = pol_width;
    *p_pol_height  = pol_height;

    float*  image_block = (float*) ring_calloc(context, (size_t) pol_height * pol_width, sizeof(float));
    float** polar_image = (float**) ring_calloc(context, pol_height, sizeof(float*));
    if(!image_block || !polar_image) {
        ring_free(context, image_block); ring_free(context, polar_image); return NULL;
    }
    polar_image[0]      = image_block;
    for(int i = 1; i < pol_height; i++)
    {
        polar_image[i] = polar_image[i - 1] + pol_width;
    }
    for(int row = 0; row < pol_height; row++)
    {
        for(int r = 0; r <= pol_width - r_scale; r++)
        {
            float theta = (float) row * 2.0 * PI / (float) pol_height;
            float fl_x =
                (float) r * cos(theta + (PI / (float) pol_height)) / (float) r_scale;
            float fl_y =
                (float) r * sin(theta + (PI / (float) pol_height)) / (float) r_scale;
            int x = iroundf(fl_x + center_x);
            int y = iroundf(fl_y + center_y);

            if(x < 0 || x >= width || y < 0 || y >= height) {
                context->status = MVVCVTK_TOMOPY_INVALID;
                ring_free(context, polar_image[0]); ring_free(context, polar_image); return NULL;
            }
            polar_image[row][r] = image[y][x];
            if(polar_image[row][r] > thresh_max)
            {
                polar_image[row][r] = thresh_max;
            }
            else if(polar_image[row][r] < thresh_min)
            {
                polar_image[row][r] = thresh_min;
            }
        }
    }

    return polar_image;
}

static float**
inverse_polar_transform(RingContext* context, float** polar_image, float center_x, float center_y,
                        int pol_width, int pol_height, int width, int height, int r_scale,
                        int over_hang)
{
    float*  image_block = (float*) ring_calloc(context, (size_t) height * width, sizeof(float));
    float** cart_image  = (float**) ring_calloc(context, height, sizeof(float*));
    if(!image_block || !cart_image) {
        ring_free(context, image_block); ring_free(context, cart_image); return NULL;
    }
    cart_image[0]       = image_block;
    for(int i = 1; i < height; i++)
    {
        cart_image[i] = cart_image[i - 1] + width;
    }
    for(int row = 0; row < height; row++)
    {
        for(int col = 0; col < width; col++)
        {
            float theta = atan2((float) (row - center_y),
                                (float) (col - center_x) - (PI / (float) pol_height));
            if(theta < 0)
            {
                theta = (float)(theta + 2.0 * PI);
            }
            int pol_row = iroundf(theta * (float) pol_height / (2.0 * PI));
            int pol_col =
                iroundf((float) r_scale *
                        sqrtf(((float) row - center_y) * ((float) row - center_y) +
                              ((float) col - center_x) * ((float) col - center_x)));
            if(pol_row >= 0 && pol_col >= 0 && pol_row < pol_height && pol_col < pol_width)
            {
                cart_image[row][col] = polar_image[pol_row][pol_col];
            }
            else
            {
                cart_image[row][col] = 0.0;
            }
        }
    }
    return cart_image;
}

static void
swap_float(float* arr, int index1, int index2)
{
    float store_value = arr[index1];
    arr[index1]       = arr[index2];
    arr[index2]       = store_value;
    return;
}

static void
swap_integer(int* arr, int index1, int index2)
{
    int store_value = arr[index1];
    arr[index1]     = arr[index2];
    arr[index2]     = store_value;
    return;
}

static int
partition_2_arrays(float* median_array, int* position_array, int left, int right,
                   int pivot_index)
{
    float pivot_value = median_array[pivot_index];
    swap_float(median_array, pivot_index, right);
    swap_integer(position_array, pivot_index, right);
    int store_index = left;
    for(int i = left; i < right; i++)
    {
        if(median_array[i] <= pivot_value)
        {
            swap_float(median_array, i, store_index);
            swap_integer(position_array, i, store_index);
            store_index += 1;
        }
    }
    swap_float(median_array, store_index, right);
    swap_integer(position_array, store_index, right);
    return store_index;
}

static void
quick_sort_2_arrays(float* median_array, int* position_array, int left, int right)
{
    if(left < right)
    {
        int pivot_index = (int) ((left + right) / 2);
        int new_pivot_index =
            partition_2_arrays(median_array, position_array, left, right, pivot_index);
        quick_sort_2_arrays(median_array, position_array, left, new_pivot_index - 1);
        quick_sort_2_arrays(median_array, position_array, new_pivot_index + 1, right);
    }
    return;
}

static void
bubble_2_arrays(float* median_array, int* position_array, int index, int length)
{
    if(length <= 1) return;
    if(index > 0 && index < length - 1)
    {
        if(median_array[index] < median_array[index - 1])
        {
            swap_float(median_array, index, index - 1);
            swap_integer(position_array, index, index - 1);
            bubble_2_arrays(median_array, position_array, index - 1, length);
        }
        else if(median_array[index] > median_array[index + 1])
        {
            swap_float(median_array, index, index + 1);
            swap_integer(position_array, index, index + 1);
            bubble_2_arrays(median_array, position_array, index + 1, length);
        }
    }
    else if(index == 0)
    {
        if(median_array[index] > median_array[index + 1])
        {
            swap_float(median_array, index, index + 1);
            swap_integer(position_array, index, index + 1);
            bubble_2_arrays(median_array, position_array, index + 1, length);
        }
    }
    else if(index == length - 1)
    {
        if(median_array[index] < median_array[index - 1])
        {
            swap_float(median_array, index, index - 1);
            swap_integer(position_array, index, index - 1);
            bubble_2_arrays(median_array, position_array, index - 1, length);
        }
    }
    return;
}

static int
median_filter_fast_1D(RingContext* context, float*** filtered_image, float*** image, int start_row,
                      int start_col, int end_row, int end_col, char axis, int kernel_rad,
                      int filter_width, int width, int height)
{
    int    row, col;
    float* median_array   = (float*) ring_calloc(context, 2 * kernel_rad + 1, sizeof(float));
    int*   position_array = (int*) ring_calloc(context, 2 * kernel_rad + 1, sizeof(int));
    if(!median_array || !position_array) {
        ring_free(context, median_array); ring_free(context, position_array); return 0;
    }
    if(axis == 'x')
    {
        for(row = start_row; row <= end_row; row++)
        {
            col = start_col;
            for(int n = -kernel_rad; n < kernel_rad + 1; n++)
            {
                int adjusted_col = col + n;
                int adjusted_row = row;
                if(adjusted_col < 0)
                {
                    adjusted_col = -adjusted_col;
                    if(row < height / 2)
                    {
                        adjusted_row += height / 2;
                    }
                    else
                    {
                        adjusted_row -= height / 2;
                    }
                    median_array[n + kernel_rad] = image[0][adjusted_row][adjusted_col];
                }
                else
                {
                    median_array[n + kernel_rad] = image[0][row][adjusted_col];
                }
                position_array[n + kernel_rad] = n + kernel_rad;
            }
            // Sort the array
            quick_sort_2_arrays(median_array, position_array, 0, 2 * kernel_rad);
            filtered_image[0][row][col] = median_array[kernel_rad];

            // Roll filter along the rest of the row
            for(col = start_col + 1; col <= end_col; col++)
            {
                float next_value     = 0.0;
                int   next_value_col = col + kernel_rad;
                if(next_value_col < width)
                {
                    next_value = image[0][row][next_value_col];
                }
                int last_value_index = 0;
                for(int i = 0; i < 2 * kernel_rad + 1; i++)
                {
                    position_array[i] -= 1;
                    if(position_array[i] < 0)
                    {
                        last_value_index  = i;
                        position_array[i] = 2 * kernel_rad;
                        median_array[i]   = next_value;
                    }
                }
                bubble_2_arrays(median_array, position_array, last_value_index,
                                2 * kernel_rad + 1);
                filtered_image[0][row][col] = median_array[kernel_rad];
            }
        }
    }
    else if(axis == 'y')
    {
        for(col = start_col; col <= end_col; col++)
        {
            row = start_row;
            for(int n = -kernel_rad; n < kernel_rad + 1; n++)
            {
                int adjusted_row = row + n;
                int adjusted_col = col;
                if(adjusted_row < 0)
                {
                    // Handle edge cases
                    adjusted_row += height;
                    median_array[n + kernel_rad] = image[0][adjusted_row][adjusted_col];
                }
                else
                {
                    median_array[n + kernel_rad] = image[0][adjusted_row][adjusted_col];
                }
                position_array[n + kernel_rad] = n + kernel_rad;
            }
            // Sort the array
            quick_sort_2_arrays(median_array, position_array, 0, 2 * kernel_rad);
            filtered_image[0][row][col] = median_array[kernel_rad];

            // Roll filter along the rest of the col
            for(row = start_row + 1; row <= end_row; row++)
            {
                float next_value     = 0.0;
                int   next_value_row = row + kernel_rad;
                if(next_value_row < height)
                {
                    next_value = image[0][next_value_row][col];
                }
                int last_value_index = 0;
                for(int i = 0; i < 2 * kernel_rad + 1; i++)
                {
                    position_array[i] -= 1;
                    if(position_array[i] < 0)
                    {
                        last_value_index  = i;
                        position_array[i] = 2 * kernel_rad;
                        median_array[i]   = next_value;
                    }
                }
                bubble_2_arrays(median_array, position_array, last_value_index,
                                2 * kernel_rad + 1);
                filtered_image[0][row][col] = median_array[kernel_rad];
            }
        }
    }
    ring_free(context, median_array);
    ring_free(context, position_array);
    return 1;
}

/* Runs slightly faster than the above mean filter, but floating-point rounding
 * causes errors on the order of 1E-10. Should be small enough error to not care
 * about, but be careful...
 */
static void
mean_filter_fast_1D(float*** filtered_image, float*** image, int start_row, int start_col,
                    int end_row, int end_col, int int_mode, int kernel_rad, int width,
                    int height)
{
    long double mean = 0, sum = 0, previous_sum = 0,
                num_elems = (double) (2 * kernel_rad + 1);
    int row, col;
    if(int_mode == INT_MODE_WRAP)
    {
        // iterate over each row of the image subset
        for(col = start_col; col <= end_col; col++)
        {
            sum = 0;
            // calculate average of first element of the column
            for(int n = -kernel_rad; n < (kernel_rad + 1); n++)
            {
                row = n + start_row;
                if(row < 0)
                {
                    row += height;
                }
                else if(row >= height)
                {
                    row -= height;
                }
                sum += image[0][row][col];
            }
            mean                              = sum / num_elems;
            filtered_image[0][start_row][col] = mean;
            previous_sum                      = sum;

            for(row = start_row + 1; row <= end_row; row++)
            {
                int last_row = (row - 1) - (kernel_rad);
                int next_row = row + (kernel_rad);
                if(last_row < 0)
                {
                    last_row += height;
                }
                if(next_row >= height)
                {
                    next_row -= height;
                }
                sum = previous_sum - image[0][last_row][col] + image[0][next_row][col];
                if(image[0][row][col] != 0)
                {
                    filtered_image[0][row][col] = sum / num_elems;
                }
                else
                {
                    filtered_image[0][row][col] = 0.0;
                }
                previous_sum = sum;
            }
        }
    }
    else if(int_mode == INT_MODE_REFLECT)
    {
        // iterate over each column of the image subset
        for(col = start_col; col <= end_col; col++)
        {
            sum = 0;
            // calculate average of first element of the column
            for(int n = -kernel_rad; n < (kernel_rad + 1); n++)
            {
                row = n;
                if(row < 0)
                {
                    row = -row;
                }
                else if(row >= height / 2)
                {
                    row = height / 2 - (row - height / 2) - 2;
                }
                sum += image[0][row][col];
            }
            mean                      = sum / num_elems;
            filtered_image[0][0][col] = mean;
            previous_sum              = sum;

            for(row = 1; row < height / 2; row++)
            {
                int last_row = (row - 1) - (kernel_rad);
                int next_row = row + (kernel_rad);
                if(last_row < 0)
                {
                    last_row = -last_row;
                }
                if(next_row >= height / 2)
                {
                    next_row = height / 2 - (next_row - height / 2) - 2;
                }
                sum = previous_sum - image[0][last_row][col] + image[0][next_row][col];
                if(image[0][row][col] != 0)
                {
                    filtered_image[0][row][col] = sum / num_elems;
                }
                else
                {
                    filtered_image[0][row][col] = 0.0;
                }
                previous_sum = sum;
            }

            sum = 0;
            // calculate average of first element of the column
            for(int n = -kernel_rad; n < (kernel_rad + 1); n++)
            {
                row = n + height / 2;
                if(row < height / 2)
                {
                    row = height / 2 + (height / 2 - row);
                }
                else if(row >= height)
                {
                    row = height - (row - height) - 2;
                }
                sum += image[0][row][col];
            }
            mean                               = sum / num_elems;
            filtered_image[0][height / 2][col] = mean;
            previous_sum                       = sum;

            for(row = height / 2 + 1; row < height; row++)
            {
                int last_row = (row - 1) - (kernel_rad);
                int next_row = row + (kernel_rad);
                if(last_row < height / 2)
                {
                    last_row = height / 2 + (height / 2 - last_row);
                }
                if(next_row >= height)
                {
                    next_row = height - (next_row - height) - 2;
                }
                sum = previous_sum - image[0][last_row][col] + image[0][next_row][col];
                if(image[0][row][col] != 0)
                {
                    filtered_image[0][row][col] = sum / num_elems;
                }
                else
                {
                    filtered_image[0][row][col] = 0.0;
                }
                previous_sum = sum;
            }
        }
    }
    return;
}

static int
ring_filter(RingContext* context, float*** polar_image, int pol_height, int pol_width, float threshold,
            int m_rad, int m_azi, int ring_width, int int_mode)
{
    float*  image_block    = (float*) ring_calloc(context, (size_t) pol_height * pol_width, sizeof(float));
    float** filtered_image = (float**) ring_calloc(context, pol_height, sizeof(float*));
    if(!image_block || !filtered_image) {
        ring_free(context, image_block); ring_free(context, filtered_image); return 0;
    }
    filtered_image[0]      = image_block;
    for(int i = 1; i < pol_height; i++)
    {
        filtered_image[i] = filtered_image[i - 1] + pol_width;
    }

    if(!median_filter_fast_1D(context, &filtered_image, polar_image, 0, 0, pol_height - 1,
                          pol_width / 3 - 1, 'x', m_rad / 3, ring_width, pol_width,
                          pol_height)) goto failed;
    if(!median_filter_fast_1D(context, &filtered_image, polar_image, 0, pol_width / 3, pol_height - 1,
                          2 * pol_width / 3 - 1, 'x', 2 * m_rad / 3, ring_width,
                          pol_width, pol_height)) goto failed;
    if(!median_filter_fast_1D(context, &filtered_image, polar_image, 0, 2 * pol_width / 3,
                          pol_height - 1, pol_width - 1, 'x', m_rad, ring_width,
                          pol_width, pol_height)) goto failed;

    // subtract filtered image from polar image to get difference image & do
    // last thresholding

    for(int row = 0; row < pol_height; row++)
    {
        for(int col = 0; col < pol_width; col++)
        {
            polar_image[0][row][col] -= filtered_image[row][col];
            if(!isfinite(polar_image[0][row][col])) {
                context->status = MVVCVTK_TOMOPY_NUMERICAL_FAILED;
                goto failed;
            }
            if(polar_image[0][row][col] > threshold ||
               polar_image[0][row][col] < -threshold)
            {
                polar_image[0][row][col] = 0;
            }
        }
    }

    /* Do Azimuthal filter #2 (faster mean, does whole column in one call)
     * using different kernel sizes for the different regions of the image
     * (based on radius)
     */

    mean_filter_fast_1D(&filtered_image, polar_image, 0, 0, pol_height - 1,
                        pol_width / 3 - 1, int_mode, m_azi / 3, pol_width, pol_height);
    mean_filter_fast_1D(&filtered_image, polar_image, 0, pol_width / 3, pol_height - 1,
                        2 * pol_width / 3 - 1, int_mode, 2 * m_azi / 3, pol_width,
                        pol_height);
    mean_filter_fast_1D(&filtered_image, polar_image, 0, 2 * pol_width / 3,
                        pol_height - 1, pol_width - 1, int_mode, m_azi, pol_width,
                        pol_height);

    // Set "polar_image" to the fully filtered data
    for(int row = 0; row < pol_height; row++)
    {
        for(int col = 0; col < pol_width; col++)
        {
            polar_image[0][row][col] = filtered_image[row][col];
            if(!isfinite(polar_image[0][row][col])) {
                context->status = MVVCVTK_TOMOPY_NUMERICAL_FAILED;
                goto failed;
            }
        }
    }

    ring_free(context, filtered_image[0]);
    ring_free(context, filtered_image);
    return 1;
failed:
    ring_free(context, filtered_image[0]);
    ring_free(context, filtered_image);
    return 0;
}

int mvvcvtk_tomopy_get_layout(int width, int height, float center_x, float center_y,
    int angular_min, int ring_width, int int_mode, MvvcvtkTomoPyLayout* layout)
{
    int max_r, pol_height, m_rad, m_azi;
    size_t plane, polar, pointer_rows;
    if(!layout || width < 8 || height < 8 || width > 16384 || height > 16384
        || !isfinite(center_x) || !isfinite(center_y)
        || center_x < 0 || center_x > width - 1 || center_y < 0 || center_y > height - 1
        || ring_width < 1 || ring_width > 64 || angular_min < 1 || angular_min > 180
        || (int_mode != INT_MODE_WRAP && int_mode != INT_MODE_REFLECT)) return MVVCVTK_TOMOPY_INVALID;
    max_r = min_distance_to_edge(center_x, center_y, width, height);
    m_rad = 2 * ring_width + 1;
    if(max_r < 3 * (m_rad + 1)) return MVVCVTK_TOMOPY_INVALID;
    pol_height = iroundf(2.0 * PI * (float) max_r);
    m_azi = (int) floor((float) pol_height / 360.0 * angular_min);
    if(pol_height < 8 || m_azi >= pol_height
        || (int_mode == INT_MODE_REFLECT && m_azi >= pol_height / 2 - 1)) return MVVCVTK_TOMOPY_INVALID;
    if(m_rad / 3 >= max_r || max_r / 3 + 2 * m_rad / 3 >= max_r
        || 2 * max_r / 3 + m_rad >= max_r) return MVVCVTK_TOMOPY_INVALID;
    plane = (size_t) width * height;
    polar = (size_t) max_r * pol_height;
    if(plane > INT_MAX || polar > INT_MAX) return MVVCVTK_TOMOPY_INVALID;
    pointer_rows = (size_t) 2 * height + (size_t) 2 * pol_height;
    if(plane > SIZE_MAX / 8 || polar > (SIZE_MAX - plane * 8) / 8) return MVVCVTK_TOMOPY_INVALID;
    layout->workspace_bytes = plane * 8 + polar * 8;
    if(pointer_rows > (SIZE_MAX - layout->workspace_bytes) / sizeof(float*)) return MVVCVTK_TOMOPY_INVALID;
    layout->workspace_bytes += pointer_rows * sizeof(float*);
    if((size_t)(2 * m_rad + 1) > (SIZE_MAX - layout->workspace_bytes) / 8) return MVVCVTK_TOMOPY_INVALID;
    layout->workspace_bytes += (size_t)(2 * m_rad + 1) * 8;
    layout->polar_width = max_r;
    layout->polar_height = pol_height;
    return MVVCVTK_TOMOPY_OK;
}

int mvvcvtk_tomopy_remove_ring(float* data, int width, int height,
    float center_x, float center_y, float thresh_max, float thresh_min, float threshold,
    int angular_min, int ring_width, int int_mode, const MvvcvtkTomoPyAllocator* allocator)
{
    MvvcvtkTomoPyLayout layout;
    RingContext context = { allocator, MVVCVTK_TOMOPY_OK };
    float** image = NULL;
    float** polar_image = NULL;
    float** ring_image = NULL;
    int pol_width = 0, pol_height = 0, m_azi;
    if(!data || (allocator && (!allocator->allocate || !allocator->release))
        || !isfinite(thresh_min) || !isfinite(thresh_max) || !isfinite(threshold)
        || thresh_min >= thresh_max || threshold <= 0
        || mvvcvtk_tomopy_get_layout(width, height, center_x, center_y, angular_min,
            ring_width, int_mode, &layout) != MVVCVTK_TOMOPY_OK) return MVVCVTK_TOMOPY_INVALID;
    for(size_t i = 0; i < (size_t)width * height; ++i) {
        if(!isfinite(data[i])) return MVVCVTK_TOMOPY_INVALID;
    }
    image = (float**)ring_calloc(&context, (size_t)height, sizeof(float*));
    if(!image) goto cleanup;
    for(int row = 0; row < height; ++row) image[row] = data + (size_t)row * width;
    polar_image = polar_transform(&context, image, center_x, center_y, width, height,
        &pol_width, &pol_height, thresh_max, thresh_min, 1, 1, ring_width);
    if(!polar_image) goto cleanup;
    m_azi = (int)floor((float)pol_height / 360.0 * angular_min);
    if(!ring_filter(&context, &polar_image, pol_height, pol_width, threshold,
        2 * ring_width + 1, m_azi, ring_width, int_mode)) goto cleanup;
    ring_image = inverse_polar_transform(&context, polar_image, center_x, center_y,
        pol_width, pol_height, width, height, 1, ring_width);
    if(!ring_image) goto cleanup;
    // Validate the whole result before the first in-place write.
    for(int row = 0; row < height; ++row) {
        for(int col = 0; col < width; ++col) {
            if(!isfinite(image[row][col] - ring_image[row][col])) {
                context.status = MVVCVTK_TOMOPY_NUMERICAL_FAILED;
                goto cleanup;
            }
        }
    }
    for(int row = 0; row < height; ++row)
        for(int col = 0; col < width; ++col) image[row][col] -= ring_image[row][col];
cleanup:
    if(ring_image) { ring_free(&context, ring_image[0]); ring_free(&context, ring_image); }
    if(polar_image) { ring_free(&context, polar_image[0]); ring_free(&context, polar_image); }
    ring_free(&context, image);
    return context.status;
}
