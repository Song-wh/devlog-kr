package main

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"

	"github.com/wailsapp/wails/v2/pkg/runtime"

	"github.com/Song-wh/devlog-kr/ai-office/internal/engine/claudecode"
	"github.com/Song-wh/devlog-kr/ai-office/internal/orchestrator"
	"github.com/Song-wh/devlog-kr/ai-office/internal/report"
	"github.com/Song-wh/devlog-kr/ai-office/internal/store"
	"github.com/Song-wh/devlog-kr/ai-office/internal/workspace"
)

// App is bound to the frontend. Every method does real work on the local
// repository and database; nothing here is simulated.
type App struct {
	ctx     context.Context
	dataDir string
	st      *store.Store
	initErr error

	mu      sync.Mutex
	running map[string]context.CancelFunc
}

func NewApp() *App {
	return &App{running: map[string]context.CancelFunc{}}
}

func (a *App) startup(ctx context.Context) {
	a.ctx = ctx
	a.dataDir = filepath.Join(os.Getenv("LOCALAPPDATA"), "ai-office")
	if err := os.MkdirAll(a.dataDir, 0o755); err != nil {
		a.initErr = err
		return
	}
	st, err := store.Open(filepath.Join(a.dataDir, "aio.db"))
	if err != nil {
		a.initErr = err
		return
	}
	a.st = st
	st.MarkInterrupted() // requests left running by a previous session
}

func (a *App) shutdown(ctx context.Context) {
	a.mu.Lock()
	for _, cancel := range a.running {
		cancel()
	}
	a.mu.Unlock()
	if a.st != nil {
		a.st.Close()
	}
}

func (a *App) ready() error {
	if a.initErr != nil {
		return a.initErr
	}
	if a.st == nil {
		return errors.New("database not open")
	}
	return nil
}

// ---- project settings ----

// Project is what the user registers once per repository.
type Project struct {
	Repo    string   `json:"repo"`
	Checks  []string `json:"checks"`
	Protect []string `json:"protect"`
}

// LastProject returns the project used last time (empty if none).
func (a *App) LastProject() (Project, error) {
	var p Project
	if err := a.ready(); err != nil {
		return p, err
	}
	v, err := a.st.Setting("last_project")
	if err != nil || v == "" {
		return p, err
	}
	err = json.Unmarshal([]byte(v), &p)
	return p, err
}

// PickFolder opens a folder dialog and returns the git repository root.
func (a *App) PickFolder() (string, error) {
	dir, err := runtime.OpenDirectoryDialog(a.ctx, runtime.OpenDialogOptions{Title: "작업할 저장소 폴더"})
	if err != nil || dir == "" {
		return "", err
	}
	top, err := workspace.TopLevel(dir)
	if err != nil {
		return "", fmt.Errorf("git 저장소가 아닙니다: %s", dir)
	}
	return top, nil
}

// ---- requests ----

// StartInput is the new-request form.
type StartInput struct {
	Project   Project `json:"project"`
	Request   string  `json:"request"`
	Mode      string  `json:"mode"` // solo / team / review
	Model     string  `json:"model"`
	BudgetUSD float64 `json:"budgetUsd"`
	FixRounds int     `json:"fixRounds"`
}

// LogLine is pushed to the frontend as the "log" event.
type LogLine struct {
	ID      string  `json:"id"`
	Seconds float64 `json:"t"`
	Role    string  `json:"role"`
	Line    string  `json:"line"`
}

// Start begins a request in the background and returns its id at once.
// Progress arrives as "log" events; the end as a "done" event.
func (a *App) Start(in StartInput) (string, error) {
	if err := a.ready(); err != nil {
		return "", err
	}
	if strings.TrimSpace(in.Request) == "" {
		return "", errors.New("요청 내용을 적어 주세요")
	}
	top, err := workspace.TopLevel(in.Project.Repo)
	if err != nil {
		return "", fmt.Errorf("git 저장소가 아닙니다: %s", in.Project.Repo)
	}
	in.Project.Repo = top
	in.Project.Checks = clean(in.Project.Checks)
	in.Project.Protect = clean(in.Project.Protect)
	if b, err := json.Marshal(in.Project); err == nil {
		a.st.SetSetting("last_project", string(b))
	}
	if in.Model == "" {
		in.Model = "sonnet"
	}
	if in.BudgetUSD <= 0 {
		in.BudgetUSD = 2
	}

	id := orchestrator.NewID()
	ctx, cancel := context.WithCancel(context.Background())
	a.mu.Lock()
	a.running[id] = cancel
	a.mu.Unlock()

	start := time.Now()
	o := &orchestrator.Orchestrator{
		Engine:  &claudecode.Engine{},
		Store:   a.st,
		DataDir: a.dataDir,
		Log: func(role, line string) {
			runtime.EventsEmit(a.ctx, "log", LogLine{ID: id, Seconds: time.Since(start).Seconds(), Role: role, Line: line})
		},
	}
	cfg := orchestrator.Config{
		ID: id, Repo: top, Request: in.Request, Checks: in.Project.Checks, Protect: in.Project.Protect,
		PMModel: in.Model, DevModel: in.Model, MaxFixRounds: in.FixRounds, BudgetUSD: in.BudgetUSD,
		TaskTimeout: 15 * time.Minute, CheckTimeout: 5 * time.Minute,
		Solo: in.Mode == "solo", Review: in.Mode == "review",
	}
	// Create the worktree and the database row now, so the request exists
	// (and setup errors reach the form) before this call returns.
	prep, err := o.Prepare(cfg)
	if err != nil {
		a.mu.Lock()
		delete(a.running, id)
		a.mu.Unlock()
		cancel()
		return "", err
	}
	go func() {
		defer func() {
			a.mu.Lock()
			delete(a.running, id)
			a.mu.Unlock()
			cancel()
		}()
		out, err := prep.Execute(ctx)
		ev := map[string]string{"id": id}
		if err != nil {
			ev["status"], ev["reason"] = store.ReqFailed, err.Error()
		} else {
			ev["status"], ev["reason"] = out.Status, out.Reason
			diffStat, _ := out.Worktree.DiffStat()
			if md, err := report.Markdown(a.st.DB(), id, diffStat); err == nil {
				os.WriteFile(filepath.Join(out.Dir, "report.md"), []byte(md), 0o644)
			}
		}
		runtime.EventsEmit(a.ctx, "done", ev)
	}()
	return id, nil
}

// Stop cancels a running request (its processes are killed).
func (a *App) Stop(id string) {
	a.mu.Lock()
	cancel := a.running[id]
	a.mu.Unlock()
	if cancel != nil {
		cancel()
	}
}

// Running lists ids of requests running in this session.
func (a *App) Running() []string {
	a.mu.Lock()
	defer a.mu.Unlock()
	ids := make([]string, 0, len(a.running))
	for id := range a.running {
		ids = append(ids, id)
	}
	return ids
}

func (a *App) List() ([]store.RequestRow, error) {
	if err := a.ready(); err != nil {
		return nil, err
	}
	return a.st.ListRequests(200)
}

// Detail is everything the request screen shows.
type Detail struct {
	Request store.RequestRow `json:"request"`
	Tasks   []store.TaskRow  `json:"tasks"`
	Checks  []store.CheckRow `json:"checks"`
	Report  string           `json:"report"`
	Diff    string           `json:"diff"`
}

const maxDiff = 200000

func (a *App) Detail(id string) (*Detail, error) {
	if err := a.ready(); err != nil {
		return nil, err
	}
	r, err := a.st.GetRequest(id)
	if err != nil {
		return nil, err
	}
	d := &Detail{Request: r}
	if d.Tasks, err = a.st.Tasks(id); err != nil {
		return nil, err
	}
	if d.Checks, err = a.st.Checks(id); err != nil {
		return nil, err
	}
	diffStat := ""
	switch r.Status {
	case store.ReqDone, store.ReqApproved:
		d.Diff, _ = workspace.DiffBranch(r.Repo, r.Base, r.Branch)
		diffStat, _ = workspace.Open(r.Repo, r.Worktree, r.Branch, r.Base).DiffStat()
	case store.ReqRunning, store.ReqFailed, store.ReqCancelled, store.ReqInterrupted:
		if _, err := os.Stat(r.Worktree); err == nil {
			d.Diff, _ = workspace.Open(r.Repo, r.Worktree, r.Branch, r.Base).Diff()
		}
	}
	if len(d.Diff) > maxDiff {
		d.Diff = d.Diff[:maxDiff] + "\n... (잘림)"
	}
	d.Report, _ = report.Markdown(a.st.DB(), id, diffStat)
	return d, nil
}

// Approve merges the request branch into the branch checked out in the
// user's repository, then removes the worktree. Only DONE requests.
func (a *App) Approve(id string) error {
	if err := a.ready(); err != nil {
		return err
	}
	r, err := a.st.GetRequest(id)
	if err != nil {
		return err
	}
	if r.Status != store.ReqDone {
		return fmt.Errorf("검증을 통과한 요청만 반영할 수 있습니다 (현재 %s)", r.Status)
	}
	if err := workspace.MergeInto(r.Repo, r.Branch, "AI Office 요청 반영: "+oneLine(r.Text, 60)); err != nil {
		return fmt.Errorf("반영 실패(원래 상태로 되돌림): %w", err)
	}
	workspace.Open(r.Repo, r.Worktree, r.Branch, r.Base).Remove(false) // keep the branch for history
	return a.st.SetRequestStatus(id, store.ReqApproved, "")
}

// Discard removes the worktree and the branch of a finished request.
func (a *App) Discard(id string) error {
	if err := a.ready(); err != nil {
		return err
	}
	a.mu.Lock()
	_, busy := a.running[id]
	a.mu.Unlock()
	if busy {
		return errors.New("실행 중인 요청은 먼저 중지하세요")
	}
	r, err := a.st.GetRequest(id)
	if err != nil {
		return err
	}
	if r.Status == store.ReqApproved || r.Status == store.ReqDiscarded {
		return fmt.Errorf("이미 %s 상태입니다", r.Status)
	}
	if err := workspace.Open(r.Repo, r.Worktree, r.Branch, r.Base).Remove(true); err != nil {
		return err
	}
	return a.st.SetRequestStatus(id, store.ReqDiscarded, "")
}

func clean(list []string) []string {
	var out []string
	for _, s := range list {
		if s = strings.TrimSpace(s); s != "" {
			out = append(out, s)
		}
	}
	return out
}

func oneLine(s string, max int) string {
	s = strings.Join(strings.Fields(s), " ")
	if r := []rune(s); len(r) > max {
		return string(r[:max]) + "..."
	}
	return s
}
