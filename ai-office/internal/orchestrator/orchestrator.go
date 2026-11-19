// Package orchestrator runs one request through the team:
// PM plan → developer tasks in order → system checks → fix rounds → report.
// Every step is recorded in the store; limits (rounds, money, time) are
// enforced here, not left to the agents.
package orchestrator

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/Song-wh/devlog-kr/ai-office/internal/check"
	"github.com/Song-wh/devlog-kr/ai-office/internal/engine"
	"github.com/Song-wh/devlog-kr/ai-office/internal/store"
	"github.com/Song-wh/devlog-kr/ai-office/internal/workspace"
)

// Config for one request.
type Config struct {
	ID           string        `json:"-"` // request id; empty = NewID()
	Repo         string        `json:"repo"`
	Request      string        `json:"request"`
	Checks       []string      `json:"checks"`         // user-defined commands; all must pass
	Protect      []string      `json:"protect"`        // globs the agents may not change
	PMModel      string        `json:"pm_model"`       // engine model for the PM
	DevModel     string        `json:"dev_model"`      // engine model for the developer
	MaxFixRounds int           `json:"max_fix_rounds"` // fix tasks after failed checks
	BudgetUSD    float64       `json:"budget_usd"`     // whole request; runs stop when spent
	TaskTimeout  time.Duration `json:"task_timeout"`
	CheckTimeout time.Duration `json:"check_timeout"`
	Solo         bool          `json:"solo"`     // skip the PM: one task = the request itself
	Review       bool          `json:"review"`   // after checks pass, a QA agent reviews the diff (team mode only)
	QAModel      string        `json:"qa_model"` // engine model for the reviewer (default: DevModel)
	// Agents assigns an employee to a role ("pm", "dev", "qa"). Its model
	// and account override the fields above; missing roles use them as is.
	Agents map[string]Agent `json:"agents,omitempty"`
}

// Agent is the employee doing a role: which account, which model, and any
// extra instructions on top of the role's own.
type Agent struct {
	Name       string `json:"name"`
	Model      string `json:"model"`
	ProfileDir string `json:"-"` // engine account folder; empty = default login
	Prompt     string `json:"prompt,omitempty"`
}

// NewID returns a request id: local time to the millisecond, usable in a
// branch name and a folder name.
func NewID() string {
	t := time.Now()
	return t.Format("20060102-150405") + fmt.Sprintf("-%03d", t.Nanosecond()/1e6)
}

// Logger receives progress lines for display.
type Logger func(role, line string)

// Orchestrator holds the shared parts.
type Orchestrator struct {
	Engine  engine.Engine
	Store   *store.Store
	DataDir string
	Log     Logger
}

// Outcome is what Run returns.
type Outcome struct {
	RequestID string
	Status    string
	Reason    string
	Worktree  *workspace.Worktree
	Dir       string // request log directory
}

var errBudget = errors.New("budget exhausted")

type run struct {
	o     *Orchestrator
	cfg   Config
	id    string
	wt    *workspace.Worktree
	dir   string
	runNo int
}

// Run executes one request to the end and returns its outcome.
func (o *Orchestrator) Run(ctx context.Context, cfg Config) (*Outcome, error) {
	p, err := o.Prepare(cfg)
	if err != nil {
		return nil, err
	}
	return p.Execute(ctx)
}

// Prepared is a request whose worktree and database row already exist.
type Prepared struct {
	r *run
}

// ID of the prepared request.
func (p *Prepared) ID() string { return p.r.id }

// Prepare creates the worktree and records the request, so callers (the app)
// can show it before any agent starts. Execute runs it.
func (o *Orchestrator) Prepare(cfg Config) (*Prepared, error) {
	if cfg.MaxFixRounds < 0 {
		cfg.MaxFixRounds = 0
	}
	if cfg.BudgetUSD <= 0 {
		cfg.BudgetUSD = 2
	}
	id := cfg.ID
	if id == "" {
		id = NewID()
	}
	wt, err := workspace.Create(cfg.Repo, filepath.Join(o.DataDir, "worktrees"), id)
	if err != nil {
		return nil, err
	}
	r := &run{o: o, cfg: cfg, id: id, wt: wt, dir: filepath.Join(o.DataDir, "requests", id)}
	if err := os.MkdirAll(r.dir, 0o755); err != nil {
		return nil, err
	}
	cj, _ := json.Marshal(cfg)
	if err := o.Store.CreateRequest(store.Request{ID: id, Text: cfg.Request, Repo: wt.Repo, Worktree: wt.Dir,
		Branch: wt.Branch, Base: wt.Base, ConfigJSON: string(cj)}); err != nil {
		return nil, err
	}
	return &Prepared{r: r}, nil
}

// Execute runs a prepared request to the end.
func (p *Prepared) Execute(ctx context.Context) (*Outcome, error) {
	r, o, id, wt := p.r, p.r.o, p.r.id, p.r.wt
	o.log("system", fmt.Sprintf("request %s  worktree %s  branch %s", id, wt.Dir, wt.Branch))

	status, reason := r.execute(ctx)
	if ctx.Err() != nil {
		status, reason = store.ReqCancelled, "stopped by user"
	}
	if status == store.ReqDone {
		if err := r.commit(); err != nil {
			status, reason = store.ReqFailed, "commit: "+err.Error()
		}
	}
	if err := o.Store.FinishRequest(id, status, reason); err != nil {
		return nil, err
	}
	return &Outcome{RequestID: id, Status: status, Reason: reason, Worktree: wt, Dir: r.dir}, nil
}

func (o *Orchestrator) log(role, line string) {
	if o.Log != nil {
		o.Log(role, line)
	}
}

// execute returns the final request status and reason.
func (r *run) execute(ctx context.Context) (string, string) {
	p, err := r.plan(ctx)
	if err != nil {
		return store.ReqFailed, "plan: " + err.Error()
	}

	// Developer tasks, in order.
	var reports []string
	for i, t := range p.Tasks {
		taskID, err := r.o.Store.AddTask(r.id, i+1, "work", "dev", t.Title, t.Instructions)
		if err != nil {
			return store.ReqFailed, err.Error()
		}
		res, err := r.runTask(ctx, taskID, "dev", r.cfg.DevModel, devPrompt(r.cfg.Request, p, i, reports), devSystemPrompt(r.cfg.Checks, r.cfg.Protect), "")
		if err != nil {
			return store.ReqFailed, fmt.Sprintf("task %d: %v", i+1, err)
		}
		reports = append(reports, res.Text)
		r.o.Store.SetTaskStatus(taskID, store.Completed, "")
	}

	// Checks and fix rounds.
	for round := 0; ; round++ {
		failures, err := r.verify(ctx, round)
		if err != nil {
			return store.ReqFailed, err.Error()
		}
		if failures == "" && r.cfg.Review && !r.cfg.Solo {
			if failures, err = r.review(ctx, round); err != nil {
				return store.ReqFailed, "review: " + err.Error()
			}
		}
		if failures == "" {
			return store.ReqDone, ""
		}
		if round >= r.cfg.MaxFixRounds {
			return store.ReqFailed, fmt.Sprintf("verification still failing after %d fix round(s)", round)
		}
		taskID, err := r.o.Store.AddTask(r.id, len(p.Tasks)+round+1, "fix", "dev",
			fmt.Sprintf("검사 실패 수정 %d회차", round+1), failures)
		if err != nil {
			return store.ReqFailed, err.Error()
		}
		r.o.Store.SetTaskStatus(taskID, store.Revision, "checks failed")
		if _, err := r.runTask(ctx, taskID, "dev", r.cfg.DevModel, fixPrompt(r.cfg.Request, round+1, failures),
			devSystemPrompt(r.cfg.Checks, r.cfg.Protect), ""); err != nil {
			return store.ReqFailed, fmt.Sprintf("fix %d: %v", round+1, err)
		}
		r.o.Store.SetTaskStatus(taskID, store.Completed, "")
	}
}

// plan asks the PM for tasks (or, in solo mode, uses the request as one task).
func (r *run) plan(ctx context.Context) (*plan, error) {
	if r.cfg.Solo {
		p := &plan{Summary: "(solo) 요청 전체를 작업 하나로 수행", Tasks: []planTask{{Title: "요청 수행", Instructions: r.cfg.Request}}}
		r.o.Store.SetPlanSummary(r.id, p.Summary)
		return p, nil
	}
	taskID, err := r.o.Store.AddTask(r.id, 0, "plan", "pm", "계획 수립", r.cfg.Request)
	if err != nil {
		return nil, err
	}
	res, err := r.runTask(ctx, taskID, "pm", r.cfg.PMModel, pmPrompt(r.cfg.Request), pmSystemPrompt(r.cfg.Checks, r.cfg.Protect), planSchema)
	if err != nil {
		return nil, err
	}
	var p plan
	if err := json.Unmarshal(res.Structured, &p); err != nil || len(p.Tasks) == 0 {
		r.o.Store.SetTaskStatus(taskID, store.Failed, "plan was not valid JSON")
		return nil, fmt.Errorf("PM did not return a valid plan: %v", err)
	}
	if len(p.Tasks) > 5 {
		p.Tasks = p.Tasks[:5]
	}
	r.o.Store.SetPlanSummary(r.id, p.Summary)
	r.o.Store.SetTaskStatus(taskID, store.Completed, fmt.Sprintf("%d task(s)", len(p.Tasks)))
	r.o.log("pm", "plan: "+p.Summary)
	for i, t := range p.Tasks {
		r.o.log("pm", fmt.Sprintf("  %d. %s  [%s]", i+1, t.Title, strings.Join(t.Files, ", ")))
	}
	return &p, nil
}

// runTask runs the engine once for a task and records everything.
func (r *run) runTask(ctx context.Context, taskID int64, role, model, prompt, system, schema string) (engine.Result, error) {
	st := r.o.Store
	spent, _ := st.CostSoFar(r.id)
	remaining := r.cfg.BudgetUSD - spent
	if remaining < 0.01 {
		st.SetTaskStatus(taskID, store.Failed, "budget exhausted")
		return engine.Result{}, errBudget
	}
	ag := r.cfg.Agents[role]
	if ag.Model != "" {
		model = ag.Model
	}
	if ag.Prompt != "" {
		system += "\n\n이 직원에게만 주는 추가 지시:\n" + ag.Prompt
	}
	who := role
	if ag.Name != "" {
		who = ag.Name
	}
	st.SetTaskStatus(taskID, store.Assigned, who)
	st.SetTaskStatus(taskID, store.InProgress, "")
	r.runNo++
	logDir := filepath.Join(r.dir, fmt.Sprintf("%02d-%s", r.runNo, role))
	spec := engine.Spec{
		Prompt: prompt, Dir: r.wt.Dir, Model: model, MaxBudgetUSD: remaining,
		Timeout: r.cfg.TaskTimeout, LogDir: logDir, SystemPrompt: system, JSONSchema: schema,
		ProfileDir: ag.ProfileDir,
	}
	if role == "pm" || role == "qa" {
		spec.Tools = []string{"Read", "Glob", "Grep"} // planners and reviewers only read
	}
	start := time.Now()
	res, runErr := r.o.Engine.Run(ctx, spec, func(ev engine.Event) {
		switch ev.Kind {
		case engine.EventToolUse:
			r.o.log(role, fmt.Sprintf("%-6s %s", ev.Tool, rel(r.wt.Dir, ev.Target)))
		case engine.EventToolError:
			r.o.log(role, "DENIED/ERROR "+oneLine(ev.Text, 140))
		}
	})
	st.AddRun(store.Run{
		RequestID: r.id, TaskID: taskID, Role: role, Engine: r.o.Engine.Name(), Model: model, Status: string(res.Status),
		Turns: res.Turns, ToolCalls: res.ToolCalls, ToolErrors: res.ToolErrors, Denials: res.Denials,
		InputTokens: res.Usage.InputTokens, OutputTokens: res.Usage.OutputTokens,
		CacheRead: res.Usage.CacheReadTokens, CacheWrite: res.Usage.CacheCreateTokens,
		CostUSD: res.Usage.CostUSD, CostEstimated: res.Usage.CostEstimated, Agent: ag.Name,
		Duration: res.Duration, LogDir: logDir, ResultText: res.Text, StartedAt: start,
	})
	r.o.log(role, fmt.Sprintf("done: %s, %.0fs, $%.4f", res.Status, res.Duration.Seconds(), res.Usage.CostUSD))
	st.SetTaskResult(taskID, 1, res.Text)
	if runErr != nil {
		st.SetTaskStatus(taskID, store.Failed, runErr.Error())
		return res, runErr
	}
	switch res.Status {
	case engine.StatusSuccess:
		return res, nil
	case engine.StatusCancelled:
		st.SetTaskStatus(taskID, store.Cancelled, "")
		return res, ctx.Err()
	default:
		note := fmt.Sprintf("engine %s (%s)", res.Status, res.Subtype)
		st.SetTaskStatus(taskID, store.Failed, note)
		return res, errors.New(note)
	}
}

// verify reverts changes to protected files, runs every check, and returns
// a failure description for the developer ("" when everything passed).
func (r *run) verify(ctx context.Context, round int) (string, error) {
	var fails []string

	changes, err := workspace.Status(r.wt.Dir)
	if err != nil {
		return "", err
	}
	var touched []string
	for _, c := range changes {
		if workspace.MatchAny(r.cfg.Protect, c.Path) {
			touched = append(touched, c.Path)
			if err := r.wt.Restore(c.Path); err != nil {
				return "", fmt.Errorf("restore %s: %w", c.Path, err)
			}
		}
	}
	if len(touched) > 0 {
		r.o.log("qa", "protected file(s) changed, reverted: "+strings.Join(touched, ", "))
		fails = append(fails, "고치면 안 되는 파일을 바꿔서 시스템이 되돌렸다: "+strings.Join(touched, ", ")+
			". 이 파일은 그대로 두고 다른 코드를 고쳐서 해결하라.")
	}

	for i, cmd := range r.cfg.Checks {
		logPath := filepath.Join(r.dir, fmt.Sprintf("check-r%d-%d.txt", round, i+1))
		res := check.Run(ctx, r.wt.Dir, cmd, r.cfg.CheckTimeout, logPath)
		r.o.Store.AddCheck(store.Check{RequestID: r.id, Round: round, Command: cmd, ExitCode: res.ExitCode,
			TimedOut: res.TimedOut, Duration: res.Duration, ProtectedChanges: strings.Join(touched, ","), LogPath: logPath})
		state := "PASS"
		if !res.Passed() {
			state = "FAIL"
			if res.TimedOut {
				state = "TIMEOUT"
			}
			fails = append(fails, fmt.Sprintf("검사 `%s` 실패(exit %d):\n%s", cmd, res.ExitCode, check.Tail(res.Output, 3000)))
		}
		r.o.log("qa", fmt.Sprintf("check %s (%.1fs): %s", state, res.Duration.Seconds(), cmd))
	}
	return strings.Join(fails, "\n\n"), nil
}

// maxReviewDiff caps the diff sent to the reviewer; it can read files itself.
const maxReviewDiff = 40000

// review asks the QA agent to judge the diff against the request.
// Returns "" when approved, otherwise the issues for the developer.
func (r *run) review(ctx context.Context, round int) (string, error) {
	diff, err := r.wt.Diff()
	if err != nil {
		return "", err
	}
	if len(diff) > maxReviewDiff {
		diff = diff[:maxReviewDiff] + "\n... (diff truncated; read the files for the rest)"
	}
	model := r.cfg.QAModel
	if model == "" {
		model = r.cfg.DevModel
	}
	taskID, err := r.o.Store.AddTask(r.id, 100+round, "review", "qa", fmt.Sprintf("QA 리뷰 %d회차", round+1), "")
	if err != nil {
		return "", err
	}
	res, err := r.runTask(ctx, taskID, "qa", model, reviewPrompt(r.cfg.Request, diff), reviewSystemPrompt(r.cfg.Checks, r.cfg.Protect), reviewSchema)
	if err != nil {
		return "", err
	}
	var rv review
	if err := json.Unmarshal(res.Structured, &rv); err != nil {
		r.o.Store.SetTaskStatus(taskID, store.Failed, "review was not valid JSON")
		return "", fmt.Errorf("reviewer did not return a valid review: %v", err)
	}
	if rv.Approved && len(rv.Issues) == 0 {
		r.o.Store.SetTaskStatus(taskID, store.Completed, "approved")
		r.o.log("qa", "review APPROVED: "+oneLine(rv.Summary, 140))
		return "", nil
	}
	r.o.Store.SetTaskStatus(taskID, store.Completed, fmt.Sprintf("%d issue(s)", len(rv.Issues)))
	r.o.log("qa", fmt.Sprintf("review: %d issue(s): %s", len(rv.Issues), oneLine(rv.Summary, 140)))
	for _, is := range rv.Issues {
		r.o.log("qa", "  - "+is.File+": "+oneLine(is.Problem, 120))
	}
	return rv.failures(), nil
}

// commit records the team's work on the request branch so it can be
// reviewed with git and merged only when the user approves.
func (r *run) commit() error {
	changes, err := workspace.Status(r.wt.Dir)
	if err != nil || len(changes) == 0 {
		return err
	}
	return r.wt.Commit("AI Office: " + oneLine(r.cfg.Request, 60))
}

func rel(dir, p string) string {
	if p == "" || !filepath.IsAbs(p) {
		return p
	}
	if r, err := filepath.Rel(dir, p); err == nil && !strings.HasPrefix(r, "..") {
		return r
	}
	return p
}
