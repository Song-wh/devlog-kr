#include "pcie.h"

#include "envcheck.h"

#include <windows.h>
#include <initguid.h>
#include <devpkey.h>
#include <pciprop.h>
#include <setupapi.h>

#include <cstdio>

namespace {

bool prop_u32(HDEVINFO set, SP_DEVINFO_DATA& dev, const DEVPROPKEY& key, uint32_t& out) {
    DEVPROPTYPE type = 0;
    BYTE buf[8] = {};
    DWORD size = 0;
    if (!SetupDiGetDevicePropertyW(set, &dev, &key, &type, buf, sizeof(buf), &size, 0)) return false;
    if (type == DEVPROP_TYPE_UINT32 && size == 4) out = *reinterpret_cast<uint32_t*>(buf);
    else if (type == DEVPROP_TYPE_BYTE && size == 1) out = buf[0];
    else if (type == DEVPROP_TYPE_UINT16 && size == 2) out = *reinterpret_cast<uint16_t*>(buf);
    else return false;
    return true;
}

std::string prop_str(HDEVINFO set, SP_DEVINFO_DATA& dev, const DEVPROPKEY& key) {
    DEVPROPTYPE type = 0;
    wchar_t buf[256] = {};
    if (!SetupDiGetDevicePropertyW(set, &dev, &key, &type, reinterpret_cast<BYTE*>(buf), sizeof(buf) - sizeof(wchar_t), nullptr, 0) ||
        type != DEVPROP_TYPE_STRING)
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, buf, -1, s.data(), n, nullptr, nullptr);
    return s;
}

const char* gen_rate(uint32_t g) {
    switch (g) {
        case 1: return "1.0 (2.5 GT/s)";
        case 2: return "2.0 (5 GT/s)";
        case 3: return "3.0 (8 GT/s)";
        case 4: return "4.0 (16 GT/s)";
        case 5: return "5.0 (32 GT/s)";
        case 6: return "6.0 (64 GT/s)";
        default: return "unknown";
    }
}

}  // namespace

std::vector<PcieDevice> query_pcie() {
    std::vector<PcieDevice> out;
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, L"PCI", nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (set == INVALID_HANDLE_VALUE) return out;
    SP_DEVINFO_DATA dev{};
    dev.cbSize = sizeof(dev);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); ++i) {
        uint32_t base = 0, sub = 0;
        if (!prop_u32(set, dev, DEVPKEY_PciDevice_BaseClass, base) || !prop_u32(set, dev, DEVPKEY_PciDevice_SubClass, sub)) continue;
        const bool gpu = base == 0x03;               // 디스플레이 컨트롤러
        const bool nvme = base == 0x01 && sub == 0x08;  // 대용량 저장소 / NVM
        if (!gpu && !nvme) continue;
        PcieDevice d;
        d.kind = gpu ? "gpu" : "nvme";
        // 내장 그래픽처럼 PCIe 링크가 없는 장치는 링크 속성이 없다 → 건너뜀
        if (!prop_u32(set, dev, DEVPKEY_PciDevice_MaxLinkWidth, d.max_width) || d.max_width == 0) continue;
        prop_u32(set, dev, DEVPKEY_PciDevice_CurrentLinkWidth, d.cur_width);
        prop_u32(set, dev, DEVPKEY_PciDevice_MaxLinkSpeed, d.max_gen);
        prop_u32(set, dev, DEVPKEY_PciDevice_CurrentLinkSpeed, d.cur_gen);
        d.name = prop_str(set, dev, DEVPKEY_Device_FriendlyName);
        if (d.name.empty()) d.name = prop_str(set, dev, DEVPKEY_Device_DeviceDesc);
        out.push_back(std::move(d));
    }
    SetupDiDestroyDeviceInfoList(set);
    return out;
}

std::vector<EnvWarning> evaluate_pcie(const std::vector<PcieDevice>& devs) {
    std::vector<EnvWarning> w;
    char buf[320];
    for (const auto& d : devs) {
        const char* what = d.kind == "gpu" ? "Graphics card" : "NVMe SSD";
        if (d.cur_width && d.max_width && d.cur_width < d.max_width) {
            std::snprintf(buf, sizeof(buf), "%s %s runs at x%u but supports x%u", what, d.name.c_str(), d.cur_width, d.max_width);
            w.push_back({"pcie_width", buf});
        }
        // 그래픽카드는 쉴 때 세대를 낮추는 게 정상이라 판정하지 않는다
        if (d.kind == "nvme" && d.cur_gen && d.max_gen && d.cur_gen < d.max_gen) {
            std::snprintf(buf, sizeof(buf), "%s %s runs at PCIe %s but supports PCIe %s", what, d.name.c_str(), gen_rate(d.cur_gen), gen_rate(d.max_gen));
            w.push_back({"pcie_gen", buf});
        }
    }
    return w;
}
