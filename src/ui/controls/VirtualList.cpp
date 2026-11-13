#include "ui/controls/VirtualList.h"

#include <algorithm>
#include <functional>
#include <optional>

namespace todo::ui {

void VirtualList::SetPageLoader(std::function<void(int)> loader) { loader_ = std::move(loader); }
void VirtualList::SetItemCount(int count) noexcept { itemCount_ = std::max(0, count); ScrollTo(offset_); }
void VirtualList::SetRowHeight(float height) noexcept { rowHeight_ = std::max(24.0F, height); }
void VirtualList::SetViewport(const D2D1_RECT_F& viewport) noexcept { viewport_ = viewport; ScrollTo(offset_); }
std::pair<int, int> VirtualList::VisibleRange() const noexcept {
    const float height = std::max(0.0F, viewport_.bottom - viewport_.top);
    const int first = std::max(0, static_cast<int>(offset_ / rowHeight_) - kOverscan);
    const int last = std::min(itemCount_, static_cast<int>((offset_ + height) / rowHeight_) + 1 + kOverscan);
    return {first, last};
}
void VirtualList::EnsureVisibleRange() {
    if (!loader_) return;
    const auto [first, last] = VisibleRange();
    const int firstPage = first / kPageSize;
    const int lastPage = std::max(firstPage, (std::max(first, last - 1)) / kPageSize);
    for (int page = firstPage; page <= lastPage; ++page) if (!loadedPages_.contains(page)) loader_(page);
}
void VirtualList::ScrollTo(float offset) noexcept {
    const float viewport = std::max(0.0F, viewport_.bottom - viewport_.top);
    offset_ = std::clamp(offset, 0.0F, std::max(0.0F, ContentHeight() - viewport));
}
void VirtualList::ScrollByWheel(int delta) noexcept { ScrollTo(offset_ - static_cast<float>(delta) / 120.0F * rowHeight_ * 3.0F); }
std::optional<int> VirtualList::RowIndexAt(float y) const noexcept {
    if (y < viewport_.top || y >= viewport_.bottom) return std::nullopt;
    const int row = static_cast<int>((y - viewport_.top + offset_) / rowHeight_);
    return row >= 0 && row < itemCount_ ? std::optional<int>(row) : std::nullopt;
}
float VirtualList::ContentHeight() const noexcept { return static_cast<float>(itemCount_) * rowHeight_; }
float VirtualList::Offset() const noexcept { return offset_; }
float VirtualList::MaximumOffset() const noexcept { return std::max(0.0F, ContentHeight() - (viewport_.bottom - viewport_.top)); }
void VirtualList::MarkPageLoaded(int page) {
    loadedPages_[page] = ++lruClock_;
    while (loadedPages_.size() > 20) {
        auto oldest = std::min_element(loadedPages_.begin(), loadedPages_.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
        loadedPages_.erase(oldest);
    }
}
void VirtualList::ClearPages() { loadedPages_.clear(); offset_ = 0; }

}  // namespace todo::ui
