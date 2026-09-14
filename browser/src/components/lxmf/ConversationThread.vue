<!-- The message pane/screen for the active peer. Day separators,
     ascending order, sticky header. The header's secondary line is the
     ONLY reachability surface (plan §9): "heard 4m ago · 2 hops", and
     silently nothing if never heard. Never a presence dot. -->
<template>
  <div class="thread">
    <!-- The whole header opens the contact card; the info icon is only the
         visual cue. Back keeps its own handler and must not bubble. -->
    <div class="thead" title="Contact info" @click="emit('open-contact', peer)">
      <button class="back" :class="{ 'back--shown': showBack }" title="Back" @click.stop="emit('back')">
        <q-icon :name="matArrowBack" size="20px" />
      </button>
      <PeerAvatar :peer="peer" :name="name" :size="30" />
      <div class="who">
        <div class="name">{{ name }}</div>
        <div class="sub">{{ reachLine }}</div>
      </div>
      <!-- Conversation signal: the peer's own direct bars, falling back to the
           gateway signal — the contact's signal overrules the gateway one. -->
      <ContactSignal :peer="peer" fallback-gw class="hdrsig" />
      <button
        class="link" :class="`link--${linkState || 'down'}`"
        :title="linkState ? 'Link open — tap to close' : 'No link — tap to open'"
        @click.stop="emit('toggle-link', peer)"
      >
        <q-icon :name="linkState ? matLink : matLinkOff" size="19px" />
      </button>
      <span class="info">
        <q-icon :name="matInfo" size="19px" />
      </span>
    </div>

    <!-- Floating sticky date: the day of the content at the top of the viewport,
         shown while scrolling when no inline separator is up there; fades after 2s. -->
    <div class="stickyday" :class="{ show: stickyShow }">{{ stickyDay }}</div>

    <div ref="scroller" class="scroll" @scroll="onScroll">
      <div v-if="buckets.length === 0" class="empty">
        No messages in this conversation yet.
      </div>
      <template v-for="b in buckets" :key="b.day">
        <div class="daysep"><span>{{ b.day }}</span></div>
        <MessageBubble
          v-for="m in b.messages" :key="m.key" :m="m" :proxied="proxied"
          :quote="quoteFor(m)"
          @resend="m2 => emit('resend', m2)"
          @menu="m2 => emit('msg-menu', m2)"
          @delete="m2 => emit('msg-delete', m2)"
          @open="m2 => emit('msg-open', m2)"
          @fetch="m2 => emit('msg-fetch', m2)"
          @reply="(m2, frag) => emit('msg-reply', m2, frag)"
          @quote-open="jumpTo"
        />
      </template>
    </div>
  </div>
</template>

<script lang="ts">
/* Where each conversation was left, keyed "<identity>/<peer>": the scroll offset
 * to come back to, or null for "was at the newest" — which follows whatever
 * arrived while away instead of freezing at an offset that is no longer the
 * bottom. Module scope, not component state, because the compact layout unmounts
 * this pane on Back and reopening is precisely when it has to hold. Not
 * persisted and deliberately so: a reload or a reboot starts every conversation
 * at its newest message. */
const leftAt = new Map<string, number | null>()
</script>

<script setup lang="ts">
import { computed, nextTick, onMounted, onBeforeUnmount, ref, watch } from 'vue'
import { matArrowBack, matInfo, matLink, matLinkOff } from '@quasar/extras/material-icons'
import PeerAvatar from './PeerAvatar.vue'
import MessageBubble from './MessageBubble.vue'
import ContactSignal from './ContactSignal.vue'
import { quoteView, type Message, type Reachability } from '../../modules/lxmf'

const props = defineProps<{
  /* Identity slot this pane views. Only to scope the per-conversation scroll
   * memory: the same peer under two identities is two conversations. */
  identity: number
  peer: string
  name: string
  buckets: { day: string; messages: Message[] }[]
  reach: Reachability | null
  /* Conversation-link state to this peer: '' (down), 'establishing', 'active'. */
  linkState?: '' | 'establishing' | 'active'
  /* This identity's mail belongs to a proxy server, which is what makes "this
   * one went direct" worth marking on a bubble at all. */
  proxied?: boolean
  /* Reveal the Back button (single-column / compact layouts). Hidden by
   * default — desktop master/detail keeps the rail permanently visible. */
  showBack?: boolean
}>()
const emit = defineEmits<{
  resend: [m: Message]
  'msg-menu': [m: Message]
  'msg-delete': [m: Message]
  'msg-open': [m: Message]
  /* Reply to this message, quoting `fragment` when the user selected one. */
  'msg-reply': [m: Message, fragment: string]
  /* Ask the proxy server for a body it withheld. */
  'msg-fetch': [m: Message]
  'open-contact': [peer: string]
  'toggle-link': [peer: string]
  back: []
  read: [peer: string]
}>()

const reachLine = computed(() => {
  const r = props.reach
  if (!r || !r.lastSeenS) return ''
  const ageS = Math.round(Date.now() / 1000 - r.lastSeenS)
  const age =
    ageS < 60 ? 'just now'
    : ageS < 3600 ? `${Math.floor(ageS / 60)}m ago`
    : ageS < 86400 ? `${Math.floor(ageS / 3600)}h ago`
    : `${Math.floor(ageS / 86400)}d ago`
  const hops = r.hops >= 0 && r.hops < 128 ? ` · ${r.hops} hop${r.hops === 1 ? '' : 's'}` : ''
  return `heard ${age}${hops}`
})

/* message_id → the message it names, over this conversation. What a reply's
 * quote block is resolved against: the reply carries an id, and the line it
 * shows comes from our own copy of the message that id names. */
const byId = computed(() => {
  const m = new Map<string, Message>()
  for (const b of props.buckets)
    for (const msg of b.messages) if (msg.messageId) m.set(msg.messageId, msg)
  return m
})
const quoteFor = (m: Message) => quoteView(m, byId.value, () => props.name)

const scroller = ref<HTMLElement | null>(null)

/* Follow a quote back to the message it came from: scroll it into view and
 * mark it briefly, so the eye finds it without the pane having to explain
 * itself. A message the conversation no longer holds simply does not move. */
function jumpTo(key: string) {
  const el = scroller.value?.querySelector<HTMLElement>(`#msg-${CSS.escape(key)}`)
  if (!el) return
  el.scrollIntoView({ behavior: 'smooth', block: 'center' })
  el.classList.remove('flash')
  void el.offsetWidth              /* restart the animation on a repeat tap */
  el.classList.add('flash')
  setTimeout(() => el.classList.remove('flash'), 1600)
}

function toBottom() {
  nextTick(() => {
    const el = scroller.value
    if (el) el.scrollTop = el.scrollHeight
  })
}
/* Starting a reply is the start of writing one, so the pane it will be written
 * in comes back to the newest message — the composition layer asks for this
 * when a reply begins. */
defineExpose({ toBottom })

/* Within 48px of the bottom counts as "looking at the newest". */
function atBottom(): boolean {
  const el = scroller.value
  return !el || el.scrollHeight - el.scrollTop - el.clientHeight <= 48
}
function reading(): boolean {
  return atBottom() && document.visibilityState === 'visible' && document.hasFocus()
}
/* Reading the newest with the window focused → keep this conversation read. */
function maybeRead() {
  if (props.peer && reading()) emit('read', props.peer)
}

/* Remember where this conversation is being read, so coming back to it lands
 * there rather than at the newest. `null` records "at the newest" — the offset
 * itself would be stale the moment anything arrives. */
function remember(peer: string) {
  const el = scroller.value
  if (el && peer) leftAt.set(`${props.identity}/${peer}`, atBottom() ? null : el.scrollTop)
}

/* Switching conversations: hold the one being left where the reader had it, and
 * put the one being opened back where they left it — at the newest if they were
 * at the newest, or have never opened it. A conversation whose messages are
 * still arriving has nothing to come back to yet, so the clamp lands it at the
 * newest and the buckets watcher below keeps it there. */
watch(() => props.peer, (peer, prev) => {
  if (prev) remember(prev)
  const y = peer ? leftAt.get(`${props.identity}/${peer}`) : null
  if (y == null) toBottom()
  else nextTick(() => {
    const el = scroller.value
    if (el) el.scrollTop = Math.min(y, el.scrollHeight - el.clientHeight)
  })
  nextTick(maybeRead)
}, { immediate: true })

/* A new/changed message follows to the bottom ONLY if we were already there —
 * never yank a reader who scrolled up into history (the watcher runs pre-DOM
 * update, so atBottom() reflects the position before the new message landed). */
watch(() => props.buckets, () => {
  if (atBottom()) { toBottom(); nextTick(maybeRead) }
}, { deep: true })

/* Floating sticky date — the day of the topmost visible content, shown while
 * scrolling unless an inline .daysep is already at the top; fades 2s after the
 * last scroll. */
const stickyDay = ref('')
const stickyShow = ref(false)
let stickyTimer: ReturnType<typeof setTimeout> | undefined
function updateSticky() {
  const el = scroller.value
  if (!el) return
  const top = el.scrollTop
  let cur = ''
  let sepAtTop = false
  el.querySelectorAll<HTMLElement>('.daysep').forEach(s => {
    const y = s.offsetTop
    if (y <= top + 1) cur = s.textContent?.trim() ?? ''
    if (y >= top && y <= top + 28) sepAtTop = true
  })
  if (!cur || sepAtTop) { stickyShow.value = false; return }
  stickyDay.value = cur
  stickyShow.value = true
  if (stickyTimer) clearTimeout(stickyTimer)
  stickyTimer = setTimeout(() => { stickyShow.value = false }, 2000)
}

function onScroll() { remember(props.peer); maybeRead(); updateSticky() }

onMounted(() => {
  document.addEventListener('visibilitychange', maybeRead)
  window.addEventListener('focus', maybeRead)
})
onBeforeUnmount(() => {
  /* Compact layouts unmount this pane on Back — that is a conversation being
   * left like any other, so record it on the way out. */
  remember(props.peer)
  if (stickyTimer) clearTimeout(stickyTimer)
  document.removeEventListener('visibilitychange', maybeRead)
  window.removeEventListener('focus', maybeRead)
})
</script>

<style scoped>
.thread { display: flex; flex-direction: column; height: 100%; overflow: hidden; position: relative; }
.thead {
  display: flex; align-items: center; gap: 10px;
  padding: 8px 10px; border-bottom: 1px solid rgba(255,255,255,0.08);
  background: #1f1f1f;
  cursor: pointer;
}
/* Hidden in desktop master/detail (rail always present); shown when the host
 * opts in via show-back — the on-device port and the browser's compact
 * single-column layout. */
.back { display: none; align-items: center; }
.back--shown { display: flex; }
.who { flex: 1; min-width: 0; }
.name {
  font-weight: 600; color: #e8e8e8; font-size: calc(14px * var(--rfs, 1));
  overflow: hidden; text-overflow: ellipsis; white-space: nowrap;
}
.sub { font-size: calc(11px * var(--rfs, 1)); color: #8a8a8a; height: 13px; }
.info, .back {
  display: inline-flex; align-items: center;
  background: none; border: none; color: #9a9a9a; cursor: pointer;
  padding: 2px; border-radius: 5px;
}
.thead:hover .info { background: rgba(255,255,255,0.08); color: #cfcfcf; }
.hdrsig { margin-right: 6px; flex: none; }
.link {
  display: inline-flex; align-items: center;
  background: none; border: none; cursor: pointer;
  padding: 2px; border-radius: 5px; color: #6a6a6a;
}
.link:hover { background: rgba(255,255,255,0.08); }
.link--active { color: #4abf6a; }
.link--establishing { color: #d6a12a; }
.scroll { flex: 1; overflow-y: auto; padding: 8px 10px; position: relative; }
/* Floating sticky date over the top of the scroll area; fades via opacity. */
.stickyday {
  position: absolute; top: 52px; left: 50%; transform: translateX(-50%);
  z-index: 5; pointer-events: none;
  background: #ffffcc; color: #000;
  font-size: calc(11px * var(--rfs, 1)); padding: 2px 12px; border-radius: 6px;
  opacity: 0; transition: opacity 0.3s;
}
.stickyday.show { opacity: 1; }
.empty { color: #888; font-style: italic; text-align: center; padding: 20px; font-size: calc(13px * var(--rfs, 1)); }
.daysep { text-align: center; margin: 10px 0 6px; }
.daysep span {
  background: #ffffcc; color: #000;
  font-size: calc(11px * var(--rfs, 1)); padding: 2px 10px; border-radius: 6px;
}
</style>
