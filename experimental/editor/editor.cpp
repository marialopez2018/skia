// Copyright 2019 Google LLC.
// Use of this source code is governed by a BSD-style license that can be found in the LICENSE file.

#include "experimental/editor/editor.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkExecutor.h"
#include "include/core/SkFontMetrics.h"
#include "include/core/SkPath.h"
#include "modules/skshaper/include/SkShaper.h"
#include "src/utils/SkUTF.h"

#include "experimental/editor/run_handler.h"
#include "experimental/editor/word_boundaries.h"

#include <algorithm>

using namespace editor;

static inline SkRect offset(SkRect r, SkIPoint p) {
    r.offset((float)p.x(), (float)p.y());
    return r;
}

static constexpr SkRect kUnsetRect{-FLT_MAX, -FLT_MAX, -FLT_MAX, -FLT_MAX};

static SkRect selection_box(const SkFontMetrics& metrics,
                            float advance,
                            SkPoint pos) {
    if (fabsf(advance) < 1.0f) {
        advance = copysignf(1.0f, advance);
    }
    return SkRect{pos.x(),
                  pos.y() + metrics.fAscent,
                  pos.x() + advance,
                  pos.y() + metrics.fDescent}.makeSorted();
}

void callback_fn(void* context,
                 const char* utf8Text,
                 size_t utf8TextBytes,
                 size_t glyphCount,
                 const SkGlyphID* glyphs,
                 const SkPoint* positions,
                 const uint32_t* clusters,
                 const SkFont& font)
{
    SkASSERT(context);
    SkASSERT(glyphCount > 0);
    SkRect* cursors = (SkRect*)context;

    SkFontMetrics metrics;
    font.getMetrics(&metrics);
    std::unique_ptr<float[]> advances(new float[glyphCount]);
    font.getWidths(glyphs, glyphCount, advances.get());

    // Loop over each cluster in this run.
    size_t clusterStart = 0;
    for (size_t glyphIndex = 0; glyphIndex < glyphCount; ++glyphIndex) {
        if (glyphIndex + 1 < glyphCount  // more glyphs
            && clusters[glyphIndex] == clusters[glyphIndex + 1]) {
            continue; // multi-glyph cluster
        }
        unsigned textBegin = clusters[glyphIndex];
        unsigned textEnd = utf8TextBytes;
        for (size_t i = 0; i < glyphCount; ++i) {
            if (clusters[i] >= textEnd) {
                textEnd = clusters[i] + 1;
            }
        }
        for (size_t i = 0; i < glyphCount; ++i) {
            if (clusters[i] > textBegin && clusters[i] < textEnd) {
                textEnd = clusters[i];
                if (textEnd == textBegin + 1) { break; }
            }
        }
        SkASSERT(glyphIndex + 1 > clusterStart);
        unsigned clusterGlyphCount = glyphIndex + 1 - clusterStart;
        const SkPoint* clusterGlyphPositions = &positions[clusterStart];
        const float* clusterAdvances = &advances[clusterStart];
        clusterStart = glyphIndex + 1;  // for next loop

        SkRect clusterBox = selection_box(metrics, clusterAdvances[0], clusterGlyphPositions[0]);
        for (unsigned i = 1; i < clusterGlyphCount; ++i) { // multiple glyphs
            clusterBox.join(selection_box(metrics, clusterAdvances[i], clusterGlyphPositions[i]));
        }
        if (textBegin + 1 == textEnd) {  // single byte, fast path.
            cursors[textBegin] = clusterBox;
            continue;
        }
        int textCount = textEnd - textBegin;
        int codePointCount = SkUTF::CountUTF8(utf8Text + textBegin, textCount);
        if (codePointCount == 1) {  // single codepoint, fast path.
            cursors[textBegin] = clusterBox;
            continue;
        }

        float width = clusterBox.width() / codePointCount;
        SkASSERT(width > 0);
        const char* ptr = utf8Text + textBegin;
        const char* end = utf8Text + textEnd;
        float x = clusterBox.left();
        while (ptr < end) {  // for each codepoint in cluster
            const char* nextPtr = ptr;
            SkUTF::NextUTF8(&nextPtr, end);
            int firstIndex = ptr - utf8Text;
            float nextX = x + width;
            cursors[firstIndex] = SkRect{x, clusterBox.top(), nextX, clusterBox.bottom()};
            x = nextX;
            ptr = nextPtr;
        }
    }
}

static bool valid_utf8(const char* ptr, size_t size) { return SkUTF::CountUTF8(ptr, size) >= 0; }

void Editor::Shape(TextLine* line, SkShaper* shaper, float width, const SkFont& font,
                   SkRect space, const char* locale) {
    SkASSERT(line);
    SkASSERT(shaper);
    const StringSlice& text = line->fText;
    line->fCursorPos.resize(text.size() + 1);
    for (SkRect& c : line->fCursorPos) {
        c = kUnsetRect;
    }
    RunHandler runHandler(text.begin(), text.size());
    runHandler.setRunCallback(callback_fn, line->fCursorPos.data());
    if (text.size()) {
        // TODO: make use of locale in shaping.
        shaper->shape(text.begin(), text.size(), font, true, width, &runHandler);
        line->fLineEndOffsets = runHandler.lineEndOffsets();
        SkASSERT(line->fLineEndOffsets.size() > 0);
        line->fLineEndOffsets.pop_back();
    }
    SkRect& last = line->fCursorPos[text.size()];
    last = space;
    if (text.size() > 0) {
        last.offset(line->fCursorPos[text.size() - 1].fRight,
                    runHandler.yOffset());  // FIXME offset down.
    }
    float h = std::max(runHandler.endPoint().y(), font.getSpacing());
    line->fHeight = (int)ceilf(h);
    line->fBlob = runHandler.makeBlob();
    line->fWordBoundaries = GetUtf8WordBoundaries(text.begin(), text.end(), locale);
    line->fShaped = true;
}

// Kind of like Python's readlines(), but without any allocation.
// Calls f() on each line.
// F is [](const char*, size_t) -> void
template <typename F>
static void readlines(const void* data, size_t size, F f) {
    const char* start = (const char*)data;
    const char* end = start + size;
    const char* ptr = start;
    while (ptr < end) {
        while (*ptr++ != '\n' && ptr < end) {}
        size_t len = ptr - start;
        SkASSERT(len > 0);
        f(start, len);
        start = ptr;
    }
}

static const StringSlice remove_newline(const char* str, size_t len) {
    return SkASSERT((str != nullptr) || (len == 0)),
           StringSlice(str, (len > 0 && str[len - 1] == '\n') ? len - 1 : len);
}

void Editor::setFont(SkFont font) {
    if (font != fFont) {
        fFont = std::move(font);
        auto shaper = SkShaper::Make();
        const char kSpace[] = " ";
        TextLine textLine(StringSlice(kSpace, strlen(kSpace)));
        Editor::Shape(&textLine, shaper.get(), FLT_MAX, fFont, SkRect{0, 0, 0, 0}, fLocale);
        fSpaceBounds = textLine.fCursorPos[0];
        this->markAllDirty();
    }
}

static SkPoint to_point(SkIPoint p) { return {(float)p.x(), (float)p.y()}; }

Editor::TextPosition Editor::getPosition(SkIPoint xy) {
    Editor::TextPosition approximatePosition;
    this->reshapeAll();
    for (size_t j = 0; j < fLines.size(); ++j) {
        const TextLine& line = fLines[j];
        SkIRect lineRect = {0,
                            line.fOrigin.y(),
                            fWidth,
                            j + 1 < fLines.size() ? fLines[j + 1].fOrigin.y() : INT_MAX};
        if (const SkTextBlob* b = line.fBlob.get()) {
            SkIRect r = b->bounds().roundOut();
            r.offset(line.fOrigin);
            lineRect.join(r);
        }
        if (!lineRect.contains(xy.x(), xy.y())) {
            continue;
        }
        SkPoint pt = to_point(xy - line.fOrigin);
        const std::vector<SkRect>& pos = line.fCursorPos;
        for (size_t i = 0; i < pos.size(); ++i) {
            if (pos[i] != kUnsetRect && pos[i].contains(pt.x(), pt.y())) {
                return Editor::TextPosition{i, j};
            }
        }
        approximatePosition = {xy.x() <= line.fOrigin.x() ? 0 : line.fText.size(), j};
    }
    return approximatePosition;
}

static inline bool is_utf8_continuation(char v) {
    return ((unsigned char)v & 0b11000000) ==
                               0b10000000;
}

static const char* next_utf8(const char* p, const char* end) {
    if (p < end) {
        do {
            ++p;
        } while (p < end && is_utf8_continuation(*p));
    }
    return p;
}

static const char* align_utf8(const char* p, const char* begin) {
    while (p > begin && is_utf8_continuation(*p)) {
        --p;
    }
    return p;
}

static const char* prev_utf8(const char* p, const char* begin) {
    return p > begin ? align_utf8(p - 1, begin) : begin;
}

SkRect Editor::getLocation(Editor::TextPosition cursor) {
    this->reshapeAll();
    if (fLines.size() > 0) {
        const TextLine& cLine = fLines[cursor.fParagraphIndex];
        SkRect pos = fSpaceBounds;
        if (cursor.fTextByteIndex < cLine.fCursorPos.size()) {
            pos = cLine.fCursorPos[cursor.fTextByteIndex];
        }
        pos.fRight = pos.fLeft + 1;
        pos.fLeft -= 1;
        return offset(pos, cLine.fOrigin);
    }
    return SkRect{0, 0, 0, 0};
}

static size_t count_char(const StringSlice& string, char value) {
    size_t count = 0;
    for (char c : string) { if (c == value) { ++count; } }
    return count;
}

Editor::TextPosition Editor::insert(TextPosition pos, const char* utf8Text, size_t byteLen) {
    if (!valid_utf8(utf8Text, byteLen)) {
        return pos;
    }
    pos = this->move(Editor::Movement::kNowhere, pos);
    if (pos.fParagraphIndex < fLines.size()) {
        fLines[pos.fParagraphIndex].fText.insert(pos.fTextByteIndex, utf8Text, byteLen);
        this->markDirty(&fLines[pos.fParagraphIndex]);
    } else {
        SkASSERT(pos.fParagraphIndex == fLines.size());
        SkASSERT(pos.fTextByteIndex == 0);
        fLines.push_back(Editor::TextLine(StringSlice(utf8Text, byteLen)));
    }
    pos = Editor::TextPosition{pos.fTextByteIndex + byteLen, pos.fParagraphIndex};
    size_t newlinecount = count_char(fLines[pos.fParagraphIndex].fText, '\n');
    if (newlinecount > 0) {
        StringSlice src = std::move(fLines[pos.fParagraphIndex].fText);
        std::vector<TextLine>::const_iterator next = fLines.begin() + pos.fParagraphIndex + 1;
        fLines.insert(next, newlinecount, TextLine());
        TextLine* line = &fLines[pos.fParagraphIndex];
        readlines(src.begin(), src.size(), [&line](const char* str, size_t l) {
            (line++)->fText = remove_newline(str, l);
        });
    }
    fNeedsReshape = true;
    return pos;
}

Editor::TextPosition Editor::remove(TextPosition pos1, TextPosition pos2) {
    pos1 = this->move(Editor::Movement::kNowhere, pos1);
    pos2 = this->move(Editor::Movement::kNowhere, pos2);
    auto cmp = [](const Editor::TextPosition& u, const Editor::TextPosition& v) { return u < v; };
    Editor::TextPosition start = std::min(pos1, pos2, cmp);
    Editor::TextPosition end = std::max(pos1, pos2, cmp);
    if (start == end || start.fParagraphIndex == fLines.size()) {
        return start;
    }
    fNeedsReshape = true;
    if (start.fParagraphIndex == end.fParagraphIndex) {
        SkASSERT(end.fTextByteIndex > start.fTextByteIndex);
        fLines[start.fParagraphIndex].fText.remove(
                start.fTextByteIndex, end.fTextByteIndex - start.fTextByteIndex);
        this->markDirty(&fLines[start.fParagraphIndex]);
    } else {
        SkASSERT(end.fParagraphIndex < fLines.size());
        auto& line = fLines[start.fParagraphIndex];
        line.fText.remove(start.fTextByteIndex,
                          line.fText.size() - start.fTextByteIndex);
        line.fText.insert(start.fTextByteIndex,
                          fLines[end.fParagraphIndex].fText.begin() + end.fTextByteIndex,
                          fLines[end.fParagraphIndex].fText.size() - end.fTextByteIndex);
        this->markDirty(&line);
        fLines.erase(fLines.begin() + start.fParagraphIndex + 1,
                     fLines.begin() + end.fParagraphIndex + 1);
    }
    return start;
}

static void append(char** dst, size_t* count, const char* src, size_t n) {
    if (*dst) {
        ::memcpy(*dst, src, n);
        *dst += n;
    }
    *count += n;
}

size_t Editor::copy(TextPosition pos1, TextPosition pos2, char* dst) const {
    size_t size = 0;
    pos1 = this->move(Editor::Movement::kNowhere, pos1);
    pos2 = this->move(Editor::Movement::kNowhere, pos2);
    auto cmp = [](const Editor::TextPosition& u, const Editor::TextPosition& v) { return u < v; };
    Editor::TextPosition start = std::min(pos1, pos2, cmp);
    Editor::TextPosition end = std::max(pos1, pos2, cmp);
    if (start == end || start.fParagraphIndex == fLines.size()) {
        return size;
    }
    if (start.fParagraphIndex == end.fParagraphIndex) {
        SkASSERT(end.fTextByteIndex > start.fTextByteIndex);
        auto& str = fLines[start.fParagraphIndex].fText;
        append(&dst, &size, str.begin() + start.fTextByteIndex,
               end.fTextByteIndex - start.fTextByteIndex);
        return size;
    }
    SkASSERT(end.fParagraphIndex < fLines.size());
    const std::vector<TextLine>::const_iterator firstP = fLines.begin() + start.fParagraphIndex;
    const std::vector<TextLine>::const_iterator lastP  = fLines.begin() + end.fParagraphIndex;
    const auto& first = firstP->fText;
    const auto& last  = lastP->fText;

    append(&dst, &size, first.begin() + start.fTextByteIndex, first.size() - start.fTextByteIndex);
    for (auto line = firstP + 1; line < lastP; ++line) {
        append(&dst, &size, "\n", 1);
        append(&dst, &size, line->fText.begin(), line->fText.size());
    }
    append(&dst, &size, "\n", 1);
    append(&dst, &size, last.begin(), end.fTextByteIndex);
    return size;
}

static inline const char* begin(const StringSlice& s) { return s.begin(); }

static inline const char* end(const StringSlice& s) { return s.end(); }

static size_t align_column(const StringSlice& str, size_t p) {
    if (p >= str.size()) {
        return str.size();
    }
    return align_utf8(begin(str) + p, begin(str)) - begin(str);
}

// returns smallest i such that list[i] > value.  value > list[i-1]
// Use a binary search since list is monotonic
template <typename T>
static size_t find_first_larger(const std::vector<T>& list, T value) {
    return (size_t)(std::upper_bound(list.begin(), list.end(), value) - list.begin());
}

Editor::TextPosition Editor::move(Editor::Movement move, Editor::TextPosition pos) const {
    // First thing: fix possible bad values.
    if (pos.fParagraphIndex >= fLines.size()) {
        pos.fParagraphIndex = fLines.size();
        pos.fTextByteIndex = 0;
    } else {
        pos.fTextByteIndex = align_column(fLines[pos.fParagraphIndex].fText, pos.fTextByteIndex);
    }
    switch (move) {
        case Editor::Movement::kNowhere:
            break;
        case Editor::Movement::kLeft:
            if (0 == pos.fTextByteIndex) {
                if (pos.fParagraphIndex > 0) {
                    --pos.fParagraphIndex;
                    pos.fTextByteIndex = fLines[pos.fParagraphIndex].fText.size();
                }
            } else {
                const auto& str = fLines[pos.fParagraphIndex].fText;
                pos.fTextByteIndex =
                    prev_utf8(begin(str) + pos.fTextByteIndex, begin(str)) - begin(str);
            }
            break;
        case Editor::Movement::kRight:
            if (pos.fParagraphIndex == fLines.size()) {
                break;
            }
            if (fLines[pos.fParagraphIndex].fText.size() == pos.fTextByteIndex) {
                if (pos.fParagraphIndex + 1 < fLines.size()) {
                    ++pos.fParagraphIndex;
                    pos.fTextByteIndex = 0;
                }
            } else {
                const auto& str = fLines[pos.fParagraphIndex].fText;
                pos.fTextByteIndex =
                    next_utf8(begin(str) + pos.fTextByteIndex, end(str)) - begin(str);
            }
            break;
        case Editor::Movement::kHome:
            if (pos.fParagraphIndex < fLines.size()) {
                const std::vector<unsigned>& list = fLines[pos.fParagraphIndex].fLineEndOffsets;
                size_t f = find_first_larger(list, SkToUInt(pos.fTextByteIndex));
                pos.fTextByteIndex = f > 0 ? list[f - 1] : 0;
            }
            break;
        case Editor::Movement::kEnd:
            if (pos.fParagraphIndex < fLines.size()) {
                const std::vector<unsigned>& list = fLines[pos.fParagraphIndex].fLineEndOffsets;
                size_t f = find_first_larger(list, SkToUInt(pos.fTextByteIndex));
                if (f < list.size()) {
                    pos.fTextByteIndex = list[f] > 0 ? list[f] - 1 : 0;
                } else {
                    pos.fTextByteIndex = fLines[pos.fParagraphIndex].fText.size();
                }
            }
            break;
        case Editor::Movement::kUp:
            if (pos.fParagraphIndex < fLines.size()) {
                const std::vector<unsigned>& list = fLines[pos.fParagraphIndex].fLineEndOffsets;
                size_t f = find_first_larger(list, SkToUInt(pos.fTextByteIndex));
                // list[f] > value.  value > list[f-1]
                if (f > 0) {
                    // not the first line in paragraph.
                    pos.fTextByteIndex -= list[f-1];
                    if (f > 1) {
                        pos.fTextByteIndex += list[f-2];
                    }
                } else {
                    if (pos.fParagraphIndex > 0) {
                        --pos.fParagraphIndex;
                        size_t r = fLines[pos.fParagraphIndex].fLineEndOffsets.size();
                        if (r > 0) {
                            pos.fTextByteIndex +=
                                fLines[pos.fParagraphIndex].fLineEndOffsets[r - 1];
                        }
                    }
                }
                pos.fTextByteIndex =
                    align_column(fLines[pos.fParagraphIndex].fText, pos.fTextByteIndex);
            }
            break;
        case Editor::Movement::kDown:
            if (pos.fParagraphIndex < fLines.size()) {
                const std::vector<unsigned>& list = fLines[pos.fParagraphIndex].fLineEndOffsets;
                size_t f = find_first_larger(list, SkToUInt(pos.fTextByteIndex));
                if (f > 0) {
                    pos.fTextByteIndex -= list[f - 1];
                }
                if (f < list.size()) {
                    pos.fTextByteIndex += list[f];
                } else if (pos.fParagraphIndex + 1 < fLines.size()) {
                    ++pos.fParagraphIndex;
                } else {
                    pos.fTextByteIndex = fLines[pos.fParagraphIndex].fText.size();
                }
                pos.fTextByteIndex =
                    align_column(fLines[pos.fParagraphIndex].fText, pos.fTextByteIndex);
            }
            break;
        case Editor::Movement::kWordLeft:
            if (pos.fParagraphIndex < fLines.size()) {
                if (pos.fTextByteIndex == 0) {
                    pos = this->move(Editor::Movement::kLeft, pos);
                    break;
                }
                const std::vector<bool>& words = fLines[pos.fParagraphIndex].fWordBoundaries;
                SkASSERT(words.size() == fLines[pos.fParagraphIndex].fText.size());
                do {
                    --pos.fTextByteIndex;
                } while (pos.fTextByteIndex > 0 && !words[pos.fTextByteIndex]);
            }
            break;
        case Editor::Movement::kWordRight:
            if (pos.fParagraphIndex < fLines.size()) {
                const StringSlice& text = fLines[pos.fParagraphIndex].fText;
                if (pos.fTextByteIndex == text.size()) {
                    pos = this->move(Editor::Movement::kRight, pos);
                    break;
                }
                const std::vector<bool>& words = fLines[pos.fParagraphIndex].fWordBoundaries;
                SkASSERT(words.size() == text.size());
                do {
                    ++pos.fTextByteIndex;
                } while (pos.fTextByteIndex < text.size() && !words[pos.fTextByteIndex]);
            }
            break;

    }
    return pos;
}

void Editor::paint(SkCanvas* c, PaintOpts options) {
    this->reshapeAll();
    if (!c) {
        return;
    }

    c->drawPaint(SkPaint(options.fBackgroundColor));

    SkPaint selection = SkPaint(options.fSelectionColor);
    auto cmp = [](const Editor::TextPosition& u, const Editor::TextPosition& v) { return u < v; };
    for (TextPosition pos = std::min(options.fSelectionBegin, options.fSelectionEnd, cmp),
                      end = std::max(options.fSelectionBegin, options.fSelectionEnd, cmp);
         pos < end;
         pos = this->move(Editor::Movement::kRight, pos))
    {
        SkASSERT(pos.fParagraphIndex < fLines.size());
        const TextLine& l = fLines[pos.fParagraphIndex];
        c->drawRect(offset(l.fCursorPos[pos.fTextByteIndex], l.fOrigin), selection);
    }

    if (fLines.size() > 0) {
        c->drawRect(Editor::getLocation(options.fCursor), SkPaint(options.fCursorColor));
    }

    SkPaint foreground = SkPaint(options.fForegroundColor);
    for (const TextLine& line : fLines) {
        if (line.fBlob) {
            c->drawTextBlob(line.fBlob.get(), line.fOrigin.x(), line.fOrigin.y(), foreground);
        }
    }
}

void Editor::markDirty(TextLine* line) {
    line->fBlob = nullptr;
    line->fShaped = false;
    line->fWordBoundaries = std::vector<bool>();
    fNeedsReshape = true;
}

void Editor::reshapeAll() {
    if (fNeedsReshape) {
        float shape_width = (float)(fWidth);
        #ifdef SK_EDITOR_GO_FAST
        SkSemaphore semaphore;
        std::unique_ptr<SkExecutor> executor = SkExecutor::MakeFIFOThreadPool(100);
        int jobCount = 0;
        for (TextLine& line : fLines) {
            if (!line.fShaped) {
                executor->add([&]() {
                    Editor::Shape(&line, SkShaper::Make().get(), shape_width,
                                  fFont, fSpaceBounds, fLocale);
                    semaphore.signal();
                }
                ++jobCount;
            });
        }
        while (jobCount-- > 0) { semaphore.wait(); }
        #else
        auto shaper = SkShaper::Make();
        int i = 0;
        for (TextLine& line : fLines) {
            if (!line.fShaped) {
                #ifdef SK_EDITOR_DEBUG_OUT
                SkDebugf("shape %d: '%.*s'\n", i, line.fText.size(), line.fText.begin());
                #endif  // SK_EDITOR_DEBUG_OUT
                Editor::Shape(&line, shaper.get(), shape_width, fFont, fSpaceBounds, fLocale);
            }
            ++i;
        }
        #endif
        int y = 0;
        for (TextLine& line : fLines) {
            line.fOrigin = {0, y};
            y += line.fHeight;
        }
        fHeight = y;
        fNeedsReshape = false;
    }
}

void Editor::setWidth(int w) {
    if (fWidth != w) {
        fWidth = w;
        this->markAllDirty();
    }
}
