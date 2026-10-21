#include <mln/test/util.hpp>
#include <mln/test/stub_file_source.hpp>
#include <mln/test/map_adapter.hpp>

#include <mln/gfx/headless_frontend.hpp>
#include <mln/map/map_options.hpp>
#include <mln/style/style.hpp>
#include <mln/util/run_loop.hpp>
#include <mln/vulkan/headless_backend.hpp>

#include <memory>
#include <thread>

using namespace mln;

namespace {

vk::Device deviceOf(HeadlessFrontend& frontend) {
    return static_cast<vulkan::RendererBackend*>(frontend.getBackend())->getDevice().get();
}

Color renderBackground(HeadlessFrontend& frontend) {
    MapAdapter map(frontend,
                   MapObserver::nullObserver(),
                   std::make_shared<StubFileSource>(ResourceOptions::Default(), ClientOptions()),
                   MapOptions().withMapMode(MapMode::Static).withSize(frontend.getSize()));
    map.getStyle().loadJSON(
        R"({"version": 8, "sources": {}, "layers": [{"id": "bg", "type": "background", "paint": {"background-color": "#ff0000"}}]})");
    const PremultipliedImage image = frontend.render(map).image;
    return {image.data[0] / 255.0f, image.data[1] / 255.0f, image.data[2] / 255.0f, image.data[3] / 255.0f};
}

} // namespace

TEST(VulkanHeadlessBackend, SharesTheDeviceOfItsThread) {
    util::RunLoop loop;
    HeadlessFrontend first{{64, 64}, 1};
    HeadlessFrontend second{{64, 64}, 1};

    EXPECT_EQ(deviceOf(first), deviceOf(second));
}

TEST(VulkanHeadlessBackend, DoesNotShareTheDeviceOfAnotherThread) {
    util::RunLoop loop;
    HeadlessFrontend first{{64, 64}, 1};

    std::thread([&] {
        util::RunLoop threadLoop;
        HeadlessFrontend second{{64, 64}, 1};
        EXPECT_NE(deviceOf(first), deviceOf(second));
    }).join();
}

TEST(VulkanHeadlessBackend, RendersAfterTheBackendThatCreatedTheDeviceIsGone) {
    util::RunLoop loop;
    auto first = std::make_unique<HeadlessFrontend>(Size{64, 64}, 1.0f);
    HeadlessFrontend second{{64, 64}, 1};
    first.reset();

    EXPECT_EQ(Color::red(), renderBackground(second));
}

TEST(VulkanHeadlessBackend, RendersWithANewDeviceAfterTheLastBackendIsGone) {
    util::RunLoop loop;
    std::make_unique<HeadlessFrontend>(Size{64, 64}, 1.0f).reset();
    HeadlessFrontend frontend{{64, 64}, 1};

    EXPECT_EQ(Color::red(), renderBackground(frontend));
}
