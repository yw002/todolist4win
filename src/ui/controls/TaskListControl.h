#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "domain/Models.h"
#include "ui/controls/Control.h"
#include "ui/controls/VirtualList.h"

namespace todo::ui {

class TaskListControl final : public Control {
public:
    TaskListControl();
    void SetTasks(const std::vector<Task>& tasks);
    void SetCategories(const std::vector<CategoryCount>& categories);
    void SetReadOnly(bool readOnly) noexcept;
    void SetCanReorder(bool canReorder) noexcept;
    void SetHasNextPage(bool value) noexcept;
    void SetCallbacks(std::function<void(const Task&, bool)> toggle, std::function<void(const Task&)> edit, std::function<void(const Task&)> remove, std::function<void(std::int64_t, std::optional<std::int64_t>, std::optional<std::int64_t>)> reorder, std::function<void()> loadMore);
    void SetTime(double nowSeconds) noexcept;
    void Clear();
    // Whether the point lands on a task row and not merely inside the list viewport: the empty
    // "暂无任务" area must not report a row, or the hand cursor would cover the whole list.
    [[nodiscard]] bool HitsRow(float x, float y) const noexcept;
    void Paint(ID2D1DeviceContext& context, RenderResources& resources) override;
    bool OnMouseDown(float x, float y, UINT keyFlags) override;
    bool OnMouseMove(float x, float y) override;
    bool OnMouseUp(float x, float y) override;
    bool OnDoubleClick(float x, float y) override;
    bool OnMouseWheel(float x, float y, int delta) override;
    bool OnKeyDown(UINT key, bool ctrl, bool shift) override;
private:
    const Task* TaskAt(float y) const;
    std::optional<int> RowAt(float y) const;
    D2D1_RECT_F RowBounds(int index) const;
    std::string CategoryName(const Task& task) const;
    VirtualList list_;
    const std::vector<Task>* tasks_{nullptr};
    std::vector<CategoryCount> categories_;
    std::function<void(const Task&, bool)> toggle_;
    std::function<void(const Task&)> edit_;
    std::function<void(const Task&)> remove_;
    std::function<void(std::int64_t, std::optional<std::int64_t>, std::optional<std::int64_t>)> reorder_;
    std::function<void()> loadMore_;
    std::unordered_map<std::int64_t, double> completionAnimations_;
    std::unordered_map<std::int64_t, bool> completionStates_;
    std::vector<std::pair<Task, int>> removedRows_;
    double removedAt_{0.0};
    std::unordered_set<std::int64_t> knownTasks_;
    std::unordered_map<std::int64_t, double> enterAnimations_;
    std::int64_t selectedId_{0};
    std::int64_t pressedId_{0};
    std::int64_t draggedId_{0};
    int pressedAction_{0};
    float dragY_{0.0F};
    float pressY_{0.0F};
    int hoverRow_{-1};
    bool readOnly_{false};
    bool canReorder_{true};
    bool hasNext_{false};
    double nowSeconds_{0.0};
};

}  // namespace todo::ui
