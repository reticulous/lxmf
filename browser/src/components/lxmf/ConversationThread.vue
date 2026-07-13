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
      <span class="info">
        <q-icon :name="matInfo" size="19px" />
      </span>
    </div>

    <div ref="scroller" class="scroll">
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
        />
      </template>
    </div>
  </div>
</template>

<script setup lang="ts">
import { computed, nextTick, ref, watch } from 'vue'
import { matArrowBack, matInfo } from '@quasar/extras/material-icons'
import PeerAvatar from './PeerAvatar.vue'
import MessageBubble from './MessageBubble.vue'
import type { Message, Reachability } from '../../modules/lxmf'

const props = defineProps<{
  peer: string
  name: string
  buckets: { day: string; messages: Message[] }[]
  reach: Reachability | null
  /* Reveal the Back button (single-column / compact layouts). Hidden by
   * default — desktop master/detail keeps the rail permanently visible. */
  showBack?: boolean
}>()
const emit = defineEmits<{
  resend: [m: Message]
  'msg-menu': [m: Message]
  'msg-delete': [m: Message]
  'open-contact': [peer: string]
  back: []
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
watch(() => props.peer, toBottom)
watch(() => props.buckets, toBottom, { deep: true, immediate: true })
</script>

<style scoped>
.thread { display: flex; flex-direction: column; height: 100%; overflow: hidden; }
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
.scroll { flex: 1; overflow-y: auto; padding: 8px 10px; }
.empty { color: #888; font-style: italic; text-align: center; padding: 20px; font-size: calc(13px * var(--rfs, 1)); }
.daysep { text-align: center; margin: 10px 0 6px; }
.daysep span {
  background: rgba(255,255,255,0.06); color: #9a9a9a;
  font-size: calc(11px * var(--rfs, 1)); padding: 2px 10px; border-radius: 9px;
}
</style>
