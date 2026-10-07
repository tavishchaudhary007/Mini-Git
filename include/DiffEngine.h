#pragma once
#include <algorithm>
#include <vector>

template <typename T>
struct DiffOp {
    enum class Kind { Keep, Add, Remove };
    Kind kind;
    T value;
};

// Generic LCS-based diff; works on any T with operator==.
template <typename T>
class DiffEngine {
public:
    static std::vector<DiffOp<T>> diff(const std::vector<T>& a, const std::vector<T>& b) {
        using Kind = typename DiffOp<T>::Kind;
        size_t n = a.size(), m = b.size();
        std::vector<std::vector<size_t>> dp(n + 1, std::vector<size_t>(m + 1, 0));
        for (size_t i = n; i-- > 0;)
            for (size_t j = m; j-- > 0;)
                dp[i][j] = (a[i] == b[j]) ? dp[i + 1][j + 1] + 1 : std::max(dp[i + 1][j], dp[i][j + 1]);

        std::vector<DiffOp<T>> out;
        size_t i = 0, j = 0;
        while (i < n && j < m) {
            if (a[i] == b[j])                      { out.push_back({Kind::Keep, a[i]});   ++i; ++j; }
            else if (dp[i + 1][j] >= dp[i][j + 1]) { out.push_back({Kind::Remove, a[i]}); ++i; }
            else                                   { out.push_back({Kind::Add, b[j]});    ++j; }
        }
        while (i < n) out.push_back({Kind::Remove, a[i++]});
        while (j < m) out.push_back({Kind::Add, b[j++]});
        return out;
    }
};
