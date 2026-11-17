// 메모리 파트 번호에서 정격 속도(XMP/EXPO 포함) 읽기.
//
// SMBIOS의 "최대 속도"는 보통 JEDEC 기본 속도라서, XMP/EXPO가 꺼져 있으면 XMP 키트인지 알 수 없다.
// 주요 제조사는 파트 번호에 정격 속도를 넣으므로 그 규칙으로 읽는다. 모르는 형식이면 0을 돌려준다.
#pragma once

#include <cstdint>
#include <string>

struct RatedSpeed {
    uint32_t mts = 0;        // 정격 속도 MT/s. 0 = 알 수 없음
    const char* brand = "";  // 규칙 이름 (예: "G.Skill", "Kingston FURY")
    bool jedec_only = false; // 제조사 기본(JEDEC) 모듈: XMP/EXPO 프로필이 없다고 보는 형식
};

RatedSpeed rated_speed_from_part(const std::string& part_number);
