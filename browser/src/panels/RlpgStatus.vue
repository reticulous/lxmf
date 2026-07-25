<!-- RlpgStatus — top-bar indicator for the selected identity's own RLPG
     store-and-forward mailbox (lxmf.id.<n>.rlpg_state): grey = configured but
     idle, yellow = connecting, green = connected. Collapses to nothing while
     no mailbox is configured (key absent / empty). -->
<template>
  <div v-if="state" class="rlpg-status" :style="{ color }" :title="tooltip">
    <RlpgIcon />
  </div>
</template>

<script setup lang="ts">
import { computed } from 'vue'
import { useDeviceStore } from 'spangap-browser/stores/device'
import { useLxmf, formatCertExpiry } from '../modules/lxmf'
import RlpgIcon from '../components/lxmf/RlpgIcon.vue'

const device = useDeviceStore()
const lx = useLxmf()

const state = computed(() => {
  const n = lx.activeIdentity.value
  if (n < 0) return ''
  const v = device.get(`lxmf.id.${n}.rlpg_state`)
  return v == null ? '' : String(v)
})

/* Own mailbox node cert (present only when a mailbox runs on this unit,
 * slot 0): show its validity as a local date, never a raw epoch. */
const tooltip = computed(() => {
  let s = `RLPG mailbox: ${state.value}`
  const cs = device.get('rlpg.id.0.cert_state')
  if (cs === 'valid' || cs === 'expired') {
    const exp = Number(device.get('rlpg.id.0.cert_expires') ?? 0)
    s += cs === 'expired'
      ? `\nCertificate expired ${formatCertExpiry(exp)}`
      : `\nCertificate valid until ${formatCertExpiry(exp)}`
  }
  return s
})

const color = computed(() =>
  state.value === 'connected'  ? '#3fa34d' :
  state.value === 'connecting' ? '#d4a017' : '#888')
</script>

<style scoped>
.rlpg-status { display: inline-flex; align-items: center; }
</style>
