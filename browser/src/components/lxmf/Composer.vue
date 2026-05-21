<!-- Text-only composer. No persisted draft (plan §4): unsent text lives
     in the state layer's in-memory per-peer map via v-model, never a
     stage=draft record. A record is born only on Send, atomically with
     cmd.send. The DIRECT hint is informational, never blocking. -->
<template>
  <div class="composer">
    <div v-if="overBudget" class="hint">
      long message — will send DIRECT (not opportunistic)
    </div>
    <div class="row">
      <textarea
        ref="ta"
        v-model="text"
        class="input"
        rows="1"
        placeholder="Message"
        @keydown.enter.exact.prevent="doSend"
        @input="autoGrow"
      />
      <button class="send" :disabled="!canSend" title="Send" @click="doSend">
        <q-icon :name="matSend" size="18px" />
      </button>
    </div>
  </div>
</template>

<script setup lang="ts">
import { computed, nextTick, ref, watch } from 'vue'
import { matSend } from '@quasar/extras/material-icons'

const props = defineProps<{ modelValue: string }>()
const emit = defineEmits<{
  'update:modelValue': [v: string]
  send: [content: string]
}>()

const text = computed({
  get: () => props.modelValue,
  set: v => emit('update:modelValue', v),
})

/* ~311 B opportunistic budget (content + ~32 B framing) — informational. */
const overBudget = computed(() =>
  new TextEncoder().encode(text.value).length + 32 > 311)

const canSend = computed(() => text.value.trim().length > 0)

const ta = ref<HTMLTextAreaElement | null>(null)
function autoGrow() {
  const el = ta.value
  if (!el) return
  el.style.height = 'auto'
  el.style.height = `${Math.min(el.scrollHeight, 120)}px`
}
watch(() => props.modelValue, () => nextTick(autoGrow))

function doSend() {
  const c = text.value.trim()
  if (!c) return
  emit('send', c)
}
</script>

<style scoped>
.composer { border-top: 1px solid rgba(255,255,255,0.08); padding: 8px 10px; }
.hint { font-size: 11px; color: #8fa6c0; margin-bottom: 5px; }
.row { display: flex; align-items: flex-end; gap: 8px; }
.input {
  flex: 1; resize: none; background: #2a2a2a; color: #e8e8e8;
  border: 1px solid rgba(255,255,255,0.12); border-radius: 16px;
  padding: 8px 12px; font-size: 13px; line-height: 1.35;
  font-family: inherit; outline: none; max-height: 120px;
  /* Auto-grow textarea: scrollHeight (content+padding) makes the UA paint
   * a vertical overlay scrollbar — the rounded pill at the right edge,
   * present even single-line. It's not a DOM node, so hide it at the UA
   * level; content past the 120px cap still scrolls via wheel/keys. */
  scrollbar-width: none;          /* Firefox */
}
.input::-webkit-scrollbar { display: none; }   /* WebKit/Blink */
.input:focus { border-color: rgba(120,170,140,0.6); }
.send {
  flex: none; width: 34px; height: 34px; border-radius: 50%;
  background: #4a7d5e; color: #eaffea; border: none; cursor: pointer;
  display: flex; align-items: center; justify-content: center;
}
.send:disabled { background: #3a3a3a; color: #777; cursor: default; }
.send:not(:disabled):hover { background: #56906c; }
</style>
