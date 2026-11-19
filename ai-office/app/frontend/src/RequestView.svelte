<script lang="ts">
  import { Approve, Detail, Discard, Stop } from '../wailsjs/go/main/App'
  import type { main } from '../wailsjs/go/models'

  type Log = { id: string; t: number; role: string; line: string }
  let { id, live, isRunning, tick, onchanged }: {
    id: string; live: Log[]; isRunning: boolean; tick: number; onchanged: () => void
  } = $props()

  let d = $state<main.Detail | null>(null)
  let error = $state('')
  let busy = $state(false)
  let tab = $state<'log' | 'diff' | 'report'>('log')

  async function load() {
    try {
      const x = await Detail(id)
      x.tasks ??= []
      x.checks ??= []
      d = x
      error = ''
    } catch (e) {
      error = String(e)
    }
  }

  // Reload when the request finishes, and while it runs whenever a log line arrives.
  $effect(() => {
    tick
    live.length
    load()
  })

  async function act(fn: (id: string) => Promise<void>, confirmText: string) {
    if (confirmText && !confirm(confirmText)) return
    busy = true
    try {
      await fn(id)
      await load()
      onchanged()
    } catch (e) {
      error = String(e)
    } finally {
      busy = false
    }
  }

  const roleColor: Record<string, string> = { pm: 'var(--blue)', dev: 'var(--accent)', qa: 'var(--green)', system: 'var(--muted)' }
  const kindName: Record<string, string> = { plan: '계획', work: '작업', fix: '수정', review: 'QA 리뷰' }
  const diffClass = (l: string) =>
    l.startsWith('+++') || l.startsWith('---') ? 'dh' : l.startsWith('+') ? 'da' : l.startsWith('-') ? 'dd' : l.startsWith('@@') ? 'dh' : ''
</script>

{#if d}
  {@const r = d.request}
  <div class="head">
    <div>
      <span class="status s-{isRunning ? 'RUNNING' : r.status}">{isRunning ? 'RUNNING' : r.status}</span>
      <span class="mono muted">{r.id} · {r.branch}</span>
    </div>
    <div class="buttons">
      {#if isRunning}
        <button class="danger" disabled={busy} onclick={() => act(async (x) => Stop(x), '실행을 중지할까요? 실행 중인 AI 프로세스가 모두 종료됩니다.')}>중지</button>
      {:else if r.status === 'DONE'}
        <button class="ok" disabled={busy} onclick={() => act(Approve, `이 변경을 원래 저장소에 반영(merge)할까요?\n${r.repo}`)}>승인하고 반영</button>
        <button disabled={busy} onclick={() => act(Discard, '작업 폴더와 브랜치를 지울까요?')}>폐기</button>
      {:else if r.status === 'FAILED' || r.status === 'CANCELLED' || r.status === 'INTERRUPTED'}
        <button disabled={busy} onclick={() => act(Discard, '작업 폴더와 브랜치를 지울까요?')}>폐기</button>
      {/if}
    </div>
  </div>
  <h2>{r.text}</h2>
  {#if r.reason}<p class="reason">{r.reason}</p>{/if}
  {#if r.plan}<p class="plan"><b>PM 계획</b> {r.plan}</p>{/if}

  <div class="stats">
    <div><span>비용</span><b>${r.costUsd.toFixed(4)}</b></div>
    <div><span>AI 실행</span><b>{r.runs}회</b></div>
    <div><span>검사</span><b>{d.checks.filter((c) => c.exitCode === 0 && !c.timedOut).length}/{d.checks.length} 통과</b></div>
    <div><span>수정</span><b>{d.tasks.filter((t) => t.kind === 'fix').length}회</b></div>
  </div>

  <table>
    <thead><tr><th>#</th><th>단계</th><th>담당</th><th>내용</th><th>상태</th><th>비용</th><th>시간</th></tr></thead>
    <tbody>
      {#each d.tasks as t (t.id)}
        <tr>
          <td class="mono">{t.seq}</td>
          <td>{kindName[t.kind] ?? t.kind}</td>
          <td style="color:{roleColor[t.role]}">{t.role.toUpperCase()}</td>
          <td>{t.title}</td>
          <td><span class="status s-{t.status === 'COMPLETED' ? 'DONE' : t.status === 'IN_PROGRESS' ? 'RUNNING' : t.status}">{t.status}</span></td>
          <td class="mono">${t.costUsd.toFixed(4)}{t.estimated ? '*' : ''}</td>
          <td class="mono">{t.seconds.toFixed(0)}s</td>
        </tr>
      {/each}
    </tbody>
  </table>

  <div class="tabs">
    <button class:on={tab === 'log'} onclick={() => (tab = 'log')}>진행 기록</button>
    <button class:on={tab === 'diff'} onclick={() => (tab = 'diff')}>변경 내용</button>
    <button class:on={tab === 'report'} onclick={() => (tab = 'report')}>보고서</button>
  </div>
  <div class="box">
    {#if tab === 'log'}
      {#if live.length === 0}
        <p class="muted">실시간 기록은 이 창에서 시작한 요청에만 보입니다. 지난 요청은 표와 보고서를 보세요.</p>
      {/if}
      {#each live as l}
        <div class="log"><span class="t">{l.t.toFixed(1)}s</span><span class="role" style="color:{roleColor[l.role]}">{l.role}</span><span>{l.line}</span></div>
      {/each}
    {:else if tab === 'diff'}
      {#if d.diff}
        <pre>{#each d.diff.split('\n') as l}<span class={diffClass(l)}>{l}{'\n'}</span>{/each}</pre>
      {:else}<p class="muted">변경 내용이 없거나 작업 폴더가 지워졌습니다.</p>{/if}
    {:else}
      <pre>{d.report}</pre>
    {/if}
  </div>
{/if}
{#if error}<p class="err">{error}</p>{/if}

<style>
  .head { display: flex; justify-content: space-between; align-items: center; gap: 12px; }
  .buttons { display: flex; gap: 8px; }
  .mono { font-family: var(--mono); }
  .muted { color: var(--muted); }
  .head .mono { margin-left: 8px; font-size: 12px; }
  h2 { font-size: 18px; margin: 14px 0 6px; line-height: 1.4; }
  .reason { color: var(--red); margin: 4px 0; }
  .plan { color: var(--muted); margin: 6px 0; line-height: 1.5; }
  .plan b { color: var(--blue); margin-right: 6px; }
  .stats { display: grid; grid-template-columns: repeat(4, 1fr); gap: 10px; margin: 16px 0; }
  .stats div { background: var(--panel); border: 1px solid var(--border); border-radius: 8px; padding: 10px 14px; }
  .stats span { display: block; color: var(--muted); font-size: 12px; }
  .stats b { font-size: 18px; font-family: var(--mono); }
  table { width: 100%; border-collapse: collapse; margin-bottom: 18px; }
  th { text-align: left; color: var(--muted); font-weight: 400; font-size: 12px; border-bottom: 1px solid var(--border); padding: 6px 8px; }
  td { padding: 7px 8px; border-bottom: 1px solid #21262d; vertical-align: top; }
  .tabs { display: flex; gap: 6px; margin-bottom: -1px; }
  .tabs button { border-radius: 6px 6px 0 0; background: transparent; }
  .tabs button.on { background: var(--panel); border-bottom-color: var(--panel); }
  .box { background: var(--panel); border: 1px solid var(--border); border-radius: 0 8px 8px 8px; padding: 14px; min-height: 240px; max-height: 520px; overflow: auto; }
  .log { font-family: var(--mono); font-size: 12.5px; line-height: 1.6; display: flex; gap: 10px; }
  .log .t { color: var(--muted); width: 56px; text-align: right; flex: none; }
  .log .role { width: 48px; flex: none; }
  .da { color: var(--green); }
  .dd { color: var(--red); }
  .dh { color: var(--blue); }
  .err { color: var(--red); }
</style>
