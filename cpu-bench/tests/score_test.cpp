// 점수 계산 확인: 비율 방향(지연은 작을수록 좋음), 기하평균, 0 처리, v1 기준값이 바뀌지 않았는지
#include "score.h"

#include <cmath>
#include <cstdio>
#include <string>

static int g_failed = 0;
static void check(bool ok, const std::string& what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++g_failed;
}
static bool near(double a, double b) { return std::fabs(a - b) < 1e-6 * std::fabs(b) + 1e-9; }

int main() {
    const auto& items = score_items();
    check(kScoreVersion == 1 && items.size() == 5, "score v1 has 5 items");
    // v1 기준값은 바꾸지 않는다 (바꾸려면 kScoreVersion을 올리고 이 테스트도 고친다)
    const double refs[] = {1000.0, 20.0, 100.0, 10000.0, 50.0};
    bool same = true;
    for (size_t i = 0; i < items.size(); ++i) same &= items[i].reference == refs[i];
    check(same, "v1 reference values unchanged");

    ScoreItem bw{"x", "x", "mem", "read", 0, false, 20.0, false};
    ScoreItem lat{"y", "y", "mem", "latency", 0, false, 100.0, true};
    check(near(score_ratio(bw, 30.0), 1.5), "higher is better: 30 / 20 = 1.5");
    check(near(score_ratio(lat, 80.0), 1.25), "lower is better: 100 / 80 = 1.25");
    check(score_ratio(lat, 0.0) == 0.0, "zero measurement -> ratio 0");

    check(near(score_from_ratios({1.0, 1.0, 1.0}), 1000.0), "all at reference -> 1000");
    check(near(score_from_ratios({2.0, 0.5}), 1000.0), "geometric mean: 2 x 0.5 -> 1000");
    check(near(score_from_ratios({4.0, 1.0}), 2000.0), "geometric mean: sqrt(4 x 1) -> 2000");
    check(score_from_ratios({1.0, 0.0}) == 0.0, "any zero ratio -> 0");
    check(score_from_ratios({}) == 0.0, "no items -> 0");

    std::printf("%s\n", g_failed ? "FAILED" : "ALL PASSED");
    return g_failed ? 1 : 0;
}
