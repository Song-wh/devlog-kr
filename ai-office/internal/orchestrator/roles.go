package orchestrator

import (
	"fmt"
	"strings"
)

// planSchema is the JSON the PM must return.
const planSchema = `{"type":"object","properties":{` +
	`"summary":{"type":"string"},` +
	`"tasks":{"type":"array","minItems":1,"maxItems":5,"items":{"type":"object","properties":{` +
	`"title":{"type":"string"},` +
	`"instructions":{"type":"string"},` +
	`"files":{"type":"array","items":{"type":"string"}}` +
	`},"required":["title","instructions","files"]}}` +
	`},"required":["summary","tasks"]}`

type plan struct {
	Summary string     `json:"summary"`
	Tasks   []planTask `json:"tasks"`
}

type planTask struct {
	Title        string   `json:"title"`
	Instructions string   `json:"instructions"`
	Files        []string `json:"files"`
}

// teamRules is what every role must know about how the team works.
func teamRules(checks, protect []string) string {
	var b strings.Builder
	b.WriteString("팀 작업 규칙:\n")
	b.WriteString("- 개발자는 이 저장소 폴더 안의 파일을 읽고 쓰는 것만 할 수 있다. 명령 실행, 패키지 설치, 네트워크 접근, 커밋은 할 수 없다.\n")
	if len(checks) > 0 {
		b.WriteString("- 모든 작업이 끝나면 시스템이 다음 검사 명령을 실행한다. 이것이 모두 통과해야 완료다: ")
		b.WriteString(strings.Join(checks, " / "))
		b.WriteString("\n- 검사에 실패하면 실패 출력과 함께 개발자에게 수정 작업이 다시 온다.\n")
	}
	if len(protect) > 0 {
		b.WriteString("- 다음 파일은 고칠 수 없다(고치면 시스템이 되돌리고 실패로 처리한다): ")
		b.WriteString(strings.Join(protect, ", "))
		b.WriteString("\n")
	}
	return b.String()
}

func pmSystemPrompt(checks, protect []string) string {
	return "너는 소프트웨어 팀의 PM이다. 사용자 요청을 분석해서 개발자가 순서대로 수행할 작업 목록을 만든다. " +
		"파일은 절대 고치지 않는다. 필요하면 저장소 파일을 읽어서 현재 상태를 파악한다.\n\n" +
		teamRules(checks, protect) + "\n" +
		"계획 규칙:\n" +
		"- 작업은 1~5개. 각 작업은 반드시 파일 변경으로 끝나야 한다. 분석·정리·테스트 실행·검토·커밋 같은 작업은 만들지 않는다(개발자가 할 수 없거나 시스템이 한다).\n" +
		"- 각 작업의 instructions는 다른 작업을 보지 않고도 수행할 수 있게 구체적으로 쓴다(바꿀 파일, 지켜야 할 동작, 주의할 경계 조건).\n" +
		"- files에는 그 작업에서 만들거나 고칠 파일 경로를 적는다.\n" +
		"- 작은 요청이면 작업 1개로 충분하다. 쓸데없이 나누지 않는다.\n" +
		"- summary는 계획을 한두 문장으로 요약한다. 모든 문장은 한국어."
}

func devSystemPrompt(checks, protect []string) string {
	return "너는 소프트웨어 팀의 개발자다. PM이 배정한 작업 하나만 수행한다. 다른 작업 범위는 건드리지 않는다.\n\n" +
		teamRules(checks, protect) + "\n" +
		"끝나면 무엇을 바꿨는지 한국어로 두세 문장으로 보고한다."
}

func pmPrompt(request string) string {
	return "사용자 요청:\n" + request
}

func devPrompt(request string, p *plan, idx int, done []string) string {
	var b strings.Builder
	fmt.Fprintf(&b, "사용자 요청:\n%s\n\nPM 계획 요약: %s\n\n전체 작업:\n", request, p.Summary)
	for i, t := range p.Tasks {
		mark := " "
		if i < idx {
			mark = "완료"
		} else if i == idx {
			mark = "지금"
		}
		fmt.Fprintf(&b, "%d. [%s] %s\n", i+1, mark, t.Title)
	}
	if len(done) > 0 {
		b.WriteString("\n앞 작업 보고:\n")
		for i, d := range done {
			fmt.Fprintf(&b, "- %d번: %s\n", i+1, oneLine(d, 400))
		}
	}
	t := p.Tasks[idx]
	fmt.Fprintf(&b, "\n지금 할 작업: %d. %s\n%s\n", idx+1, t.Title, t.Instructions)
	if len(t.Files) > 0 {
		fmt.Fprintf(&b, "대상 파일: %s\n", strings.Join(t.Files, ", "))
	}
	return b.String()
}

// reviewSchema is the JSON the reviewer must return.
const reviewSchema = `{"type":"object","properties":{` +
	`"approved":{"type":"boolean"},` +
	`"summary":{"type":"string"},` +
	`"issues":{"type":"array","items":{"type":"object","properties":{` +
	`"file":{"type":"string"},"problem":{"type":"string"},"fix":{"type":"string"}` +
	`},"required":["file","problem","fix"]}}` +
	`},"required":["approved","summary","issues"]}`

type review struct {
	Approved bool          `json:"approved"`
	Summary  string        `json:"summary"`
	Issues   []reviewIssue `json:"issues"`
}

type reviewIssue struct {
	File    string `json:"file"`
	Problem string `json:"problem"`
	Fix     string `json:"fix"`
}

func reviewSystemPrompt(checks, protect []string) string {
	return "너는 소프트웨어 팀의 QA 리뷰어다. 개발자가 바꾼 내용(diff)이 사용자 요청과 저장소의 명세 문서를 실제로 만족하는지 검토한다. " +
		"파일은 절대 고치지 않는다. 필요하면 저장소 파일(명세, 관련 코드)을 읽어서 확인한다.\n\n" +
		teamRules(checks, protect) + "\n" +
		"검토 규칙:\n" +
		"- 자동 검사는 이미 통과했다. 검사가 잡지 못하는 결함을 찾는다: 요청·명세의 요구사항 누락, 경계 조건 위반, 잘못된 동작.\n" +
		"- 코드 스타일, 이름, 주석, 취향 문제는 지적하지 않는다. 확실한 결함만 issues에 넣는다.\n" +
		"- 각 issue에는 어느 파일의 무엇이 왜 틀렸는지(가능하면 명세의 해당 문장이나 반례 입력)와 고치는 방법을 쓴다.\n" +
		"- 결함이 없으면 approved=true, issues는 빈 배열. 모든 문장은 한국어."
}

func reviewPrompt(request, diff string) string {
	return "사용자 요청:\n" + request + "\n\n개발자가 바꾼 내용(git diff):\n```diff\n" + diff + "\n```"
}

func (rv *review) failures() string {
	var b strings.Builder
	fmt.Fprintf(&b, "QA 리뷰에서 결함이 발견됐다: %s\n", rv.Summary)
	for i, is := range rv.Issues {
		fmt.Fprintf(&b, "%d. [%s] %s\n   고칠 방법: %s\n", i+1, is.File, is.Problem, is.Fix)
	}
	return b.String()
}

func fixPrompt(request string, round int, failures string) string {
	return fmt.Sprintf("사용자 요청:\n%s\n\n작업을 모두 마쳤지만 검증에서 문제가 나왔다(수정 %d회차). "+
		"아래 내용을 보고 원인을 찾아 코드를 고쳐라. 검사나 테스트를 고쳐서 통과시키지 말고 기능을 고친다.\n\n%s", request, round, failures)
}

func oneLine(s string, max int) string {
	s = strings.Join(strings.Fields(s), " ")
	if r := []rune(s); len(r) > max {
		return string(r[:max]) + "..."
	}
	return s
}
