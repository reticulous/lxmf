<!-- MessagesWindows — the self-mounting wrapper around MessagesWindow: one
     Messages window per usable LXMF identity, or a single FALLBACK_ID window
     when there are none (so the "create an identity" guidance stays
     reachable). Registered as a bare window mount (component only) from
     registerLxmf(); owns its own v-for and the per-identity visible/focus
     records, so the registry contract stays one-component-per-straddle. -->
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
</template>

<script setup lang="ts">
import { computed } from 'vue'
import MessagesWindow from './MessagesWindow.vue'
import { messagesVisibleById, messagesFocusById, useLxmf, FALLBACK_ID } from '../modules/lxmf'

const lxmf = useLxmf()
const lxmfWindows = computed(() => {
  const u = lxmf.usableIdentities.value
  return u.length
    ? u.map(i => ({ n: i.n, displayName: i.displayName }))
    : [{ n: FALLBACK_ID, displayName: '' }]
})
</script>
