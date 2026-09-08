<!-- Direction-aware message bubble. Outbound right, inbound left.
     The status footer is the honest-receipts surface (plan §6):
       • in flight (…) until proven delivered — no reassuring single check.
       • TWO ticks (delivered) = cryptographic proof; there is no third.
       • no proof before the timeout is a FAILURE ("no proof received"),
         shown with the error inline + one-tap Resend.
     Emits intent only; the composition layer decides what it does. -->
<template>
  <div class="row" :class="m.dir === 'out' ? 'out' : 'in'">
    <div v-if="menuOpen" class="menu-bg" @click="menuOpen = false"
         @contextmenu.prevent="menuOpen = false"></div>

    <div class="moreWrap" :class="{ pinned: menuOpen }">
      <button class="more" title="More"
              @click.stop="menuOpen = !menuOpen">
        <q-icon :name="matMoreVert" size="16px" />
      </button>
      <div v-if="menuOpen" class="msgmenu">
        <button class="mi danger"
                @click="menuOpen = false; emit('delete', m)">
          <q-icon :name="matDelete" size="15px" /> Delete
        </button>
      </div>
    </div>

    <div class="bubble" :class="{ muted: m.status === LxmfStatus.Cancelled }"
         @click="emit('open', m)"
         @contextmenu.prevent="emit('menu', m)">
      <!-- A proxy server withheld this body — it is over the link's inline
           threshold. Offer the download instead of the text; the bubble fills
           in when it lands. The size is why the offer is worth making rather
           than pushing it unasked. -->
      <button v-if="m.bodyAbsent" class="download" @click.stop="emit('fetch', m)">
        <q-icon :name="matDownload" size="15px" /> Download {{ m.bodySize }} bytes
      </button>
      <div v-else class="content"><template v-for="(seg, i) in segments" :key="i"><a
          v-if="seg.link" class="nomad-link"
          @click.stop="openNomad(seg.link.hash, seg.link.path)"
        >{{ seg.text }}</a><a
          v-else-if="seg.web" class="nomad-link"
          :href="seg.web" target="_blank" rel="noopener noreferrer" @click.stop
        >{{ seg.text }}</a><span v-else>{{ seg.text }}</span></template></div>

      <!-- meta: ALL-CAPS status name (outbound, left, smaller) · time · glyph.
           glyph: … in flight · ✓✓ delivered (green) · ✓ a machine that is not
           mine has it (grey, ON_PROXY / ON_PN) · ✕ cancelled (grey) / gave-up
           tries==255 (red). -->
      <div class="meta">
        <span v-if="m.dir === 'out' && m.status !== LxmfStatus.Delivered"
              class="statusName">{{ statusName }}</span>
        <span class="time">{{ clock }}</span>
        <template v-if="m.dir === 'out'">
          <span v-if="m.status === LxmfStatus.Delivered" class="chip ticks"
                title="delivered — cryptographic proof received">
            <DeliveryTicks variant="delivered" />
          </span>
          <!-- Held by a proxy server, or uploaded to a propagation node: one
               open circle + check — a machine that is not mine has it. Before
               the gave-up test, since these carry tries==255. A refusal or a
               real failure falls through to ✕. -->
          <span v-else-if="m.status === LxmfStatus.OnProxy ||
                           m.status === LxmfStatus.OnPn" class="chip ticks"
                title="a machine that is not mine has it (proxy server / propagation node)">
            <DeliveryTicks variant="sent" />
          </span>
          <span v-else-if="m.status === LxmfStatus.Cancelled" class="chip">
            <q-icon :name="matClose" size="15px" />
          </span>
          <span v-else-if="m.tries === LXMF_TRIES_GAVEUP" class="chip bad">
            <q-icon :name="matClose" size="15px" />
          </span>
          <span v-else class="chip dots">…</span>
        </template>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { computed, ref } from 'vue'
import { matClose, matMoreVert, matDelete, matDownload }
  from '@quasar/extras/material-icons'
import DeliveryTicks from './DeliveryTicks.vue'
import { type Message, segmentMessage, openNomad, formatMsgTime,
         lxmfStatusName, LxmfStatus, LXMF_TRIES_GAVEUP } from '../../modules/lxmf'

const props = defineProps<{ m: Message }>()

/* Split the body into text + tappable Nomad-page-link runs. */
const segments = computed(() => segmentMessage(props.m.content))
const emit = defineEmits<{
  resend: [m: Message]
  menu: [m: Message]
  delete: [m: Message]
  open: [m: Message]
  fetch: [m: Message]
}>()

const menuOpen = ref(false)

const clock = computed(() => formatMsgTime(props.m.ts))
const statusName = computed(() => lxmfStatusName(props.m.status))
</script>

<style scoped>
.row { display: flex; align-items: center; gap: 3px; margin: 2px 0; }
.row.out { justify-content: flex-end; }
.row.in  { justify-content: flex-start; }
.row.in .moreWrap { order: 2; }

.menu-bg { position: fixed; inset: 0; z-index: 20; }

.moreWrap {
  position: relative; flex: none;
  opacity: 0; transition: opacity 0.1s;
}
.row:hover .moreWrap,
.moreWrap.pinned { opacity: 1; }
.more {
  display: flex; align-items: center; justify-content: center;
  background: none; border: none; color: #8a8a8a;
  cursor: pointer; padding: 3px; border-radius: 50%;
}
.more:hover { background: rgba(255,255,255,0.08); color: #cfcfcf; }

.msgmenu {
  position: absolute; top: 100%; z-index: 21;
  min-width: 132px; padding: 4px;
  background: #2b2b2b; border: 1px solid rgba(255,255,255,0.10);
  border-radius: 8px; box-shadow: 0 6px 22px rgba(0,0,0,0.5);
}
.row.out .msgmenu { right: 0; }
.row.in  .msgmenu { left: 0; }
.mi {
  display: flex; align-items: center; gap: 8px; width: 100%;
  background: none; border: none; color: #e8e8e8;
  font-size: calc(13px * var(--rfs, 1)); text-align: left; padding: 7px 9px;
  border-radius: 5px; cursor: pointer;
}
.mi:hover { background: rgba(255,255,255,0.07); }
.mi.danger { color: #d98a8a; }
.mi.danger:hover { background: rgba(217,138,138,0.14); }
.download {
  display: flex; align-items: center; gap: 6px;
  background: rgba(255,255,255,0.08); border: none; border-radius: 6px;
  color: #cfe0f5; font-size: calc(12px * var(--rfs, 1));
  padding: 5px 8px; margin: 1px 0 3px; cursor: pointer;
}
.download:hover { background: rgba(255,255,255,0.14); }

.bubble {
  max-width: 78%;
  padding: 6px 10px 4px;
  border-radius: 12px;
  font-size: calc(13px * var(--rfs, 1));
  line-height: 1.35;
  color: #e8e8e8;
  word-break: break-word;
  white-space: pre-wrap;
}
.row.out .bubble { background: #2c6bed; border-bottom-right-radius: 4px; }
.row.in  .bubble { background: #2a2a2a; border-bottom-left-radius: 4px; }
.bubble.muted { opacity: 0.55; }
/* Nomad page links quoted in a message — tap opens the Nomad browser. Matches
   the micron renderer's link blue; lighter on the blue outbound bubble. */
.nomad-link { color: #6db3ff; text-decoration: underline; cursor: pointer; word-break: break-all; }
.nomad-link:hover { color: #9ccbff; }
.row.out .nomad-link { color: #d4e6ff; }
.row.out .nomad-link:hover { color: #ffffff; }
.meta {
  display: flex; align-items: center; gap: 5px;
  justify-content: flex-end;
  margin-top: 2px;
  font-size: calc(11px * var(--rfs, 1));
  color: #c2c2c2;
}
/* status name pushed to the left in smaller print; time + glyph stay right. */
.statusName {
  flex: 1 1 auto; min-width: 0;
  font-size: calc(10px * var(--rfs, 1)); letter-spacing: 0.3px;
  color: #b0b0b0;
  overflow: hidden; text-overflow: ellipsis; white-space: nowrap;
}
.time { font-variant-numeric: tabular-nums; }
.chip { display: inline-flex; align-items: center; color: #8a93a0; }
.chip.ok  { color: #4abf6a; }
.chip.bad { color: #d9534f; }
/* delivery ticks inherit the bubble's own text colour (white on the blue
   outbound bubble); the glyph occludes itself with the bubble background. */
.chip.ticks { color: inherit; }
.chip.dots { font-weight: 700; line-height: 1; }
.bubble { cursor: pointer; }
</style>
