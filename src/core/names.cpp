#include "core/names.h"

#include <algorithm>
#include <cstdint>

namespace tto {

namespace {

size_t distance(const std::string& a, const std::string& b) {
    std::vector<size_t> row(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) row[j] = j;
    for (size_t i = 1; i <= a.size(); ++i) {
        size_t diag = row[0];
        row[0] = i;
        for (size_t j = 1; j <= b.size(); ++j) {
            size_t up = row[j];
            row[j] = std::min({row[j] + 1, row[j - 1] + 1, diag + (a[i - 1] == b[j - 1] ? 0 : 1)});
            diag = up;
        }
    }
    return row[b.size()];
}

}

std::string closest_name(const std::string& word, const std::vector<std::string>& names) {
    if (word.empty() || word.size() > 64) return {};
    std::string best;
    size_t best_d = SIZE_MAX;
    for (const auto& n : names) {
        if (n == word) continue;
        size_t d = distance(word, n);
        size_t shorter = std::min(word.size(), n.size());
        bool prefix = shorter >= 4 && (n.compare(0, word.size(), word) == 0 || word.compare(0, n.size(), n) == 0);
        if (!prefix && d > std::max<size_t>(1, shorter / 2)) continue;
        if (prefix) d = std::min(d, size_t(1));
        if (d < best_d) {
            best_d = d;
            best = n;
        }
    }
    return best;
}

std::string did_you_mean(const std::string& word, const std::vector<std::string>& names) {
    std::string n = closest_name(word, names);
    return n.empty() ? std::string() : " (did you mean " + n + "?)";
}

}
