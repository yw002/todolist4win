#include "ui/controls/BodyEditor.h"

#include <algorithm>
#include <cmath>
#include <string_view>
#include <utility>

#include <d2d1_1helper.h>

#include "common/Sha256.h"
#include "ui/ClipboardImage.h"
#include "ui/Dpi.h"

namespace todo::ui {

namespace {

// Inset inside the field border. The caret and the first glyph share this origin, so the padding is
// also what a click at the very edge snaps to.
constexpr float kPaddingX = 10.0F;
constexpr float kPaddingY = 8.0F;
// Vertical gap between two blocks: enough to tell a paragraph from an image without wasting rows in
// the 120 DIP viewport.
constexpr float kBlockGap = 4.0F;
// §5.4 / §6.5: an image never renders taller than this; the rest is reached by scrolling.
constexpr float kMaxImageDisplayHeight = 180.0F;
constexpr float kScrollbarWidth = 3.0F;
constexpr float kScrollbarGap = 5.0F;
constexpr float kFallbackLineHeight = 20.0F;
constexpr std::size_t kMaxBlockUndo = 32;

[[nodiscard]] float ClampValue(float value, float low, float high) noexcept {
    return std::max(low, std::min(value, high));
}

[[nodiscard]] bool IsText(const EditorBlock& block) noexcept { return block.kind == todo::BodyBlockKind::Text; }

}  // namespace

BodyEditor::BodyEditor() {
    blocks_.push_back(EditorBlock{});
    PrepareTextInput(0);
}

void BodyEditor::PrepareTextInput(std::size_t index) {
    if (index >= blocks_.size()) return;
    EditorBlock& block = blocks_[index];
    if (!IsText(block)) return;
    block.input.SetMultiline(true);
    block.input.SetPlaceholder(L"");
    block.input.SetText(block.text);
    block.input.SetCaret(block.text.size());
    block.input.OnFocus(focused_ && index == activeBlock_);
    block.input.SetOnChanged([this, index](const std::wstring& value) {
        if (index >= blocks_.size()) return;
        blocks_[index].text = value;
        // A character edit belongs to TextBox's own undo stack; the block-level stack must not
        // consume the next Ctrl+Z.
        lastOperationWasBlockLevel_ = false;
        layoutDirty_ = true;
        EnsureVisible(index);
    });
}

void BodyEditor::PrepareAllTextInputs() {
    for (std::size_t index = 0; index < blocks_.size(); ++index) PrepareTextInput(index);
}

void BodyEditor::SetDocument(const todo::BodyDocument& document) {
    blocks_.clear();
    blocks_.reserve(document.blocks.size());
    for (const auto& source : document.blocks) {
        EditorBlock block;
        block.kind = source.kind;
        block.text = Dpi::Utf8ToWide(source.text);
        block.imageKey = source.image.key;
        block.imageWidth = source.image.width;
        block.imageHeight = source.image.height;
        block.rawJson = source.rawJson;
        blocks_.push_back(std::move(block));
    }
    if (blocks_.empty()) blocks_.push_back(EditorBlock{});
    PrepareAllTextInputs();
    // Caret at the beginning: EnsureVisible tracks the active caret, so leaving it at the end would
    // open a long note scrolled to its last line.
    for (auto& block : blocks_) if (IsText(block)) block.input.SetCaret(0);
    activeBlock_ = 0;
    scrollOffset_ = 0.0F;
    contentHeight_ = 0.0F;
    blockUndo_.clear();
    lastOperationWasBlockLevel_ = false;
    layoutDirty_ = true;
    layoutWidth_ = 0.0F;
    editorImageError_.clear();
    pendingError_.clear();
    ClampScroll();
}

void BodyEditor::SyncTextFromInputs() {
    for (auto& block : blocks_) {
        if (IsText(block)) block.text = block.input.Text();
    }
}

void BodyEditor::SyncInputsFromText() {
    for (auto& block : blocks_) {
        if (!IsText(block)) continue;
        block.input.SetText(block.text);
        block.input.SetCaret(block.text.size());
    }
}

todo::BodyDocument BodyEditor::GetDocument() const {
    const_cast<BodyEditor*>(this)->SyncTextFromInputs();
    todo::BodyDocument document;
    document.version = todo::kBodyDocVersion;
    document.legacy = false;
    document.blocks.reserve(blocks_.size());
    for (const auto& block : blocks_) {
        todo::BodyBlock out;
        out.kind = block.kind;
        out.rawJson = block.rawJson;
        if (block.kind == todo::BodyBlockKind::Text) {
            out.text = Dpi::WideToUtf8(block.text);
        } else if (block.kind == todo::BodyBlockKind::Image) {
            out.image.key = block.imageKey;
            out.image.mime = "image/png";
            out.image.width = block.imageWidth;
            out.image.height = block.imageHeight;
        }
        document.blocks.push_back(std::move(out));
    }
    return document;
}

std::vector<todo::BodyImage> BodyEditor::GetNewImages() const { return newImages_; }

void BodyEditor::SetImages(const std::vector<todo::BodyImage>& images) {
    cache_.SetImages(images);
    // Images pasted in this session are not in the database yet; the async load that just replaced
    // the cache must not evict them or the block falls back to the "unavailable" placeholder.
    for (const auto& image : newImages_) cache_.AddImage(image);
}

void BodyEditor::SetEditorImageError(std::string message) { editorImageError_ = std::move(message); }

std::string BodyEditor::TakeError() {
    std::string message = std::move(pendingError_);
    pendingError_.clear();
    return message;
}

TextBox* BodyEditor::ActiveTextBox() noexcept {
    if (!focused_ || activeBlock_ >= blocks_.size()) return nullptr;
    EditorBlock& block = blocks_[activeBlock_];
    return IsText(block) ? &block.input : nullptr;
}

void BodyEditor::Clear() noexcept {
    blocks_.clear();
    blocks_.push_back(EditorBlock{});
    PrepareTextInput(0);
    activeBlock_ = 0;
    scrollOffset_ = 0.0F;
    contentHeight_ = 0.0F;
    blockUndo_.clear();
    lastOperationWasBlockLevel_ = false;
    layoutDirty_ = true;
    layoutWidth_ = 0.0F;
    newImages_.clear();
    cache_.Clear();
    editorImageError_.clear();
    pendingError_.clear();
    draggingScrollbar_ = false;
    Control::OnFocus(false);
}

void BodyEditor::OnFocus(bool focused) {
    Control::OnFocus(focused);
    for (std::size_t index = 0; index < blocks_.size(); ++index) {
        if (!IsText(blocks_[index])) continue;
        blocks_[index].input.OnFocus(focused && index == activeBlock_);
    }
}

bool BodyEditor::HitTest(float x, float y) const noexcept { return Control::HitTest(x, y); }

D2D1_RECT_F BodyEditor::ContentRect() const noexcept {
    const float right = bounds_.right - kPaddingX - kScrollbarWidth - kScrollbarGap;
    return D2D1::RectF(bounds_.left + kPaddingX, bounds_.top + kPaddingY,
                       std::max(bounds_.left + kPaddingX + 1.0F, right), bounds_.bottom - kPaddingY);
}

D2D1_RECT_F BodyEditor::Viewport() const noexcept { return ContentRect(); }

void BodyEditor::RebuildLayouts(RenderResources& resources) {
    const D2D1_RECT_F content = ContentRect();
    const float width = std::max(1.0F, content.right - content.left);
    layoutWidth_ = width;

    float y = 0.0F;
    for (auto& block : blocks_) {
        block.top = y;
        // Unknown blocks are preserved byte-for-byte for a newer schema, but there is nothing to
        // render or edit here; reserve one line so the document does not collapse.
        if (block.kind == todo::BodyBlockKind::Text) {
            block.layout = resources.CreateBodyLayout(block.text, width, 8192.0F);
            float height = kFallbackLineHeight;
            if (block.layout) {
                DWRITE_TEXT_METRICS metrics{};
                if (SUCCEEDED(block.layout->GetMetrics(&metrics)) && metrics.height > 0.0F) {
                    height = metrics.height;
                }
            }
            block.heightDip = std::max(height, 14.0F);
        } else if (block.kind == todo::BodyBlockKind::Image) {
            const float sourceWidth = block.imageWidth > 0 ? static_cast<float>(block.imageWidth) : 320.0F;
            const float sourceHeight = block.imageHeight > 0 ? static_cast<float>(block.imageHeight) : 180.0F;
            const float scale = std::min(width / sourceWidth, kMaxImageDisplayHeight / sourceHeight);
            block.imageDrawnWidth = sourceWidth * scale;
            block.imageDrawnHeight = sourceHeight * scale;
            block.heightDip = std::max(block.imageDrawnHeight, 24.0F);
        } else {
            block.layout.Reset();
            block.heightDip = kFallbackLineHeight;
        }
        y += block.heightDip + kBlockGap;
    }
    contentHeight_ = blocks_.empty() ? 0.0F : std::max(0.0F, y - kBlockGap);
    layoutDirty_ = false;
}

void BodyEditor::RecomputeContentHeight(RenderResources& resources) { RebuildLayouts(resources); }

void BodyEditor::ClampScroll() noexcept {
    const D2D1_RECT_F view = Viewport();
    const float viewportHeight = std::max(0.0F, view.bottom - view.top);
    const float maxScroll = std::max(0.0F, contentHeight_ - viewportHeight);
    scrollOffset_ = ClampValue(scrollOffset_, 0.0F, maxScroll);
}

void BodyEditor::EnsureVisible(std::size_t blockIndex) {
    if (blockIndex >= blocks_.size()) return;
    const D2D1_RECT_F view = Viewport();
    const float viewportHeight = std::max(0.0F, view.bottom - view.top);
    const EditorBlock& block = blocks_[blockIndex];

    // A caret must stay visible even when its paragraph is taller than the viewport, so for the active
    // text block scroll to the caret's LINE, not to the whole block rect.
    if (IsText(block) && blockIndex == activeBlock_ && block.layout) {
        float caretX = 0.0F;
        float caretY = 0.0F;
        DWRITE_HIT_TEST_METRICS metrics{};
        const std::size_t caret = std::min(block.input.Caret(), block.text.size());
        if (SUCCEEDED(block.layout->HitTestTextPosition(static_cast<UINT32>(caret), FALSE, &caretX, &caretY, &metrics))) {
            const float lineHeight = metrics.height > 0.0F ? metrics.height : kFallbackLineHeight;
            const float lineTop = block.top + caretY;
            const float lineBottom = lineTop + lineHeight;
            if (lineTop < scrollOffset_) scrollOffset_ = lineTop;
            else if (lineBottom > scrollOffset_ + viewportHeight) scrollOffset_ = lineBottom - viewportHeight;
            ClampScroll();
            return;
        }
    }

    const float top = block.top;
    const float bottom = block.top + block.heightDip;
    if (block.heightDip > viewportHeight) {
        // A picture taller than the viewport cannot fit; anchor its top instead of its bottom so the
        // user starts at the first visible row.
        if (top < scrollOffset_ || top > scrollOffset_ + viewportHeight) scrollOffset_ = top;
    } else if (top < scrollOffset_) {
        scrollOffset_ = top;
    } else if (bottom > scrollOffset_ + viewportHeight) {
        scrollOffset_ = bottom - viewportHeight;
    }
    ClampScroll();
}

void BodyEditor::ActivateBlock(std::size_t index, bool caretAtEnd) {
    if (index >= blocks_.size()) return;
    if (activeBlock_ < blocks_.size() && IsText(blocks_[activeBlock_])) {
        blocks_[activeBlock_].input.OnFocus(false);
    }
    activeBlock_ = index;
    EditorBlock& block = blocks_[index];
    if (IsText(block)) {
        block.input.OnFocus(focused_);
        if (caretAtEnd) {
            block.input.SetCaret(block.text.size());
        } else {
            block.input.SetCaret(std::min(block.input.Caret(), block.text.size()));
        }
    }
    EnsureVisible(index);
    layoutDirty_ = true;
}

bool BodyEditor::OnMouseDown(float x, float y, UINT keyFlags) {
    (void)keyFlags;
    if (!visible_) return false;
    if (!enabled_ || !Control::HitTest(x, y)) return false;
    hovered_ = true;

    const D2D1_RECT_F view = Viewport();
    const float viewportHeight = std::max(0.0F, view.bottom - view.top);
    const float scrollbarLeft = bounds_.right - kPaddingX - kScrollbarWidth - kScrollbarGap;
    if (contentHeight_ > viewportHeight && x >= scrollbarLeft) {
        draggingScrollbar_ = true;
        dragStartY_ = y;
        dragStartOffset_ = scrollOffset_;
        return true;
    }

    if (paintResources_ && (layoutDirty_ || std::abs(layoutWidth_ - (view.right - view.left)) > 0.5F)) {
        RebuildLayouts(*paintResources_);
    }

    const float contentY = y - view.top + scrollOffset_;
    std::size_t hitIndex = blocks_.size();
    for (std::size_t index = 0; index < blocks_.size(); ++index) {
        if (contentY >= blocks_[index].top && contentY <= blocks_[index].top + blocks_[index].heightDip) {
            hitIndex = index;
            break;
        }
    }
    if (hitIndex >= blocks_.size()) {
        // Click below the last block: keep typing at the end of the final text block.
        for (std::size_t index = blocks_.size(); index-- > 0;) {
            if (!IsText(blocks_[index])) continue;
            ActivateBlock(index, true);
            return true;
        }
        return true;
    }

    if (IsText(blocks_[hitIndex])) {
        ActivateBlock(hitIndex, false);
        EditorBlock& block = blocks_[hitIndex];
        if (block.layout) {
            BOOL trailing = FALSE;
            BOOL inside = FALSE;
            DWRITE_HIT_TEST_METRICS metrics{};
            if (SUCCEEDED(block.layout->HitTestPoint(x - view.left, contentY - block.top, &trailing, &inside, &metrics))) {
                const std::size_t caret = static_cast<std::size_t>(metrics.textPosition) + (trailing ? 1u : 0u);
                block.input.SetCaret(std::min(caret, block.text.size()));
            }
        }
        return true;
    }

    ActivateBlock(hitIndex, false);
    return true;
}

bool BodyEditor::OnMouseMove(float x, float y) {
    const bool changed = Control::OnMouseMove(x, y);
    if (!draggingScrollbar_) return changed;
    const D2D1_RECT_F view = Viewport();
    const float viewportHeight = std::max(0.0F, view.bottom - view.top);
    const float maxScroll = std::max(0.0F, contentHeight_ - viewportHeight);
    if (maxScroll <= 0.0F || viewportHeight <= 0.0F) return true;
    const float visibleRatio = std::min(1.0F, viewportHeight / std::max(contentHeight_, 1.0F));
    const float thumbHeight = std::max(24.0F, viewportHeight * visibleRatio);
    const float travel = std::max(1.0F, viewportHeight - thumbHeight);
    scrollOffset_ = ClampValue(dragStartOffset_ + (y - dragStartY_) / travel * maxScroll, 0.0F, maxScroll);
    return true;
}

bool BodyEditor::OnMouseUp(float x, float y) {
    (void)x;
    (void)y;
    if (draggingScrollbar_) {
        draggingScrollbar_ = false;
        return true;
    }
    return false;
}

bool BodyEditor::OnMouseWheel(float x, float y, int delta) {
    if (!visible_ || !Control::HitTest(x, y)) return false;
    const D2D1_RECT_F view = Viewport();
    const float viewportHeight = std::max(0.0F, view.bottom - view.top);
    if (contentHeight_ <= viewportHeight) return false;
    scrollOffset_ -= static_cast<float>(delta) / static_cast<float>(WHEEL_DELTA) * 48.0F;
    ClampScroll();
    return true;
}

bool BodyEditor::OnChar(wchar_t value) {
    if (!focused_) return false;
    if (TextBox* input = ActiveTextBox()) return input->OnChar(value);
    return false;
}

void BodyEditor::PushBlockUndo() {
    blockUndo_.push_back(GetDocument());
    if (blockUndo_.size() > kMaxBlockUndo) blockUndo_.erase(blockUndo_.begin());
    lastOperationWasBlockLevel_ = true;
}

bool BodyEditor::HandleBlockUndo() {
    if (!lastOperationWasBlockLevel_ || blockUndo_.empty()) return false;
    todo::BodyDocument snapshot = std::move(blockUndo_.back());
    blockUndo_.pop_back();
    // SetDocument resets the per-document undo stack; keep the remaining snapshots so repeated
    // Ctrl+Z keeps walking back through block-level operations.
    std::vector<todo::BodyDocument> remaining = std::move(blockUndo_);
    SetDocument(snapshot);
    blockUndo_ = std::move(remaining);
    lastOperationWasBlockLevel_ = false;
    return true;
}

bool BodyEditor::HandlePaste(bool forceText) {
    const ClipboardSnapshot snapshot = ReadClipboard(forceText);
    if (snapshot.error.code != ErrorCode::None) {
        pendingError_ = snapshot.error.message;
        return true;
    }
    if (!snapshot.hasImage) {
        if (!snapshot.text.empty()) InsertTextAtCaret(snapshot.text);
        return true;
    }

    if (snapshot.png.size() > todo::kMaxImageBytes) {
        pendingError_ = "图片超过 4 MB，请压缩后再粘贴";
        return true;
    }
    if (snapshot.width == 0 || snapshot.height == 0 ||
        snapshot.width > todo::kMaxImageDimension || snapshot.height > todo::kMaxImageDimension) {
        pendingError_ = "图片尺寸过大，最多 8192 × 8192";
        return true;
    }
    if (static_cast<std::uint64_t>(snapshot.width) * snapshot.height > todo::kMaxImagePixels) {
        pendingError_ = "图片像素数超过 3200 万，请压缩后再粘贴";
        return true;
    }
    std::size_t imageBlocks = 0;
    for (const auto& block : blocks_) if (block.kind == todo::BodyBlockKind::Image) ++imageBlocks;
    if (imageBlocks + 1 > todo::kMaxImagesPerTask) {
        pendingError_ = "单个任务最多 " + std::to_string(todo::kMaxImagesPerTask) + " 张图片";
        return true;
    }
    std::size_t totalBytes = snapshot.png.size();
    for (const auto& image : newImages_) totalBytes += image.bytes.size();
    if (totalBytes > todo::kMaxTaskImageBytes) {
        pendingError_ = "单个任务的图片总大小超过 16 MB";
        return true;
    }

    const std::string key = Sha256::Hex(
        std::string_view(reinterpret_cast<const char*>(snapshot.png.data()), snapshot.png.size()));
    const bool already = std::any_of(newImages_.begin(), newImages_.end(),
                                     [&key](const todo::BodyImage& image) { return image.key == key; });
    if (!already) {
        todo::BodyImage image;
        image.key = key;
        image.mime = "image/png";
        image.width = snapshot.width;
        image.height = snapshot.height;
        image.bytes.resize(snapshot.png.size());
        std::transform(snapshot.png.begin(), snapshot.png.end(), image.bytes.begin(),
                       [](std::uint8_t value) { return static_cast<std::byte>(value); });
        newImages_.push_back(std::move(image));
        cache_.AddImage(newImages_.back());
    }

    PushBlockUndo();
    SplitForImage(key, snapshot.width, snapshot.height);
    return true;
}

void BodyEditor::InsertTextAtCaret(const std::wstring& text) {
    if (text.empty()) return;
    TextBox* input = ActiveTextBox();
    if (!input) {
        // The selected block is an image: give the pasted text its own block underneath it.
        const std::size_t insertAt = std::min(activeBlock_ + 1, blocks_.size());
        EditorBlock tail;
        tail.text = text;
        blocks_.insert(blocks_.begin() + static_cast<std::ptrdiff_t>(insertAt), std::move(tail));
        PrepareAllTextInputs();
        activeBlock_ = insertAt;
        if (activeBlock_ < blocks_.size()) blocks_[activeBlock_].input.OnFocus(focused_);
        layoutDirty_ = true;
        EnsureVisible(activeBlock_);
        return;
    }
    input->InsertText(text);
    layoutDirty_ = true;
}

void BodyEditor::SplitForImage(const std::string& key, std::uint32_t width, std::uint32_t height) {
    EditorBlock image;
    image.kind = todo::BodyBlockKind::Image;
    image.imageKey = key;
    image.imageWidth = width;
    image.imageHeight = height;

    if (blocks_.empty()) blocks_.push_back(EditorBlock{});
    if (activeBlock_ >= blocks_.size()) activeBlock_ = blocks_.size() - 1;

    if (!IsText(blocks_[activeBlock_])) {
        const std::size_t insertAt = activeBlock_ + 1;
        blocks_.insert(blocks_.begin() + static_cast<std::ptrdiff_t>(insertAt), std::move(image));
        blocks_.insert(blocks_.begin() + static_cast<std::ptrdiff_t>(insertAt + 1), EditorBlock{});
        activeBlock_ = insertAt + 1;
        PrepareAllTextInputs();
    } else {
        EditorBlock& current = blocks_[activeBlock_];
        const std::size_t caret = std::min(current.input.Caret(), current.text.size());
        const std::wstring head = current.text.substr(0, caret);
        const std::wstring tail = current.text.substr(caret);
        const std::size_t index = activeBlock_;
        if (head.empty()) {
            // "前半为空则丢弃该文本块"; the image takes its slot and the tail keeps an empty block.
            blocks_.erase(blocks_.begin() + static_cast<std::ptrdiff_t>(index));
            blocks_.insert(blocks_.begin() + static_cast<std::ptrdiff_t>(index), std::move(image));
            EditorBlock tailBlock;
            tailBlock.text = tail;
            blocks_.insert(blocks_.begin() + static_cast<std::ptrdiff_t>(index + 1), std::move(tailBlock));
            activeBlock_ = index + 1;
            PrepareAllTextInputs();
            blocks_[activeBlock_].input.SetCaret(0);
        } else {
            current.text = head;
            EditorBlock tailBlock;
            tailBlock.text = tail;
            blocks_.insert(blocks_.begin() + static_cast<std::ptrdiff_t>(index + 1), std::move(image));
            blocks_.insert(blocks_.begin() + static_cast<std::ptrdiff_t>(index + 2), std::move(tailBlock));
            activeBlock_ = index + 2;
            PrepareAllTextInputs();
            // The insertion point sits right after the pasted picture, at the start of the tail.
            blocks_[activeBlock_].input.SetCaret(0);
        }
    }
    lastOperationWasBlockLevel_ = true;
    layoutDirty_ = true;
    ClampScroll();
    EnsureVisible(activeBlock_);
}

void BodyEditor::DeleteSelectedImageBlock() {
    if (blocks_.empty() || activeBlock_ >= blocks_.size()) return;
    if (blocks_[activeBlock_].kind != todo::BodyBlockKind::Image) return;

    PushBlockUndo();
    const std::size_t index = activeBlock_;
    blocks_.erase(blocks_.begin() + static_cast<std::ptrdiff_t>(index));
    if (blocks_.empty()) blocks_.push_back(EditorBlock{});
    // §6.3: two text neighbours are joined with a single newline so the removed boundary stays
    // visible; otherwise the blocks stay separate.
    if (index > 0 && index < blocks_.size() && IsText(blocks_[index - 1]) && IsText(blocks_[index])) {
        std::wstring& left = blocks_[index - 1].text;
        const std::wstring& right = blocks_[index].text;
        if (!left.empty() && !right.empty()) left += L"\n";
        left += right;
        blocks_.erase(blocks_.begin() + static_cast<std::ptrdiff_t>(index));
    }
    activeBlock_ = std::min(index > 0 ? index - 1 : 0, blocks_.size() - 1);
    while (activeBlock_ > 0 && !IsText(blocks_[activeBlock_])) --activeBlock_;
    PrepareAllTextInputs();
    if (IsText(blocks_[activeBlock_])) {
        blocks_[activeBlock_].input.SetCaret(blocks_[activeBlock_].text.size());
    }
    lastOperationWasBlockLevel_ = true;
    layoutDirty_ = true;
    EnsureVisible(activeBlock_);
}

bool BodyEditor::MergeWithPreviousBlock() {
    if (activeBlock_ == 0 || activeBlock_ >= blocks_.size()) return false;
    PushBlockUndo();
    const std::size_t current = activeBlock_;
    const std::size_t previous = current - 1;
    if (blocks_[previous].kind == todo::BodyBlockKind::Image) {
        blocks_.erase(blocks_.begin() + static_cast<std::ptrdiff_t>(previous));
        activeBlock_ = previous;
        PrepareAllTextInputs();
    } else {
        const std::size_t boundary = blocks_[previous].text.size();
        blocks_[previous].text += blocks_[current].text;
        blocks_.erase(blocks_.begin() + static_cast<std::ptrdiff_t>(current));
        activeBlock_ = previous;
        PrepareAllTextInputs();
        blocks_[activeBlock_].input.SetCaret(boundary);
    }
    lastOperationWasBlockLevel_ = true;
    layoutDirty_ = true;
    EnsureVisible(activeBlock_);
    return true;
}

bool BodyEditor::MergeWithNextBlock() {
    if (activeBlock_ + 1 >= blocks_.size()) return false;
    PushBlockUndo();
    const std::size_t next = activeBlock_ + 1;
    if (blocks_[next].kind == todo::BodyBlockKind::Image) {
        blocks_.erase(blocks_.begin() + static_cast<std::ptrdiff_t>(next));
        // The caret stays in the current text block.
        PrepareAllTextInputs();
        blocks_[activeBlock_].input.SetCaret(blocks_[activeBlock_].text.size());
    } else {
        const std::size_t boundary = blocks_[activeBlock_].text.size();
        blocks_[activeBlock_].text += blocks_[next].text;
        blocks_.erase(blocks_.begin() + static_cast<std::ptrdiff_t>(next));
        PrepareAllTextInputs();
        blocks_[activeBlock_].input.SetCaret(boundary);
    }
    lastOperationWasBlockLevel_ = true;
    layoutDirty_ = true;
    EnsureVisible(activeBlock_);
    return true;
}

bool BodyEditor::MoveCaretHorizontally(bool forward) {
    if (blocks_.empty()) return false;
    if (forward) {
        if (activeBlock_ + 1 >= blocks_.size()) return false;
        ActivateBlock(activeBlock_ + 1, false);
    } else {
        if (activeBlock_ == 0) return false;
        ActivateBlock(activeBlock_ - 1, true);
    }
    return true;
}

bool BodyEditor::MoveCaretVertically(bool up) {
    if (blocks_.empty() || !IsText(blocks_[activeBlock_])) return false;
    const std::size_t line = CaretLineNumber();
    const std::size_t lines = LineCount(activeBlock_);
    if (up && line == 0) return MoveCaretHorizontally(false);
    if (!up && line + 1 >= lines) return MoveCaretHorizontally(true);
    return false;
}

std::size_t BodyEditor::CaretLineNumber() const {
    if (activeBlock_ >= blocks_.size() || !IsText(blocks_[activeBlock_])) return 0;
    const EditorBlock& block = blocks_[activeBlock_];
    if (!block.layout) return 0;
    float pointX = 0.0F;
    float pointY = 0.0F;
    DWRITE_HIT_TEST_METRICS hit{};
    const std::size_t caret = std::min(block.input.Caret(), block.text.size());
    if (FAILED(block.layout->HitTestTextPosition(static_cast<UINT32>(caret), FALSE, &pointX, &pointY, &hit))) return 0;

    UINT32 count = 0;
    if (FAILED(block.layout->GetLineMetrics(nullptr, 0, &count)) || count == 0) return 0;
    std::vector<DWRITE_LINE_METRICS> lines(count);
    UINT32 actual = 0;
    if (FAILED(block.layout->GetLineMetrics(lines.data(), count, &actual))) return 0;
    float top = 0.0F;
    for (UINT32 index = 0; index < actual; ++index) {
        if (pointY < top + lines[index].height) return index;
        top += lines[index].height;
    }
    return actual > 0 ? actual - 1 : 0;
}

std::size_t BodyEditor::LineCount(std::size_t blockIndex) const {
    if (blockIndex >= blocks_.size() || !IsText(blocks_[blockIndex])) return 1;
    const EditorBlock& block = blocks_[blockIndex];
    if (!block.layout) return 1;
    UINT32 count = 0;
    if (FAILED(block.layout->GetLineMetrics(nullptr, 0, &count)) || count == 0) return 1;
    return count;
}

std::size_t BodyEditor::TextBlockCount() const noexcept {
    std::size_t count = 0;
    for (const auto& block : blocks_) if (IsText(block)) ++count;
    return count;
}

bool BodyEditor::OnKeyDown(UINT key, bool ctrl, bool shift) {
    if (!visible_ || !focused_ || blocks_.empty()) return false;
    if (ctrl && key == 'V') return HandlePaste(shift);
    if (ctrl && key == 'Z') return HandleBlockUndo();

    const D2D1_RECT_F view = Viewport();
    const float page = std::max(24.0F, view.bottom - view.top - kFallbackLineHeight);
    if (key == VK_PRIOR) {
        scrollOffset_ -= page;
        ClampScroll();
        return true;
    }
    if (key == VK_NEXT) {
        scrollOffset_ += page;
        ClampScroll();
        return true;
    }
    if (key == VK_HOME) {
        if (TextBox* input = ActiveTextBox()) {
            input->SetCaret(0);
            layoutDirty_ = true;
            return true;
        }
        return false;
    }
    if (key == VK_END) {
        if (TextBox* input = ActiveTextBox()) {
            input->SetCaret(input->Text().size());
            layoutDirty_ = true;
            EnsureVisible(activeBlock_);
            return true;
        }
        return false;
    }

    EditorBlock* block = activeBlock_ < blocks_.size() ? &blocks_[activeBlock_] : nullptr;
    if (!block) return false;
    if (!IsText(*block)) {
        // Image block selected: all editing keys operate on the block itself.
        if (key == VK_BACK || key == VK_DELETE) {
            DeleteSelectedImageBlock();
            return true;
        }
        if (key == VK_LEFT) return MoveCaretHorizontally(false);
        if (key == VK_RIGHT) return MoveCaretHorizontally(true);
        if (key == VK_UP) return MoveCaretHorizontally(false);
        if (key == VK_DOWN) return MoveCaretHorizontally(true);
        if (key == VK_RETURN) {
            PushBlockUndo();
            blocks_.insert(blocks_.begin() + static_cast<std::ptrdiff_t>(activeBlock_ + 1), EditorBlock{});
            PrepareAllTextInputs();
            ++activeBlock_;
            blocks_[activeBlock_].input.OnFocus(true);
            layoutDirty_ = true;
            EnsureVisible(activeBlock_);
            return true;
        }
        return false;
    }

    const std::size_t caret = block->input.Caret();
    if (key == VK_LEFT && !shift && caret == 0) return MoveCaretHorizontally(false);
    if (key == VK_RIGHT && !shift && caret == block->text.size()) return MoveCaretHorizontally(true);
    if (key == VK_BACK && !shift && caret == 0) return MergeWithPreviousBlock();
    if (key == VK_DELETE && !shift && caret == block->text.size()) return MergeWithNextBlock();
    if (key == VK_UP && !shift && MoveCaretVertically(true)) return true;
    if (key == VK_DOWN && !shift && MoveCaretVertically(false)) return true;

    const bool handled = block->input.OnKeyDown(key, ctrl, shift);
    if (handled) {
        layoutDirty_ = true;
        EnsureVisible(activeBlock_);
    }
    return handled;
}

void BodyEditor::Paint(ID2D1DeviceContext& context, RenderResources& resources) {
    if (!visible_) return;
    paintResources_ = &resources;

    const bool hover = enabled_ && hovered_ && !focused_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> background;
    D2D1_COLOR_F color = resources.Colors().panelStrong;
    if (focused_ || hover) {
        color.r = color.r * 0.94F + 0.06F;
        color.g = color.g * 0.94F + 0.06F;
        color.b = color.b * 0.94F + 0.06F;
    }
    context.CreateSolidColorBrush(color, &background);
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> hoverBorder;
    if (hover) {
        D2D1_COLOR_F borderColor = resources.Colors().accent;
        borderColor.a = 0.35F;
        context.CreateSolidColorBrush(borderColor, &hoverBorder);
    }
    resources.DrawRoundRect(bounds_, 9.0F, background.Get(),
                            focused_ ? resources.AccentBrush() : hover ? hoverBorder.Get() : resources.BorderBrush(),
                            focused_ ? 1.5F : hover ? 1.3F : 1.0F);

    const D2D1_RECT_F view = Viewport();
    // Layouts only depend on the text and the available width; rebuilding them every frame would
    // allocate a DWrite layout per block 60 times a second.
    if (layoutDirty_ || std::abs(layoutWidth_ - (view.right - view.left)) > 0.5F) RebuildLayouts(resources);
    ClampScroll();
    EnsureVisible(activeBlock_);
    ClampScroll();

    const float top = view.top - scrollOffset_;
    context.PushAxisAlignedClip(view, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    for (std::size_t index = 0; index < blocks_.size(); ++index) {
        EditorBlock& block = blocks_[index];
        const float blockTop = top + block.top;
        if (blockTop + block.heightDip < view.top - 1.0F || blockTop > view.bottom + 1.0F) continue;
        if (IsText(block)) {
            if (block.layout) {
                resources.DrawTextLayout(D2D1::Point2F(view.left, blockTop), block.layout.Get(), resources.TextBrush());
            }
        } else if (block.kind == todo::BodyBlockKind::Image) {
            const D2D1_RECT_F rect = D2D1::RectF(view.left, blockTop, view.left + block.imageDrawnWidth,
                                                 blockTop + block.imageDrawnHeight);
            if (!cache_.Paint(context, resources, block.imageKey, rect)) DrawPlaceholder(context, resources, rect);
            if (activeBlock_ == index) {
                Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> accent;
                D2D1_COLOR_F accentColor = resources.Colors().accent;
                accentColor.a = 0.9F;
                if (SUCCEEDED(context.CreateSolidColorBrush(accentColor, &accent))) {
                    resources.DrawRoundRect(rect, 4.0F, nullptr, accent.Get(), 1.6F);
                }
            }
        } else {
            resources.DrawText(L"〔不支持的内容〕", D2D1::RectF(view.left, blockTop, view.right, blockTop + kFallbackLineHeight),
                               resources.MutedBrush(), false, true);
        }
    }

    const bool emptyDocument = blocks_.size() == 1 && IsText(blocks_[0]) && blocks_[0].text.empty();
    if (emptyDocument) {
        resources.DrawText(placeholder_, D2D1::RectF(view.left, view.top, view.right, view.top + kFallbackLineHeight),
                           resources.MutedBrush());
    }

    if (focused_ && activeBlock_ < blocks_.size() && IsText(blocks_[activeBlock_])) {
        EditorBlock& block = blocks_[activeBlock_];
        float caretX = 0.0F;
        float caretY = 0.0F;
        float lineHeight = kFallbackLineHeight;
        const std::size_t caret = std::min(block.input.Caret(), block.text.size());
        if (block.layout) {
            DWRITE_HIT_TEST_METRICS metrics{};
            if (SUCCEEDED(block.layout->HitTestTextPosition(static_cast<UINT32>(caret), FALSE, &caretX, &caretY, &metrics))) {
                if (metrics.height > 0.0F) lineHeight = metrics.height;
            }
        }
        const float caretTop = top + block.top + caretY;
        if (caretTop + lineHeight >= view.top && caretTop <= view.bottom) {
            context.DrawLine(D2D1::Point2F(view.left + caretX, caretTop),
                             D2D1::Point2F(view.left + caretX, caretTop + lineHeight),
                             resources.AccentBrush(), 1.0F);
        }
        // The inner TextBox owns IME; pointing it at the caret line is what places the composition
        // window next to the text instead of in the dialog's top-left corner.
        block.input.SetBounds(D2D1::RectF(view.left + caretX, caretTop, view.left + caretX + 1.0F, caretTop + lineHeight));
    }

    context.PopAxisAlignedClip();

    const float viewportHeight = std::max(0.0F, view.bottom - view.top);
    if (contentHeight_ > viewportHeight) DrawScrollbar(context, resources);
}

void BodyEditor::DrawScrollbar(ID2D1DeviceContext& context, RenderResources& resources) const {
    const D2D1_RECT_F view = Viewport();
    const float viewportHeight = std::max(0.0F, view.bottom - view.top);
    if (viewportHeight <= 0.0F) return;
    const float trackLeft = bounds_.right - kPaddingX - kScrollbarWidth;
    const float visibleRatio = std::min(1.0F, viewportHeight / std::max(contentHeight_, 1.0F));
    const float thumbHeight = std::max(24.0F, viewportHeight * visibleRatio);
    const float maxScroll = std::max(0.0F, contentHeight_ - viewportHeight);
    const float travel = std::max(1.0F, viewportHeight - thumbHeight);
    const float thumbTop = view.top + (maxScroll > 0.0F ? scrollOffset_ / maxScroll * travel : 0.0F);

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> thumb;
    D2D1_COLOR_F color = resources.Colors().muted;
    color.a = 0.35F;
    if (FAILED(context.CreateSolidColorBrush(color, &thumb))) return;
    resources.DrawRoundRect(D2D1::RectF(trackLeft, thumbTop, trackLeft + kScrollbarWidth, thumbTop + thumbHeight),
                            kScrollbarWidth * 0.5F, thumb.Get());
}

void BodyEditor::DrawPlaceholder(ID2D1DeviceContext& context, RenderResources& resources,
                                 const D2D1_RECT_F& rect) const {
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> fill;
    D2D1_COLOR_F color = resources.Colors().muted;
    color.a = 0.14F;
    if (FAILED(context.CreateSolidColorBrush(color, &fill))) return;
    resources.DrawRoundRect(rect, 6.0F, fill.Get(), resources.BorderBrush(), 1.0F);
    resources.DrawText(L"图片不可用", rect, resources.MutedBrush(), false, true, DWRITE_TEXT_ALIGNMENT_CENTER);
}

}  // namespace todo::ui
