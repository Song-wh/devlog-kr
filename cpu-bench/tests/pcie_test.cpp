// PCIe 링크 판정: 직접 만든 장치 목록으로 경고가 맞게 나오는지
#include "envcheck.h"
#include "pcie.h"

#include <cstdio>
#include <string>

static int fails = 0;
static void expect(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++fails;
}
static int count(const std::vector<EnvWarning>& w, const char* id) {
    int n = 0;
    for (const auto& x : w) n += x.id == id;
    return n;
}

int main() {
    // 정상: 그래픽카드 x16/x16(쉬어서 1세대여도 경고 없음), NVMe 4세대 x4
    auto w = evaluate_pcie({{"gpu", "GeForce", 1, 4, 16, 16}, {"nvme", "NVMe", 4, 4, 4, 4}});
    expect(w.empty(), "healthy devices -> no warning (idle GPU gen drop ignored)");
    // 그래픽카드가 x4로 연결
    w = evaluate_pcie({{"gpu", "GeForce", 4, 4, 4, 16}});
    expect(count(w, "pcie_width") == 1 && w[0].message.find("x4 but supports x16") != std::string::npos, "GPU x4 of x16 -> pcie_width");
    // x8 전용 그래픽카드가 x8 → 정상
    w = evaluate_pcie({{"gpu", "Radeon x8", 4, 4, 8, 8}});
    expect(w.empty(), "x8-only GPU at x8 -> no warning");
    // NVMe 4세대 장치가 3세대 슬롯
    w = evaluate_pcie({{"nvme", "NVMe", 3, 4, 4, 4}});
    expect(count(w, "pcie_gen") == 1, "NVMe gen3 of gen4 -> pcie_gen");
    // NVMe x2
    w = evaluate_pcie({{"nvme", "NVMe", 4, 4, 2, 4}});
    expect(count(w, "pcie_width") == 1, "NVMe x2 of x4 -> pcie_width");
    // 값이 0(알 수 없음)이면 판정하지 않음
    w = evaluate_pcie({{"gpu", "?", 0, 0, 0, 16}, {"nvme", "?", 0, 4, 0, 4}});
    expect(w.empty(), "unknown values -> no warning");
    // 실제 PC에서 조회가 돌아가는지만 확인 (장치 수는 PC마다 다름)
    auto real = query_pcie();
    std::printf("this PC: %zu PCIe GPU/NVMe device(s)\n", real.size());
    for (const auto& d : real) std::printf("  %s gen %u/%u x%u/x%u %s\n", d.kind.c_str(), d.cur_gen, d.max_gen, d.cur_width, d.max_width, d.name.c_str());
    return fails ? 1 : 0;
}
