// 네트워크 연결 점검: 유선 랜이 100Mbps 이하로 연결됐는지(케이블·포트 문제), Wi-Fi 연결 속도.
// Windows가 이미 알고 있는 어댑터 정보(GetAdaptersAddresses)만 읽는다. 외부로 아무것도 보내지 않는다.
// 인터넷 속도(통신사·공유기)는 재지 않는다. 이 PC와 공유기 사이의 "연결 속도"만 본다.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct EnvWarning;

struct NetLink {
    std::string kind;         // "ethernet" | "wifi"
    std::string name;         // 어댑터 설명 (예: "Realtek PCIe 2.5GbE Family Controller")
    uint64_t rx_bps = 0;      // 받기 연결 속도
    uint64_t tx_bps = 0;      // 보내기 연결 속도
};

std::vector<NetLink> query_netlinks();

// "net_slow_ethernet": 유선 연결이 100Mbps 이하 (요즘 랜카드는 거의 1Gbps 이상 → 케이블·포트·공유기 포트 의심)
// "net_slow_wifi": Wi-Fi 연결 속도 100Mbps 미만 (신호가 약하거나 오래된 규격)
std::vector<EnvWarning> evaluate_netlinks(const std::vector<NetLink>& links);
