/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// Compute shader to clamp alpha channel to [0, 1] in R16G16B16A16_FLOAT render
// targets. This is needed for k_2_10_10_10_FLOAT format in the RTV path, as the
// actual format has 2-bit fixed-point alpha [0, 1], but the host uses
// R16G16B16A16_FLOAT which doesn't clamp. Without this, destination alpha
// values exceeding [0, 1] from previous draws cause incorrect blending.

// Width and height of the render target region to process.
cbuffer XeAlphaClampConstants : register(b0) {
  uint xe_alpha_clamp_width;
  uint xe_alpha_clamp_height;
};

RWTexture2D<float4> xe_alpha_clamp_target : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 xe_thread_id : SV_DispatchThreadID) {
  // Early out if beyond dimensions.
  if (xe_thread_id.x >= xe_alpha_clamp_width ||
      xe_thread_id.y >= xe_alpha_clamp_height) {
    return;
  }

  float4 color = xe_alpha_clamp_target[xe_thread_id.xy];
  color.a = saturate(color.a);
  xe_alpha_clamp_target[xe_thread_id.xy] = color;
}
