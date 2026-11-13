#pragma once

#include <functional>
#include <map>
#include <optional>
#include <vector>

#include <d2d1_1.h>

namespace todo::ui {

class VirtualList {
public:
    static constexpr int kPageSize = 200;
    static constexpr int kOverscan = 5;
    void SetPageLoader(std::function<void(int)> loader);
    void SetItemCount(int count) noexcept;
    void SetRowHeight(float height) noexcept;
    void SetViewport(const D2D1_RECT_F& viewport) noexcept;
    void EnsureVisibleRange();
    void ScrollTo(float offset) noexcept;
    void ScrollByWheel(int delta) noexcept;
    [[nodiscard]] std::optional<int> RowIndexAt(float y) const noexcept;
    [[nodiscard]] std::pair<int, int> VisibleRange() const noexcept;
    [[nodiscard]] float ContentHeight() const noexcept;
    [[nodiscard]] float Offset() const noexcept;
    [[nodiscard]] float MaximumOffset() const noexcept;
    void MarkPageLoaded(int page);
    void ClearPages();
private:
    D2D1_RECT_F viewport_{};
    std::function<void(int)> loader_;
    std::map<int, std::size_t> loadedPages_;
    std::size_t lruClock_{0};
    int itemCount_{0};
    float rowHeight_{64.0F};
    float offset_{0.0F};
};

}  // namespace todo::ui
