#include <mln/storage/file_source.hpp>
#include <mln/storage/resource.hpp>
#include <mln/storage/response.hpp>
#include <mln/text/glyph_manager.hpp>
#include <mln/text/glyph_manager_observer.hpp>
#include <mln/text/glyph_pbf.hpp>
#include <mln/util/async_request.hpp>
#include <mln/util/std.hpp>
#include <mln/util/tiny_sdf.hpp>
#include <mln/util/image.hpp>
#include <mln/util/hash.hpp>
#include <mln/util/logging.hpp>

#include <algorithm>
#include <fstream>
#include <functional>
#include <mutex>
#include <string_view>
#include <unordered_map>

namespace mln {

namespace {
GlyphManagerObserver nullObserver;

// Every Map has its own GlyphManager, and font stacks that end in the same font
// get the same glyphs from the server. Parsed glyphs never change, so all the
// GlyphManagers of the process share one copy of each; the last one to drop a
// glyph frees it.
class SharedGlyphs {
public:
    static SharedGlyphs& get() {
        // Never destroyed: GlyphManagers may outlive static destruction.
        static auto* shared = new SharedGlyphs();
        return *shared;
    }

    Immutable<Glyph> share(Glyph&& glyph) {
        const std::size_t hash = hashOf(glyph);
        std::scoped_lock lock(mutex);
        auto [first, last] = glyphs.equal_range(hash);
        for (auto it = first; it != last; ++it) {
            if (auto known = it->second.lock(); known && isSame(**known, glyph)) {
                return *known;
            }
        }
        Immutable<Glyph> added = makeMutable<Glyph>(std::move(glyph));
        glyphs.emplace(hash, added);
        if (glyphs.size() >= nextSweep) {
            util::erase_if(glyphs, [](const auto& entry) { return entry.second.expired(); });
            nextSweep = std::max(minSweep, 2 * glyphs.size());
        }
        return added;
    }

    // Ranges are compared by their glyphs, which share() has already made unique.
    std::shared_ptr<const std::map<GlyphID, Immutable<Glyph>>> shareRange(std::map<GlyphID, Immutable<Glyph>>&& range) {
        std::size_t hash = 0;
        for (const auto& [id, glyph] : range) {
            util::hash_combine(hash, glyph.get());
        }
        std::scoped_lock lock(mutex);
        auto [first, last] = ranges.equal_range(hash);
        for (auto it = first; it != last; ++it) {
            if (auto known = it->second.lock(); known && isSame(*known, range)) {
                return known;
            }
        }
        auto added = std::make_shared<const std::map<GlyphID, Immutable<Glyph>>>(std::move(range));
        ranges.emplace(hash, added);
        if (ranges.size() >= nextRangeSweep) {
            util::erase_if(ranges, [](const auto& entry) { return entry.second.expired(); });
            nextRangeSweep = std::max(minSweep, 2 * ranges.size());
        }
        return added;
    }

private:
    static std::size_t hashOf(const Glyph& glyph) {
        const auto& m = glyph.metrics;
        const std::string_view bitmap(reinterpret_cast<const char*>(glyph.bitmap.data.get()), glyph.bitmap.bytes());
        return util::hash(glyph.id.hash, m.width, m.height, m.left, m.top, m.advance, m.isDoubleResolution, bitmap);
    }

    static bool isSame(const Glyph& a, const Glyph& b) {
        return a.id.hash == b.id.hash && a.metrics == b.metrics && a.bitmap.size == b.bitmap.size &&
               a.bitmap == b.bitmap;
    }

    static bool isSame(const std::map<GlyphID, Immutable<Glyph>>& a, const std::map<GlyphID, Immutable<Glyph>>& b) {
        return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](const auto& x, const auto& y) {
            return x.first.hash == y.first.hash && x.second == y.second;
        });
    }

    static constexpr std::size_t minSweep = 4096;
    std::mutex mutex;
    std::unordered_multimap<std::size_t, WeakImmutable<Glyph>> glyphs;
    std::size_t nextSweep = minSweep;
    std::unordered_multimap<std::size_t, std::weak_ptr<const std::map<GlyphID, Immutable<Glyph>>>> ranges;
    std::size_t nextRangeSweep = minSweep;
};
} // namespace

GlyphManager::GlyphManager(std::unique_ptr<LocalGlyphRasterizer> localGlyphRasterizer_)
    : observer(&nullObserver),
      localGlyphRasterizer(std::move(localGlyphRasterizer_)) {}

GlyphManager::~GlyphManager() {
    hbShapers.clear(); // clear harfbuzz + freetype face before library;
}

void GlyphManager::getGlyphs(GlyphRequestor& requestor, GlyphDependencies glyphDependencies, FileSource& fileSource) {
    auto dependencies = std::make_shared<GlyphDependencies>(std::move(glyphDependencies));
    {
        std::scoped_lock readWriteLock(rwLock);
        // Figure out which glyph ranges need to be fetched. For each range that
        // does need to be fetched, record an entry mapping the requestor to a
        // shared pointer containing the dependencies. When the shared pointer
        // becomes unique, we know that all the dependencies for that requestor have
        // been fetched, and can notify it of completion.
        for (const auto& dependency : dependencies->glyphs) {
            const FontStack& fontStack = dependency.first;
            Entry& entry = entries[fontStack];

            const GlyphIDs& glyphIDs = dependency.second;
            std::unordered_set<GlyphRange> ranges;
            for (const auto& glyphID : glyphIDs) {
                if (localGlyphRasterizer->canRasterizeGlyph(fontStack, glyphID)) {
                    if (entry.glyphs.find(glyphID) == entry.glyphs.end()) {
                        entry.glyphs.emplace(glyphID, makeMutable<Glyph>(generateLocalSDF(fontStack, glyphID)));
                    }
                } else {
                    ranges.insert(getGlyphRange(glyphID));
                }
            }

            for (const auto& range : ranges) {
                auto it = entry.ranges.find(range);
                if (it == entry.ranges.end() || !it->second.parsed) {
                    GlyphRequest& request = entry.ranges[range];
                    request.requestors[&requestor] = dependencies;
                    requestRange(request, fontStack, range, fileSource);
                }
            }
        }
    }

    // If the shared dependencies pointer is already unique, then all dependent
    // glyph ranges have already been loaded. Send a notification immediately.
    if (dependencies.use_count() == 1) {
        notify(requestor, *dependencies);
    }
}

Glyph GlyphManager::generateLocalSDF(const FontStack& fontStack, GlyphID glyphID) {
    Glyph local = localGlyphRasterizer->rasterizeGlyph(fontStack, glyphID);
    const double radius = local.metrics.isDoubleResolution ? 16.0 : 8.0;
    local.bitmap = util::transformRasterToSDF(local.bitmap, radius, .25);
    return local;
}

void GlyphManager::requestRange(GlyphRequest& request,
                                const FontStack& fontStack,
                                const GlyphRange& range,
                                FileSource& fileSource) {
    if (request.req) {
        return;
    }
    Resource res(Resource::Kind::Unknown, "");
    switch (range.type) {
        case GlyphIDType::FontPBF:
            res = Resource::glyphs(glyphURL, fontStack, std::pair<uint16_t, uint16_t>{range.first, range.second});
            break;
        default: {
            std::string url = getFontFaceURL(range.type);
            if (url.size()) {
                res = Resource::fontFace(url);
            } else {
                Log::Error(Event::Style, "Try download a glyph doesn't in current faces");
            }

        } break;
    }

    observer->onGlyphsRequested(fontStack, range);

    request.req = fileSource.request(
        res, [this, fontStack, range](const Response& response) { processResponse(response, fontStack, range); });
}

void GlyphManager::processResponse(const Response& res, const FontStack& fontStack, const GlyphRange& range) {
    if (res.error) {
        observer->onGlyphsError(fontStack, range, std::make_exception_ptr(std::runtime_error(res.error->message)));
        return;
    }

    if (res.notModified) {
        return;
    }

    {
        std::scoped_lock readWriteLock(rwLock);

        Entry& entry = entries[fontStack];
        GlyphRequest& request = entry.ranges[range];

        if (!res.noContent) {
            std::vector<Glyph> glyphs;

            try {
                if (range.type == GlyphIDType::FontPBF) {
                    glyphs = parseGlyphPBF(range, *res.data);
                } else {
                    if (loadHBShaper(fontStack, range.type, *res.data)) {
                        Glyph temp;
                        temp.id = GlyphID(0, range.type);
                        glyphs.emplace_back(std::move(temp));
                    }
                }
            } catch (...) {
                observer->onGlyphsError(fontStack, range, std::current_exception());
                return;
            }

            if (range.type == GlyphIDType::FontPBF) {
                std::map<GlyphID, Immutable<Glyph>> rangeGlyphs;
                for (auto& glyph : glyphs) {
                    auto id = glyph.id;
                    rangeGlyphs.insert_or_assign(id, SharedGlyphs::get().share(std::move(glyph)));
                }
                request.glyphs = SharedGlyphs::get().shareRange(std::move(rangeGlyphs));
            } else {
                for (auto& glyph : glyphs) {
                    auto id = glyph.id;
                    if (!localGlyphRasterizer->canRasterizeGlyph(fontStack, id)) {
                        entry.glyphs.erase(id);
                        entry.glyphs.emplace(id, makeMutable<Glyph>(std::move(glyph)));
                    }
                }
            }
        }

        request.parsed = true;

        for (auto& pair : request.requestors) {
            GlyphRequestor& requestor = *pair.first;
            const std::shared_ptr<GlyphDependencies>& dependencies = pair.second;
            if (dependencies.use_count() == 1) {
                notify(requestor, *dependencies);
            }
        }

        request.requestors.clear();
    }

    observer->onGlyphsLoaded(fontStack, range);
}

void GlyphManager::setObserver(GlyphManagerObserver* observer_) {
    observer = observer_ ? observer_ : &nullObserver;
}

void GlyphManager::notify(GlyphRequestor& requestor, const GlyphDependencies& glyphDependencies) {
    GlyphMap response;

    for (const auto& dependency : glyphDependencies.glyphs) {
        const FontStack& fontStack = dependency.first;
        const GlyphIDs& glyphIDs = dependency.second;

        Glyphs& glyphs = response[FontStackHasher()(fontStack)];
        Entry& entry = entries[fontStack];

        for (const auto& glyphID : glyphIDs) {
            glyphs.emplace(glyphID, findGlyph(fontStack, entry, glyphID));
        }
    }

    requestor.onGlyphsAvailable(response, glyphDependencies.shapes);
}

void GlyphManager::removeRequestor(GlyphRequestor& requestor) {
    std::scoped_lock readWriteLock(rwLock);
    for (auto& entry : entries) {
        for (auto& range : entry.second.ranges) {
            range.second.requestors.erase(&requestor);
        }
    }
}

void GlyphManager::evict(const std::set<FontStack>& keep) {
    std::scoped_lock readWriteLock(rwLock);
    util::erase_if(entries, [&](const auto& entry) { return keep.count(entry.first) == 0; });
}

std::shared_ptr<HBShaper> GlyphManager::getHBShaper(FontStack fontStack, GlyphIDType type) {
    if (hbShapers.find(fontStack) != hbShapers.end()) {
        auto& glyphs = hbShapers[fontStack];
        if (glyphs.find(type) != glyphs.end()) {
            return glyphs[type];
        }
    }

    return nullptr;
}

bool GlyphManager::loadHBShaper(const FontStack& fontStack, GlyphIDType type, const std::string& data) {
    auto shaper = std::make_shared<HBShaper>(type, data, ftLibrary);
    if (!shaper->valid()) return false;
    hbShapers[fontStack][type] = shaper;
    return true;
}

Immutable<Glyph> GlyphManager::getGlyph(const FontStack& fontStack, GlyphID glyphID) {
    auto& entry = entries[fontStack];
    if (auto glyph = findGlyph(fontStack, entry, glyphID)) return *glyph;

    if (glyphID.complex.type != FontPBF) {
        auto shaper = getHBShaper(fontStack, glyphID.complex.type);
        if (shaper) {
            auto glyph = shaper->rasterizeGlyph(glyphID);

            glyph.bitmap = util::transformRasterToSDF(glyph.bitmap, 8, .25);
            entry.glyphs.emplace(glyphID, makeMutable<Glyph>(std::move(glyph)));
            return entry.glyphs.at(glyphID);
        }
    }

    Glyph empty;

    return makeMutable<Glyph>(std::move(empty));
}

std::optional<Immutable<Glyph>> GlyphManager::findGlyph(const FontStack& fontStack,
                                                        const Entry& entry,
                                                        GlyphID glyphID) {
    if (auto it = entry.glyphs.find(glyphID); it != entry.glyphs.end()) {
        return it->second;
    }
    if (glyphID.complex.type != GlyphIDType::FontPBF || localGlyphRasterizer->canRasterizeGlyph(fontStack, glyphID)) {
        return std::nullopt;
    }
    auto range = entry.ranges.find(getGlyphRange(glyphID));
    if (range == entry.ranges.end() || !range->second.glyphs) {
        return std::nullopt;
    }
    if (auto it = range->second.glyphs->find(glyphID); it != range->second.glyphs->end()) {
        return it->second;
    }
    return std::nullopt;
}

void GlyphManager::hbShaping(const std::u16string& text,
                             const FontStack& font,
                             GlyphIDType type,
                             std::vector<GlyphID>& glyphIDs,
                             std::vector<HBShapeAdjust>& adjusts) {
    auto shaper = getHBShaper(font, type);
    if (shaper) {
        shaper->createComplexGlyphIDs(text, glyphIDs, adjusts);
    }
}

std::string GlyphManager::getFontFaceURL(GlyphIDType type) {
    std::string url;

    if (fontFaces) {
        for (auto& face : *fontFaces) {
            if (face.type == type) {
                url = face.url;
                break;
            }
        }
    }

    return url;
}

} // namespace mln
