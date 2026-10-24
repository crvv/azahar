// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include <array>
#include <utility>
#include <catch2/catch_test_macros.hpp>
#include "video_core/rasterizer_cache/surface_base.h"

namespace {

VideoCore::SurfaceParams MakeParams(PAddr addr, u32 width, u32 height) {
    VideoCore::SurfaceParams params;
    params.addr = addr;
    params.width = width;
    params.height = height;
    params.is_tiled = true;
    params.pixel_format = VideoCore::PixelFormat::RGBA8;
    params.UpdateParams();
    return params;
}

} // namespace

TEST_CASE("Surface copies preserve tile addresses across different strides", "[video_core]") {
    using namespace VideoCore;
    constexpr PAddr base = 0x180a6280;
    const std::array layouts = {
        std::pair{MakeParams(base, 128, 200), MakeParams(base, 64, 128)},
        std::pair{MakeParams(base, 64, 104), MakeParams(base, 128, 256)},
        std::pair{MakeParams(base + 0x6800, 32, 64), MakeParams(base, 64, 128)},
        std::pair{MakeParams(base, 128, 256), MakeParams(base + 0x6800, 32, 64)},
        std::pair{MakeParams(base, 96, 64), MakeParams(base + 256, 64, 64)},
    };

    for (const auto& [src_params, dest] : layouts) {
        CAPTURE(src_params.addr, src_params.width, dest.addr, dest.width);
        const SurfaceBase source{src_params, {}};
        SurfaceRegions remaining{source.GetInterval() & dest.GetInterval()};
        u32 copies = 0;
        while (!remaining.empty()) {
            REQUIRE(++copies <= 128);
            const auto requested = *remaining.begin();
            const auto copy = source.GetCopyableInterval(dest.FromInterval(requested), requested);
            REQUIRE_FALSE(boost::icl::is_empty(copy));
            REQUIRE((copy & requested) == copy);
            REQUIRE(source.CanCopy(dest, copy));

            const auto subrect = dest.FromInterval(copy);
            REQUIRE(subrect.GetInterval() == copy);
            const auto src_rect = source.GetSubRect(subrect);
            const auto dst_rect = dest.GetSubRect(subrect);
            for (u32 y = 0; y < subrect.height; y += 8) {
                for (u32 x = 0; x < subrect.width; x += 8) {
                    const u32 src_tile =
                        ((source.height - src_rect.top + y) / 8) * (source.stride / 8) +
                        (src_rect.left + x) / 8;
                    const u32 dst_tile =
                        ((dest.height - dst_rect.top + y) / 8) * (dest.stride / 8) +
                        (dst_rect.left + x) / 8;
                    REQUIRE(source.addr + src_tile * 256 == dest.addr + dst_tile * 256);
                }
            }
            remaining.erase(copy);
        }
    }
}

TEST_CASE("Surface copies stay inside the requested valid interval", "[video_core]") {
    using namespace VideoCore;
    constexpr PAddr base = 0x180a6280;
    SurfaceBase source{MakeParams(base, 128, 200), {}};
    const auto dest = MakeParams(base, 64, 128);
    // This starts halfway through a source tile row. Rounding the request back
    // to a full rectangle must not cause an already copied row to be selected again.
    const SurfaceInterval requested{base + 2048, base + 6144};
    source.MarkInvalid({base + 4096, base + 6144});
    const auto copy = source.GetCopyableInterval(dest.FromInterval(requested), requested);
    REQUIRE(copy == SurfaceInterval{base + 2048, base + 4096});
    REQUIRE(source.CanCopy(dest, copy));
    REQUIRE(source.IsRegionValid(copy));

    source.MarkInvalid(copy);
    REQUIRE(
        boost::icl::is_empty(source.GetCopyableInterval(dest.FromInterval(requested), requested)));
}

TEST_CASE("Matching surface strides retain rectangular copies", "[video_core]") {
    using namespace VideoCore;
    constexpr PAddr base = 0x180a6280;
    const SurfaceBase source{MakeParams(base, 128, 200), {}};
    const auto dest = MakeParams(base, 128, 128);
    const auto copy = source.GetCopyableInterval(dest, dest.GetInterval());
    REQUIRE(copy == dest.GetInterval());
    REQUIRE(source.CanCopy(dest, copy));
}

TEST_CASE("Surface stride copies use the source mip level", "[video_core]") {
    using namespace VideoCore;
    constexpr PAddr base = 0x180a6280;
    auto src_params = MakeParams(base, 64, 64);
    src_params.levels = 3;
    src_params.UpdateParams();
    const SurfaceBase source{src_params, {}};
    const auto dest = MakeParams(base + 64 * 64 * 4, 64, 16);
    SurfaceRegions remaining{dest.GetInterval()};
    u32 copies = 0;
    while (!remaining.empty()) {
        REQUIRE(++copies <= 16);
        const auto requested = *remaining.begin();
        const auto copy = source.GetCopyableInterval(dest.FromInterval(requested), requested);
        REQUIRE_FALSE(boost::icl::is_empty(copy));
        REQUIRE((copy & requested) == copy);
        REQUIRE(source.CanCopy(dest, copy));

        const auto subrect = dest.FromInterval(copy);
        REQUIRE(subrect.GetInterval() == copy);
        const u32 level = source.LevelOf(copy.lower());
        REQUIRE(level == 1);
        const auto src_rect = source.GetSubRect(subrect);
        const auto dst_rect = dest.GetSubRect(subrect);
        CAPTURE(copies, copy.lower(), copy.upper(), src_rect.left, src_rect.top, src_rect.right,
                src_rect.bottom);
        REQUIRE(src_rect.right <= (source.stride >> level));
        REQUIRE(src_rect.top <= (source.height >> level));
        REQUIRE(src_rect.bottom <= src_rect.top);
        for (u32 y = 0; y < subrect.height; y += 8) {
            for (u32 x = 0; x < subrect.width; x += 8) {
                const u32 src_tile = (((source.height >> level) - src_rect.top + y) / 8) *
                                         ((source.stride >> level) / 8) +
                                     (src_rect.left + x) / 8;
                const u32 dst_tile = ((dest.height - dst_rect.top + y) / 8) * (dest.stride / 8) +
                                     (dst_rect.left + x) / 8;
                REQUIRE(source.mipmap_offsets[level] + src_tile * 256 ==
                        dest.addr + dst_tile * 256);
            }
        }
        remaining.erase(copy);
    }
}

TEST_CASE("Linear surfaces copy one row across different strides", "[video_core]") {
    using namespace VideoCore;
    constexpr PAddr base = 0x180a6280;
    auto src_params = MakeParams(base, 8, 8);
    src_params.is_tiled = false;
    src_params.UpdateParams();
    auto dest = MakeParams(base, 16, 8);
    dest.is_tiled = false;
    dest.UpdateParams();
    const SurfaceBase source{src_params, {}};
    const SurfaceInterval requested{base + 4 * 4, base + 16 * 4};
    const auto copy = source.GetCopyableInterval(dest.FromInterval(requested), requested);
    REQUIRE(copy == SurfaceInterval{base + 4 * 4, base + 8 * 4});
    REQUIRE(source.CanCopy(dest, copy));
}
