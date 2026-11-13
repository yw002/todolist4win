#pragma once

#include <functional>
#include <optional>
#include <vector>

#include "app/AppController.h"
#include "ui/controls/Button.h"
#include "ui/controls/HistoryListControl.h"
#include "ui/controls/PopupMenu.h"
#include "ui/controls/TextBox.h"

namespace todo::ui {

class HistoryPage final : public Control {
public:
    HistoryPage(AppController& controller, std::function<void(const std::vector<std::int64_t>&)> deleteEvents);
    void Paint(ID2D1DeviceContext& context, RenderResources& resources) override;
    bool OnMouseDown(float x, float y, UINT keyFlags) override;
    bool OnMouseMove(float x, float y) override;
    bool OnMouseUp(float x, float y) override;
    bool OnMouseWheel(float x, float y, int delta) override;
    bool OnKeyDown(UINT key, bool ctrl, bool shift) override;
    void FocusSearch();
    TextBox* ActiveTextBox() noexcept;
    Control* FocusedControl() noexcept;
    // Cursor support: distinguishes text fields from clickable list rows at a DIP position.
    [[nodiscard]] bool HitTextField(float x, float y) const noexcept;
    [[nodiscard]] bool HitRow(float x, float y) const noexcept;
private:
    void ApplyFilters();
    AppController& controller_;
    TextBox search_;
    TextBox startDate_;
    TextBox endDate_;
    Button category_;
    Button apply_;
    Button restore_;
    Button remove_;
    PopupMenu categoryMenu_;
    HistoryListControl list_;
    Control* pressed_{nullptr};
    Control* focused_{nullptr};
    std::optional<std::int64_t> categoryId_;
    std::uint64_t observedWrite_{0};
};

}  // namespace todo::ui
