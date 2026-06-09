<!-- Composition root for the standalone Announces monitor (browser form
     factor). FloatingWindow + AnnouncesView. The "Message" row action
     is the bridge into messaging: set the shared activePeer and raise
     the Messages window (plan §7.1). -->
<template>
  <FloatingWindow
    id="lxmf-announces"
    :title="title"
    :visible="visible"
    :default-geom="defaultGeom"
    :min-size="{ w: 24, h: 12 }"
    @update:visible="v => emit('update:visible', v)"
  >
    <template #default>
      <AnnouncesView :announces="lxmf.announces.value" @message="onMessage" />
    </template>
  </FloatingWindow>
</template>

<script setup lang="ts">
import FloatingWindow from 'spangap-browser/components/FloatingWindow.vue'
import AnnouncesView from '../components/lxmf/AnnouncesView.vue'
import { useLxmf, showMessages } from '../modules/lxmf'

defineProps<{ visible: boolean; title: string }>()
const emit = defineEmits<{ 'update:visible': [value: boolean] }>()

const defaultGeom = { x: 14, y: 10, w: 70, h: 60 }
const lxmf = useLxmf()

function onMessage(peer: string) {
  lxmf.openPeer(peer)
  showMessages(lxmf.activeIdentity.value)   // raise the active identity's window
}
</script>
