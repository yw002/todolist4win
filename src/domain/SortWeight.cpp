#include "domain/SortWeight.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <vector>

namespace todo::SortWeight {

double NewTaskWeight(double currentMaximum) noexcept {
    if (!std::isfinite(currentMaximum)) return kStep;
    // Validate the RESULT instead of predicting the boundary. The previous guard compared against
    // (max - kStep), but that expression rounds straight back to DBL_MAX in double precision, so
    // the test never fired and DBL_MAX + kStep silently became +inf. Checking the sum also catches
    // the "kStep is absorbed by rounding" case, where adding a step would produce a duplicate
    // weight and corrupt the ordering.
    const double next = currentMaximum + kStep;
    if (!std::isfinite(next) || next <= currentMaximum) return kStep;
    return next;
}

Result<double> Between(std::optional<double> previous, std::optional<double> next) {
    if ((previous && !std::isfinite(*previous)) || (next && !std::isfinite(*next))) {
        return Result<double>::Failure({ErrorCode::Conflict, "排序权重需要重新编号"});
    }
    if (previous && next) {
        if (*previous >= *next) return Result<double>::Failure({ErrorCode::InvalidInput, "排序位置无效"});
        if (NeedsRenumbering(*previous, *next)) return Result<double>::Failure({ErrorCode::Conflict, "排序间隙过小，需要重新编号"});
        return Result<double>::Success(*previous + (*next - *previous) / 2.0);
    }
    if (previous) return Result<double>::Success(NewTaskWeight(*previous));
    if (next) return Result<double>::Success(*next - kStep);
    return Result<double>::Success(kStep);
}

bool NeedsRenumbering(double previous, double next) noexcept {
    const double weight = previous + (next - previous) / 2.0;
    return !std::isfinite(weight) || next - previous < kMinimumGap || weight == previous || weight == next;
}

Result<void> Renumber(std::vector<Task>& tasks) {
    std::stable_sort(tasks.begin(), tasks.end(), [](const Task& a, const Task& b) {
        if (a.sortWeight != b.sortWeight) return a.sortWeight < b.sortWeight;
        return a.id < b.id;
    });
    for (std::size_t i = 0; i < tasks.size(); ++i) {
        const double weight = static_cast<double>(i + 1) * kStep;
        if (!std::isfinite(weight)) return Result<void>::Failure({ErrorCode::Internal, "排序权重溢出"});
        tasks[i].sortWeight = weight;
    }
    return Result<void>::Success();
}

}  // namespace todo::SortWeight
