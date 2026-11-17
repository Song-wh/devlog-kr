# cpu-bench

Windows PC용 무료 진단형 CPU 벤치마크. 점수만 보여 주는 게 아니라 **내 PC가 제 성능을 내는지, 왜 이 점수인지, 뭘 하면 나아지는지**까지 알려 주는 것이 목표입니다.

> 아직 개발 초기입니다. 지금은 측정 러너(CLI)와 부하 모듈 두 개(정수, 메모리)가 있습니다.

## 구조

```
[제어 앱 (예정)] ── Named Pipe(JSON) ──> [측정 러너 cpu-bench-runner.exe] ── LoadLibrary ──> [부하 모듈 bench_*.dll]
```

- **러너**는 측정만 하는 별도 프로세스입니다. 우선순위 HIGH(REALTIME은 쓰지 않음), 스레드를 코어에 고정하고, 하이브리드 CPU에서는 싱글 스레드 측정을 성능 코어(P코어)에서 합니다.
- **부하 모듈**은 공통 C 인터페이스([include/bench_api.h](include/bench_api.h))를 구현한 DLL입니다.
- 측정 절차: 워밍업 1회 → N회 측정 → **중앙값** 채택, 회차 간 편차(CV)가 2%를 넘으면 경고.

## checksum으로 하는 두 가지 검증

모듈은 같은 입력으로 같은 계산(작업 단위)을 반복합니다. 그래서 결과값(checksum)은 항상 같아야 합니다.

1. **계산이 실제로 일어났는지**: 결과가 기대값과 맞아야 합니다. 같은 계산을 Python 표준 라이브러리로 따로 구현한 [tools/int_reference.py](tools/int_reference.py), [tools/mem_reference.py](tools/mem_reference.py)와 결과를 비교합니다.
2. **PC가 안정적인지**: 반복 도중 결과가 한 번이라도 달라지면 불안정한 오버클럭이나 메모리 설정(XMP)을 의심할 수 있습니다. 러너는 이 경우 경고하고 종료 코드 3을 돌려줍니다.

## 부하 모듈

| 모듈 | 내용 | 상태 |
|---|---|---|
| `int` | CRC-32, SHA-256, 정렬, 숫자 파싱 | 동작 (압축/해제는 추가 예정) |
| `fp` | 행렬곱, N-body, FFT (SSE/AVX2/AVX-512 경로 자동 선택) | 예정 |
| `mem` | 읽기·쓰기·복사 대역폭, 캐시·DRAM 지연 (작업 크기를 바꿔 가며 L1→DRAM) | 동작 (SSE2) |
| `mt` | 원자적 연산 경합, lock-free 큐, 스레드 확장 곡선 | 예정 |

### 메모리 모듈 (`mem`)

| 측정 종류 (`--workload`) | 내용 | 표시 |
|---|---|---|
| `read` (기본) | 데이터를 읽어 합계 | GB/s |
| `write` | 고정 패턴 기록 | GB/s |
| `copy` | 원본 → 대상 복사. STREAM처럼 읽은 바이트와 쓴 바이트를 모두 셉니다 | GB/s |
| `latency` | 무작위 순서로 이어진 포인터를 따라가기. 다음 주소를 알려면 앞 로드가 끝나야 해서, 로드 1회 시간이 곧 지연입니다 | ns |

- `--size`는 전체 스레드가 다루는 데이터 크기입니다(기본 256 MiB, L3보다 충분히 큼). 스레드마다 같은 크기 구간을 맡고, `copy`는 원본·대상이 절반씩입니다.
- `sweep`으로 크기를 2배씩 늘리면 L1 → L2 → L3 → DRAM 구간에서 값이 계단처럼 바뀝니다.
- 알아 둘 점
  - 지연은 4 KiB 페이지 기준이라, 수 MiB를 넘으면 TLB 미스 비용이 포함됩니다(L3 안이라도 값이 커짐). 큰 페이지는 관리자 권한이 필요해서 쓰지 않습니다.
  - 대역폭 커널은 지금 SSE2(16바이트)입니다. DRAM 구간에는 충분하지만 L1·L2 구간은 CPU 연산 폭이 한계라 실제 캐시 대역폭보다 낮게 나옵니다. AVX2 경로는 FP 모듈과 함께 추가할 예정입니다.

개발 PC(i5-14400F) 실측 예:

| 항목 | 싱글 스레드 | 16스레드 |
|---|---|---|
| 읽기 256 MiB | 22.3 GB/s | 81.7 GB/s |
| 쓰기 256 MiB | 15.7 GB/s | 49.3 GB/s |
| 복사 256 MiB | 25.2 GB/s | 64.6 GB/s |
| 지연 256 MiB | 94.3 ns | |

## 빌드

필요한 것: Visual Studio 2022 Build Tools (C++ 데스크톱 워크로드, CMake 포함). clang-cl 빌드도 지원합니다.

```
cmake --preset msvc
cmake --build --preset msvc
ctest --preset msvc
```

결과물은 `build/msvc/bin/`에 생깁니다.

## 사용법

```
cpu-bench-runner info                       # CPU, 코어 구성, 캐시, 메모리(슬롯·채널·속도)
cpu-bench-runner check                      # 측정 전 환경 점검: 전원, 전원 관리 옵션, 백그라운드 CPU 사용률
cpu-bench-runner run int                    # 싱글 스레드 (성능 코어 고정)
cpu-bench-runner run int --mt               # 모든 논리 코어
cpu-bench-runner run int --threads 4 --duration-ms 3000 --runs 7 --json
cpu-bench-runner run mem --workload read --mt   # 메모리 읽기 대역폭, 모든 논리 코어
cpu-bench-runner run mem --workload latency --size 64M
cpu-bench-runner sweep mem --workload latency   # 16 KiB → 512 MiB 지연 곡선
```

| 옵션 | 기본값 | 설명 |
|---|---|---|
| `--threads N` / `--mt` | 1 | 스레드 수 / 전체 논리 코어 |
| `--duration-ms N` | 2000 | 측정 1회 길이 |
| `--runs N` | 5 | 측정 횟수 (워밍업 1회 별도) |
| `--seed N` | 1 | 입력 데이터 seed |
| `--no-pin` | | 코어 고정 끄기 |
| `--json` | | JSON으로 출력 (제어 앱·자동화용) |
| `--workload W` | 모듈 기본값 | 측정 종류 (`mem`: read, write, copy, latency) |
| `--size N` | 모듈 기본값 | 작업 데이터 크기. `16K`, `256M`, `1G`처럼 씁니다 (1024 단위) |

`sweep`은 `--size` 대신 `--from`(기본 16K), `--to`(기본 512M)를 받고, 측정 1회 500 ms × 3회가 기본입니다.

종료 코드: `0` 정상, `1` 실행 오류, `2` 사용법 오류, `3` checksum 불일치

## 측정 전 환경 점검 (`check`)

점수가 낮게 나오는 흔한 원인을 측정 전에 확인합니다. **읽기만 하고 설정은 바꾸지 않습니다.**

| 항목 | 경고 조건 |
|---|---|
| 전원 | 배터리로 동작 중 (노트북은 배터리에서 클럭을 낮춤) |
| 전원 관리 옵션 / 전원 모드 | 절전, 또는 Windows 전원 모드가 "최고의 전원 효율" |
| 최대 프로세서 상태 | 100% 미만 (보통 터보 부스트가 꺼짐) |
| 유휴 상태 CPU 사용률 | 3% 초과 (그만큼 멀티스레드 점수가 떨어질 수 있음). 사용률이 높은 프로세스 상위 5개를 보여 줌 |

프로세스 사용률은 작업 관리자와 같은 방식(`NtQuerySystemInformation`)으로 읽어서, 일반 권한으로 열 수 없는 시스템 프로세스와 백신도 포함됩니다. 프로세스 목록은 화면에만 표시하고 어디에도 보내지 않습니다.

## 개인정보

러너는 아무것도 전송하지 않습니다. `info`의 메모리 정보는 SMBIOS에서 읽는데, 모듈의 시리얼 번호와 자산 태그는 읽지 않습니다. 결과 업로드 기능이 생기더라도 시리얼 번호, 컴퓨터 이름, 계정명, IP는 수집하지 않고, 보내기 전에 전송 내용을 보여 주고 동의를 받을 예정입니다.
