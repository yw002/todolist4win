#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_set>
#include <vector>

#include "domain/Models.h"
#include "ui/controls/Control.h"
#include "ui/controls/VirtualList.h"

namespace todo::ui {

class HistoryListControl final : public Control {
public:
    HistoryListControl();
    void SetEvents(const std::vector<HistoryEvent>& events);
    void SetReadOnly(bool readOnly) noexcept;
    void SetHasNextPage(bool value) noexcept;
    void SetCallbacks(std::function<void(std::int64_t)> restore, std::function<void(std::int64_t)> remove, std::function<void()> loadMore);
    [[nodiscard]] std::vector<std::int64_t> SelectedEventIds() const;
    [[nodiscard]] std::vector<std::int64_t> SelectedTaskIds() const;
    void ClearSelection();
    // Whether the point lands on an event row and not merely inside the list viewport, so the hand
    // cursor does not cover the empty area of the history list.
    [[nodiscard]] bool HitsRow(float x, float y) const noexcept;
    void Paint(ID2D1DeviceContext& context, RenderResources& resources) override;
    bool OnMouseDown(float x, float y, UINT keyFlags) override;
    bool OnMouseUp(float x, float y) override;
    bool OnMouseMove(float x, float y) override;
    bool OnMouseWheel(float x, float y, int delta) override;
private:
    std::optional<int> RowAt(float y) const;
    VirtualList list_;
    const std::vector<HistoryEvent>* events_{nullptr};
    std::unordered_set<std::int64_t> selected_;
    std::function<void(std::int64_t)> restore_;
    std::function<void(std::int64_t)> remove_;
    std::function<void()> loadMore_;
    std::int64_t pressedId_{0};
    int pressedAction_{0};
    int hoverRow_{-1};
    bool readOnly_{false};
    bool hasNext_{false};
};

}  // namespace todo::ui
