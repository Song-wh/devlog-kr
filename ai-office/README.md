# AI Office (가칭)

요청 하나를 던지면 AI 팀(PM·개발·QA)이 작업을 나누고, 실제로 내 프로젝트 코드를 고치고, 테스트로 검증한 뒤 **내가 승인해야만 반영되는** Windows 앱입니다. 팀이 쓴 비용과 시간, 실패 횟수를 전부 기록해서 "AI 팀이 정말 혼자보다 나은가"를 숫자로 확인하는 것이 목표입니다.

> 상태: **개발 중** — 아직 화면 앱은 없고, 작업 엔진을 시험하는 명령줄 도구만 있습니다.

## 지금 있는 것: `aio-probe` (엔진 실험 도구)

코딩 에이전트(현재 Claude Code)를 내 저장소의 **별도 작업 폴더(git worktree)와 별도 브랜치**에서 실행합니다. 무엇을 했는지, 토큰과 비용이 얼마나 들었는지, 어떤 파일이 바뀌었는지 보여 줍니다.

- 원래 작업 폴더와 브랜치는 건드리지 않습니다(실행 전후를 비교해서 확인).
- 에이전트는 작업 폴더 안의 파일만 읽고 쓸 수 있고, 셸 명령은 실행할 수 없습니다.
- 시간 제한이나 Ctrl+C로 멈추면 에이전트가 띄운 프로세스까지 모두 종료합니다(Windows Job Object).
- 끝난 뒤 내가 정한 검사 명령(빌드·테스트)을 실행해 통과 여부를 판정합니다. AI가 "완료했습니다"라고 말하는 것과 실제로 테스트가 통과하는 것은 다른 문제라서입니다.

### 필요한 것
- Windows 10/11, Go 1.22 이상, Git
- Claude Code 설치 및 로그인(`claude` 명령이 동작해야 함)

### 빌드와 실행
```
cd ai-office
go build -o bin/aio-probe.exe ./cmd/aio-probe

# 데모 프로젝트를 git 저장소로 복사
xcopy /E /I demos\textstats C:\aio-lab\textstats
cd C:\aio-lab\textstats && git init && git add -A && git commit -m start

aio-probe -repo C:\aio-lab\textstats -check "go test ./..." ^
  -prompt "stats.go의 Count 함수를 구현해서 테스트가 통과하게 해줘. 테스트 파일은 고치지 마."
```

주요 옵션: `-model`(기본 sonnet), `-timeout`(기본 10분), `-budget`(실행당 최대 달러, 기본 1.0), `-check`(검사 명령), `-cleanup`(끝나면 작업 폴더와 브랜치 삭제), `-data`(작업 폴더·기록 위치, 기본 `%LOCALAPPDATA%\ai-office`).

실행 기록은 `runs\<시각>\`에 남습니다: 원본 이벤트(`stream.jsonl`), 변경 내용(`diff.patch`), 검사 출력(`check.txt`), 요약(`summary.json`).

비용은 API 표준 가격 기준 추정치입니다. 구독 요금제로 쓰는 경우 실제 청구액과 다릅니다.

## 앞으로
1. PM이 요청을 작업과 완료 조건으로 나누고, 개발 → QA 검증 → 재작업을 반복하는 흐름
2. 화면 앱(Wails), 승인 화면(변경 내용·요약·테스트 결과·비용)
3. 두 번째 엔진(Codex), 혼자 vs 팀 측정 비교
