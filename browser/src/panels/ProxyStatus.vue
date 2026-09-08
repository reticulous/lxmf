<!-- ProxyStatus — top-bar indicator for the selected identity's Channel to the
     server holding its account (lxmf.id.<n>.proxy_link): grey = down, amber =
     connecting, green = active. Collapses to nothing while this device answers
     on its own address (key absent / empty), which is the common case.

     While it is not green, mail is arriving at the server and not here — which
     is the whole reason a proxied user wants this at a glance. -->
<template>
  <div v-if="link" class="proxy-status" :style="{ color }" :title="tooltip">
    <ProxyIcon />
  </div>
</template>

<script setup lang="ts">
import { computed } from 'vue'
import { useDeviceStore } from 'spangap-browser/stores/device'
import { useLxmf } from '../modules/lxmf'
import ProxyIcon from '../components/lxmf/ProxyIcon.vue'

const device = useDeviceStore()
const lx = useLxmf()

const link = computed(() => {
  const n = lx.activeIdentity.value
  if (n < 0) return ''
  const v = device.get(`lxmf.id.${n}.proxy_link`)
  return v == null ? '' : String(v)
})

/* The firmware publishes the whole line finished; this only picks it up. */
const tooltip = computed(() => {
  const n = lx.activeIdentity.value
  if (n < 0) return ''
  return String(device.get(`lxmf.id.${n}.proxy_text`) ?? '')
})

const color = computed(() =>
  link.value === 'active'     ? '#3fa34d' :
  link.value === 'connecting' ? '#d4a017' : '#888')
</script>

<style scoped>
.proxy-status { display: inline-flex; align-items: center; }
</style>
