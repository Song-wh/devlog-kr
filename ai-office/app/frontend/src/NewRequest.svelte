<script lang="ts">
  import { onMount } from 'svelte'
  import { Employees, LastProjectID, Projects, Start } from '../wailsjs/go/main/App'
  import type { main, store } from '../wailsjs/go/models'

  let { onstarted, onemployees, onprojects }: {
    onstarted: (id: string) => void; onemployees: () => void; onprojects: () => void
  } = $props()

  let projects = $state<store.Project[]>([])
  let projectId = $state(0)
  let staff = $state<main.EmployeeView[]>([])
  let team = $state<Record<string, number>>({ ask: 0, pm: 0, dev: 0, qa: 0 })
  let request = $state('')
  let mode = $state('review')
  let budget = $state(2)
  let fixRounds = $state(3)
  let busy = $state(false)
  let error = $state('')

  type Mode = { v: string; name: string; desc: string; slots: string[] }
  const readModes: Mode[] = [
    { v: 'ask', name: '질문하기', desc: '프로젝트 코드를 읽고 질문에 답합니다', slots: ['ask'] },
    { v: 'plan', name: '기획만', desc: 'PM이 현재 구조에 맞춘 기획서를 씁니다', slots: ['pm'] },
    { v: 'audit', name: '검토만', desc: 'QA가 지금 코드의 결함을 찾아 보고합니다', slots: ['qa'] },
  ]
  const codeModes: Mode[] = [
    { v: 'solo', name: '혼자 개발', desc: '개발자 한 명이 요청 전체를 수행', slots: ['dev'] },
    { v: 'team', name: '팀', desc: 'PM이 작업을 나누고 개발자가 수행, 검사로 확인', slots: ['pm', 'dev'] },
    { v: 'review', name: '팀 + QA 리뷰', desc: '검사 통과 뒤 QA가 변경 내용을 요청과 비교', slots: ['pm', 'dev', 'qa'] },
  ]
  const allModes = [...readModes, ...codeModes]
  const slotName: Record<string, string> = { ask: '답변할 직원', pm: 'PM', dev: '개발자', qa: 'QA' }
  let current = $derived(allModes.find((m) => m.v === mode)!)
  let isCode = $derived(codeModes.some((m) => m.v === mode))
  let project = $derived(projects.find((p) => p.id === projectId))

  // "ask" can go to anyone; other slots only to employees with that role.
  const candidates = (slot: string) => (slot === 'ask' ? staff : staff.filter((e) => e.role === slot))

  onMount(async () => {
    try {
      ;[projects, staff] = await Promise.all([Projects().then((x) => x ?? []), Employees().then((x) => x ?? [])])
      const last = await LastProjectID()
      projectId = projects.some((p) => p.id === last) ? last : (projects[0]?.id ?? 0)
      for (const slot of ['ask', 'pm', 'dev', 'qa']) team[slot] = candidates(slot)[0]?.id ?? 0
    } catch (e) {
      error = String(e)
    }
  })

  async function start() {
    error = ''
    busy = true
    try {
      const id = await Start({ projectId, request, mode, team, budgetUsd: Number(budget), fixRounds: Number(fixRounds) } as any)
      request = ''
      onstarted(id)
    } catch (e) {
      error = String(e)
    } finally {
      busy = false
    }
  }

  const account = (e: main.EmployeeView) =>
    e.connection === 'own' ? (e.status?.loggedIn ? '전용 계정' : '전용 계정 · 로그인 필요') : '기본 로그인'
</script>

<h1>새 요청</h1>

<span class="lbl">프로젝트 <button class="link" onclick={onprojects}>프로젝트 관리</button></span>
{#if projects.length === 0}
  <div class="empty">먼저 프로젝트를 등록하세요. <button class="primary" onclick={onprojects}>프로젝트 등록</button></div>
{:else}
  <select bind:value={projectId}>
    {#each projects as p}<option value={p.id}>{p.name} — {p.repo}</option>{/each}
  </select>
  {#if project}
    <p class="small">검사: {project.checks?.length ? project.checks.join(' · ') : '없음'} · 보호: {project.protect?.length ? project.protect.join(', ') : '없음'}{project.notes ? ' · 프로젝트 설명 있음' : ''}</p>
  {/if}
{/if}

<label for="req">요청</label>
<textarea id="req" rows="6" bind:value={request} placeholder={mode === 'ask' ? '무엇이 궁금한가요? 예: 결제 처리 흐름이 어떻게 되어 있어?' : mode === 'plan' ? '무엇을 기획할까요? 예: 월별 통계 화면을 추가하고 싶어' : mode === 'audit' ? '어디를 검토할까요? (비우면 핵심 부분 전체)' : '무엇을 해 달라고 할까요?'}></textarea>

<span class="lbl">진행 방식</span>
<div class="group">읽기만 · 프로젝트 파일을 바꾸지 않음</div>
<div class="modes">
  {#each readModes as m}
    <button class="mode" class:on={mode === m.v} onclick={() => (mode = m.v)}><b>{m.name}</b><span>{m.desc}</span></button>
  {/each}
</div>
<div class="group">코드 수정 · 별도 브랜치에서 작업, 승인해야 반영</div>
<div class="modes">
  {#each codeModes as m}
    <button class="mode" class:on={mode === m.v} onclick={() => (mode = m.v)}><b>{m.name}</b><span>{m.desc}</span></button>
  {/each}
</div>

<span class="lbl">참여 직원 <button class="link" onclick={onemployees}>직원 관리</button></span>
<div class="slots">
  {#each current.slots as slot (slot)}
    <div>
      <label for={'t-' + slot}>{slotName[slot]}</label>
      <select id={'t-' + slot} bind:value={team[slot]}>
        {#each candidates(slot) as e}
          <option value={e.id}>{e.name} · {e.model} · {account(e)}</option>
        {:else}
          <option value={0}>이 역할의 직원이 없습니다</option>
        {/each}
      </select>
    </div>
  {/each}
</div>

<div class="slots">
  <div>
    <label for="budget">예산 (요청 전체, USD)</label>
    <input id="budget" type="number" min="0.1" step="0.5" bind:value={budget} />
  </div>
  {#if isCode}
    <div>
      <label for="fix">최대 수정 횟수</label>
      <input id="fix" type="number" min="0" max="10" bind:value={fixRounds} />
    </div>
  {/if}
</div>

{#if error}<p class="err">{error}</p>{/if}
<div class="actions">
  <button class="primary" disabled={busy || !projectId || !request.trim()} onclick={start}>시작</button>
</div>

<style>
  h1 { margin: 0 0 6px; font-size: 22px; }
  .lbl { display: block; color: var(--muted); font-size: 12px; margin: 16px 0 6px; }
  .group { color: var(--muted); font-size: 12px; margin: 10px 0 6px; }
  .small { color: var(--muted); font-size: 12px; margin: 6px 0 0; }
  .modes { display: grid; grid-template-columns: repeat(3, 1fr); gap: 10px; }
  .mode { text-align: left; padding: 10px 12px; display: flex; flex-direction: column; gap: 4px; }
  .mode span { color: var(--muted); font-size: 12px; line-height: 1.4; }
  .mode.on { border-color: var(--accent); background: rgba(232, 135, 95, .1); }
  .slots { display: grid; grid-template-columns: repeat(3, 1fr); gap: 16px; }
  .empty { display: flex; gap: 12px; align-items: center; color: var(--muted); }
  .actions { margin-top: 20px; }
  .err { color: var(--red); }
  .link { background: none; border: none; color: var(--blue); padding: 0 0 0 8px; font-size: 12px; }
</style>
