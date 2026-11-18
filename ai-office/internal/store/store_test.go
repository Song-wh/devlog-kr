package store

import (
	"path/filepath"
	"testing"
	"time"
)

func TestLifecycleAndInterrupt(t *testing.T) {
	path := filepath.Join(t.TempDir(), "aio.db")
	s, err := Open(path)
	if err != nil {
		t.Fatal(err)
	}
	if err := s.CreateRequest(Request{ID: "r1", Text: "do it", Repo: "R", Worktree: "W", Branch: "B", Base: "abc"}); err != nil {
		t.Fatal(err)
	}
	id, err := s.AddTask("r1", 1, "work", "dev", "t", "i")
	if err != nil {
		t.Fatal(err)
	}
	for _, st := range []string{Assigned, InProgress} {
		if err := s.SetTaskStatus(id, st, ""); err != nil {
			t.Fatal(err)
		}
	}
	if err := s.AddRun(Run{RequestID: "r1", TaskID: id, Role: "dev", Engine: "e", Model: "m", Status: "success",
		CostUSD: 0.25, StartedAt: time.Now()}); err != nil {
		t.Fatal(err)
	}
	s.AddRun(Run{RequestID: "r1", Role: "pm", Engine: "e", Model: "m", Status: "success", CostUSD: 0.5, StartedAt: time.Now()})
	if c, _ := s.CostSoFar("r1"); c != 0.75 {
		t.Errorf("cost = %v", c)
	}
	s.Close()

	// A new process finds the request still RUNNING: it was interrupted.
	s, err = Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	n, err := s.MarkInterrupted()
	if err != nil || n != 1 {
		t.Fatalf("MarkInterrupted = %d, %v", n, err)
	}
	var rs, ts string
	s.DB().QueryRow(`SELECT status FROM requests WHERE id='r1'`).Scan(&rs)
	s.DB().QueryRow(`SELECT status FROM tasks WHERE id=?`, id).Scan(&ts)
	if rs != ReqInterrupted || ts != Failed {
		t.Errorf("request %s task %s", rs, ts)
	}
	var events int
	s.DB().QueryRow(`SELECT COUNT(*) FROM task_events WHERE task_id=?`, id).Scan(&events)
	if events != 4 { // PENDING, ASSIGNED, IN_PROGRESS, FAILED
		t.Errorf("events = %d", events)
	}
}
