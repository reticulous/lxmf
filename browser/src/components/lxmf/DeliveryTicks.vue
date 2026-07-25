<!-- Signal-style delivery ticks: overlapping circles, each with its own check.
       sent      → one open circle + check           (reached a mailbox / one tick)
       delivered → two open circles + two checks       (delivered to the recipient)
       read      → two filled circles + two checks      (read; unused for now)
     Each circle is a same-radius white (a ring when open, a disc when closed)
     over a blue backing disc one step larger — so the white is identical in
     size whether open or closed, and the 1px blue rim that peeks out sits
     OUTSIDE the white. Where the front (right) circle overlaps the back one,
     that rim is the visible seam between them. White is currentColor (the
     bubble text); `bg` is the bubble background (the rim / knockout colour).
     These only render on the outgoing bubble. -->
<template>
  <svg :width="size" :height="size" viewBox="0 0 24 24" fill="none"
       xmlns="http://www.w3.org/2000/svg" class="ticks">
    <template v-if="variant === 'sent'">
      <!-- one circle, optically matched to a single circle of the delivered
           pair: a lone circle reads larger than the same circle paired, so it
           is drawn a hair smaller (r 5.15 vs 5.2) to sit equal to the eye -->
      <circle cx="12" cy="12" r="5.15" stroke="currentColor" stroke-width="1.6" />
      <path d="M9.82 12.1 11.2 13.5 14.08 10.12" stroke="currentColor" stroke-width="1.6"
            stroke-linecap="round" stroke-linejoin="round" />
    </template>

    <template v-else>
      <!-- back (left) circle -->
      <circle cx="8.5" cy="12" r="7" :fill="bg" />
      <circle v-if="variant === 'read'" cx="8.5" cy="12" r="6" fill="currentColor" />
      <circle v-else cx="8.5" cy="12" r="5.2" stroke="currentColor" stroke-width="1.6" />
      <path d="M6.3 12.1 7.7 13.5 10.6 10.1" :stroke="checkColor" stroke-width="1.6"
            stroke-linecap="round" stroke-linejoin="round" />
      <!-- front (right) circle: its blue backing occludes the overlap and leaves
           the 1px rim as the seam; its white matches the back's radius -->
      <circle cx="15.5" cy="12" r="7" :fill="bg" />
      <circle v-if="variant === 'read'" cx="15.5" cy="12" r="6" fill="currentColor" />
      <circle v-else cx="15.5" cy="12" r="5.2" stroke="currentColor" stroke-width="1.6" />
      <path d="M13.3 12.1 14.7 13.5 17.6 10.1" :stroke="checkColor" stroke-width="1.6"
            stroke-linecap="round" stroke-linejoin="round" />
    </template>
  </svg>
</template>

<script setup lang="ts">
import { computed } from 'vue'

const props = withDefaults(defineProps<{
  variant: 'sent' | 'delivered' | 'read'
  size?: number
  bg?: string        // bubble background — rim / occlusion / read knockout colour
}>(), { size: 15, bg: '#2c6bed' })

/* Open circles carry a white check; filled (read) circles knock it out in bg. */
const checkColor = computed(() => props.variant === 'read' ? props.bg : 'currentColor')
</script>

<style scoped>
.ticks { display: block; }
</style>
