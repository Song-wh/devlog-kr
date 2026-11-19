<script lang="ts">
  import { onMount } from 'svelte'
  import { List, Running } from '../wailsjs/go/main/App'
  import { EventsOn } from '../wailsjs/runtime/runtime'
  import type { store } from '../wailsjs/go/models'
  import NewRequest from './NewRequest.svelte'
  import RequestView from './RequestView.svelte'

  type Log = { id: string; t: number; role: string; line: string }

  let requests = $state<store.RequestRow[]>([])
  let selected = $state<string | null>(null) // null = new request form
  let running = $state<string[]>([])
  let logs = $state<Record<string, Log[]>>({})
  let error = $state('')
  let tick = $state(0) // bumps when a request finishes so the detail reloads

  async function refresh() {
    try {
      requests = (await List()) ?? []
      running = (await Running()) ?? []
    } catch (e) {
      error = String(e)
    }
  }

  onMount(() => {
    refresh()
    const offLog = EventsOn('log', (l: Log) => {
      logs[l.id] = [...(logs[l.id] ?? []), l]
      refresh() // costs and statuses change with every step
    })
    const offDone = EventsOn('done', () => {
      refresh()
      tick++
    })
    return () => {
      offLog()
      offDone()
    }
  })

  function started(id: string) {
    logs[id] = []
    selected = id
    refresh()
  }

  const cost = (c: number) => '$' + c.toFixed(3)
  const short = (s: string) => (s.length > 60 ? s.slice(0, 60) + '…' : s)
</script>

<div class="layout">
  <aside>
    <div class="brand">AI Office</div>
    <button class="primary new" onclick={() => (selected = null)}>+ 새 요청</button>
    <div class="list">
      {#each requests as r (r.id)}
        <button class="item" class:active={selected === r.id} onclick={() => (selected = r.id)}>
          <div class="row1">
            <span class="status s-{r.status}">{running.includes(r.id) ? 'RUNNING' : r.status}</span>
            <span class="cost">{cost(r.costUsd)}</span>
          </div>
          <div class="text">{short(r.text)}</div>
          <div class="when">{r.id}</div>
        </button>
      {:else}
        <div class="empty">아직 요청이 없습니다</div>
      {/each}
    </div>
  </aside>

  <main>
    {#if error}<div class="err">{error}</div>{/if}
    {#if selected === null}
      <NewRequest onstarted={started} />
    {:else}
      {#key selected}
        <RequestView id={selected} live={logs[selected] ?? []} isRunning={running.includes(selected)} {tick} onchanged={refresh} />
      {/key}
    {/if}
  </main>
</div>

<style>
  .layout { display: grid; grid-template-columns: 300px 1fr; height: 100%; }
  aside { background: var(--panel); border-right: 1px solid var(--border); display: flex; flex-direction: column; min-height: 0; }
  .brand { font-weight: 700; font-size: 18px; padding: 18px 16px 10px; color: var(--accent); }
  .new { margin: 0 12px 10px; }
  .list { overflow-y: auto; flex: 1; padding: 0 8px 12px; }
  .item { display: block; width: 100%; text-align: left; background: transparent; border: 1px solid transparent; margin-bottom: 4px; padding: 9px 10px; }
  .item.active { background: var(--panel2); border-color: var(--border); }
  .row1 { display: flex; justify-content: space-between; align-items: center; }
  .cost { font-family: var(--mono); font-size: 12px; color: var(--muted); }
  .text { margin-top: 6px; line-height: 1.35; }
  .when { margin-top: 4px; font-family: var(--mono); font-size: 11px; color: var(--muted); }
  .empty { color: var(--muted); padding: 12px; }
  main { overflow-y: auto; padding: 24px 28px; min-width: 0; }
  .err { color: var(--red); margin-bottom: 12px; }
</style>
