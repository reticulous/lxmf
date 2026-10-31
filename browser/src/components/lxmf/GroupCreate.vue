<!-- New group: a name and the contacts it starts with. The group is created on
     this device (or on the account's proxy, when it has one) as an identity of
     its own, and the account creating it is its owner and first member — so
     it is not offered here. Presentational: candidates in, intent out. -->
<template>
  <div class="gc-bg" @click="emit('close')">
    <div class="gc" @click.stop>
      <div class="ghead">New group</div>
      <input v-model="name" class="gname" placeholder="Group name" maxlength="48"
             autocomplete="off" @keydown.esc="emit('close')" />
      <!-- Hidden: the group's announce carries no name, so only its members
           ever learn it — from the group itself. -->
      <label class="pick">
        <input v-model="hidden" type="checkbox" />
        <span class="pname">Hide group name</span>
      </label>
      <div class="sect">Members</div>
      <div class="picks">
        <div v-if="!candidates.length" class="empty">No contacts to add yet.</div>
        <label v-for="c in candidates" :key="c.peer" class="pick">
          <input v-model="picked" type="checkbox" :value="c.peer" />
          <PeerAvatar :peer="c.peer" :name="c.name" :size="26" />
          <span class="pname">{{ c.name }}</span>
        </label>
      </div>
      <div class="btns">
        <button class="btn" @click="emit('close')">Cancel</button>
        <button class="btn go" :disabled="!canCreate" @click="create">Create</button>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { computed, ref } from 'vue'
import PeerAvatar from './PeerAvatar.vue'

const props = defineProps<{ candidates: { peer: string; name: string }[] }>()
const emit = defineEmits<{ close: []; create: [name: string, peers: string[], hidden: boolean] }>()

const name = ref('')
const hidden = ref(true)
const picked = ref<string[]>([])
const canCreate = computed(() => !!name.value.trim() && picked.value.length > 0)

function create() {
  if (!canCreate.value) return
  const live = new Set(props.candidates.map(c => c.peer))
  emit('create', name.value.trim(), picked.value.filter(p => live.has(p)), hidden.value)
}
</script>

<style scoped>
.gc-bg {
  position: absolute; inset: 0; background: rgba(0,0,0,0.45); z-index: 8;
  display: flex; align-items: center; justify-content: center;
}
.gc {
  width: 100%; max-width: 340px; max-height: 80%; margin: 12px;
  background: #262626; border-radius: 12px; padding: 14px;
  display: flex; flex-direction: column; gap: 8px;
  box-shadow: 0 6px 24px rgba(0,0,0,0.45);
}
.ghead { color: #e8e8e8; font-weight: 600; font-size: calc(14px * var(--rfs, 1)); }
.gname {
  background: #2a2a2a; color: #e8e8e8;
  border: 1px solid rgba(255,255,255,0.12); border-radius: 8px;
  padding: 8px 12px; font-size: calc(13px * var(--rfs, 1)); outline: none;
}
.gname:focus { border-color: rgba(120,170,140,0.6); }
.sect {
  color: #aaa; font-size: calc(11px * var(--rfs, 1)); text-transform: uppercase;
  letter-spacing: 0.05em; margin-top: 4px;
}
.picks { flex: 1; min-height: 60px; overflow-y: auto; }
.pick {
  display: flex; align-items: center; gap: 8px; padding: 5px 2px; cursor: pointer;
  color: #d8d8d8; font-size: calc(13px * var(--rfs, 1));
}
.pick:hover { background: rgba(255,255,255,0.04); }
.pname { flex: 1; min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.empty { color: #888; font-style: italic; font-size: calc(12px * var(--rfs, 1)); padding: 6px 2px; }
.btns { display: flex; justify-content: flex-end; gap: 8px; margin-top: 4px; }
.btn {
  background: none; border: 1px solid rgba(255,255,255,0.18); color: #d8d8d8;
  border-radius: 8px; padding: 5px 14px; font-size: calc(13px * var(--rfs, 1)); cursor: pointer;
}
.btn.go { background: #3a5d47; border-color: #3a5d47; color: #eaffea; }
.btn:disabled { background: #333; border-color: #333; color: #777; cursor: default; }
</style>
