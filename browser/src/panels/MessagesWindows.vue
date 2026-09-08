<!-- MessagesWindows — the self-mounting wrapper around MessagesWindow: one
     Messages window per usable LXMF identity, or a single FALLBACK_ID window
     when there are none (so the "create an identity" guidance stays
     reachable). Registered as a bare window mount (component only) from
     registerLxmf(); owns its own v-for and the per-identity visible/focus
     records, so the registry contract stays one-component-per-straddle.

     It also renders the identity chooser the dock icon raises when there is
     more than one account. The chooser only ever OPENS a window: the windows
     are independent, so picking a second account puts it beside the first
     rather than replacing it. -->
<template>
  <MessagesWindow
    v-for="w in lxmfWindows"
    :key="w.n"
    :identity="w.n"
    :visible="messagesVisibleById[w.n] ?? false"
    :focus-token="messagesFocusById[w.n] ?? 0"
    :title="w.displayName ? `LXMF Messages - ${w.displayName}` : 'LXMF Messages'"
    @update:visible="v => (messagesVisibleById[w.n] = v)"
  />

  <Teleport to="body">
    <div v-if="identityChooserOpen" class="chooser-back" @click.self="close">
      <div class="chooser" role="dialog" aria-label="Choose an LXMF account">
        <div class="head">Which account?</div>
        <button
          v-for="i in lxmf.usableIdentities.value"
          :key="i.n"
          class="row"
          @click="pick(i.n)"
        >
          <span class="name">{{ i.displayName }}</span>
          <span class="addr">{{ i.destHash.slice(0, 16) }}…</span>
          <span v-if="messagesVisibleById[i.n]" class="open">open</span>
        </button>
      </div>
    </div>
  </Teleport>
</template>

<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted } from 'vue'
import MessagesWindow from './MessagesWindow.vue'
import {
  messagesVisibleById, messagesFocusById, identityChooserOpen,
  showMessages, useLxmf, FALLBACK_ID,
} from '../modules/lxmf'

const lxmf = useLxmf()
const lxmfWindows = computed(() => {
  const u = lxmf.usableIdentities.value
  return u.length
    ? u.map(i => ({ n: i.n, displayName: i.displayName }))
    : [{ n: FALLBACK_ID, displayName: '' }]
})

function close() { identityChooserOpen.value = false }
function pick(n: number) { close(); showMessages(n) }

/* Escape dismisses, as it does for every other overlay. Bound while this mount
 * lives (one instance, registered once) rather than per open — the handler is
 * a no-op unless the chooser is up. */
function onKey(e: KeyboardEvent) {
  if (e.key === 'Escape' && identityChooserOpen.value) close()
}
onMounted(() => window.addEventListener('keydown', onKey))
onBeforeUnmount(() => window.removeEventListener('keydown', onKey))
</script>

<style scoped>
.chooser-back {
  position: fixed; inset: 0; z-index: 4000;
  background: rgba(0, 0, 0, 0.45);
  display: flex; align-items: center; justify-content: center;
}
.chooser {
  min-width: 260px; max-width: 90vw;
  background: #1e1e1e; color: #e8e8e8;
  border: 1px solid rgba(255, 255, 255, 0.14); border-radius: 10px;
  box-shadow: 0 12px 40px rgba(0, 0, 0, 0.5);
  padding: 8px; display: flex; flex-direction: column; gap: 4px;
}
.head { padding: 6px 8px 8px; font-size: 12px; color: #9aa3ad; }
.row {
  display: grid; grid-template-columns: 1fr auto; gap: 2px 10px;
  align-items: baseline; text-align: left;
  background: #2a2a2a; color: inherit;
  border: 1px solid transparent; border-radius: 6px;
  padding: 8px 10px; font: inherit; cursor: pointer;
}
.row:hover, .row:focus-visible { border-color: rgba(120, 170, 140, 0.6); outline: none; }
.name { font-size: 14px; }
.addr { grid-column: 1; font-size: 11px; color: #8a93a0; font-family: monospace; }
.open { grid-row: 1 / span 2; font-size: 11px; color: #78aa8c; }
</style>
