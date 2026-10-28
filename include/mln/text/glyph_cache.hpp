#pragma once

#include <mln/text/glyph.hpp>
#include <mln/util/immutable.hpp>

#include <cstddef>
#include <map>
#include <memory>

namespace mln {

// Keeps one copy of equal glyphs (same id, metrics and bitmap) and of equal glyph ranges. intern() returns
// the copy already in use if there is one, and otherwise stores and returns the one it is given, as in string
// interning. The cache only holds weak references: a glyph is destroyed with the last Immutable<Glyph> that
// points to it, and a later intern() removes its entry. It can be used from several threads.
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
