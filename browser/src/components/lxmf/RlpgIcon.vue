<!-- RlpgIcon — the RLPG store-and-forward mailbox glyph: a small right-pointing
     arrow whose shaft is dashed (a message parked hop-by-hop rather than
     delivered live). Strokes in currentColor, so the parent's `color` tints it;
     collapses nothing itself — show/hide is the parent's call. -->
<template>
  <svg class="rlpg" viewBox="0 0 14 14" aria-hidden="true">
    <path d="M1 7h7" stroke-dasharray="2 2" />
    <path d="M8 3.5 12 7l-4 3.5" />
  </svg>
</template>

<style scoped>
.rlpg {
  width: 13px; height: 13px; flex: none;
  fill: none; stroke: currentColor; stroke-width: 1.6;
  stroke-linecap: round; stroke-linejoin: round;
}
</style>
