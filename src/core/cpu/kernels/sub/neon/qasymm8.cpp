/*
 * Copyright (c) 2021 Arm Limited.
 *
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include "arm_compute/core/Helpers.h"
#include "arm_compute/core/ITensor.h"
#include "arm_compute/core/Types.h"
#include "arm_compute/core/utils/misc/Traits.h"
#include "src/core/NEON/wrapper/intrinsics/intrinsics.h"
#include "src/core/helpers/WindowHelpers.h"

namespace arm_compute
{
namespace cpu
{
void sub_qasymm8_neon(const ITensor *src0, const ITensor *src1, ITensor *dst, const ConvertPolicy &policy, const Window &window)
{
    ARM_COMPUTE_UNUSED(policy);

    // Create input windows
    Window input1_win = window.broadcast_if_dimension_le_one(src0->info()->tensor_shape());
    Window input2_win = window.broadcast_if_dimension_le_one(src1->info()->tensor_shape());

    // Clear X Dimension on execution window as we handle manually
    Window win = window;
    win.set(Window::DimX, Window::Dimension(0, 1, 1));

    const int  window_step_x         = 16;
    const auto window_start_x        = static_cast<int>(window.x().start());
    const auto window_end_x          = static_cast<int>(window.x().end());
    const bool is_broadcast_across_x = src0->info()->tensor_shape().x() != src1->info()->tensor_shape().x();

    const UniformQuantizationInfo iq1_info = src0->info()->quantization_info().uniform();
    const UniformQuantizationInfo iq2_info = src1->info()->quantization_info().uniform();
    const UniformQuantizationInfo oq_info  = dst->info()->quantization_info().uniform();

    const float32x4_t invvscaleo = vdupq_n_f32(1.f / oq_info.scale);
    const float32x4_t voffseto   = vdupq_n_f32(oq_info.offset);

    if(is_broadcast_across_x)
    {
        const bool                    is_broadcast_input_2 = input2_win.x().step() == 0;
        Window                        broadcast_win        = is_broadcast_input_2 ? input2_win : input1_win;
        Window                        non_broadcast_win    = !is_broadcast_input_2 ? input2_win : input1_win;
        const ITensor                *broadcast_tensor     = is_broadcast_input_2 ? src1 : src0;
        const ITensor                *non_broadcast_tensor = !is_broadcast_input_2 ? src1 : src0;
        const UniformQuantizationInfo broadcast_qinfo      = broadcast_tensor->info()->quantization_info().uniform();
        const UniformQuantizationInfo non_broadcast_qinfo  = non_broadcast_tensor->info()->quantization_info().uniform();
        const float32x4_t             vscale1              = is_broadcast_input_2 ? vdupq_n_f32(iq1_info.scale) : vdupq_n_f32(iq2_info.scale);
        const float32x4_t             vscale2              = is_broadcast_input_2 ? vdupq_n_f32(iq2_info.scale) : vdupq_n_f32(iq1_info.scale);
        const int32x4_t               voffset1             = is_broadcast_input_2 ? vdupq_n_s32(iq1_info.offset) : vdupq_n_s32(iq2_info.offset);
        const int32x4_t               voffset2             = is_broadcast_input_2 ? vdupq_n_s32(iq2_info.offset) : vdupq_n_s32(iq1_info.offset);

        // Clear X Dimension on execution window as we handle manually
        non_broadcast_win.set(Window::DimX, Window::Dimension(0, 1, 1));

        Iterator broadcast_input(broadcast_tensor, broadcast_win);
        Iterator non_broadcast_input(non_broadcast_tensor, non_broadcast_win);
        Iterator output(dst, win);

        execute_window_loop(win, [&](const Coordinates &)
        {
            const auto non_broadcast_input_ptr = reinterpret_cast<const uint8_t *>(non_broadcast_input.ptr());
            const auto output_ptr              = reinterpret_cast<uint8_t *>(output.ptr());

            const auto broadcast_value     = *reinterpret_cast<const uint8_t *>(broadcast_input.ptr());
            const auto broadcast_value_vec = wrapper::vdup_n(static_cast<uint8_t>(broadcast_value), wrapper::traits::vector_128_tag{});

            const float32x4x4_t bf =
            {
                {
                    vmulq_f32(vcvtq_f32_s32(vsubq_s32(wrapper::vreinterpret(wrapper::vmovl(wrapper::vgetlow(wrapper::vmovl(wrapper::vgetlow(broadcast_value_vec))))), voffset2)), vscale2),
                    vmulq_f32(vcvtq_f32_s32(vsubq_s32(wrapper::vreinterpret(wrapper::vmovl(wrapper::vgethigh(wrapper::vmovl(wrapper::vgetlow(broadcast_value_vec))))), voffset2)), vscale2),
                    vmulq_f32(vcvtq_f32_s32(vsubq_s32(wrapper::vreinterpret(wrapper::vmovl(wrapper::vgetlow(wrapper::vmovl(wrapper::vgethigh(broadcast_value_vec))))), voffset2)), vscale2),
                    vmulq_f32(vcvtq_f32_s32(vsubq_s32(wrapper::vreinterpret(wrapper::vmovl(wrapper::vgethigh(wrapper::vmovl(wrapper::vgethigh(broadcast_value_vec))))), voffset2)), vscale2),
                }
            };

            // Compute S elements per iteration
            int x = window_start_x;
            for(; x <= (window_end_x - window_step_x); x += window_step_x)
            {
                const auto a = wrapper::vloadq(non_broadcast_input_ptr + x);

                const float32x4x4_t af =
                {
                    {
                        vmulq_f32(vcvtq_f32_s32(vsubq_s32(wrapper::vreinterpret(wrapper::vmovl(wrapper::vgetlow(wrapper::vmovl(wrapper::vgetlow(a))))), voffset1)), vscale1),
                        vmulq_f32(vcvtq_f32_s32(vsubq_s32(wrapper::vreinterpret(wrapper::vmovl(wrapper::vgethigh(wrapper::vmovl(wrapper::vgetlow(a))))), voffset1)), vscale1),
                        vmulq_f32(vcvtq_f32_s32(vsubq_s32(wrapper::vreinterpret(wrapper::vmovl(wrapper::vgetlow(wrapper::vmovl(wrapper::vgethigh(a))))), voffset1)), vscale1),
                        vmulq_f32(vcvtq_f32_s32(vsubq_s32(wrapper::vreinterpret(wrapper::vmovl(wrapper::vgethigh(wrapper::vmovl(wrapper::vgethigh(a))))), voffset1)), vscale1),
                    }
                };

                const int32x4x4_t rf =
                {
                    {
#ifdef __aarch64_
                        vcvtnq_s32_f32(vmlaq_f32(voffseto, !is_broadcast_input_2 ? vsubq_f32(bf.val[0], af.val[0]) : vsubq_f32(af.val[0], bf.val[0]), invvscaleo)),
                        vcvtnq_s32_f32(vmlaq_f32(voffseto, !is_broadcast_input_2 ? vsubq_f32(bf.val[1], af.val[1]) : vsubq_f32(af.val[1], bf.val[1]), invvscaleo)),
                        vcvtnq_s32_f32(vmlaq_f32(voffseto, !is_broadcast_input_2 ? vsubq_f32(bf.val[2], af.val[2]) : vsubq_f32(af.val[2], bf.val[2]), invvscaleo)),
                        vcvtnq_s32_f32(vmlaq_f32(voffseto, !is_broadcast_input_2 ? vsubq_f32(bf.val[3], af.val[3]) : vsubq_f32(af.val[3], bf.val[3]), invvscaleo)),
#else  //__aarch64__
                        vcvtq_s32_f32(vmlaq_f32(voffseto, !is_broadcast_input_2 ? vsubq_f32(bf.val[0], af.val[0]) : vsubq_f32(af.val[0], bf.val[0]), invvscaleo)),
                        vcvtq_s32_f32(vmlaq_f32(voffseto, !is_broadcast_input_2 ? vsubq_f32(bf.val[1], af.val[1]) : vsubq_f32(af.val[1], bf.val[1]), invvscaleo)),
                        vcvtq_s32_f32(vmlaq_f32(voffseto, !is_broadcast_input_2 ? vsubq_f32(bf.val[2], af.val[2]) : vsubq_f32(af.val[2], bf.val[2]), invvscaleo)),
                        vcvtq_s32_f32(vmlaq_f32(voffseto, !is_broadcast_input_2 ? vsubq_f32(bf.val[3], af.val[3]) : vsubq_f32(af.val[3], bf.val[3]), invvscaleo)),
#endif //__aarch64__
                    }
                };

                const auto pa = vqmovun_s16(vcombine_s16(vqmovn_s32(rf.val[0]), vqmovn_s32(rf.val[1])));
                const auto pb = vqmovun_s16(vcombine_s16(vqmovn_s32(rf.val[2]), vqmovn_s32(rf.val[3])));
                wrapper::vstore(output_ptr + x, wrapper::vcombine(pa, pb));
            }

            // Compute left-over elements
            for(; x < window_end_x; ++x)
            {
                const float afs   = static_cast<int32_t>(*(non_broadcast_input_ptr + x) - non_broadcast_qinfo.offset) * non_broadcast_qinfo.scale;
                const float bfs   = static_cast<int32_t>(broadcast_value - broadcast_qinfo.offset) * broadcast_qinfo.scale;
                *(output_ptr + x) = quantize_qasymm8(is_broadcast_input_2 ? afs - bfs : bfs - afs, dst->info()->quantization_info());
            }
        },
        broadcast_input, non_broadcast_input, output);
    }
    else
    {
        const float32x4_t vscale1  = vdupq_n_f32(iq1_info.scale);
        const float32x4_t vscale2  = vdupq_n_f32(iq2_info.scale);
        const int32x4_t   voffset1 = vdupq_n_s32(iq1_info.offset);
        const int32x4_t   voffset2 = vdupq_n_s32(iq2_info.offset);

        // Clear X Dimension on execution window as we handle manually
        input1_win.set(Window::DimX, Window::Dimension(0, 1, 1));
        input2_win.set(Window::DimX, Window::Dimension(0, 1, 1));

        Iterator input1(src0, input1_win);
        Iterator input2(src1, input2_win);
        Iterator output(dst, win);

        execute_window_loop(win, [&](const Coordinates &)
        {
            const auto input1_ptr = reinterpret_cast<const uint8_t *>(input1.ptr());
            const auto input2_ptr = reinterpret_cast<const uint8_t *>(input2.ptr());
            const auto output_ptr = reinterpret_cast<uint8_t *>(output.ptr());

            // Compute S elements per iteration
            int x = window_start_x;
            for(; x <= (window_end_x - window_step_x); x += window_step_x)
            {
                const auto a = wrapper::vloadq(input1_ptr + x);
                const auto b = wrapper::vloadq(input2_ptr + x);

                const float32x4x4_t af =
                {
                    {
                        vmulq_f32(vcvtq_f32_s32(vsubq_s32(wrapper::vreinterpret(wrapper::vmovl(wrapper::vgetlow(wrapper::vmovl(wrapper::vgetlow(a))))), voffset1)), vscale1),
                        vmulq_f32(vcvtq_f32_s32(vsubq_s32(wrapper::vreinterpret(wrapper::vmovl(wrapper::vgethigh(wrapper::vmovl(wrapper::vgetlow(a))))), voffset1)), vscale1),
                        vmulq_f32(vcvtq_f32_s32(vsubq_s32(wrapper::vreinterpret(wrapper::vmovl(wrapper::vgetlow(wrapper::vmovl(wrapper::vgethigh(a))))), voffset1)), vscale1),
                        vmulq_f32(vcvtq_f32_s32(vsubq_s32(wrapper::vreinterpret(wrapper::vmovl(wrapper::vgethigh(wrapper::vmovl(wrapper::vgethigh(a))))), voffset1)), vscale1),
                    }
                };

                const float32x4x4_t bf =
                {
                    {
                        vmulq_f32(vcvtq_f32_s32(vsubq_s32(wrapper::vreinterpret(wrapper::vmovl(wrapper::vgetlow(wrapper::vmovl(wrapper::vgetlow(b))))), voffset2)), vscale2),
                        vmulq_f32(vcvtq_f32_s32(vsubq_s32(wrapper::vreinterpret(wrapper::vmovl(wrapper::vgethigh(wrapper::vmovl(wrapper::vgetlow(b))))), voffset2)), vscale2),
                        vmulq_f32(vcvtq_f32_s32(vsubq_s32(wrapper::vreinterpret(wrapper::vmovl(wrapper::vgetlow(wrapper::vmovl(wrapper::vgethigh(b))))), voffset2)), vscale2),
                        vmulq_f32(vcvtq_f32_s32(vsubq_s32(wrapper::vreinterpret(wrapper::vmovl(wrapper::vgethigh(wrapper::vmovl(wrapper::vgethigh(b))))), voffset2)), vscale2),
                    }
                };

                const int32x4x4_t rf =
                {
                    {
#ifdef __aarch64__
                        vcvtnq_s32_f32(vmlaq_f32(voffseto, vsubq_f32(af.val[0], bf.val[0]), invvscaleo)),
                        vcvtnq_s32_f32(vmlaq_f32(voffseto, vsubq_f32(af.val[1], bf.val[1]), invvscaleo)),
                        vcvtnq_s32_f32(vmlaq_f32(voffseto, vsubq_f32(af.val[2], bf.val[2]), invvscaleo)),
                        vcvtnq_s32_f32(vmlaq_f32(voffseto, vsubq_f32(af.val[3], bf.val[3]), invvscaleo)),
#else  //__aarch64__
                        vcvtq_s32_f32(vmlaq_f32(voffseto, vsubq_f32(af.val[0], bf.val[0]), invvscaleo)),
                        vcvtq_s32_f32(vmlaq_f32(voffseto, vsubq_f32(af.val[1], bf.val[1]), invvscaleo)),
                        vcvtq_s32_f32(vmlaq_f32(voffseto, vsubq_f32(af.val[2], bf.val[2]), invvscaleo)),
                        vcvtq_s32_f32(vmlaq_f32(voffseto, vsubq_f32(af.val[3], bf.val[3]), invvscaleo)),
#endif //__aarch64__
                    }
                };

                const auto pa = vqmovun_s16(vcombine_s16(vqmovn_s32(rf.val[0]), vqmovn_s32(rf.val[1])));
                const auto pb = vqmovun_s16(vcombine_s16(vqmovn_s32(rf.val[2]), vqmovn_s32(rf.val[3])));
                wrapper::vstore(output_ptr + x, wrapper::vcombine(pa, pb));
            }

            // Compute left-over elements
            for(; x < window_end_x; ++x)
            {
                const float afs = static_cast<int32_t>((*(input1_ptr + x)) - iq1_info.offset) * iq1_info.scale;
                const float bfs = static_cast<int32_t>((*(input2_ptr + x)) - iq2_info.offset) * iq2_info.scale;

                *(output_ptr + x) = quantize_qasymm8((afs - bfs), dst->info()->quantization_info());
            }
        },
        input1, input2, output);
    }
}

} // namespace cpu
} // namespace arm_compute