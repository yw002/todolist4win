#pragma once

// Body editor control: a vertically scrollable sequence of text blocks and image blocks.
//
// Text blocks own a TextBox solely for WM_CHAR / IME / caret / character-level undo; the TextBox is
// NEVER painted. All painting goes through IDWriteTextLayout created with the dedicated WRAP format
// from RenderResources, so long text wraps instead of being silently clipped. Mouse hit-testing uses
// IDWriteTextLayout::HitTestPoint, not a width-based binary search.
//
// Ctrl+V is handled here (before the inner TextBox sees the key) with a SINGLE call to
// ui::ReadClipboard(forceText); the inner TextBox::Paste() must never run for body text.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <d2d1_1.h>
#include <dwrite_3.h>
#include <wrl/client.h>

#include "domain/BodyDocument.h"
#include "ui/controls/BodyImageCache.h"
#include "ui/controls/Control.h"
#include "ui/controls/TextBox.h"

namespace todo::ui {

struct EditorBlock {
    todo::BodyBlockKind kind{todo::BodyBlockKind::Text};
    std::wstring text;                                  // Text block, UTF-16
    TextBox input;                                      // IME / caret / char undo owner, never painted
    std::string imageKey;                               // Image block
    std::uint32_t imageWidth{0};
    std::uint32_t imageHeight{0};
    std::string rawJson;                                // unknown block: verbatim JSON to preserve
    float top{0.0F};                                    // layout result, DIP, relative to content origin
    float heightDip{0.0F};
    float imageDrawnWidth{0.0F};
    float imageDrawnHeight{0.0F};
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;   // text blocks only
};

class BodyEditor final : public Control {
public:
    BodyEditor();
    ~BodyEditor() override = default;

    // ---- §7 frozen contract -------------------------------------------------
    void SetDocument(const todo::BodyDocument& document);
    [[nodiscard]] todo::BodyDocument GetDocument() const;
    [[nodiscard]] std::vector<todo::BodyImage> GetNewImages() const;
    void SetImages(const std::vector<todo::BodyImage>& images);
    [[nodiscard]] TextBox* ActiveTextBox() noexcept;
    bool OnKeyDown(UINT key, bool ctrl, bool shift) override;
    bool OnMouseWheel(float x, float y, int delta) override;

    // ---- control plumbing (Visible / SetBounds / Bounds are inherited) -----
    void Clear() noexcept;
    void Paint(ID2D1DeviceContext& context, RenderResources& resources) override;
    void OnFocus(bool focused) override;
    [[nodiscard]] bool HitTest(float x, float y) const noexcept override;
    bool OnMouseDown(float x, float y, UINT keyFlags) override;
    bool OnMouseMove(float x, float y) override;
    bool OnMouseUp(float x, float y) override;
    bool OnChar(wchar_t value) override;

    // Async image-load result: a non-empty summary means at least one blob is unavailable in the
    // local database; missing blocks render a gray placeholder instead of staying blank.
    void SetEditorImageError(std::string message);
    [[nodiscard]] const std::string& EditorImageError() const noexcept { return editorImageError_; }
    // A paste rejected by the size/count limits is reported once: the dialog moves this into its own
    // error strip instead of the editor painting a message over the content.
    [[nodiscard]] std::string TakeError();

private:
    void PrepareTextInput(std::size_t index);
    void PrepareAllTextInputs();
    bool MergeWithPreviousBlock();
    bool MergeWithNextBlock();
    void SyncTextFromInputs();
    void SyncInputsFromText();
    void RebuildLayouts(RenderResources& resources);
    void RecomputeContentHeight(RenderResources& resources);
    void ClampScroll() noexcept;
    void EnsureVisible(std::size_t blockIndex);
    void PushBlockUndo();
    void InsertTextAtCaret(const std::wstring& text);
    void SplitForImage(const std::string& key, std::uint32_t width, std::uint32_t height);
    void DeleteSelectedImageBlock();
    bool HandlePaste(bool forceText);
    bool HandleBlockUndo();
    bool MoveCaretHorizontally(bool forward);
    bool MoveCaretVertically(bool up);
    [[nodiscard]] std::size_t CaretLineNumber() const;
    [[nodiscard]] std::size_t LineCount(std::size_t blockIndex) const;
    [[nodiscard]] std::size_t TextBlockCount() const noexcept;
    [[nodiscard]] D2D1_RECT_F ContentRect() const noexcept;
    [[nodiscard]] D2D1_RECT_F Viewport() const noexcept;
    void DrawScrollbar(ID2D1DeviceContext& context, RenderResources& resources) const;
    void DrawPlaceholder(ID2D1DeviceContext& context, RenderResources& resources,
                         const D2D1_RECT_F& rect) const;
    void ActivateBlock(std::size_t index, bool caretAtEnd);

    std::vector<EditorBlock> blocks_;
    std::size_t activeBlock_{0};
    std::vector<todo::BodyImage> newImages_;
    std::vector<todo::BodyDocument> blockUndo_;
    bool lastOperationWasBlockLevel_{false};
    BodyImageCache cache_;
    std::wstring placeholder_{L"输入正文，Ctrl+V 可粘贴图片"};
    std::string editorImageError_;
    float scrollOffset_{0.0F};
    float contentHeight_{0.0F};
    bool draggingScrollbar_{false};
    float dragStartY_{0.0F};
    float dragStartOffset_{0.0F};
    // Set by Paint so mouse/keyboard handlers can rebuild layouts (and measure the caret) outside a
    // paint pass. RenderResources lives for the whole window lifetime and is rebuilt in place.
    RenderResources* paintResources_{nullptr};
    bool layoutDirty_{true};
    float layoutWidth_{0.0F};
    std::string pendingError_;
};

}  // namespace todo::ui
