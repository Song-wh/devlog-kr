// 네트워크 연결 판정: 직접 만든 어댑터 목록으로
#include "envcheck.h"
#include "netlink.h"

#include <cstdio>

static int fails = 0;
static void expect(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++fails;
}

int main() {
    expect(evaluate_netlinks({{"ethernet", "Realtek 2.5GbE", 1000000000ull, 1000000000ull}}).empty(), "ethernet 1 Gbps -> ok");
    expect(evaluate_netlinks({{"ethernet", "Intel", 2500000000ull, 2500000000ull}}).empty(), "ethernet 2.5 Gbps -> ok");
    auto w = evaluate_netlinks({{"ethernet", "Realtek", 100000000ull, 100000000ull}});
    expect(w.size() == 1 && w[0].id == "net_slow_ethernet" && w[0].message.find("100 Mbps") != std::string::npos, "ethernet 100 Mbps -> warning");
    expect(evaluate_netlinks({{"ethernet", "Old", 10000000ull, 10000000ull}}).size() == 1, "ethernet 10 Mbps -> warning");
    expect(evaluate_netlinks({{"wifi", "AX211", 1201000000ull, 1201000000ull}}).empty(), "wifi 1.2 Gbps -> ok");
    w = evaluate_netlinks({{"wifi", "AX211", 54000000ull, 54000000ull}});
    expect(w.size() == 1 && w[0].id == "net_slow_wifi", "wifi 54 Mbps -> warning");
    auto real = query_netlinks();
    std::printf("this PC: %zu link(s)\n", real.size());
    for (const auto& l : real) std::printf("  %s %llu/%llu bps %s\n", l.kind.c_str(), static_cast<unsigned long long>(l.rx_bps), static_cast<unsigned long long>(l.tx_bps), l.name.c_str());
    return fails ? 1 : 0;
}
