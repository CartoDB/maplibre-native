#include <mln/test/util.hpp>

#include <mln/map/mode.hpp>
#include <mln/map/transform_state.hpp>
#include <mln/renderer/tile_pyramid.hpp>

using namespace mln;

namespace {

TransformState transformState(const Size size) {
    TransformState state;
    state.setSize(size);
    state.setMinZoom(0);
    state.setMaxZoom(22);
    return state;
}

} // namespace

TEST(TilePyramid, CacheSizeInContinuousModeCoversTheZoomRange) {
    EXPECT_EQ(11u, TilePyramid::cacheSize(MapMode::Continuous, transformState({512, 512}), 512, 1));
    EXPECT_EQ(44u, TilePyramid::cacheSize(MapMode::Continuous, transformState({1024, 1024}), 512, 9));
}

TEST(TilePyramid, CacheSizeInStillModesIsTwiceTheIdealTiles) {
    EXPECT_EQ(2u, TilePyramid::cacheSize(MapMode::Tile, transformState({512, 512}), 512, 1));
    EXPECT_EQ(18u, TilePyramid::cacheSize(MapMode::Static, transformState({1024, 1024}), 512, 9));
}

TEST(TilePyramid, CacheSizeInStillModesKeepsTwoTilesWithoutIdealTiles) {
    EXPECT_EQ(2u, TilePyramid::cacheSize(MapMode::Tile, transformState({512, 512}), 512, 0));
}

TEST(TilePyramid, CacheSizeInStillModesNeverExceedsTheContinuousOne) {
    TransformState state = transformState({512, 512});
    state.setMaxZoom(1);

    EXPECT_EQ(1u, TilePyramid::cacheSize(MapMode::Static, state, 512, 4));
}
