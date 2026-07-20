<!-- ContactSignal — amber link-quality bars for a peer's own direct signal
     (lxmf.contactsig.<peer>.*, the RSSI/SNR of the last zero-hop radio packet
     from them). With `fallback-gw`, falls back to the gateway signal (rnsd.gw.*)
     when we have no direct sample for the peer — the conversation-header rule
     where a contact's own signal OVERRULES the gateway one. Collapses to nothing
     when there's no signal to show. -->
<template>
  <span v-if="bars > 0 && opacity > 0" class="csig" :style="{ opacity }" :aria-label="`signal ${bars} of 4`">
    <i v-for="n in 4" :key="n" :class="{ on: n <= bars }"></i>
  </span>
</template>

<script setup lang="ts">
import { computed, ref, onMounted, onUnmounted } from 'vue'
import { useDeviceStore } from 'spangap-browser/stores/device'
import { loraBars } from '../../modules/lxmf'

const props = defineProps<{ peer: string; fallbackGw?: boolean }>()
const device = useDeviceStore()

function pf(v: unknown): number | undefined {
  if (v === null || v === undefined || v === '') return undefined
  const n = Number(v)
  return Number.isFinite(n) ? n : undefined
}
function barsAt(prefix: string): number {
  return loraBars(pf(device.get(`${prefix}.rssi`)), pf(device.get(`${prefix}.snr`)))
}

const directBars = computed(() => (props.peer ? barsAt(`lxmf.contactsig.${props.peer}`) : 0))
const gwBars = computed(() => (props.fallbackGw ? barsAt('rnsd.gw') : 0))
// The peer's own signal overrules the gateway; only the gateway fallback fades.
const usingGw = computed(() => directBars.value <= 0 && gwBars.value > 0)
const bars = computed(() => (directBars.value > 0 ? directBars.value : gwBars.value))

/* Age-fade for the gateway fallback only, mirroring the top-bar indicator:
 * linear to 0 over 30 min from rnsd.gw.timestamp (device unix seconds). The
 * ticking clock only runs where a gateway fallback is possible (the header). */
const FADE_S = 30 * 60
const nowSec = ref(Math.floor(Date.now() / 1000))
let timer: ReturnType<typeof setInterval> | undefined
onMounted(() => { if (props.fallbackGw) timer = setInterval(() => { nowSec.value = Math.floor(Date.now() / 1000) }, 20000) })
onUnmounted(() => { if (timer) clearInterval(timer) })

const opacity = computed(() => {
  if (!usingGw.value) return 1
  const ts = pf(device.get('rnsd.gw.timestamp'))
  if (ts === undefined) return 1
  const age = nowSec.value - ts
  if (age <= 0) return 1
  if (age >= FADE_S) return 0
  return 1 - age / FADE_S
})
</script>

<style scoped>
.csig { display: inline-flex; align-items: flex-end; gap: 1px; height: 10px; flex: none; }
.csig i { width: 2px; border-radius: 1px; background: rgba(224, 180, 34, 0.28); }
.csig i:nth-child(1) { height: 4px; }
.csig i:nth-child(2) { height: 6px; }
.csig i:nth-child(3) { height: 8px; }
.csig i:nth-child(4) { height: 10px; }
.csig i.on { background: #ffd400; }
</style>
