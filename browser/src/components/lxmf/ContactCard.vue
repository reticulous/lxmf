<!-- Per-peer detail overlay. The `trust` flag renders as a Signal-style
     Verified badge; the destination hash + ratchet are the
     safety-number analog (compare-to-verify, no QR in v1 — plan §4/§6).
     Presentational: data in, intent out. -->
<template>
  <div class="card">
    <div class="chead">
      <button class="x" title="Close" @click="emit('close')">
        <q-icon :name="matArrowBack" size="20px" />
      </button>
      <span>Contact</span>
    </div>

    <div class="body">
      <div class="hero">
        <PeerAvatar :peer="peer" :name="name" :size="72" />
        <div class="hname">{{ name }}</div>
        <div v-if="verified" class="verified">
          <q-icon :name="matVerifiedUser" size="15px" /> Verified
        </div>
        <div v-else class="unverified">Not verified</div>
      </div>

      <div v-if="reachLine" class="reach">{{ reachLine }}</div>

      <!-- Grouped in fours for eye comparison; the copy button still yields
           the bare hex (paste targets want it unspaced). -->
      <div class="sect">Destination hash</div>
      <div class="addr">
        <span class="addrhex">{{ groupedHash }}</span>
        <button class="copy" :title="copied ? 'Copied' : 'Copy'" @click="copyHash">
          <q-icon :name="copied ? matCheck : matContentCopy" size="15px" />
        </button>
      </div>

      <template v-if="ratchet">
        <div class="sect">Ratchet</div>
        <div class="sn small">{{ ratchet }}</div>
      </template>

      <button class="danger" @click="emit('delete-conversation', peer)">
        Delete conversation
      </button>
    </div>
  </div>
</template>

<script setup lang="ts">
import { computed, ref } from 'vue'
import { matArrowBack, matVerifiedUser, matContentCopy, matCheck }
  from '@quasar/extras/material-icons'
import PeerAvatar from './PeerAvatar.vue'
import type { Contact, Reachability } from '../../modules/lxmf'

const props = defineProps<{
  peer: string
  name: string
  contact: Contact | null
  reach: Reachability | null
  ratchet: string
}>()
const emit = defineEmits<{
  close: []
  'delete-conversation': [peer: string]
}>()

const verified = computed(() => (props.contact?.trust ?? 0) >= 1)

const copied = ref(false)
let copiedTimer: ReturnType<typeof setTimeout> | undefined
async function copyHash() {
  try { await navigator.clipboard.writeText(props.peer) } catch { /* ignore */ }
  copied.value = true
  clearTimeout(copiedTimer)
  copiedTimer = setTimeout(() => { copied.value = false }, 1500)
}

const groupedHash = computed(() =>
  (props.peer.match(/.{1,4}/g) ?? []).join(' '))

const reachLine = computed(() => {
  const r = props.reach
  if (!r || !r.lastSeenS) return ''
  const ageS = Math.round(Date.now() / 1000 - r.lastSeenS)
  const age =
    ageS < 60 ? 'just now'
    : ageS < 3600 ? `${Math.floor(ageS / 60)}m ago`
    : ageS < 86400 ? `${Math.floor(ageS / 3600)}h ago`
    : `${Math.floor(ageS / 86400)}d ago`
  const hops = r.hops >= 0 && r.hops < 128 ? ` · ${r.hops} hop${r.hops === 1 ? '' : 's'}` : ''
  return `Last heard on the mesh ${age}${hops}`
})
</script>

<style scoped>
.card { position: absolute; inset: 0; background: #1c1c1c; z-index: 6;
        display: flex; flex-direction: column; }
.chead {
  display: flex; align-items: center; gap: 8px;
  padding: 8px 10px; border-bottom: 1px solid rgba(255,255,255,0.08);
  color: #e8e8e8; font-weight: 600; font-size: calc(14px * var(--rfs, 1));
}
.x { background: none; border: none; color: #9a9a9a; cursor: pointer; padding: 2px; }
.body { flex: 1; overflow-y: auto; padding: 16px; }
.hero { text-align: center; margin-bottom: 14px; display: flex;
        flex-direction: column; align-items: center; gap: 6px; }
.hname { color: #e8e8e8; font-size: calc(17px * var(--rfs, 1)); font-weight: 600; margin-top: 6px; }
.verified {
  display: inline-flex; align-items: center; gap: 4px;
  color: #6fb98f; font-size: calc(12px * var(--rfs, 1));
}
.unverified { color: #888; font-size: calc(12px * var(--rfs, 1)); }
.reach { text-align: center; color: #8a8a8a; font-size: calc(12px * var(--rfs, 1)); margin-bottom: 12px; }
.sect {
  color: #aaa; font-size: calc(12px * var(--rfs, 1)); text-transform: uppercase;
  letter-spacing: 0.05em; margin: 16px 0 6px;
}
.sn {
  font-family: 'JetBrains Mono', 'Menlo', monospace; font-size: calc(13px * var(--rfs, 1));
  color: #c8d8c8; background: #232323; border-radius: 8px;
  padding: 10px 12px; word-break: break-word; line-height: 1.6;
}
.sn.small { font-size: calc(11px * var(--rfs, 1)); color: #9a9a9a; }
.addr {
  display: flex; align-items: center; gap: 8px;
  background: #232323; border-radius: 8px; padding: 8px 8px 8px 12px;
}
.addrhex {
  flex: 1; min-width: 0;
  font-family: 'JetBrains Mono', 'Menlo', monospace; font-size: calc(12px * var(--rfs, 1));
  color: #c8d8c8; line-height: 1.5;
}
.copy {
  flex: none; background: none; border: none; color: #9a9a9a;
  cursor: pointer; padding: 4px; border-radius: 5px;
}
.copy:hover { background: rgba(255,255,255,0.08); color: #cfcfcf; }
.danger {
  margin-top: 22px; width: 100%; background: none;
  border: 1px solid #a05656; color: #d98a8a; border-radius: 8px;
  padding: 9px; font-size: calc(13px * var(--rfs, 1)); cursor: pointer;
}
.danger:hover { background: rgba(160,86,86,0.15); }
</style>
