# AI Office 화면 앱 (Wails v2 + Svelte 5)

명령줄 도구(`aio-run`)와 같은 오케스트레이터를 화면에서 씁니다.

- **직원 관리**: 직원마다 이름, 역할(PM / 개발자 / QA), 모델, 추가 지시, AI 계정 연결을 정합니다.
  - *이 PC 기본 로그인*: 지금 PC의 Claude Code에 로그인된 계정을 그대로 씁니다.
  - *전용 계정(구독 / Console)*: 직원마다 별도의 Claude Code 설정 폴더(`%LOCALAPPDATA%i-office\profiles\employee-N`)를 두고, **[로그인]**을 누르면 Claude Code의 공식 `claude auth login`이 브라우저 로그인을 엽니다. 로그인·콜백·자격 증명은 Claude Code가 처리하고 앱은 계정 정보를 저장하지 않습니다. 브라우저가 안 열리면 화면에 나오는 링크나 코드 붙여넣기로 마칠 수 있습니다.
  - 구독 로그인은 본인 PC에서 본인이 쓰는 용도로만 쓰세요. 다른 사람에게 배포하는 용도라면 Console(API 과금)을 쓰는 것이 Anthropic 정책에 맞습니다.
- **새 요청**: 저장소 폴더, 검사 명령, 보호 파일, 요청, 진행 방식(혼자 / 팀 / 팀 + QA 리뷰), 역할별 참여 직원, 예산
- **요청 화면**: 단계별 표(담당·상태·비용·시간), 실시간 진행 기록, 변경 내용(diff), 보고서
- **승인하고 반영**: 검증을 통과한 요청만. 요청 브랜치를 원래 저장소의 현재 브랜치에 merge합니다(충돌이 나면 되돌리고 알림).
- **폐기**: 작업 폴더와 브랜치를 지웁니다.
- **중지**: 실행 중인 AI 프로세스를 모두 종료합니다.

기록은 `%LOCALAPPDATA%\ai-office\aio.db`에 남고, 앱이 꺼진 동안 끝나지 못한 요청은 다음 실행 때 `INTERRUPTED`로 표시됩니다.

## 빌드
```
go install github.com/wailsapp/wails/v2/cmd/wails@v2.17.1
cd ai-office\app
wails build        # build\bin\ai-office.exe
```
