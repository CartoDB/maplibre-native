#pragma once

#include <mln/text/glyph.hpp>
#include <mln/util/immutable.hpp>

#include <cstddef>
#include <map>
#include <memory>

namespace mln {

// Keeps one copy of equal glyphs (same id, metrics and bitmap) and of equal glyph ranges: intern() returns
// the copy that is alive, or keeps the one it is given. It doesn't own them, it only holds weak references:
// a glyph lives while someone else holds it, and its entry is dropped as the table grows. It can be used
// from several threads.
class GlyphCache {
public:
    GlyphCache();
    ~GlyphCache();
    GlyphCache(const GlyphCache&) = delete;
    GlyphCache& operator=(const GlyphCache&) = delete;

    using Range = std::map<GlyphID, Immutable<Glyph>>;

    Immutable<Glyph> intern(Glyph&&);

    // Ranges are matched by the glyphs they point to: intern the glyphs first.
    std::shared_ptr<const Range> intern(Range&&);

    // Number of glyphs alive, not counting ranges. It walks the table: meant for tests.
    std::size_t size() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace mln
