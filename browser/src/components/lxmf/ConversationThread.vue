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
          v-for="m in b.messages" :key="m.key" :m="m"
          @resend="m2 => emit('resend', m2)"
          @menu="m2 => emit('msg-menu', m2)"
          @delete="m2 => emit('msg-delete', m2)"
          @open="m2 => emit('msg-open', m2)"
        />
      </template>
    </div>
  </div>
</template>

<script setup lang="ts">
import { computed, nextTick, onMounted, onBeforeUnmount, ref, watch } from 'vue'
import { matArrowBack, matInfo, matLink, matLinkOff } from '@quasar/extras/material-icons'
import PeerAvatar from './PeerAvatar.vue'
import MessageBubble from './MessageBubble.vue'
import ContactSignal from './ContactSignal.vue'
import type { Message, Reachability } from '../../modules/lxmf'

const props = defineProps<{
  peer: string
  name: string
  buckets: { day: string; messages: Message[] }[]
  reach: Reachability | null
  /* Conversation-link state to this peer: '' (down), 'establishing', 'active'. */
  linkState?: '' | 'establishing' | 'active'
  /* Reveal the Back button (single-column / compact layouts). Hidden by
   * default — desktop master/detail keeps the rail permanently visible. */
  showBack?: boolean
}>()
const emit = defineEmits<{
  resend: [m: Message]
  'msg-menu': [m: Message]
  'msg-delete': [m: Message]
  'msg-open': [m: Message]
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

const scroller = ref<HTMLElement | null>(null)

function toBottom() {
  nextTick(() => {
    const el = scroller.value
    if (el) el.scrollTop = el.scrollHeight
  })
}
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

/* Opening / switching a conversation always jumps to the bottom (newest), then
 * marks read if we're actually looking at it. */
watch(() => props.peer, () => { toBottom(); nextTick(maybeRead) }, { immediate: true })

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

function onScroll() { maybeRead(); updateSticky() }   /* + floating date */

onMounted(() => {
  document.addEventListener('visibilitychange', maybeRead)
  window.addEventListener('focus', maybeRead)
})
onBeforeUnmount(() => {
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
