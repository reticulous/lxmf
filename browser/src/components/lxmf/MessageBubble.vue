<!-- Direction-aware message bubble. Outbound right, inbound left.
     The status footer is the honest-receipts surface (plan §6):
       • in flight (…) until proven delivered — no reassuring single check.
       • TWO ticks (delivered) = cryptographic proof; there is no third.
       • no proof before the timeout is a FAILURE ("no proof received"),
         shown with the error inline + one-tap Resend.
     Emits intent only; the composition layer decides what it does. -->
<template>
  <!-- The row carries the record key as its DOM id: following a quote back to
       the message it came from is a scroll in the pane above, and that pane
       knows the key it is looking for, not this component. -->
  <div :id="`msg-${m.key}`" class="row" :class="m.dir === 'out' ? 'out' : 'in'">
    <!-- The three actions a message has, in Signal's order of danger: reply,
         what it is, delete. Revealed by hovering the row, by clicking the
         bubble, or by selecting text in it — a click no longer walks off to the
         detail page, which is what the (i) is for. -->
    <div class="acts" :class="{ pinned: acts }">
      <!-- A reply names its message by that message's id, and an outbound one
           has none until the firmware has packed it — so until then there is
           nothing to reply to and the action says so rather than misfiring. -->
      <button class="act" :disabled="!m.messageId"
              :title="m.messageId ? 'Reply' : 'Reply — once this message has been sent'"
              @click.stop="doReply">
        <q-icon :name="matReply" size="16px" />
      </button>
      <button class="act" title="Message details" @click.stop="acts = false; emit('open', m)">
        <q-icon :name="matInfo" size="16px" />
      </button>
      <button class="act danger" title="Delete" @click.stop="acts = false; emit('delete', m)">
        <q-icon :name="matDelete" size="16px" />
      </button>
    </div>

    <div ref="bubbleEl" class="bubble" :class="{ muted: m.status === LxmfStatus.Cancelled }"
         @click="acts = true"
         @mouseup="onSelect"
         @contextmenu.prevent="emit('menu', m)">
      <!-- What this message replies to: the quoted line, drawn from OUR copy of
           that message, with a tap back to it. A reply to a message this device
           does not hold says only that much. -->
      <button v-if="quote" class="quote" :class="{ orphan: !quote.key }"
              @click.stop="quote.key && emit('quote-open', quote.key)">
        <span v-if="quote.label" class="qwho">{{ quote.label }}</span>
        <span class="qtext">{{ quote.text || 'Replying to a message…' }}</span>
      </button>
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
           mine has it (grey, ON_OUR_PROXY / ON_PN) · ✕ cancelled (grey) /
           gave-up tries==255 (red). -->
      <div class="meta">
        <span v-if="m.dir === 'out' && !delivered"
              class="statusName">{{ statusName }}</span>
        <span class="time">{{ clock }}</span>
        <!-- Carried by a link of ours rather than by the machine that holds
             our address. Only while proxied, where the two are different
             journeys; unproxied every message goes this way and the mark would
             say nothing. Before the ticks on an outgoing message: it is about
             how the message travelled, which comes before how it landed. -->
        <span v-if="proxied && m.viaLink" class="chip link"
              title="direct — carried by an open link to this peer, not by your proxy">
          <q-icon :name="matLink" size="13px" />
        </span>
        <template v-if="m.dir === 'out'">
          <span v-if="delivered" class="chip ticks"
                :title="m.status === LxmfStatus.OurProxyDelivered
                          ? 'delivered — our proxy has the recipient\'s proof'
                          : 'delivered — cryptographic proof received'">
            <DeliveryTicks variant="delivered" />
          </span>
          <!-- Held by our proxy, or uploaded to a propagation node: one open
               circle + check — a machine that is not mine has it. Before the
               gave-up test, since these carry tries==255. A refusal or the
               proxy's own giving-up falls through to ✕. -->
          <span v-else-if="m.status === LxmfStatus.OnOurProxy ||
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
import { computed, onBeforeUnmount, ref, watch } from 'vue'
import { matClose, matReply, matInfo, matDelete, matDownload, matLink }
  from '@quasar/extras/material-icons'
import DeliveryTicks from './DeliveryTicks.vue'
import { type Message, type QuoteView, segmentMessage, openNomad, formatMsgTime,
         lxmfStatusName, LxmfStatus, LXMF_TRIES_GAVEUP } from '../../modules/lxmf'

const props = defineProps<{
  m: Message
  /* What this message replies to, already resolved against the conversation by
   * the pane that holds it — a bubble sees one message and cannot look another
   * up. Null when this message is not a reply. */
  quote?: QuoteView | null
  /* This identity's mail belongs to a proxy server — what makes a direct
   * message worth marking as one. */
  proxied?: boolean
}>()

/* Split the body into text + tappable Nomad-page-link runs. */
const segments = computed(() => segmentMessage(props.m.content))
const emit = defineEmits<{
  resend: [m: Message]
  menu: [m: Message]
  delete: [m: Message]
  open: [m: Message]
  fetch: [m: Message]
  /* Reply to this message, quoting `fragment` of it when the user selected one
   * rather than the message as a whole. */
  reply: [m: Message, fragment: string]
  /* Scroll the conversation to the message this one quotes. */
  'quote-open': [key: string]
}>()

/* The action row, held open once the bubble has been clicked or text in it
 * selected; a hover shows it without pinning it. Held open, it is dismissed by
 * a press anywhere outside this message — watched on the document rather than
 * behind a full-screen scrim, which would sit between the reader and the text
 * they are trying to select. */
const acts = ref(false)
const bubbleEl = ref<HTMLElement | null>(null)

function onDocDown(e: Event) {
  const row = bubbleEl.value?.closest('.row')
  if (row && e.target instanceof Node && row.contains(e.target)) return
  acts.value = false
}
watch(acts, on => {
  if (on) document.addEventListener('pointerdown', onDocDown, true)
  else document.removeEventListener('pointerdown', onDocDown, true)
})
onBeforeUnmount(() => document.removeEventListener('pointerdown', onDocDown, true))

/* The part of THIS bubble the user has selected, if any. Selecting a fragment
 * is how a reply comes to quote one line rather than the whole message, so the
 * selection has to be read before the click that starts the reply can collapse
 * it — hence reading it here, off the bubble, and not from the composer. */
function selectedText(): string {
  const el = bubbleEl.value
  const sel = window.getSelection()
  if (!el || !sel || sel.isCollapsed || sel.rangeCount === 0) return ''
  const r = sel.getRangeAt(0)
  if (!el.contains(r.commonAncestorContainer)) return ''
  return sel.toString().trim()
}

/* Selecting text is a way of pointing at a message, so it reveals the actions
 * the same way a click does — and unlike a click it must not be mistaken for
 * one, which is why the bubble's own click only ever opens this row. */
function onSelect() {
  if (selectedText()) acts.value = true
}

function doReply() {
  if (!props.m.messageId) return
  const frag = selectedText()
  acts.value = false
  emit('reply', props.m, frag)
}

const clock = computed(() => formatMsgTime(props.m.ts))
const statusName = computed(() => lxmfStatusName(props.m.status))

/* Two ticks, by either route: our own proof, or our proxy relaying the
 * recipient's. The two are the same fact about the message and differ only in
 * which machine holds the proof — which the title says and the glyph does not
 * need to. */
const delivered = computed(() =>
  props.m.status === LxmfStatus.Delivered ||
  props.m.status === LxmfStatus.OurProxyDelivered)
</script>

<style scoped>
.row { display: flex; align-items: center; gap: 3px; margin: 2px 0; }
.row.out { justify-content: flex-end; }
.row.in  { justify-content: flex-start; }
.row.in .acts { order: 2; }

.acts {
  position: relative; z-index: 21; flex: none;
  display: flex; align-items: center; gap: 1px;
  opacity: 0; transition: opacity 0.1s;
}
.row:hover .acts,
.acts.pinned { opacity: 1; }
.act {
  display: flex; align-items: center; justify-content: center;
  background: none; border: none; color: #8a8a8a;
  cursor: pointer; padding: 3px; border-radius: 50%;
}
.act:hover { background: rgba(255,255,255,0.08); color: #cfcfcf; }
.act:disabled { color: #555; cursor: default; background: none; }
.act.danger:hover { background: rgba(217,138,138,0.14); color: #d98a8a; }
.download {
  display: flex; align-items: center; gap: 6px;
  background: rgba(255,255,255,0.08); border: none; border-radius: 6px;
  color: #cfe0f5; font-size: calc(12px * var(--rfs, 1));
  padding: 5px 8px; margin: 1px 0 3px; cursor: pointer;
}
.download:hover { background: rgba(255,255,255,0.14); }

/* Quote block at the head of a reply: an accent bar, who wrote the quoted
 * message, and its line. Sits inside the balloon, dimmer than the reply
 * itself — it is context, not the message. */
.quote {
  display: block; width: 100%; text-align: left;
  margin: 3px 0 4px; padding: 3px 7px;
  background: rgba(0,0,0,0.22); border: none;
  border-left: 3px solid rgba(255,255,255,0.45); border-radius: 4px;
  color: inherit; font: inherit; cursor: pointer;
}
.quote.orphan { cursor: default; }
.quote:not(.orphan):hover { background: rgba(0,0,0,0.32); }
.qwho {
  display: block;
  font-size: calc(11px * var(--rfs, 1)); font-weight: 600;
  color: #b9c9dd; line-height: 1.25;
}
.row.out .qwho { color: #dce9ff; }
.qtext {
  display: block;
  font-size: calc(12px * var(--rfs, 1)); line-height: 1.3;
  color: #c8c8c8; white-space: pre-wrap; word-break: break-word;
}
.row.out .qtext { color: #e2ecfb; }

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
/* Dimmer than the ticks beside it: how a message travelled is a footnote to
 * whether it arrived, not a competitor for the same glance. */
.chip.link { color: #6f7b8a; }
/* Selectable: a fragment picked out here is what a reply quotes. */
.bubble { cursor: text; user-select: text; }

/* Arrived at by following a quote: a brief ring, long enough to find the
 * message by eye and gone before it becomes decoration. */
.row.flash .bubble { animation: flash 1.6s ease-out; }
@keyframes flash {
  0%, 55% { box-shadow: 0 0 0 2px rgba(120,170,140,0.85); }
  100%    { box-shadow: 0 0 0 2px rgba(120,170,140,0); }
}
</style>
