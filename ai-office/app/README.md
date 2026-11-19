# AI Office 화면 앱 (Wails v2 + Svelte 5)

명령줄 도구(`aio-run`)와 같은 오케스트레이터를 화면에서 씁니다.

- **새 요청**: 저장소 폴더, 검사 명령, 보호 파일, 요청, 진행 방식(혼자 / 팀 / 팀 + QA 리뷰), 모델, 예산
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
