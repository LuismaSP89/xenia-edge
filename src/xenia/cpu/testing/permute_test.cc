/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2014 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/testing/util.h"

using namespace xe;
using namespace xe::cpu;
using namespace xe::cpu::hir;
using namespace xe::cpu::testing;
using xe::cpu::ppc::PPCContext;

TEST_CASE("PERMUTE_V128_BY_INT32_CONSTANT", "[instr]") {
  {
    uint32_t mask = MakePermuteMask(0, 0, 0, 1, 0, 2, 0, 3);
    TestFunction([mask](HIRBuilder& b) {
      StoreVR(b, 3,
              b.Permute(b.LoadConstantUint32(mask), LoadVR(b, 4), LoadVR(b, 5),
                        INT32_TYPE));
      b.Return();
    })
        .Run(
            [](PPCContext* ctx) {
              ctx->v[4] = vec128i(0, 1, 2, 3);
              ctx->v[5] = vec128i(4, 5, 6, 7);
            },
            [](PPCContext* ctx) {
              auto result = ctx->v[3];
              REQUIRE(result == vec128i(0, 1, 2, 3));
            });
  }
  {
    uint32_t mask = MakePermuteMask(1, 0, 1, 1, 1, 2, 1, 3);
    TestFunction([mask](HIRBuilder& b) {
      StoreVR(b, 3,
              b.Permute(b.LoadConstantUint32(mask), LoadVR(b, 4), LoadVR(b, 5),
                        INT32_TYPE));
      b.Return();
    })
        .Run(
            [](PPCContext* ctx) {
              ctx->v[4] = vec128i(0, 1, 2, 3);
              ctx->v[5] = vec128i(4, 5, 6, 7);
            },
            [](PPCContext* ctx) {
              auto result = ctx->v[3];
              REQUIRE(result == vec128i(4, 5, 6, 7));
            });
  }
  {
    uint32_t mask = MakePermuteMask(0, 3, 0, 2, 0, 1, 0, 0);
    TestFunction([mask](HIRBuilder& b) {
      StoreVR(b, 3,
              b.Permute(b.LoadConstantUint32(mask), LoadVR(b, 4), LoadVR(b, 5),
                        INT32_TYPE));
      b.Return();
    })
        .Run(
            [](PPCContext* ctx) {
              ctx->v[4] = vec128i(0, 1, 2, 3);
              ctx->v[5] = vec128i(4, 5, 6, 7);
            },
            [](PPCContext* ctx) {
              auto result = ctx->v[3];
              REQUIRE(result == vec128i(3, 2, 1, 0));
            });
  }
  {
    uint32_t mask = MakePermuteMask(1, 3, 1, 2, 1, 1, 1, 0);
    TestFunction([mask](HIRBuilder& b) {
      StoreVR(b, 3,
              b.Permute(b.LoadConstantUint32(mask), LoadVR(b, 4), LoadVR(b, 5),
                        INT32_TYPE));
      b.Return();
    })
        .Run(
            [](PPCContext* ctx) {
              ctx->v[4] = vec128i(0, 1, 2, 3);
              ctx->v[5] = vec128i(4, 5, 6, 7);
            },
            [](PPCContext* ctx) {
              auto result = ctx->v[3];
              REQUIRE(result == vec128i(7, 6, 5, 4));
            });
  }
}

TEST_CASE("PERMUTE_V128_BY_V128", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(b, 3,
            b.Permute(LoadVR(b, 3), LoadVR(b, 4), LoadVR(b, 5), INT8_TYPE));
    b.Return();
  });
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[3] =
            vec128b(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
        ctx->v[4] =
            vec128b(100, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
        ctx->v[5] = vec128b(16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28,
                            29, 30, 31);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        REQUIRE(result == vec128b(100, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
                                  13, 14, 15));
      });
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[3] = vec128b(16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28,
                            29, 30, 31);
        ctx->v[4] =
            vec128b(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
        ctx->v[5] = vec128b(116, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28,
                            29, 30, 31);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        REQUIRE(result == vec128b(116, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26,
                                  27, 28, 29, 30, 31));
      });
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[3] =
            vec128b(15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0);
        ctx->v[4] =
            vec128b(100, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
        ctx->v[5] = vec128b(16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28,
                            29, 30, 31);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        REQUIRE(result == vec128b(15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3,
                                  2, 1, 100));
      });
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[3] = vec128b(31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19,
                            18, 17, 16);
        ctx->v[4] =
            vec128b(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
        ctx->v[5] = vec128b(16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28,
                            29, 30, 131);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        REQUIRE(result == vec128b(131, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21,
                                  20, 19, 18, 17, 16));
      });
}

// Test PERMUTE with both src2 and src3 zero (AVX512 optimization path)
TEST_CASE("PERMUTE_V128_WITH_BOTH_ZERO", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(b, 3,
            b.Permute(LoadVR(b, 3), b.LoadZeroVec128(), b.LoadZeroVec128(),
                      INT8_TYPE));
    b.Return();
  });
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[3] =
            vec128b(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        REQUIRE(result ==
                vec128b(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0));
      });
}

// Test PERMUTE with src3 = zero, permuting from src2 (AVX512 optimization path)
TEST_CASE("PERMUTE_V128_WITH_ZERO_SRC3", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(
        b, 3,
        b.Permute(LoadVR(b, 3), LoadVR(b, 4), b.LoadZeroVec128(), INT8_TYPE));
    b.Return();
  });

  // Identity indices
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[3] =
            vec128b(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
        ctx->v[4] = vec128b(10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120,
                            130, 140, 150, 160);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        REQUIRE(result == vec128b(10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110,
                                  120, 130, 140, 150, 160));
      });

  // Reverse order indices
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[3] =
            vec128b(15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0);
        ctx->v[4] = vec128b(10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120,
                            130, 140, 150, 160);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        REQUIRE(result == vec128b(160, 150, 140, 130, 120, 110, 100, 90, 80, 70,
                                  60, 50, 40, 30, 20, 10));
      });

  // Indices that select specific bytes repeatedly
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[3] =
            vec128b(0, 0, 0, 0, 5, 5, 5, 5, 10, 10, 10, 10, 15, 15, 15, 15);
        ctx->v[4] = vec128b(10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120,
                            130, 140, 150, 160);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        REQUIRE(result == vec128b(10, 10, 10, 10, 60, 60, 60, 60, 110, 110, 110,
                                  110, 160, 160, 160, 160));
      });

  // Indices >= 16 should produce zero (tests masking behavior)
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[3] =
            vec128b(0, 1, 2, 16, 17, 18, 19, 20, 8, 9, 10, 24, 25, 26, 27, 28);
        ctx->v[4] = vec128b(10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120,
                            130, 140, 150, 160);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        REQUIRE(result == vec128b(10, 20, 30, 0, 0, 0, 0, 0, 90, 100, 110, 0, 0,
                                  0, 0, 0));
      });

  // All indices pointing to same element
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[3] = vec128b(7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7);
        ctx->v[4] = vec128b(10, 20, 30, 40, 50, 60, 70, 255, 90, 100, 110, 120,
                            130, 140, 150, 160);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        REQUIRE(result == vec128b(255, 255, 255, 255, 255, 255, 255, 255, 255,
                                  255, 255, 255, 255, 255, 255, 255));
      });
}

// Test PERMUTE with constant src2 and src3 = zero (AVX512 optimization path)
TEST_CASE("PERMUTE_V128_WITH_CONST_SRC2_ZERO_SRC3", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(b, 3,
            b.Permute(LoadVR(b, 3),
                      b.LoadConstantVec128(vec128b(11, 22, 33, 44, 55, 66, 77,
                                                   88, 99, 111, 121, 131, 141,
                                                   151, 161, 171)),
                      b.LoadZeroVec128(), INT8_TYPE));
    b.Return();
  });
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[3] =
            vec128b(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        REQUIRE(result == vec128b(11, 22, 33, 44, 55, 66, 77, 88, 99, 111, 121,
                                  131, 141, 151, 161, 171));
      });
}

// Test PERMUTE edge cases with src3 = zero (AVX512 optimization path)
TEST_CASE("PERMUTE_V128_ZERO_SRC3_EDGE_CASES", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(
        b, 3,
        b.Permute(LoadVR(b, 3), LoadVR(b, 4), b.LoadZeroVec128(), INT8_TYPE));
    b.Return();
  });

  // Test boundary: index = 15 (last valid), 16 (first invalid)
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[3] = vec128b(15, 16, 15, 16, 15, 16, 15, 16, 15, 16, 15, 16, 15,
                            16, 15, 16);
        ctx->v[4] =
            vec128b(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        REQUIRE(result == vec128b(15, 0, 15, 0, 15, 0, 15, 0, 15, 0, 15, 0, 15,
                                  0, 15, 0));
      });

  // Test with high bit indices (should mask to valid range or zero)
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[3] = vec128b(31, 63, 127, 255, 0, 1, 2, 3, 32, 48, 64, 80, 96,
                            112, 128, 144);
        ctx->v[4] = vec128b(10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120,
                            130, 140, 150, 160);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        // Fallback produces: [00000000 0A141E28 0A000A00 0A000A00]
        REQUIRE(result == vec128b(0, 0, 0, 0, 10, 20, 30, 40, 10, 0, 10, 0, 10,
                                  0, 10, 0));
      });

  // Test with src4 having zero bytes to ensure they're selected correctly
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[3] =
            vec128b(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
        ctx->v[4] = vec128b(0, 0, 0, 0, 255, 255, 255, 255, 128, 128, 128, 128,
                            1, 2, 3, 4);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        REQUIRE(result == vec128b(0, 0, 0, 0, 255, 255, 255, 255, 128, 128, 128,
                                  128, 1, 2, 3, 4));
      });

  // Test with non-zero patterns in src2 (floating point bit patterns that might
  // be sensitive)
  test.Run(
      [](PPCContext* ctx) {
        // Use bit patterns that look like valid floats
        ctx->v[3] =
            vec128b(0, 4, 8, 12, 1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15);
        ctx->v[4] = vec128b(0x3F, 0x80, 0x00, 0x00,   // 1.0f
                            0x40, 0x00, 0x00, 0x00,   // 2.0f
                            0x40, 0x40, 0x00, 0x00,   // 3.0f
                            0x40, 0x80, 0x00, 0x00);  // 4.0f
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        // Fallback produces: [3F404040 80004080 00000000 00000000]
        REQUIRE(result == vec128b(0x3F, 0x40, 0x40, 0x40, 0x80, 0x00, 0x40,
                                  0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                  0x00, 0x00));
      });

  // Test with pattern that might expose register aliasing issues
  test.Run(
      [](PPCContext* ctx) {
        // Sequential indices with a specific pattern
        ctx->v[3] =
            vec128b(15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0);
        ctx->v[4] = vec128b(0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x11, 0x22,
                            0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0x00);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        // Fallback produces: [00998877 66554433 2211FFEE DDCCBBAA]
        REQUIRE(result == vec128b(0x00, 0x99, 0x88, 0x77, 0x66, 0x55, 0x44,
                                  0x33, 0x22, 0x11, 0xFF, 0xEE, 0xDD, 0xCC,
                                  0xBB, 0xAA));
      });

  // Test that might expose register aliasing bug (chained permutes)
  test.Run(
      [](PPCContext* ctx) {
        // First permute result becomes input to test potential register reuse
        ctx->v[3] =
            vec128b(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
        ctx->v[4] = vec128b(0xFF, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA, 0x99, 0x88,
                            0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x00);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        // Simple identity-ish permute to ensure data isn't corrupted
        REQUIRE(result == vec128b(0xFF, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA, 0x99,
                                  0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22,
                                  0x11, 0x00));
      });

  // Test alternating valid/invalid pattern
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[3] =
            vec128b(0, 20, 1, 21, 2, 22, 3, 23, 4, 24, 5, 25, 6, 26, 7, 27);
        ctx->v[4] = vec128b(100, 101, 102, 103, 104, 105, 106, 107, 108, 109,
                            110, 111, 112, 113, 114, 115);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        REQUIRE(result == vec128b(100, 0, 101, 0, 102, 0, 103, 0, 104, 0, 105,
                                  0, 106, 0, 107, 0));
      });

  // Test wraparound behavior: indices with only lower bits set
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[3] =
            vec128b(0, 16, 1, 17, 2, 18, 3, 19, 4, 20, 5, 21, 6, 22, 7, 23);
        ctx->v[4] = vec128b(10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22,
                            23, 24, 25);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        REQUIRE(result == vec128b(10, 0, 11, 0, 12, 0, 13, 0, 14, 0, 15, 0, 16,
                                  0, 17, 0));
      });

  // Test all invalid indices (all should be zero)
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[3] = vec128b(16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28,
                            29, 30, 31);
        ctx->v[4] = vec128b(255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                            255, 255, 255, 255, 255, 255);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        REQUIRE(result ==
                vec128b(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0));
      });

  // Test pattern that might reveal incorrect masking: indices 0-15 in reverse
  // then indices 16-31
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[3] = vec128b(15, 14, 13, 12, 11, 10, 9, 8, 16, 17, 18, 19, 20,
                            21, 22, 23);
        ctx->v[4] = vec128b(200, 201, 202, 203, 204, 205, 206, 207, 208, 209,
                            210, 211, 212, 213, 214, 215);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        REQUIRE(result == vec128b(215, 214, 213, 212, 211, 210, 209, 208, 0, 0,
                                  0, 0, 0, 0, 0, 0));
      });

  // Test with indices exactly at boundary (15) for all positions
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[3] = vec128b(15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
                            15, 15, 15);
        ctx->v[4] =
            vec128b(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 99);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        REQUIRE(result == vec128b(99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
                                  99, 99, 99, 99, 99));
      });
}

// Test PERMUTE with constant src1 identity (tests XOR swap logic)
TEST_CASE("PERMUTE_V128_CONST_SRC1_IDENTITY", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(b, 3,
            b.Permute(b.LoadConstantVec128(vec128b(0, 1, 2, 3, 4, 5, 6, 7, 8, 9,
                                                   10, 11, 12, 13, 14, 15)),
                      LoadVR(b, 4), b.LoadZeroVec128(), INT8_TYPE));
    b.Return();
  });

  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] = vec128b(100, 101, 102, 103, 104, 105, 106, 107, 108, 109,
                            110, 111, 112, 113, 114, 115);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        REQUIRE(result == vec128b(100, 101, 102, 103, 104, 105, 106, 107, 108,
                                  109, 110, 111, 112, 113, 114, 115));
      });
}

// Test PERMUTE with constant src1 XOR swap pattern
TEST_CASE("PERMUTE_V128_CONST_SRC1_XOR_SWAP", "[instr]") {
  // Index 0 XOR 3 = 3, Index 1 XOR 3 = 2, Index 2 XOR 3 = 1, Index 3 XOR 3 = 0
  TestFunction test([](HIRBuilder& b) {
    StoreVR(b, 3,
            b.Permute(b.LoadConstantVec128(vec128b(3, 2, 1, 0, 7, 6, 5, 4, 11,
                                                   10, 9, 8, 15, 14, 13, 12)),
                      LoadVR(b, 4), b.LoadZeroVec128(), INT8_TYPE));
    b.Return();
  });

  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] =
            vec128b(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        // Fallback produces: [03020100 07060504 0B0A0908 0F0E0D0C]
        REQUIRE(result ==
                vec128b(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12));
      });
}

// Test PERMUTE with constant src1 invalid after XOR
TEST_CASE("PERMUTE_V128_CONST_SRC1_INVALID_AFTER_XOR", "[instr]") {
  // Indices 16,17,18,19 XOR 3 = 19,18,17,16 (all still >= 16, should be zero)
  TestFunction test([](HIRBuilder& b) {
    StoreVR(
        b, 3,
        b.Permute(b.LoadConstantVec128(vec128b(16, 17, 18, 19, 20, 21, 22, 23,
                                               24, 25, 26, 27, 28, 29, 30, 31)),
                  LoadVR(b, 4), b.LoadZeroVec128(), INT8_TYPE));
    b.Return();
  });

  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] = vec128b(255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                            255, 255, 255, 255, 255, 255);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        REQUIRE(result ==
                vec128b(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0));
      });
}

// Test PERMUTE with constant src1 XOR boundary transition
TEST_CASE("PERMUTE_V128_CONST_SRC1_XOR_BOUNDARY", "[instr]") {
  // 13 XOR 3 = 14, 14 XOR 3 = 13, 15 XOR 3 = 12, 16 XOR 3 = 19
  TestFunction test([](HIRBuilder& b) {
    StoreVR(
        b, 3,
        b.Permute(b.LoadConstantVec128(vec128b(13, 14, 15, 16, 13, 14, 15, 16,
                                               13, 14, 15, 16, 13, 14, 15, 16)),
                  LoadVR(b, 4), b.LoadZeroVec128(), INT8_TYPE));
    b.Return();
  });

  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] =
            vec128b(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        // Fallback produces: [0D0E0F00 0D0E0F00 0D0E0F00 0D0E0F00]
        REQUIRE(result == vec128b(13, 14, 15, 0, 13, 14, 15, 0, 13, 14, 15, 0,
                                  13, 14, 15, 0));
      });
}

// Test PERMUTE with constant src1 all 0x03
TEST_CASE("PERMUTE_V128_CONST_SRC1_ALL_THREE", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(b, 3,
            b.Permute(b.LoadConstantVec128(vec128b(3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
                                                   3, 3, 3, 3, 3, 3)),
                      LoadVR(b, 4), b.LoadZeroVec128(), INT8_TYPE));
    b.Return();
  });

  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] = vec128b(10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120,
                            130, 140, 150, 160);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        // Fallback produces: [28282828 28282828 28282828 28282828] (0x28 = 40
        // decimal)
        REQUIRE(result == vec128b(40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40,
                                  40, 40, 40, 40, 40));
      });
}
