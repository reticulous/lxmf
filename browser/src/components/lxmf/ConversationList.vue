<!-- Signal's left rail. Presentational: props in, intent out. No
     device-store access, no pane/screen knowledge. Reachability is
     deliberately NOT shown here (plan §9 — kept Signal-clean). -->
<template>
  <div class="list">
    <div class="list-head">
      <span class="title">Peers</span>
      <button class="new" title="New message" @click="emit('compose')">
        <q-icon :name="matEdit" size="18px" />
      </button>
    </div>

    <div class="conv-scroll">
      <div v-if="conversations.length === 0" class="empty">
        No conversations yet. Start one with “New message”.
      </div>

      <div v-for="c in conversations" :key="c.peer"
           class="conv" :class="{ active: c.peer === activePeer }"
           @click="emit('open-peer', c.peer)">
        <PeerAvatar :peer="c.peer" :name="c.name" :size="40" />
        <div class="mid">
          <div class="line1">
            <span class="name">{{ c.name }}</span>
            <span class="time">{{ formatAge(c.ts) }}</span>
          </div>
          <div class="line2">
            <span class="preview">
              <template v-if="c.last">
                <span v-if="c.last.dir === 'out'" class="you">You: </span>{{ c.last.content || '—' }}
              </template>
            </span>
            <span v-if="c.unread > 0" class="badge">{{ c.unread }}</span>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { matEdit } from '@quasar/extras/material-icons'
import PeerAvatar from './PeerAvatar.vue'
import type { Conversation } from '../../modules/lxmf'

defineProps<{ conversations: Conversation[]; activePeer: string }>()
const emit = defineEmits<{ 'open-peer': [peer: string]; compose: [] }>()

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
.conv-scroll { flex: 1; min-height: 0; overflow-y: auto; }
.list-head {
  display: flex; align-items: center; justify-content: space-between;
  padding: 8px 10px; border-bottom: 1px solid rgba(255,255,255,0.08);
}
.title { font-weight: 600; color: #e8e8e8; font-size: 14px; }
.new {
  background: none; border: none; color: #9fb8d8; cursor: pointer;
  display: flex; padding: 2px; border-radius: 5px;
}
.new:hover { background: rgba(255,255,255,0.08); }
.empty { color: #888; font-style: italic; padding: 16px; font-size: 13px; text-align: center; }
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
  flex: 1; min-width: 0; font-weight: 500; color: #e8e8e8; font-size: 13px;
  overflow: hidden; text-overflow: ellipsis; white-space: nowrap;
}
.time { color: #888; font-size: 11px; flex: none; }
.preview {
  flex: 1; min-width: 0; color: #9a9a9a; font-size: 12px;
  overflow: hidden; text-overflow: ellipsis; white-space: nowrap;
}
.you { color: #7a7a7a; }
.badge {
  flex: none; background: #4a7d5e; color: #eaffea; font-size: 11px;
  min-width: 18px; height: 18px; border-radius: 9px; padding: 0 5px;
  display: flex; align-items: center; justify-content: center;
  font-variant-numeric: tabular-nums;
}
</style>
