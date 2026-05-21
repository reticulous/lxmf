<!-- Deterministic identicon. Pure function of peer hash + resolved name
     (peerAvatar() in modules/lxmf.ts) so it is identical here and on the
     320×240 device port. No DOM-only logic. -->
<template>
  <div class="avatar" :style="style" :title="peer">{{ glyph }}</div>
</template>

<script setup lang="ts">
import { computed } from 'vue'
import { peerAvatar } from '../../modules/lxmf'

const props = withDefaults(defineProps<{
  peer: string
  name?: string
  size?: number
}>(), { name: '', size: 36 })

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
