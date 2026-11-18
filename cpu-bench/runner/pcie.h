// PCIe 링크 점검: 그래픽카드·NVMe SSD가 장치가 지원하는 레인 수·세대로 연결돼 있는지.
// Windows 장치 속성(SetupAPI, DEVPKEY_PciDevice_*)만 읽는다. 드라이버·관리자 권한 불필요, 읽기 전용.
//
// 알아 둘 점: 그래픽카드는 쉬고 있을 때 전력을 아끼려고 링크 속도(세대)를 낮춘다(예: 4.0 → 1.1).
// 그래서 그래픽카드의 "속도"는 경고하지 않고 레인 수(폭)만 판정한다. 레인 수는 쉬어도 보통 그대로다.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct EnvWarning;

struct PcieDevice {
    std::string kind;      // "gpu" | "nvme"
    std::string name;      // 장치 설명 (그래픽카드는 제품명, NVMe는 컨트롤러 설명)
    uint32_t cur_gen = 0;  // 현재 링크 세대 (1=2.5GT/s, 2=5, 3=8, 4=16, 5=32), 0 = 알 수 없음
    uint32_t max_gen = 0;  // 장치가 지원하는 최대 세대
    uint32_t cur_width = 0;  // 현재 레인 수 (x1, x4, x8, x16)
    uint32_t max_width = 0;  // 장치가 지원하는 최대 레인 수
};

std::vector<PcieDevice> query_pcie();

// 판정: 레인 수가 장치 최대보다 적으면 "pcie_width"(그래픽카드·NVMe),
// NVMe의 세대가 장치 최대보다 낮으면 "pcie_gen"(슬롯이 구세대이거나 공유 레인).
std::vector<EnvWarning> evaluate_pcie(const std::vector<PcieDevice>& devs);
