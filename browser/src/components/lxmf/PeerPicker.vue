<!-- "New message" picker. Search over peerDirectory: Contacts first
     (you've talked to them), then Announces (name substring + hash).
     This is the ONLY place Announces feeds messaging (plan §4).
     Selecting emits the peer; the composition layer opens the thread. -->
<template>
  <div class="picker">
    <div class="phead">
      <span>New message</span>
      <button class="x" title="Close" @click="emit('close')">
        <q-icon :name="matClose" size="18px" />
      </button>
    </div>

    <input
      ref="box" v-model="q" class="search"
      placeholder="Name or 32-hex address" autocomplete="off"
      @keydown.enter="onEnter" @keydown.esc="emit('close')"
    />

    <div class="results">
      <div v-if="rawHexReady" class="entry" @click="pick(hexQuery)">
        <PeerAvatar :peer="hexQuery" name="" :size="34" />
        <div class="m">
          <div class="n">Message this address</div>
          <div class="h">{{ hexQuery }}</div>
        </div>
      </div>

      <div v-if="known.length" class="grp">Contacts</div>
      <div v-for="r in known" :key="'k' + r.peer" class="entry" @click="pick(r.peer)">
        <PeerAvatar :peer="r.peer" :name="r.name" :size="34" />
        <div class="m"><div class="n">{{ r.name }}</div><div class="h">{{ r.peer }}</div></div>
      </div>

      <div v-if="heard.length" class="grp">On the Mesh</div>
      <div v-for="r in heard" :key="'a' + r.peer" class="entry" @click="pick(r.peer)">
        <PeerAvatar :peer="r.peer" :name="r.name" :size="34" />
        <div class="m"><div class="n">{{ r.name }}</div><div class="h">{{ r.peer }}</div></div>
      </div>

      <div v-if="!known.length && !heard.length && !rawHexReady" class="empty">
        No matches.
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { computed, onMounted, ref } from 'vue'
import { matClose } from '@quasar/extras/material-icons'
import PeerAvatar from './PeerAvatar.vue'

const props = defineProps<{
  directory: { peer: string; name: string; known: boolean }[]
}>()
const emit = defineEmits<{ pick: [peer: string]; close: [] }>()

const q = ref('')
const box = ref<HTMLInputElement | null>(null)
onMounted(() => box.value?.focus())

const isHex64 = (s: string) => /^[0-9a-f]{32}$/i.test(s)
const hexQuery = computed(() => q.value.trim().toLowerCase())
const rawHexReady = computed(() =>
  isHex64(hexQuery.value) && !props.directory.some(d => d.peer === hexQuery.value))

const filtered = computed(() => {
  const needle = q.value.trim().toLowerCase()
  if (!needle) return props.directory
  return props.directory.filter(d =>
    d.name.toLowerCase().includes(needle) || d.peer.toLowerCase().includes(needle))
})
const known = computed(() => filtered.value.filter(d => d.known))
const heard = computed(() => filtered.value.filter(d => !d.known))

function pick(peer: string) { emit('pick', peer); emit('close') }
function onEnter() {
  if (rawHexReady.value) return pick(hexQuery.value)
  const only = [...known.value, ...heard.value]
  if (only.length === 1) pick(only[0]!.peer)
}
</script>

<style scoped>
.picker {
  position: absolute; inset: 0; background: #1c1c1c; z-index: 5;
  display: flex; flex-direction: column;
}
.phead {
  display: flex; align-items: center; justify-content: space-between;
  padding: 8px 10px; border-bottom: 1px solid rgba(255,255,255,0.08);
  color: #e8e8e8; font-weight: 600; font-size: calc(14px * var(--rfs, 1));
}
.x { background: none; border: none; color: #9a9a9a; cursor: pointer; padding: 2px; }
.search {
  margin: 10px; background: #2a2a2a; color: #e8e8e8;
  border: 1px solid rgba(255,255,255,0.12); border-radius: 8px;
  padding: 8px 12px; font-size: calc(13px * var(--rfs, 1)); outline: none;
}
.search:focus { border-color: rgba(120,170,140,0.6); }
.results { flex: 1; overflow-y: auto; padding-bottom: 8px; }
.grp {
  color: #888; font-size: calc(11px * var(--rfs, 1)); text-transform: uppercase;
  letter-spacing: 0.05em; padding: 8px 12px 4px;
}
.entry {
  display: flex; gap: 10px; align-items: center;
  padding: 7px 12px; cursor: pointer;
}
.entry:hover { background: rgba(255,255,255,0.05); }
.m { min-width: 0; flex: 1; }
.n { color: #e8e8e8; font-size: calc(13px * var(--rfs, 1)); font-weight: 500;
     overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.h { color: #888; font-size: calc(11px * var(--rfs, 1)); font-family: 'JetBrains Mono', monospace;
     overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.empty { color: #888; font-style: italic; text-align: center; padding: 20px; font-size: calc(13px * var(--rfs, 1)); }
</style>
