<!-- Deterministic identicon. Pure function of peer hash + resolved name
     (peerAvatar() in modules/lxmf.ts) so it is identical here and on the
     320×240 device port. No DOM-only logic. A group draws the people glyph
     in place of initials, on the same per-hash colour. -->
<template>
  <div class="avatar" :style="style" :title="peer">
    <q-icon v-if="group" :name="matGroup" :size="`${Math.round(size * 0.58)}px`" />
    <template v-else>{{ glyph }}</template>
  </div>
</template>

<script setup lang="ts">
import { computed } from 'vue'
import { matGroup } from '@quasar/extras/material-icons'
import { peerAvatar } from '../../modules/lxmf'

const props = withDefaults(defineProps<{
  peer: string
  name?: string
  size?: number
  group?: boolean
}>(), { name: '', size: 36, group: false })

const a = computed(() => peerAvatar(props.peer, props.name))
const glyph = computed(() => a.value.glyph)
const style = computed(() => ({
  width: `${props.size}px`,
  height: `${props.size}px`,
  fontSize: `${Math.round(props.size * 0.4)}px`,
  background: `hsl(${a.value.hue} 45% 32%)`,
  color: `hsl(${a.value.hue} 70% 86%)`,
}))
</script>

<style scoped>
.avatar {
  border-radius: 50%;
  display: flex;
  align-items: center;
  justify-content: center;
  font-weight: 600;
  flex: none;
  user-select: none;
  letter-spacing: 0.02em;
}
</style>
