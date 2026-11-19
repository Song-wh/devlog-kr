package orchestrator

// Two more meeting formats:
//
// Draft-critique (planning): the first member writes a draft, the others
// critique it, the author revises; it ends when every critic approves or the
// round limit is reached.
//
// Discussion: everyone states a position, then reads the others and answers
// them for a fixed number of rounds; a moderator writes the conclusion.

import (
	"context"
	"encoding/json"
	"fmt"
	"strings"

	"github.com/song-circle/devlog-kr/ai-office/internal/engine"
	"github.com/song-circle/devlog-kr/ai-office/internal/store"
)

const critiqueSchema = `{"type":"object","properties":{` +
	`"approve":{"type":"boolean"},"summary":{"type":"string"},` +
	`"points":{"type":"array","items":{"type":"object","properties":{` +
	`"point":{"type":"string"},"why":{"type":"string"},"suggestion":{"type":"string"}` +
	`},"required":["point","why","suggestion"]}}` +
	`},"required":["approve","summary","points"]}`

const positionSchema = `{"type":"object","properties":{` +
	`"position":{"type":"string"},` +
	`"reasons":{"type":"array","items":{"type":"string"}},` +
	`"changed":{"type":"boolean"},` +
	`"agree_with":{"type":"array","items":{"type":"string"}},` +
	`"disagree":{"type":"array","items":{"type":"object","properties":{"name":{"type":"string"},"why":{"type":"string"}},"required":["name","why"]}}` +
	`},"required":["position","reasons","changed","agree_with","disagree"]}`

type critique struct {
	Approve bool   `json:"approve"`
	Summary string `json:"summary"`
	Points  []struct {
		Point      string `json:"point"`
		Why        string `json:"why"`
		Suggestion string `json:"suggestion"`
	} `json:"points"`
}

type position struct {
	Position  string   `json:"position"`
	Reasons   []string `json:"reasons"`
	Changed   bool     `json:"changed"`
	AgreeWith []string `json:"agree_with"`
	Disagree  []struct {
		Name string `json:"name"`
		Why  string `json:"why"`
	} `json:"disagree"`
}

func (r *run) rounds() int {
	if r.cfg.MeetingRounds > 0 {
		return r.cfg.MeetingRounds
	}
	return 2
}

func (r *run) startMeeting(kind, title, subject string, panel []Agent) (int64, error) {
	st := r.o.Store
	taskID, err := st.AddTask(r.id, 200+len(r.meetings), "meeting", "pm", title, subject)
	if err != nil {
		return 0, err
	}
	r.meetings = append(r.meetings, taskID)
	st.SetTaskStatus(taskID, store.Assigned, fmt.Sprintf("%d명", len(panel)))
	st.SetTaskStatus(taskID, store.InProgress, "")
	names := make([]string, len(panel))
	for i, ag := range panel {
		names[i] = ag.Name
	}
	r.o.log("meeting", fmt.Sprintf("%s (%s): %s", title, kind, strings.Join(names, ", ")))
	return taskID, nil
}

func (r *run) note(taskID int64, meeting string, round int, sender, kind, content string, data any, cost float64) {
	r.o.Store.AddMessage(store.Message{RequestID: r.id, TaskID: taskID, Meeting: meeting, Round: round,
		Sender: sender, Kind: kind, Content: content, DataJSON: mustJSON(data), CostUSD: cost})
}

// draftText is what the others see of a draft: the JSON for structured
// drafts, otherwise the text.
func draftText(res engine.Result) string {
	if len(res.Structured) > 0 {
		return string(res.Structured)
	}
	return res.Text
}

// draftCritique runs a planning meeting. authorSystem is the author's role;
// schema (optional) is the draft's JSON format. Returns the final draft.
func (r *run) draftCritique(ctx context.Context, panel []Agent, title, subject, authorSystem, schema string) (engine.Result, error) {
	taskID, err := r.startMeeting("draft-critique", title, subject, panel)
	if err != nil {
		return engine.Result{}, err
	}
	author, critics := panel[0], panel[1:]
	model := r.cfg.PMModel
	fail := func(err error) (engine.Result, error) {
		r.o.Store.SetTaskStatus(taskID, store.Failed, err.Error())
		return engine.Result{}, err
	}

	draft, err := r.runAgent(ctx, taskID, "pm", author, model, subject, authorSystem+draftRule, schema, false)
	if err != nil {
		return fail(err)
	}
	r.note(taskID, "draft-critique", 0, author.Name, "draft", oneLine(draftText(draft), 300), map[string]string{"draft": draftText(draft)}, draft.Usage.CostUSD)

	for round := 1; round <= r.rounds(); round++ {
		cs := make([]critique, len(critics))
		err = parallel(critics, func(i int, ag Agent) error {
			res, err := r.runAgent(ctx, taskID, "pm", ag, model, critiquePrompt(subject, author.Name, draftText(draft)),
				critiqueSystemPrompt(), critiqueSchema, false)
			if err != nil {
				return fmt.Errorf("%s: %w", ag.Name, err)
			}
			if err := json.Unmarshal(res.Structured, &cs[i]); err != nil {
				return fmt.Errorf("%s: critique was not valid JSON: %v", ag.Name, err)
			}
			verdict := "수정 요청"
			if cs[i].Approve {
				verdict = "승인"
			}
			r.note(taskID, "draft-critique", round, ag.Name, "critique", fmt.Sprintf("%s · %s (지적 %d개)", verdict, cs[i].Summary, len(cs[i].Points)), cs[i], res.Usage.CostUSD)
			return nil
		})
		if err != nil {
			return fail(err)
		}
		approved := 0
		for _, c := range cs {
			if c.Approve {
				approved++
			}
		}
		r.o.log("meeting", fmt.Sprintf("round %d: %d/%d approve", round, approved, len(critics)))
		if approved == len(critics) {
			break
		}
		revised, err := r.runAgent(ctx, taskID, "pm", author, model, revisePrompt(subject, draftText(draft), critics, cs),
			authorSystem+reviseRule, schema, false)
		if err != nil {
			return fail(err)
		}
		draft = revised
		r.note(taskID, "draft-critique", round, author.Name, "revision", oneLine(draftText(draft), 300), map[string]string{"draft": draftText(draft)}, draft.Usage.CostUSD)
	}
	r.o.Store.SetTaskStatus(taskID, store.Completed, "")
	return draft, nil
}

// discussion runs a meeting where members state and defend positions; the
// first member also moderates and writes the conclusion (markdown).
func (r *run) discussion(ctx context.Context, panel []Agent, subject string) (string, error) {
	taskID, err := r.startMeeting("discussion", "회의", subject, panel)
	if err != nil {
		return "", err
	}
	model := r.cfg.DevModel
	latest := make([]position, len(panel))
	var transcript strings.Builder
	for round := 1; round <= r.rounds(); round++ {
		prev := append([]position(nil), latest...)
		err = parallel(panel, func(i int, ag Agent) error {
			prompt := subject
			if round > 1 {
				prompt += "\n\n지금까지 다른 참석자들의 입장:\n" + othersPositions(panel, prev, i)
			}
			res, err := r.runAgent(ctx, taskID, "pm", ag, model, prompt, discussionSystemPrompt(round, ag.Name), positionSchema, false)
			if err != nil {
				return fmt.Errorf("%s: %w", ag.Name, err)
			}
			if err := json.Unmarshal(res.Structured, &latest[i]); err != nil {
				return fmt.Errorf("%s: position was not valid JSON: %v", ag.Name, err)
			}
			kind := "opinion"
			if round > 1 {
				kind = "rebuttal"
			}
			r.note(taskID, "discussion", round, ag.Name, kind, latest[i].Position, latest[i], res.Usage.CostUSD)
			return nil
		})
		if err != nil {
			r.o.Store.SetTaskStatus(taskID, store.Failed, err.Error())
			return "", err
		}
		fmt.Fprintf(&transcript, "\n### %d라운드\n", round)
		for i, ag := range panel {
			p := latest[i]
			fmt.Fprintf(&transcript, "- **%s**: %s", ag.Name, p.Position)
			if p.Changed {
				transcript.WriteString(" (입장 바꿈)")
			}
			for _, d := range p.Disagree {
				fmt.Fprintf(&transcript, "\n  - %s에게 반대: %s", d.Name, d.Why)
			}
			transcript.WriteString("\n")
		}
	}
	mod := panel[0]
	res, err := r.runAgent(ctx, taskID, "pm", mod, model, subject+"\n\n회의 기록:"+transcript.String(), moderatorSystemPrompt(), "", false)
	if err != nil {
		r.o.Store.SetTaskStatus(taskID, store.Failed, err.Error())
		return "", err
	}
	r.note(taskID, "discussion", r.rounds()+1, mod.Name+" (사회)", "conclusion", oneLine(res.Text, 300), map[string]string{"conclusion": res.Text}, res.Usage.CostUSD)
	r.o.Store.SetTaskStatus(taskID, store.Completed, "")
	return res.Text + "\n\n---\n## 회의 기록\n" + transcript.String(), nil
}

func othersPositions(panel []Agent, ps []position, self int) string {
	var b strings.Builder
	for i, ag := range panel {
		if i == self {
			continue
		}
		fmt.Fprintf(&b, "- %s: %s\n  근거: %s\n", ag.Name, ps[i].Position, strings.Join(ps[i].Reasons, " / "))
	}
	if self >= 0 {
		fmt.Fprintf(&b, "- (지난 라운드의 너, %s): %s\n", panel[self].Name, ps[self].Position)
	}
	return b.String()
}

const draftRule = "\n\n이번 회의 규칙(초안): 다른 직원들이 이 초안을 비판하고, 너는 그 비판을 받아 고친다. 처음부터 근거를 분명히 쓴다."

const reviseRule = "\n\n이번 회의 규칙(수정): 비판을 하나씩 검토해서, 맞는 지적은 반영하고 틀린 지적은 반영하지 않는다. " +
	"반영하지 않은 지적이 있으면 문서 끝(구조화 형식이면 summary)에 이유를 짧게 남긴다. 고친 전체본을 다시 낸다."

func critiquePrompt(subject, author, draft string) string {
	return subject + "\n\n" + author + "가 쓴 초안:\n" + draft
}

func revisePrompt(subject, draft string, critics []Agent, cs []critique) string {
	var b strings.Builder
	b.WriteString(subject)
	b.WriteString("\n\n네가 쓴 지난 초안:\n")
	b.WriteString(draft)
	b.WriteString("\n\n받은 비판:\n")
	for i, c := range cs {
		if c.Approve && len(c.Points) == 0 {
			fmt.Fprintf(&b, "- %s: 승인\n", critics[i].Name)
			continue
		}
		for _, p := range c.Points {
			fmt.Fprintf(&b, "- %s: %s (이유: %s / 제안: %s)\n", critics[i].Name, p.Point, p.Why, p.Suggestion)
		}
	}
	return b.String()
}

func critiqueSystemPrompt() string {
	return readOnlyRule + "너는 기획 회의의 검토자다. 다른 직원이 쓴 기획 초안을 사용자 요청과 실제 코드에 비추어 비판한다. " +
		"빠진 요구사항, 실제 코드와 맞지 않는 가정, 작업 순서·완료 조건의 문제, 더 단순한 방법이 있는지 본다. " +
		"사소한 표현 문제는 지적하지 않는다. 고칠 것이 없으면 approve=true, points는 비운다."
}

func discussionSystemPrompt(round int, name string) string {
	s := readOnlyRule + "너는 회의 참석자 " + name + "다. 사용자의 질문(결정할 문제)에 대해 실제 코드와 사실에 근거해 입장을 낸다. "
	if round == 1 {
		return s + "1라운드: 다른 참석자 의견을 보기 전에 네 입장과 근거를 낸다. changed=false, agree_with·disagree는 비운다."
	}
	return s + "다른 참석자의 입장을 읽고 동의하는 사람(agree_with)과 반대하는 이유(disagree)를 밝힌다. " +
		"설득되었으면 입장을 바꾸고 changed=true. 근거 없는 고집도, 근거 없는 동조도 하지 않는다."
}

func moderatorSystemPrompt() string {
	return readOnlyRule + "너는 회의 사회자다. 회의 기록을 바탕으로 결론을 정리한다. 새 주장을 덧붙이지 않는다.\n" +
		"구성: 1. 결론(한두 문장) 2. 합의된 점 3. 남은 이견(누가, 무엇에, 왜) 4. 사용자가 정해야 할 선택지와 각각의 장단점 5. 다음 행동"
}
