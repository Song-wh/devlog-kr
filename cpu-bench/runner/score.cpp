#include "score.h"

#include <cmath>

double score_ratio(const ScoreItem& item, double measured) {
    if (measured <= 0 || item.reference <= 0) return 0.0;
    return item.lower_is_better ? item.reference / measured : measured / item.reference;
}

double score_from_ratios(const std::vector<double>& ratios) {
    if (ratios.empty()) return 0.0;
    double log_sum = 0;
    for (double r : ratios) {
        if (r <= 0) return 0.0;
        log_sum += std::log(r);
    }
    return 1000.0 * std::exp(log_sum / double(ratios.size()));
}
