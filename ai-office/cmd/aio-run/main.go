// aio-run sends one request through the team (PM → developer → checks →
// fixes) in an isolated worktree, records everything in SQLite, commits the
// result on its own branch and prints a report. Nothing is merged: applying
// the branch is the user's decision.
//
//	aio-run -repo C:\path\to\repo -check "go test ./..." -request "..."
package main

import (
	"context"
	"flag"
	"fmt"
	"os"
	"os/signal"
	"path/filepath"
	"strings"
	"time"

	"golang.org/x/sys/windows"

	"github.com/Song-wh/devlog-kr/ai-office/internal/engine/claudecode"
	"github.com/Song-wh/devlog-kr/ai-office/internal/orchestrator"
	"github.com/Song-wh/devlog-kr/ai-office/internal/report"
	"github.com/Song-wh/devlog-kr/ai-office/internal/store"
)

type multiFlag []string

func (m *multiFlag) String() string     { return strings.Join(*m, "; ") }
func (m *multiFlag) Set(v string) error { *m = append(*m, v); return nil }

func main() { os.Exit(run()) }

func run() int {
	var checks, protect multiFlag
	var (
		repo        = flag.String("repo", "", "git repository to work on (required)")
		request     = flag.String("request", "", "what you want done")
		requestFile = flag.String("request-file", "", "read the request from a UTF-8 file")
		pmModel     = flag.String("pm-model", "sonnet", "model for the PM")
		devModel    = flag.String("dev-model", "sonnet", "model for the developer")
		fixRounds   = flag.Int("fix-rounds", 3, "max fix rounds after failed checks")
		budget      = flag.Float64("budget", 2.0, "max USD for the whole request")
		taskTO      = flag.Duration("task-timeout", 10*time.Minute, "max time per agent run")
		checkTO     = flag.Duration("check-timeout", 5*time.Minute, "max time per check")
		solo        = flag.Bool("solo", false, "no PM: the developer gets the request as one task")
		reviewFlag  = flag.Bool("review", true, "after checks pass, a QA agent reviews the diff (team mode)")
		qaModel     = flag.String("qa-model", "", "model for the QA reviewer (default: dev model)")
		dataDir     = flag.String("data", defaultDataDir(), "worktrees, logs and database")
	)
	flag.Var(&checks, "check", "check command run in the worktree (repeatable); all must pass")
	flag.Var(&protect, "protect", "glob of files the agents may not change (repeatable), e.g. *_test.go")
	flag.Parse()
	windows.SetConsoleOutputCP(65001)

	if *repo == "" || (*request == "") == (*requestFile == "") {
		fmt.Fprintln(os.Stderr, "usage: aio-run -repo DIR (-request TEXT | -request-file FILE) [-check CMD]... [-protect GLOB]... [options]")
		flag.PrintDefaults()
		return 2
	}
	text := *request
	if *requestFile != "" {
		b, err := os.ReadFile(*requestFile)
		if err != nil {
			return fail(err)
		}
		text = string(b)
	}
	if len(checks) == 0 {
		fmt.Println("note: no -check given; the team's work will not be verified")
	}

	if err := os.MkdirAll(*dataDir, 0o755); err != nil {
		return fail(err)
	}
	st, err := store.Open(filepath.Join(*dataDir, "aio.db"))
	if err != nil {
		return fail(err)
	}
	defer st.Close()
	if n, err := st.MarkInterrupted(); err != nil {
		return fail(err)
	} else if n > 0 {
		fmt.Printf("note: %d earlier request(s) were left running and are now marked INTERRUPTED\n", n)
	}

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt)
	defer stop()

	start := time.Now()
	o := &orchestrator.Orchestrator{
		Engine:  &claudecode.Engine{},
		Store:   st,
		DataDir: *dataDir,
		Log: func(role, line string) {
			fmt.Printf("[%6.1fs] %-6s %s\n", time.Since(start).Seconds(), role, line)
		},
	}
	out, err := o.Run(ctx, orchestrator.Config{
		Repo: *repo, Request: text, Checks: checks, Protect: protect,
		PMModel: *pmModel, DevModel: *devModel, MaxFixRounds: *fixRounds, BudgetUSD: *budget,
		TaskTimeout: *taskTO, CheckTimeout: *checkTO, Solo: *solo, Review: *reviewFlag, QAModel: *qaModel,
	})
	if err != nil {
		return fail(err)
	}

	diffStat, _ := out.Worktree.DiffStat()
	md, err := report.Markdown(st.DB(), out.RequestID, diffStat)
	if err != nil {
		return fail(err)
	}
	os.WriteFile(filepath.Join(out.Dir, "report.md"), []byte(md), 0o644)
	fmt.Println()
	fmt.Println(md)
	if out.Status == store.ReqDone {
		fmt.Printf("Review:  git -C \"%s\" diff %.8s..%s\n", out.Worktree.Repo, out.Worktree.Base, out.Worktree.Branch)
		fmt.Printf("Apply:   git -C \"%s\" merge %s\n", out.Worktree.Repo, out.Worktree.Branch)
		fmt.Printf("Discard: git -C \"%s\" worktree remove --force \"%s\" && git -C \"%s\" branch -D %s\n",
			out.Worktree.Repo, out.Worktree.Dir, out.Worktree.Repo, out.Worktree.Branch)
		return 0
	}
	return 1
}

func defaultDataDir() string {
	if d := os.Getenv("LOCALAPPDATA"); d != "" {
		return filepath.Join(d, "ai-office")
	}
	return "ai-office-data"
}

func fail(err error) int {
	fmt.Fprintln(os.Stderr, "error:", err)
	return 1
}
