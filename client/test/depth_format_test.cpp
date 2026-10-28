/* WiVRn VR streaming
 * Copyright (C) 2026 General Robotics
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "../vivarium/depth_format.h"
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>

void check(bool value, const char * message)
{
    if (not value) throw std::runtime_error(message);
}
int main(int argc, char ** argv)
{
    if (argc != 2) return 2;
    using namespace vivarium;
    // The production header writer, not a parser/re-encoder of the expected bytes.
    const std::array<depth_view, 2> views{{
        {-0.875f, 0.75f, 0.8125f, -0.78125f, -0.03125f, 1.625f, -0.0625f, 0, 0, 0, 1, 0.125f, 8},
        {-0.75f, 0.875f, 0.8125f, -0.78125f, 0.03125f, 1.625f, -0.0625f, 0, 0, 0, 1, 0.125f, 8},
    }};
    std::vector<uint8_t> actual;
    depth_header(actual, 1'000'000'123'000'000, 1'000'000'130'000'000, 4, 3, views);
    for (int view = 0; view < 2; ++view)
        for (int row = 0; row < 3; ++row)
            for (auto mm : (view == 0 ? std::array<uint16_t, 4>{500, 750, 1000, 1250}
                                      : std::array<uint16_t, 4>{0, 2000, 2000, 65535})) depth_pixel(actual, mm);
    std::ifstream fixture(std::string(argv[1]) + "/depth.bin", std::ios::binary);
    check(bool(fixture), "missing M16c depth golden");
    const std::vector<uint8_t> expected{std::istreambuf_iterator<char>(fixture), std::istreambuf_iterator<char>()};
    check(actual == expected, "production DEPTH serializer differs from M16c golden");
    check(depth_mm(0, 0.125f, 8) == 125, "near plane");
    check(depth_mm(65535, 0.125f, 8) == 0, "invalid sentinel");
    check(depth_mm(65534, 0.125f, 8) == 7992, "finite far endpoint");
    check(depth_mm(32768, 0.125f, 8) == 246, "finite interior");
    check(depth_mm(32768, 0.125f, 0) == 250, "META finite reversed planes mean infinity");
    check(depth_mm(32768, 0.125f, std::numeric_limits<float>::infinity()) == 250, "infinite far plane");
    check(depth_mm(65534, 0.125f, 0) == 65535, "millimetre saturation");
    check(depth_mm(0, 0.0001f, 1) == 0, "sub-millimetre rounds to invalid zero");
    check(depth_mm(0, 0.0015f, 1) == 2, "millimetre rounding");
    check(depth_mm(1, -1, 8) == 0, "negative near invalid");
    check(depth_mm(1, 0, 8) == 0, "zero near invalid");
    check(depth_mm(1, std::numeric_limits<float>::quiet_NaN(), 8) == 0, "NaN near invalid");
    check(depth_mm(1, 0.125f, std::numeric_limits<float>::quiet_NaN()) == 0, "NaN far invalid");
    check(depth_mm(1, 0.125f, 0.125f) == 0, "equal planes invalid");
    check(depth_mm(65536, 0.125f, 8) == 0, "out-of-range sample invalid");
    std::cout << "DEPTH serializer matches M16c golden; 15 depth boundary checks passed\n";
}
