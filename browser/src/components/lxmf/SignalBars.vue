<!-- SignalBars — amber link-quality bars. `local` (0..4) renders as an ascending
     set; when `remote` is a number (>= 0) a mirrored DESCENDING set is drawn
     first, so a link with both readings shows as a valley (remote down-staircase,
     then local up) — the same shape as the on-device widget. It sizes its own
     width from how many bars are present. Renders nothing when there's no signal. -->
<template>
  <span v-if="local > 0 || hasRemote" class="sig" :aria-label="`signal ${local} of 4`">
    <template v-if="hasRemote">
      <i v-for="n in 4" :key="'r' + n" :class="{ on: n <= remoteLit }"
         :style="{ height: `${10 - 2 * n}px` }"></i>
    </template>
    <i v-for="n in 4" :key="'l' + n" :class="{ on: n <= local }" :style="{ height: `${2 * n}px` }"></i>
  </span>
</template>

<script setup lang="ts">
import { computed } from 'vue'
const props = defineProps<{ local: number; remote?: number }>()
const hasRemote = computed(() => typeof props.remote === 'number' && props.remote >= 0)
const remoteLit = computed(() => props.remote ?? 0)
</script>

<style scoped>
.sig { display: inline-flex; align-items: flex-end; gap: 1px; height: 10px; }
.sig i { width: 2px; border-radius: 1px; background: rgba(224, 180, 34, 0.28); }
.sig i.on { background: #ffd400; }
</style>
