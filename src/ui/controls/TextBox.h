#pragma once

#include <functional>
#include <string>
#include <vector>

#include "ui/controls/Control.h"

namespace todo::ui {

class TextBox final : public Control {
public:
    TextBox();
    void SetText(const std::wstring& text, bool notify = false);
    void SetPlaceholder(std::wstring text);
    void SetPassword(bool password) noexcept;
    void SetMultiline(bool multiline) noexcept;
    void SetReadOnly(bool readOnly) noexcept;
    void SetOnChanged(std::function<void(const std::wstring&)> callback);
    void SetOnSubmit(std::function<void()> callback);
    [[nodiscard]] const std::wstring& Text() const noexcept;
    [[nodiscard]] std::size_t Caret() const noexcept;
    void SelectAll() noexcept;
    void Paint(ID2D1DeviceContext& context, RenderResources& resources) override;
    bool OnMouseDown(float x, float y, UINT keyFlags) override;
    bool OnMouseMove(float x, float y) override;
    bool OnMouseUp(float x, float y) override;
    bool OnKeyDown(UINT key, bool ctrl, bool shift) override;
    bool OnChar(wchar_t value) override;
    void OnFocus(bool focused) override;
    bool HandleImeStart(HWND hwnd);
    bool HandleImeComposition(HWND hwnd, LPARAM lParam);
    bool HandleImeEnd(HWND hwnd);
    void UpdateImePosition(HWND hwnd, UINT dpi);

private:
    struct Selection { std::size_t start; std::size_t end; };
    Selection SelectedRange() const noexcept;
    void SaveUndo();
    void Insert(const std::wstring& value);
    void EraseSelection();
    void Changed();
    std::size_t IndexAt(float x, RenderResources* resources = nullptr);
    float CaretX(std::size_t index, RenderResources& resources) const;
    std::wstring DisplayText() const;
    bool Copy() const;
    bool Cut();
    bool Paste();
    std::wstring text_;
    std::wstring composition_;
    std::wstring placeholder_;
    std::vector<std::wstring> undo_;
    std::size_t caret_{0};
    std::size_t anchor_{0};
    bool password_{false};
    bool multiline_{false};
    bool readOnly_{false};
    bool selecting_{false};
    std::function<void(const std::wstring&)> onChanged_;
    std::function<void()> onSubmit_;
    // Non-owning cache set by Paint(). RenderEngine keeps one RenderResources instance for the
    // entire window lifetime and rebuilds its contents in place, so this pointer never dangles;
    // IndexAt() additionally guards against the object being temporarily released.
    RenderResources* paintResources_{nullptr};
};

}  // namespace todo::ui
