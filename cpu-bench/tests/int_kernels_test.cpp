// 정수 모듈 정확성 테스트: 표준 테스트 벡터 + Python 독립 구현(tools/int_reference.py)과 digest 비교
#include "int_kernels.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_failed = 0;

static void check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failed;
}

static std::string hex(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        s += d[p[i] >> 4];
        s += d[p[i] & 15];
    }
    return s;
}

static std::string sha_hex(const std::string& msg) {
    uint8_t out[32];
    intk::sha256(reinterpret_cast<const uint8_t*>(msg.data()), msg.size(), out);
    return hex(out, 32);
}

int main() {
    const std::string c9 = "123456789";
    check(intk::crc32(reinterpret_cast<const uint8_t*>(c9.data()), c9.size()) == 0xCBF43926u,
          "crc32(\"123456789\") == 0xCBF43926");

    check(sha_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "sha256(\"\")");
    check(sha_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256(\"abc\")");
    check(sha_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
          "sha256(56-byte message, two padding blocks)");

    uint32_t count = 0;
    const std::string t = "12;0;99999;";
    check(intk::parse_sum(t.data(), t.size(), &count) == 100011 && count == 3, "parse_sum(\"12;0;99999;\")");

    // Python 독립 구현과 같은 digest가 나와야 한다 (python tools/int_reference.py <seed>)
    struct Golden { uint64_t seed, digest; };
    const Golden golden[] = {{1, 0x4f5e675ada8881aeull}, {42, 0xc3812a61653df216ull}};
    std::vector<uint32_t> scratch(intk::kSortCount);
    for (const auto& g : golden) {
        intk::Input in = intk::make_input(g.seed);
        uint64_t d = intk::run_unit(in, scratch.data());
        char what[96];
        std::snprintf(what, sizeof(what), "run_unit(seed %llu) == 0x%016llx (got 0x%016llx)",
                      static_cast<unsigned long long>(g.seed), static_cast<unsigned long long>(g.digest),
                      static_cast<unsigned long long>(d));
        check(d == g.digest, what);
        check(intk::run_unit(in, scratch.data()) == d, "run_unit is repeatable");
    }

    std::printf("%s\n", g_failed ? "FAILED" : "ALL PASSED");
    return g_failed ? 1 : 0;
}
