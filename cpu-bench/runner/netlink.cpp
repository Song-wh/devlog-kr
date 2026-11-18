#include "netlink.h"

#include "envcheck.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <iphlpapi.h>
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>

namespace {

std::string narrow_w(const wchar_t* w) {
    if (!w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

// 가상·터널 어댑터(Hyper-V, VPN, VMware, VirtualBox, 블루투스 등)는 실제 연결이 아니라서 뺀다
bool is_virtual(const wchar_t* desc) {
    static const wchar_t* kWords[] = {L"Hyper-V", L"Virtual", L"VPN", L"TAP", L"VMware", L"VirtualBox", L"Bluetooth",
                                      L"Loopback", L"WAN Miniport", L"Npcap", L"Tunnel", L"WireGuard", L"Tailscale"};
    if (!desc) return true;
    for (const wchar_t* w : kWords)
        if (std::wcsstr(desc, w)) return true;
    return false;
}

std::string mbps(uint64_t bps) {
    char buf[32];
    if (bps >= 1000000000ull) std::snprintf(buf, sizeof(buf), "%.1f Gbps", double(bps) / 1e9);
    else std::snprintf(buf, sizeof(buf), "%llu Mbps", static_cast<unsigned long long>(bps / 1000000));
    return buf;
}

}  // namespace

std::vector<NetLink> query_netlinks() {
    std::vector<NetLink> out;
    ULONG size = 32 * 1024;
    std::vector<unsigned char> buf;
    ULONG rc = ERROR_BUFFER_OVERFLOW;
    for (int tries = 0; tries < 4 && rc == ERROR_BUFFER_OVERFLOW; ++tries) {
        buf.resize(size);
        rc = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
                                  reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    }
    if (rc != NO_ERROR) return out;
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        const bool eth = a->IfType == IF_TYPE_ETHERNET_CSMACD;
        const bool wifi = a->IfType == IF_TYPE_IEEE80211;
        if ((!eth && !wifi) || is_virtual(a->Description)) continue;
        if (a->ReceiveLinkSpeed == 0 || a->ReceiveLinkSpeed == UINT64_MAX) continue;
        out.push_back({eth ? "ethernet" : "wifi", narrow_w(a->Description), a->ReceiveLinkSpeed, a->TransmitLinkSpeed});
    }
    return out;
}

std::vector<EnvWarning> evaluate_netlinks(const std::vector<NetLink>& links) {
    std::vector<EnvWarning> w;
    for (const auto& l : links) {
        const uint64_t bps = std::max(l.rx_bps, l.tx_bps);
        if (l.kind == "ethernet" && bps <= 100000000ull)
            w.push_back({"net_slow_ethernet", "Wired adapter " + l.name + " is linked at " + mbps(bps) + " (most adapters support 1 Gbps or more)"});
        if (l.kind == "wifi" && bps < 100000000ull)
            w.push_back({"net_slow_wifi", "Wi-Fi adapter " + l.name + " is linked at " + mbps(bps)});
    }
    return w;
}
