#include "domain/HistoryPagination.h"

#include <functional>
#include <sstream>
#include <string>

namespace todo::HistoryPagination {

std::string QuerySignature(const HistoryQuery& query) {
    std::ostringstream out;
    out << query.filter.keyword << '|' << query.filter.categoryId.value_or(0) << '|';
    if (query.filter.timeRange) out << query.filter.timeRange->start.ToString() << '|' << query.filter.timeRange->end.ToString();
    return std::to_string(std::hash<std::string>{}(out.str()));
}

bool IsCursorCompatible(const HistoryQuery& query) {
    if (!query.page.cursor) return true;
    return query.page.cursor->signature == QuerySignature(query);
}

HistoryCursor MakeNextHistoryCursor(const HistoryEvent& last, const HistoryQuery& query, const std::string& signature) {
    (void)query;  // signature is derived from the query by the caller and passed in explicitly
    return HistoryCursor{last.eventAt, last.id, signature};
}

}  // namespace todo::HistoryPagination
