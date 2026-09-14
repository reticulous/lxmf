<!-- Text-only composer. No persisted draft (plan §4): unsent text lives
     in the state layer's in-memory per-peer map via v-model, never a
     stage=draft record. A record is born only on Send, atomically with
     cmd.send. The DIRECT hint is informational, never blocking. -->
<template>
  <div class="composer">
    <!-- What the next message will reply to, sitting where the reply itself
         will end up: same quote block, directly over the typing area, with an
         (x) that abandons the reply and keeps the text. -->
    <div v-if="quote" class="replybar">
      <div class="rq">
        <div class="rqwho">
          <q-icon :name="matReply" size="13px" /> Replying to {{ quote.label }}
        </div>
        <div class="rqtext">{{ quote.text }}</div>
      </div>
      <button class="rqx" title="Cancel reply" @click="emit('cancel-quote')">
        <q-icon :name="matClose" size="16px" />
      </button>
    </div>
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
import { matSend, matReply, matClose } from '@quasar/extras/material-icons'

const props = defineProps<{
  modelValue: string
  /* The message the next send replies to — who wrote it and the line being
   * quoted — or null when this is an ordinary message. */
  quote?: { label: string; text: string } | null
}>()
const emit = defineEmits<{
  'update:modelValue': [v: string]
  send: [content: string]
  'cancel-quote': []
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

/* A reply that has just been picked leaves the cursor here: the quote appearing
 * above the field IS the invitation to type, and reaching for the field
 * afterwards is a step the user never needs to take. Only on the way in — a
 * cancelled reply leaves focus where the reader put it. */
watch(() => props.quote, (q, was) => {
  if (!q) return
  /* By value, not by identity: the quote is recomputed on unrelated changes
   * (a peer's name arriving), and those must not pull the cursor back here. */
  if (was && was.label === q.label && was.text === q.text) return
  nextTick(() => ta.value?.focus())
})

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
.replybar {
  display: flex; align-items: center; gap: 6px;
  margin-bottom: 6px; padding: 4px 6px;
  background: rgba(255,255,255,0.05);
  border-left: 3px solid rgba(120,170,140,0.8); border-radius: 4px;
}
.rq { flex: 1; min-width: 0; }
.rqwho {
  display: flex; align-items: center; gap: 4px;
  font-size: calc(11px * var(--rfs, 1)); font-weight: 600; color: #9fc3ae;
}
.rqtext {
  font-size: calc(12px * var(--rfs, 1)); color: #bdbdbd;
  overflow: hidden; text-overflow: ellipsis; white-space: nowrap;
}
.rqx {
  flex: none; display: flex; align-items: center; justify-content: center;
  background: none; border: none; color: #9a9a9a;
  cursor: pointer; padding: 3px; border-radius: 50%;
}
.rqx:hover { background: rgba(255,255,255,0.08); color: #e0e0e0; }
.hint { font-size: calc(11px * var(--rfs, 1)); color: #8fa6c0; margin-bottom: 5px; }
.row { display: flex; align-items: flex-end; gap: 8px; }
.input {
  flex: 1; resize: none; background: #2a2a2a; color: #e8e8e8;
  border: 1px solid rgba(255,255,255,0.12); border-radius: 16px;
  padding: 8px 12px; font-size: calc(13px * var(--rfs, 1)); line-height: 1.35;
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
