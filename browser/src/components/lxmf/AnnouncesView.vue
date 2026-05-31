<!-- Standalone mesh-activity monitor (Sideband's "Announce Stream").
     Same visual language as panels/NodesWindow.vue. Cross-identity,
     ephemeral. Row action → Message (hands off to a conversation).
     Presentational: announces in, "message" intent out. -->
<template>
  <div class="ann-body">
    <div v-if="announces.length === 0" class="empty">
      Nothing heard on the mesh yet. Bring up a transport and wait for announces.
    </div>
    <table v-else class="ann-table">
      <thead>
        <tr>
          <th>Name</th>
          <th>Address</th>
          <th class="num">Hops</th>
          <th class="num">Cost</th>
          <th class="num">Heard</th>
          <th></th>
        </tr>
      </thead>
      <tbody>
        <tr v-for="a in announces" :key="a.hash">
          <td class="name">{{ a.name || '—' }}</td>
          <td class="mono trunc" :title="a.hash">{{ a.hash }}</td>
          <td class="num">{{ a.hops >= 128 ? '?' : a.hops }}</td>
          <td class="num">{{ a.cost || '—' }}</td>
          <td class="num">{{ formatAge(a.lastSeen) }}</td>
          <td class="act">
            <button class="msg" @click="emit('message', a.hash)">Message</button>
          </td>
        </tr>
      </tbody>
    </table>
  </div>
</template>

<script setup lang="ts">
import type { Announce } from '../../modules/lxmf'

defineProps<{ announces: Announce[] }>()
const emit = defineEmits<{ message: [peer: string] }>()

function formatAge(epochSecs: number): string {
  if (!epochSecs) return '—'
  const ageS = Math.round(Date.now() / 1000 - epochSecs)
  if (ageS < 0)     return `${Math.round(epochSecs)}`
  if (ageS < 60)    return `${ageS}s ago`
  if (ageS < 3600)  return `${Math.floor(ageS / 60)}m ago`
  if (ageS < 86400) return `${Math.floor(ageS / 3600)}h ago`
  return `${Math.floor(ageS / 86400)}d ago`
}
</script>

<style scoped>
.ann-body { height: 100%; overflow: auto; padding: 8px; color: #d8d8d8; font-size: 13px; }
.empty { color: #888; font-style: italic; padding: 12px; text-align: center; }
.ann-table { width: 100%; border-collapse: collapse; }
.ann-table th, .ann-table td {
  text-align: left; padding: 4px 8px;
  border-bottom: 1px solid rgba(255,255,255,0.08);
}
.ann-table th {
  position: sticky; top: 0; background: #1f1f1f; color: #aaa;
  font-weight: 600; font-size: 12px;
}
.mono { font-family: 'JetBrains Mono', 'Menlo', monospace; font-size: 12px; color: #b8b8b8; }
.trunc { max-width: 180px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.num { text-align: right; font-variant-numeric: tabular-nums; }
.name { font-weight: 500; color: #e8e8e8; }
.act { text-align: right; }
.msg {
  background: none; border: 1px solid #6fa185; color: #9fd8b8;
  border-radius: 5px; padding: 1px 9px; font-size: 12px; cursor: pointer;
}
.msg:hover { background: rgba(120,170,140,0.18); }
</style>
