package orchestrator

import (
	"context"
	"encoding/json"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"

	"github.com/Song-wh/devlog-kr/ai-office/internal/engine"
	"github.com/Song-wh/devlog-kr/ai-office/internal/store"
)

// fakeEngine plays the PM, the developer and the reviewer from a script.
type fakeEngine struct {
	t       *testing.T
	calls   []engine.Spec
	dev     []func(dir string) // one per developer call, in order
	reviews []review           // one per review call; approve when exhausted
	devN    int
	revN    int
}

func (f *fakeEngine) Name() string { return "fake" }

func (f *fakeEngine) Run(ctx context.Context, s engine.Spec, on func(engine.Event)) (engine.Result, error) {
	f.calls = append(f.calls, s)
	res := engine.Result{Status: engine.StatusSuccess, Usage: engine.Usage{CostUSD: 0.10}}
	switch {
	case strings.Contains(s.JSONSchema, `"approved"`): // QA reviewer
		rv := review{Approved: true, Summary: "ok", Issues: []reviewIssue{}}
		if f.revN < len(f.reviews) {
			rv = f.reviews[f.revN]
		}
		f.revN++
		res.Structured, _ = json.Marshal(rv)
		return res, nil
	case s.JSONSchema != "": // PM
		res.Structured, _ = json.Marshal(plan{Summary: "두 단계", Tasks: []planTask{
			{Title: "a 만들기", Instructions: "a.txt", Files: []string{"a.txt"}},
			{Title: "b 만들기", Instructions: "b.txt", Files: []string{"b.txt"}},
		}})
		return res, nil
	}
	if f.devN >= len(f.dev) {
		f.t.Fatalf("unexpected developer call %d", f.devN)
	}
	f.dev[f.devN](s.Dir)
	f.devN++
	res.Text = "did it"
	return res, nil
}

// Checks pass, but the reviewer finds a defect once; the developer fixes it
// and the second review approves.
func TestReviewRejectsThenApproves(t *testing.T) {
	repo := newRepo(t)
	data := t.TempDir()
	st, _ := store.Open(filepath.Join(data, "aio.db"))
	defer st.Close()
	fe := &fakeEngine{t: t,
		dev: []func(string){
			func(d string) { write(t, d, "a.txt", "a\n") },
			func(d string) { write(t, d, "b.txt", "b\n") },
			func(d string) { write(t, d, "b.txt", "b fixed\n") },
		},
		reviews: []review{{Approved: false, Summary: "b가 명세와 다름", Issues: []reviewIssue{{File: "b.txt", Problem: "fixed가 빠짐", Fix: "fixed 추가"}}}},
	}
	o := &Orchestrator{Engine: fe, Store: st, DataDir: data}
	out, err := o.Run(context.Background(), Config{Repo: repo, Request: "x", MaxFixRounds: 2, BudgetUSD: 5,
		Checks: []string{"findstr b b.txt"}, Review: true})
	if err != nil {
		t.Fatal(err)
	}
	if out.Status != store.ReqDone {
		t.Fatalf("status %s (%s)", out.Status, out.Reason)
	}
	if fe.revN != 2 || fe.devN != 3 {
		t.Errorf("reviews %d, dev calls %d", fe.revN, fe.devN)
	}
	// The reviewer only reads, and its issues reach the developer.
	for _, c := range fe.calls {
		if strings.Contains(c.JSONSchema, `"approved"`) && strings.Join(c.Tools, ",") != "Read,Glob,Grep" {
			t.Errorf("reviewer tools %v", c.Tools)
		}
	}
	if fix := fe.calls[4].Prompt; !strings.Contains(fix, "fixed가 빠짐") {
		t.Errorf("fix prompt lacks review issue:\n%s", fix)
	}
	if b, _ := os.ReadFile(filepath.Join(out.Worktree.Dir, "b.txt")); strings.TrimSpace(string(b)) != "b fixed" {
		t.Errorf("b.txt = %q", b)
	}
}

func write(t *testing.T, dir, name, content string) {
	t.Helper()
	if err := os.WriteFile(filepath.Join(dir, name), []byte(content), 0o644); err != nil {
		t.Fatal(err)
	}
}

func newRepo(t *testing.T) string {
	t.Helper()
	dir := t.TempDir()
	write(t, dir, "guard.txt", "original\n")
	for _, args := range [][]string{
		{"init", "-q"}, {"add", "-A"},
		{"-c", "user.name=t", "-c", "user.email=t@t", "commit", "-qm", "init"},
	} {
		if out, err := exec.Command("git", append([]string{"-C", dir}, args...)...).CombinedOutput(); err != nil {
			t.Fatalf("git %v: %v %s", args, err, out)
		}
	}
	return dir
}

// The check passes only when b.txt says "ok". The first attempt writes the
// wrong content and also edits a protected file; the fix round corrects it.
func TestPlanWorkCheckFixCommit(t *testing.T) {
	repo := newRepo(t)
	data := t.TempDir()
	st, err := store.Open(filepath.Join(data, "aio.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer st.Close()

	fe := &fakeEngine{t: t, dev: []func(string){
		func(d string) { write(t, d, "a.txt", "a\n") },
		func(d string) { write(t, d, "b.txt", "wrong\n"); write(t, d, "guard.txt", "tampered\n") },
		func(d string) { write(t, d, "b.txt", "ok\n") },
	}}
	var lines []string
	o := &Orchestrator{Engine: fe, Store: st, DataDir: data, Log: func(role, l string) { lines = append(lines, role+": "+l) }}
	out, err := o.Run(context.Background(), Config{
		Repo: repo, Request: "a와 b를 만들어", MaxFixRounds: 2, BudgetUSD: 1,
		Checks:  []string{`findstr ok b.txt`},
		Protect: []string{"guard.txt"},
	})
	if err != nil {
		t.Fatal(err)
	}
	if out.Status != store.ReqDone {
		t.Fatalf("status %s (%s)\n%s", out.Status, out.Reason, strings.Join(lines, "\n"))
	}
	if len(fe.calls) != 4 { // pm, dev a, dev b, fix
		t.Errorf("engine calls = %d", len(fe.calls))
	}
	fix := fe.calls[3].Prompt
	if !strings.Contains(fix, "guard.txt") || !strings.Contains(fix, "findstr") {
		t.Errorf("fix prompt lacks the failure details:\n%s", fix)
	}
	// Protected file was restored; the result is committed on the branch.
	if b, _ := os.ReadFile(filepath.Join(out.Worktree.Dir, "guard.txt")); strings.TrimSpace(string(b)) != "original" {
		t.Errorf("guard.txt = %q", b)
	}
	files, _ := exec.Command("git", "-C", out.Worktree.Dir, "show", "--name-only", "--format=", "HEAD").Output()
	if got := strings.Fields(string(files)); strings.Join(got, ",") != "a.txt,b.txt" {
		t.Errorf("committed files = %v", got)
	}
	// The user's own folder is untouched.
	if _, err := os.Stat(filepath.Join(repo, "a.txt")); err == nil {
		t.Error("a.txt appeared in the original folder")
	}

	var checks, failedChecks, runs int
	var cost float64
	db := st.DB()
	db.QueryRow(`SELECT COUNT(*), SUM(exit_code<>0) FROM checks WHERE request_id=?`, out.RequestID).Scan(&checks, &failedChecks)
	db.QueryRow(`SELECT COUNT(*), SUM(cost_usd) FROM runs WHERE request_id=?`, out.RequestID).Scan(&runs, &cost)
	if checks != 2 || failedChecks != 1 || runs != 4 || cost < 0.399 || cost > 0.401 {
		t.Errorf("checks %d (failed %d), runs %d, cost %.3f", checks, failedChecks, runs, cost)
	}
}

func TestBudgetStopsTheTeam(t *testing.T) {
	repo := newRepo(t)
	data := t.TempDir()
	st, _ := store.Open(filepath.Join(data, "aio.db"))
	defer st.Close()
	fe := &fakeEngine{t: t, dev: []func(string){func(d string) { write(t, d, "a.txt", "a") }}}
	o := &Orchestrator{Engine: fe, Store: st, DataDir: data}
	// PM 0.10 + dev 0.10 = 0.20 spent; the second task finds no money left.
	out, err := o.Run(context.Background(), Config{Repo: repo, Request: "x", BudgetUSD: 0.2})
	if err != nil {
		t.Fatal(err)
	}
	if out.Status != store.ReqFailed || !strings.Contains(out.Reason, "budget") {
		t.Errorf("status %s reason %q", out.Status, out.Reason)
	}
	if len(fe.calls) != 2 {
		t.Errorf("engine calls = %d, want 2", len(fe.calls))
	}
}

// An employee assigned to a role brings its own model, account folder and
// extra instructions to every run of that role.
func TestAgentsOverrideRole(t *testing.T) {
	repo := newRepo(t)
	data := t.TempDir()
	st, _ := store.Open(filepath.Join(data, "aio.db"))
	defer st.Close()
	fe := &fakeEngine{t: t, dev: []func(string){
		func(d string) { write(t, d, "a.txt", "a") },
		func(d string) { write(t, d, "b.txt", "b") },
	}}
	o := &Orchestrator{Engine: fe, Store: st, DataDir: data}
	out, err := o.Run(context.Background(), Config{Repo: repo, Request: "x", BudgetUSD: 5, PMModel: "sonnet", DevModel: "sonnet",
		Agents: map[string]Agent{"dev": {Name: "김개발", Model: "opus", ProfileDir: `C:\p\dev`, Prompt: "주석은 한국어로"}}})
	if err != nil || out.Status != store.ReqDone {
		t.Fatalf("%v %+v", err, out)
	}
	pm, dev := fe.calls[0], fe.calls[1]
	if pm.Model != "sonnet" || pm.ProfileDir != "" {
		t.Errorf("pm spec %q %q", pm.Model, pm.ProfileDir)
	}
	if dev.Model != "opus" || dev.ProfileDir != `C:\p\dev` || !strings.Contains(dev.SystemPrompt, "주석은 한국어로") {
		t.Errorf("dev spec model %q profile %q", dev.Model, dev.ProfileDir)
	}
	var agent string
	st.DB().QueryRow(`SELECT agent FROM runs WHERE request_id=? AND role='dev' LIMIT 1`, out.RequestID).Scan(&agent)
	if agent != "김개발" {
		t.Errorf("run agent = %q", agent)
	}
}
