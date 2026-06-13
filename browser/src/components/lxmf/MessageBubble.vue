<!-- Direction-aware message bubble. Outbound right, inbound left.
     The status footer is the honest-receipts surface (plan §6):
       • TWO ticks, never three — there is no network read receipt.
       • single-tick-forever is NORMAL for opportunistic, not "stuck".
       • failed → "!" + last_error inline + one-tap Resend.
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

    <div class="bubble" :class="{ muted: m.stage === 'cancelled' }"
         @contextmenu.prevent="emit('menu', m)">
      <div class="content">{{ m.content }}</div>

      <div class="meta">
        <span class="time">{{ clock }}</span>

        <!-- status glyphs: … in flight · ✓ sent (grey) · ✓✓ delivered
             (green) · ✕ failed/cancelled (red). lastError as tooltip. -->
        <template v-if="m.dir === 'out'">
          <span v-if="m.stage === 'queued' || m.stage === 'sending'"
                class="chip dots" :title="m.lastError || m.stage">…</span>
          <span v-else-if="m.stage === 'sent'" class="chip"
                :title="m.lastError || 'sent — no delivery proof yet'">
            <q-icon :name="matDone" size="15px" />
          </span>
          <span v-else-if="m.stage === 'delivered'" class="chip ok"
                title="delivered — cryptographic proof received">
            <q-icon :name="matDoneAll" size="15px" />
          </span>
          <span v-else-if="m.stage === 'failed' || m.stage === 'cancelled'"
                class="chip bad" :title="m.lastError || m.stage">
            <q-icon :name="matClose" size="15px" />
          </span>
        </template>
      </div>

      <!-- transient sub-text while sending: "establishing link"… -->
      <div v-if="m.stage === 'sending' && m.lastError" class="subnote">
        {{ m.lastError }}
      </div>

      <!-- failed: error inline + one-tap resend (no auto-retry exists) -->
      <div v-if="m.stage === 'failed'" class="failrow">
        <span class="err">{{ m.lastError || 'send failed' }}</span>
        <button class="resend" @click="emit('resend', m)">Resend</button>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { computed, ref } from 'vue'
import { matDone, matDoneAll, matClose, matMoreVert, matDelete }
  from '@quasar/extras/material-icons'
import type { Message } from '../../modules/lxmf'

const props = defineProps<{ m: Message }>()
const emit = defineEmits<{
  resend: [m: Message]
  menu: [m: Message]
  delete: [m: Message]
}>()

const menuOpen = ref(false)

const clock = computed(() =>
  props.m.ts
    ? new Date(props.m.ts * 1000).toLocaleTimeString(undefined,
        { hour: '2-digit', minute: '2-digit' })
    : '')
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
.meta {
  display: flex; align-items: center; gap: 5px;
  justify-content: flex-end;
  margin-top: 2px;
  font-size: calc(11px * var(--rfs, 1));
  color: #9a9a9a;
}
.time { font-variant-numeric: tabular-nums; }
.chip { display: inline-flex; align-items: center; color: #8a93a0; }
.chip.ok  { color: #4abf6a; }
.chip.bad { color: #d9534f; }
.chip.dots { font-weight: 700; line-height: 1; }
.subnote { margin-top: 3px; font-size: calc(11px * var(--rfs, 1)); color: #8fa6c0; font-style: italic; }
.failrow {
  margin-top: 4px; display: flex; align-items: center; gap: 8px;
  font-size: calc(11px * var(--rfs, 1));
}
.err { color: #d98a8a; }
.resend {
  background: none; border: 1px solid #d98a8a; color: #d98a8a;
  border-radius: 5px; padding: 1px 8px; font-size: calc(11px * var(--rfs, 1)); cursor: pointer;
}
.resend:hover { background: rgba(217, 138, 138, 0.15); }
</style>
