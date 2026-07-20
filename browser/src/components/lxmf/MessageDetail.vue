<!-- Per-message detail overlay: every field we store about a message, plus the
     routing telemetry from the lxmf.msgmeta store (interface, RNS first hop,
     hops). Opened by clicking a bubble. Presentational: data in, close out. -->
<template>
  <div class="card">
    <div class="chead">
      <button class="x" title="Close" @click="emit('close')">
        <q-icon :name="matArrowBack" size="20px" />
      </button>
      <span>Message details</span>
    </div>

    <div class="body">
      <div class="hero">
        <span class="dir" :class="m.dir">{{ m.dir === 'in' ? 'Incoming' : 'Outgoing' }}</span>
        <span class="status">{{ statusName }}</span>
        <!-- Bars XOR "L": direct → signal bars (valley when the peer reported a
             remote reading); relayed → "L". -->
        <span v-if="lora && (relayed || bars || hasRemote)" class="lora" :title="m.iface">
          <span v-if="relayed" class="l">L</span>
          <SignalBars v-else :local="bars" :remote="hasRemote ? remoteBars : undefined" />
        </span>
      </div>

      <template v-if="m.title">
        <div class="sect">Title</div>
        <div class="sn text">{{ m.title }}</div>
      </template>

      <div class="sect">Content</div>
      <div class="sn text">{{ m.content || '—' }}</div>

      <!-- Routing telemetry (msgmeta). Absent for DIRECT/Resource transfers. -->
      <div class="sect">Routing</div>
      <div v-if="meta" class="kv">
        <div class="k">Interface</div><div class="v">{{ meta.iface || '—' }}</div>
        <div class="k">Hops</div><div class="v">{{ meta.hops }}</div>
        <div class="k">First hop</div>
        <div class="v mono">{{ firstHop }}</div>
        <template v-if="meta.rssi">
          <div class="k">{{ m.dir === 'in' ? 'RSSI' : 'RSSI (proof)' }}</div><div class="v">{{ meta.rssi }} dBm</div>
        </template>
        <template v-if="meta.snr">
          <div class="k">{{ m.dir === 'in' ? 'SNR' : 'SNR (proof)' }}</div><div class="v">{{ meta.snr }} dB</div>
        </template>
        <!-- Remote reading: the peer's own rx of the message we sent (rx-report
             proof); outbound only, and only when the peer is reticulous. -->
        <template v-if="meta.remoteRssi">
          <div class="k">Remote RSSI</div><div class="v">{{ meta.remoteRssi }} dBm</div>
        </template>
        <template v-if="meta.remoteSnr">
          <div class="k">Remote SNR</div><div class="v">{{ meta.remoteSnr }} dB</div>
        </template>
      </div>
      <div v-else class="sn small">No routing data recorded (DIRECT/Resource, or pre-dating this message).</div>

      <div class="sect">Peer</div>
      <div class="addr">
        <span class="addrhex">{{ peerName }}<br>{{ grouped(m.peer) }}</span>
        <button class="copy" :title="copiedKey === 'peer' ? 'Copied' : 'Copy'" @click="copy('peer', m.peer)">
          <q-icon :name="copiedKey === 'peer' ? matCheck : matContentCopy" size="15px" />
        </button>
      </div>

      <div class="sect">Message ID</div>
      <div class="addr">
        <span class="addrhex">{{ grouped(m.messageId) || '—' }}</span>
        <button v-if="m.messageId" class="copy" :title="copiedKey === 'mid' ? 'Copied' : 'Copy'"
                @click="copy('mid', m.messageId)">
          <q-icon :name="copiedKey === 'mid' ? matCheck : matContentCopy" size="15px" />
        </button>
      </div>

      <template v-if="isReply">
        <div class="sect">In reply to</div>
        <div class="sn small">{{ grouped(m.replyTo!) }}</div>
      </template>

      <div class="sect">Details</div>
      <div class="kv">
        <div class="k">Timestamp</div><div class="v">{{ sentTime }}</div>
        <template v-if="m.dir === 'out'">
          <div class="k">Method</div><div class="v">{{ m.method || 'auto' }}</div>
          <div class="k">Attempts</div><div class="v">{{ triesLabel }}</div>
        </template>
        <template v-else>
          <div class="k">Read</div><div class="v">{{ m.read ? 'yes' : 'no' }}</div>
        </template>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { computed, ref } from 'vue'
import { matArrowBack, matContentCopy, matCheck } from '@quasar/extras/material-icons'
import { type Message, type MsgMeta, lxmfStatusName, loraBars, LXMF_TRIES_GAVEUP } from '../../modules/lxmf'
import SignalBars from './SignalBars.vue'

const props = defineProps<{
  m: Message
  meta: MsgMeta | null
  peerName: string
}>()
const emit = defineEmits<{ close: [] }>()

const statusName = computed(() => lxmfStatusName(props.m.status))
const lora = computed(() => (props.m.iface ?? '').startsWith('LoRa'))
const bars = computed(() => loraBars(
  props.meta?.rssi ? parseFloat(props.meta.rssi) : undefined,
  props.meta?.snr  ? parseFloat(props.meta.snr)  : undefined,
))
/* Remote reading (peer's rx of our outbound message, from an rx-report proof):
 * the second, descending set of the valley. Present only for a reticulous peer. */
const hasRemote = computed(() => !!(props.meta?.remoteRssi || props.meta?.remoteSnr))
const remoteBars = computed(() => loraBars(
  props.meta?.remoteRssi ? parseFloat(props.meta.remoteRssi) : undefined,
  props.meta?.remoteSnr  ? parseFloat(props.meta.remoteSnr)  : undefined,
))
/* hops is the raw RNS count (1 = directly received, >1 = relayed): "L" for
 * relayed, bars for direct. */
const relayed = computed(() => (props.meta?.hops ?? props.m.hops ?? 0) > 1)

const grouped = (hex: string) => (hex.match(/.{1,4}/g) ?? []).join(' ')

/* first_hop is 64-hex; all-zero (or empty) means a direct neighbour. */
const firstHop = computed(() => {
  const fh = props.meta?.firstHop ?? ''
  return fh && !/^0+$/.test(fh) ? grouped(fh) : 'direct (no transit node)'
})

const isReply = computed(() => {
  const r = props.m.replyTo ?? ''
  return r.length > 0 && !/^0+$/.test(r)
})

const sentTime = computed(() =>
  props.m.ts ? new Date(props.m.ts * 1000).toLocaleString() : '—')
const triesLabel = computed(() =>
  props.m.tries === LXMF_TRIES_GAVEUP ? 'gave up' : String(props.m.tries))

const copiedKey = ref('')
let copiedTimer: ReturnType<typeof setTimeout> | undefined
async function copy(key: string, val: string) {
  try { await navigator.clipboard.writeText(val) } catch { /* ignore */ }
  copiedKey.value = key
  clearTimeout(copiedTimer)
  copiedTimer = setTimeout(() => { copiedKey.value = '' }, 1500)
}
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
.hero { display: flex; align-items: center; gap: 8px; margin-bottom: 6px; }
.dir { font-weight: 600; font-size: calc(15px * var(--rfs, 1)); color: #e8e8e8; }
.dir.in  { color: #9ec9ff; }
.dir.out { color: #9fe0b0; }
.status { color: #8a8a8a; font-size: calc(12px * var(--rfs, 1)); text-transform: uppercase; letter-spacing: 0.04em; }
/* Quiet LoRa indicator: ghost amber "L" + link-quality bars. Matches the bubble. */
.lora { display: inline-flex; align-items: flex-end; gap: 3px; line-height: 1; }
.lora .l { font-weight: 700; font-size: calc(10px * var(--rfs, 1)); color: #e0b422; }
.bars { display: inline-flex; align-items: flex-end; gap: 1px; height: 10px; }
.bars i { width: 2px; border-radius: 1px; background: rgba(224, 180, 34, 0.28); }
.bars i.on { background: #ffd400; }
.bars i:nth-child(1) { height: 4px; }
.bars i:nth-child(2) { height: 6px; }
.bars i:nth-child(3) { height: 8px; }
.bars i:nth-child(4) { height: 10px; }
.sect {
  color: #aaa; font-size: calc(12px * var(--rfs, 1)); text-transform: uppercase;
  letter-spacing: 0.05em; margin: 16px 0 6px;
}
.sn {
  font-family: 'JetBrains Mono', 'Menlo', monospace; font-size: calc(13px * var(--rfs, 1));
  color: #c8d8c8; background: #232323; border-radius: 8px;
  padding: 10px 12px; word-break: break-word; line-height: 1.6;
}
.sn.text { font-family: inherit; color: #e8e8e8; white-space: pre-wrap; }
.sn.small { font-size: calc(11px * var(--rfs, 1)); color: #9a9a9a; }
.kv {
  display: grid; grid-template-columns: max-content 1fr; gap: 4px 14px;
  background: #232323; border-radius: 8px; padding: 10px 12px;
}
.k { color: #8a8a8a; font-size: calc(12px * var(--rfs, 1)); }
.v { color: #e0e0e0; font-size: calc(12px * var(--rfs, 1)); word-break: break-word; }
.v.mono { font-family: 'JetBrains Mono', 'Menlo', monospace; }
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
</style>
