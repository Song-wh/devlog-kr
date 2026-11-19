<script lang="ts">
  import { onMount } from 'svelte'
  import { DeleteProject, PickFolder, Projects, SaveProject } from '../wailsjs/go/main/App'
  import type { store } from '../wailsjs/go/models'

  let list = $state<store.Project[]>([])
  let editing = $state<any | null>(null)
  let error = $state('')

  async function load() {
    try {
      list = (await Projects()) ?? []
    } catch (e) {
      error = String(e)
    }
  }
  onMount(load)

  function edit(p?: store.Project) {
    error = ''
    editing = p
      ? { ...p, checksText: (p.checks ?? []).join('\n'), protectText: (p.protect ?? []).join(', ') }
      : { id: 0, name: '', repo: '', checksText: '', protectText: '', notes: '' }
  }

  async function pick() {
    try {
      const dir = await PickFolder()
      if (dir) {
        editing.repo = dir
        if (!editing.name) editing.name = dir.split(/[\\/]/).pop()
      }
    } catch (e) {
      error = String(e)
    }
  }

  async function save() {
    error = ''
    try {
      await SaveProject({
        id: editing.id, name: editing.name, repo: editing.repo, notes: editing.notes,
        checks: editing.checksText.split('\n'), protect: editing.protectText.split(','),
      } as any)
      editing = null
      await load()
    } catch (e) {
      error = String(e)
    }
  }

  async function remove(p: store.Project) {
    if (!confirm(`${p.name} 프로젝트를 목록에서 지울까요? 폴더와 파일은 그대로 둡니다.`)) return
    await DeleteProject(p.id)
    await load()
  }
</script>

<div class="top">
  <div>
    <h1>프로젝트</h1>
    <p class="hint">한 번 등록하면 요청할 때 프로젝트만 고르면 됩니다. <b>프로젝트 설명</b>(기술 스택, 지켜야 할 규칙, 배경)은 모든 직원의 지시에 자동으로 붙습니다.</p>
  </div>
  <button class="primary" onclick={() => edit()}>+ 프로젝트 등록</button>
</div>
{#if error}<p class="err">{error}</p>{/if}

{#if editing}
  <div class="card edit">
    <div class="grid2">
      <div>
        <label for="pn">이름</label>
        <input id="pn" bind:value={editing.name} placeholder="예: 가계부 앱" />
      </div>
      <div>
        <label for="pr">폴더 (git 저장소)</label>
        <div class="pick">
          <input id="pr" bind:value={editing.repo} placeholder="C:\path\to\repo" />
          <button onclick={pick}>폴더 선택</button>
        </div>
      </div>
    </div>
    <div class="grid2">
      <div>
        <label for="pc">검사 명령 (한 줄에 하나, 코드 수정 요청에서 모두 통과해야 완료)</label>
        <textarea id="pc" rows="3" bind:value={editing.checksText} placeholder="go test ./..."></textarea>
      </div>
      <div>
        <label for="pp">AI가 고칠 수 없는 파일 (쉼표로 구분)</label>
        <input id="pp" bind:value={editing.protectText} placeholder="*_test.go" />
      </div>
    </div>
    <label for="no">프로젝트 설명 (모든 직원에게 전달)</label>
    <textarea id="no" rows="5" bind:value={editing.notes} placeholder={'예:\n- Go 1.22, 표준 라이브러리만 사용\n- 금액은 정수 센트로 다룬다\n- 사용자 화면 문구는 한국어'}></textarea>
    <div class="actions">
      <button class="primary" onclick={save}>저장</button>
      <button onclick={() => (editing = null)}>취소</button>
    </div>
  </div>
{/if}

<div class="cards">
  {#each list as p (p.id)}
    <div class="card">
      <div class="row">
        <div><b class="name">{p.name}</b> <span class="mono muted">{p.repo}</span></div>
        <div class="btns">
          <button onclick={() => edit(p)}>수정</button>
          <button class="danger" onclick={() => remove(p)}>삭제</button>
        </div>
      </div>
      <div class="meta">
        <span>검사: {p.checks?.length ? p.checks.join(' · ') : '없음'}</span>
        <span>보호: {p.protect?.length ? p.protect.join(', ') : '없음'}</span>
      </div>
      {#if p.notes}<pre class="notes">{p.notes}</pre>{/if}
    </div>
  {:else}
    {#if !editing}<p class="muted">등록된 프로젝트가 없습니다. 오른쪽 위 버튼으로 등록하세요.</p>{/if}
  {/each}
</div>

<style>
  .top { display: flex; justify-content: space-between; align-items: flex-start; gap: 20px; }
  .top .primary { white-space: nowrap; }
  h1 { margin: 0 0 6px; font-size: 22px; }
  .hint { color: var(--muted); margin: 0 0 14px; line-height: 1.5; max-width: 900px; }
  .cards { display: grid; gap: 12px; }
  .card { background: var(--panel); border: 1px solid var(--border); border-radius: 10px; padding: 14px 16px; }
  .card.edit { border-color: var(--accent); margin-bottom: 16px; }
  .row { display: flex; justify-content: space-between; align-items: center; gap: 12px; }
  .btns { display: flex; gap: 6px; }
  .name { font-size: 16px; margin-right: 8px; }
  .mono { font-family: var(--mono); font-size: 12px; }
  .muted { color: var(--muted); }
  .meta { display: flex; gap: 24px; color: var(--muted); font-size: 12px; margin-top: 8px; }
  .notes { margin-top: 10px; color: var(--text); background: var(--panel2); border-radius: 6px; padding: 8px 10px; }
  .grid2 { display: grid; grid-template-columns: 1fr 1fr; gap: 14px; }
  .pick { display: flex; gap: 8px; }
  .pick button { white-space: nowrap; }
  .actions { margin-top: 14px; display: flex; gap: 8px; }
  .err { color: var(--red); }
</style>
