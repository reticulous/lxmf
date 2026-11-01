<!-- The LXMF left rail. Formerly a Signal-style recent-conversation list
     with a "New message" pencil that opened a separate picker overlay;
     now the picker IS the rail (mirrors Nomad Browser's sidebar). A
     search box on top filters two sections: "Contacts" (peers you've
     messaged — shown with rich conversation preview / unread / time) and
     "On the Mesh" (announced peers you haven't talked to yet). A bare
     32-hex query that isn't a contact yet offers "message this address",
     which adds it as a contact and opens it. The button right of the
     search box starts a new group. Presentational: props in,
     intent out — no device-store access, no pane/screen knowledge. -->
<template>
  <div class="list">
    <div class="searchrow">
      <input
        v-model="q" class="search"
        placeholder="Search or 32-hex address" autocomplete="off"
        @keydown.enter="onEnter" @keydown.esc="q = ''"
      />
      <button class="newgroup" title="New group" @click="emit('new-group')">
        <q-icon :name="matGroupAdd" size="20px" />
      </button>
    </div>

    <div class="conv-scroll">
      <div v-if="rawHexReady" class="grp">New</div>
      <div v-if="rawHexReady" class="conv" @click="addHex">
        <PeerAvatar :peer="hexQuery" name="" :size="40" />
        <div class="mid">
          <div class="line1"><span class="name">Message this address</span></div>
          <div class="line2"><span class="preview mono">{{ hexQuery }}</span></div>
        </div>
      </div>

      <template v-if="contacts.length">
        <div class="grp">Contacts</div>
        <div v-for="c in contacts" :key="'k' + c.peer"
             class="conv" :class="{ active: c.peer === activePeer }"
             @click="emit('open-peer', c.peer)">
          <PeerAvatar :peer="c.peer" :name="c.name" :size="40" :group="c.group" />
          <div class="mid">
            <div class="line1">
              <span class="name">{{ c.name }}</span>
              <ContactSignal :peer="c.peer" />
              <span v-if="c.conv" class="time">{{ formatAge(c.conv.ts) }}</span>
            </div>
            <div class="line2">
              <span class="preview" :class="{ mono: !c.conv }">
                <template v-if="c.conv && c.conv.last">
                  <span v-if="c.conv.last.dir === 'out'" class="you">You: </span>{{ c.conv.last.content || '—' }}
                </template>
                <template v-else>{{ c.peer }}</template>
              </span>
              <span v-if="c.conv && c.conv.unread > 0" class="badge">{{ c.conv.unread }}</span>
            </div>
          </div>
        </div>
      </template>

      <template v-if="heard.length">
        <div class="grp">On the Mesh</div>
        <div v-for="r in heard" :key="'a' + r.peer"
             class="conv" :class="{ active: r.peer === activePeer }"
             @click="emit('open-peer', r.peer)">
          <PeerAvatar :peer="r.peer" :name="r.name" :size="40" />
          <div class="mid">
            <div class="line1"><span class="name">{{ r.name }}</span></div>
            <div class="line2"><span class="preview mono">{{ r.peer }}</span></div>
          </div>
        </div>
      </template>

      <div v-if="!contacts.length && !heard.length && !rawHexReady" class="empty">
        <template v-if="q.trim()">No matches.</template>
        <template v-else>No contacts yet. Search the mesh above to start a conversation.</template>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { computed, ref } from 'vue'
import { matGroupAdd } from '@quasar/extras/material-icons'
import PeerAvatar from './PeerAvatar.vue'
import ContactSignal from './ContactSignal.vue'
import { hexAddr, type Conversation } from '../../modules/lxmf'

const props = defineProps<{
  conversations: Conversation[]
  directory: { peer: string; name: string; known: boolean; group: boolean }[]
  activePeer: string
}>()
const emit = defineEmits<{ 'open-peer': [peer: string]; 'add-peer': [peer: string]; 'new-group': [] }>()

const q = ref('')

const isHex64 = (s: string) => /^[0-9a-f]{32}$/i.test(s)
/* The query read as an address: spaces dropped, a 64-hex hash cut to 32. */
const hexQuery = computed(() => hexAddr(q.value))
const rawHexReady = computed(() =>
  isHex64(hexQuery.value) && !props.directory.some(d => d.peer === hexQuery.value && d.known))

/* Conversation metadata (preview / unread / time) keyed by peer, so a
 * Contacts row can render its thread state without the rail knowing how
 * conversations are derived. */
const convByPeer = computed(() => {
  const m = new Map<string, Conversation>()
  for (const c of props.conversations) m.set(c.peer, c)
  return m
})

const filtered = computed(() => {
  const needle = q.value.trim().toLowerCase()
  if (!needle) return props.directory
  const hexNeedle = hexQuery.value
  return props.directory.filter(d =>
    d.name.toLowerCase().includes(needle) || d.peer.toLowerCase().includes(hexNeedle))
})

/* Contacts carry their conversation (if any) and sort by recency —
 * threads with traffic first (newest on top), the rest alphabetically. */
const contacts = computed(() =>
  filtered.value
    .filter(d => d.known)
    .map(d => ({ ...d, conv: convByPeer.value.get(d.peer) ?? null }))
    .sort((a, b) => {
      const ta = a.conv?.ts ?? 0, tb = b.conv?.ts ?? 0
      if (ta !== tb) return tb - ta
      return a.name.localeCompare(b.name)
    }))

/* On-the-mesh peers keep peerDirectory's order (announces, newest heard
 * first). */
const heard = computed(() => filtered.value.filter(d => !d.known))

function addHex() {
  const peer = hexQuery.value
  q.value = ''
  emit('add-peer', peer)
}

function onEnter() {
  if (rawHexReady.value) return addHex()
  const only = [...contacts.value, ...heard.value]
  if (only.length === 1) emit('open-peer', only[0]!.peer)
}

function formatAge(epochSecs: number): string {
  if (!epochSecs) return ''
  const ageS = Math.round(Date.now() / 1000 - epochSecs)
  if (ageS < 60)    return 'now'
  if (ageS < 3600)  return `${Math.floor(ageS / 60)}m`
  if (ageS < 86400) return `${Math.floor(ageS / 3600)}h`
  return `${Math.floor(ageS / 86400)}d`
}
</script>

<style scoped>
.list { display: flex; flex-direction: column; height: 100%; overflow: hidden; }
.searchrow { display: flex; align-items: center; gap: 4px; margin: 10px 6px 10px 10px; }
.newgroup {
  flex: none; background: none; border: none; color: #9a9a9a;
  cursor: pointer; padding: 5px; border-radius: 6px; display: flex;
}
.newgroup:hover { background: rgba(255,255,255,0.08); color: #e8e8e8; }
.search {
  flex: 1; min-width: 0; background: #2a2a2a; color: #e8e8e8;
  border: 1px solid rgba(255,255,255,0.12); border-radius: 8px;
  padding: 8px 12px; font-size: calc(13px * var(--rfs, 1)); outline: none;
}
.search:focus { border-color: rgba(120,170,140,0.6); }
.conv-scroll { flex: 1; min-height: 0; overflow-y: auto; }
.grp {
  color: #888; font-size: calc(11px * var(--rfs, 1)); text-transform: uppercase;
  letter-spacing: 0.05em; padding: 8px 12px 4px;
}
.empty { color: #888; font-style: italic; padding: 16px; font-size: calc(13px * var(--rfs, 1)); text-align: center; line-height: 1.4; }
.conv {
  display: flex; gap: 10px; padding: 9px 10px; cursor: pointer;
  border-bottom: 1px solid rgba(255,255,255,0.05);
}
.conv:hover { background: rgba(255,255,255,0.04); }
.conv.active { background: rgba(120,170,140,0.14); }
.mid { flex: 1; min-width: 0; }
.line1, .line2 { display: flex; align-items: baseline; gap: 8px; }
.line2 { margin-top: 2px; }
.name {
  flex: 1; min-width: 0; font-weight: 500; color: #e8e8e8; font-size: calc(13px * var(--rfs, 1));
  overflow: hidden; text-overflow: ellipsis; white-space: nowrap;
}
.time { color: #888; font-size: calc(11px * var(--rfs, 1)); flex: none; }
.preview {
  flex: 1; min-width: 0; color: #9a9a9a; font-size: calc(12px * var(--rfs, 1));
  overflow: hidden; text-overflow: ellipsis; white-space: nowrap;
}
.preview.mono { font-family: 'JetBrains Mono', monospace; font-size: calc(11px * var(--rfs, 1)); }
.you { color: #7a7a7a; }
.badge {
  flex: none; background: #2563a0; color: #fff; font-size: calc(11px * var(--rfs, 1));
  min-width: 18px; height: 18px; border-radius: 9px; padding: 0 5px;
  display: flex; align-items: center; justify-content: center;
  font-variant-numeric: tabular-nums;
}
</style>
