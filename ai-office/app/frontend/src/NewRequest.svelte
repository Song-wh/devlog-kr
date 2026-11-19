<script lang="ts">
  import { onMount } from 'svelte'
  import { LastProject, PickFolder, Start } from '../wailsjs/go/main/App'

  let { onstarted }: { onstarted: (id: string) => void } = $props()

  let repo = $state('')
  let checks = $state('')
  let protect = $state('')
  let request = $state('')
  let mode = $state('review')
  let model = $state('sonnet')
  let budget = $state(2)
  let fixRounds = $state(3)
  let busy = $state(false)
  let error = $state('')

  onMount(async () => {
    try {
      const p = await LastProject()
      repo = p.repo ?? ''
      checks = (p.checks ?? []).join('\n')
      protect = (p.protect ?? []).join(', ')
    } catch (e) {
      error = String(e)
    }
  })

  async function pick() {
    try {
      const dir = await PickFolder()
      if (dir) repo = dir
    } catch (e) {
      error = String(e)
    }
  }

  async function start() {
    error = ''
    busy = true
    try {
      const id = await Start({
        project: { repo, checks: checks.split('\n'), protect: protect.split(',') },
        request, mode, model, budgetUsd: Number(budget), fixRounds: Number(fixRounds),
      } as any)
      request = ''
      onstarted(id)
    } catch (e) {
      error = String(e)
    } finally {
      busy = false
    }
  }

  const modes = [
    ['solo', '혼자', '개발자 한 명이 요청 전체를 수행'],
    ['team', '팀', 'PM이 작업을 나누고 개발자가 수행, 검사 명령으로 확인'],
    ['review', '팀 + QA 리뷰', '검사 통과 뒤 QA가 변경 내용을 요청·명세와 비교'],
  ]
</script>

<h1>새 요청</h1>
<p class="hint">AI 팀은 별도 작업 폴더(git worktree)와 별도 브랜치에서 일합니다. 원래 폴더와 브랜치는 내가 <b>반영</b>을 누르기 전까지 바뀌지 않습니다.</p>

<label for="repo">저장소 폴더</label>
<div class="pick">
  <input id="repo" bind:value={repo} placeholder="C:\path\to\repo" />
  <button onclick={pick}>폴더 선택</button>
</div>

<div class="cols">
  <div>
    <label for="checks">검사 명령 (한 줄에 하나, 모두 통과해야 완료)</label>
    <textarea id="checks" rows="3" bind:value={checks} placeholder="go test ./..."></textarea>
  </div>
  <div>
    <label for="protect">AI가 고칠 수 없는 파일 (쉼표로 구분)</label>
    <input id="protect" bind:value={protect} placeholder="*_test.go" />
    <p class="small">바꾸면 시스템이 되돌리고 실패로 처리합니다.</p>
  </div>
</div>

<label for="req">요청</label>
<textarea id="req" rows="6" bind:value={request} placeholder="무엇을 해 달라고 할까요?"></textarea>

<span class="lbl">진행 방식</span>
<div class="modes">
  {#each modes as [v, name, desc]}
    <button class="mode" class:on={mode === v} onclick={() => (mode = v)}>
      <b>{name}</b><span>{desc}</span>
    </button>
  {/each}
</div>

<div class="cols3">
  <div>
    <label for="model">모델</label>
    <select id="model" bind:value={model}>
      <option value="sonnet">sonnet</option>
      <option value="opus">opus</option>
      <option value="haiku">haiku</option>
    </select>
  </div>
  <div>
    <label for="budget">예산 (요청 전체, USD)</label>
    <input id="budget" type="number" min="0.1" step="0.5" bind:value={budget} />
  </div>
  <div>
    <label for="fix">최대 수정 횟수</label>
    <input id="fix" type="number" min="0" max="10" bind:value={fixRounds} />
  </div>
</div>

{#if error}<p class="err">{error}</p>{/if}
<div class="actions">
  <button class="primary" disabled={busy || !repo || !request.trim()} onclick={start}>시작</button>
</div>

<style>
  h1 { margin: 0 0 6px; font-size: 22px; }
  .hint { color: var(--muted); margin: 0 0 8px; line-height: 1.5; }
  .pick { display: flex; gap: 8px; }
  .pick button { white-space: nowrap; }
  .cols { display: grid; grid-template-columns: 1fr 1fr; gap: 16px; }
  .cols3 { display: grid; grid-template-columns: 1fr 1fr 1fr; gap: 16px; }
  .small { color: var(--muted); font-size: 12px; margin: 6px 0 0; }
  .lbl { display: block; color: var(--muted); font-size: 12px; margin: 14px 0 6px; }
  .modes { display: grid; grid-template-columns: repeat(3, 1fr); gap: 10px; }
  .mode { text-align: left; padding: 10px 12px; display: flex; flex-direction: column; gap: 4px; }
  .mode span { color: var(--muted); font-size: 12px; line-height: 1.4; }
  .mode.on { border-color: var(--accent); background: rgba(232, 135, 95, .1); }
  .actions { margin-top: 20px; }
  .err { color: var(--red); }
</style>
