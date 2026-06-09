<!-- Browser composition root (plan §7.1). One FloatingWindow, a
     master/detail two-pane: the ConversationList rail (a searchable
     Contacts / On-the-Mesh directory — the primary way to open or start
     a conversation) + ConversationThread + Composer, with ContactCard as
     an in-window overlay. This is the ONLY place layout is decided; views
     below stay layout-agnostic so the on-device port is a second root,
     not a rewrite. The identity selector is rendered only when there is
     more than one identity (single-identity stays invisible). -->
<template>
  <FloatingWindow
    :id="`lxmf-messages-${identity}`"
    :title="title"
    :visible="visible"
    :default-geom="defaultGeom"
    :min-size="{ w: 34, h: 18 }"
    @update:visible="v => emit('update:visible', v)"
  >
    <template #titlebar-right>
      <span class="rfs-btn" title="Smaller" @click="zoomOut">-</span>
      <span class="rfs-btn" title="Larger" @click="zoomIn">+</span>
    </template>

    <template #default>
      <div class="msg-root" :style="{ '--rfs': scale }">
        <div v-if="lxmf.usableIdentities.value.length === 0" class="noident">
          <template v-if="lxmf.identities.value.length === 0">
            No LXMF identity yet. Create one in
            <em>Settings → Mesh Network → LXMF Messages</em> to send and receive messages.
          </template>
          <template v-else>
            No <strong>connected</strong> identity. A slot exists in storage
            but the firmware hasn’t brought it up (no keypair — e.g. leftover
            test state). Create a real identity in
            <em>Settings → Mesh Network → LXMF Messages</em>.
          </template>
        </div>

        <div v-else class="panes">
          <div class="rail" :style="{ width: railW + 'px' }">
            <ConversationList
              :conversations="lxmf.conversations.value"
              :directory="lxmf.peerDirectory.value"
              :active-peer="lxmf.activePeer.value"
              @open-peer="openPeer"
            />
          </div>

          <div class="splitter" :class="{ dragging }" @mousedown="startResize"></div>

          <div class="detail">
            <div v-if="!lxmf.activePeer.value" class="placeholder">
              Select a conversation, or start a new one.
            </div>
            <template v-else>
              <ConversationThread
                :peer="lxmf.activePeer.value"
                :name="lxmf.displayName(lxmf.activePeer.value)"
                :buckets="lxmf.activeConversation.value"
                :reach="lxmf.reachability(lxmf.activePeer.value)"
                @resend="m => lxmf.resend(m.peer, m.key)"
                @msg-menu="m => (menuMsg = m)"
                @msg-delete="askDeleteMsg"
                @open-contact="showContact = true"
                @back="lxmf.activePeer.value = ''"
              />
              <Composer
                :model-value="draft"
                @update:model-value="setDraft"
                @send="onSend"
              />
            </template>
          </div>
        </div>

        <ContactCard
          v-if="showContact && lxmf.activePeer.value"
          :peer="lxmf.activePeer.value"
          :name="lxmf.displayName(lxmf.activePeer.value)"
          :contact="lxmf.contactOf(lxmf.activePeer.value)"
          :reach="lxmf.reachability(lxmf.activePeer.value)"
          :ratchet="ratchetOf(lxmf.activePeer.value)"
          @close="showContact = false"
          @delete-conversation="onDeleteConversation"
        />

        <!-- message action sheet (Signal long-press analog) -->
        <div v-if="menuMsg" class="sheet-bg" @click="menuMsg = null">
          <div class="sheet" @click.stop>
            <button @click="copyMsg">Copy text</button>
            <button
              v-if="menuMsg.dir === 'out' && (menuMsg.stage === 'queued' || menuMsg.stage === 'sending')"
              @click="cancelMsg">Cancel send</button>
            <button class="danger" @click="deleteMsg">Delete message</button>
            <button @click="menuMsg = null">Cancel</button>
          </div>
        </div>
      </div>
    </template>
  </FloatingWindow>
</template>

<script setup lang="ts">
import { computed, ref, toRef } from 'vue'
import FloatingWindow from 'spangap-browser/components/FloatingWindow.vue'
import ConversationList from '../components/lxmf/ConversationList.vue'
import ConversationThread from '../components/lxmf/ConversationThread.vue'
import Composer from '../components/lxmf/Composer.vue'
import ContactCard from '../components/lxmf/ContactCard.vue'
import { useLxmf, type Message } from '../modules/lxmf'
import { useWinZoom } from 'rns/lib/winZoom'

const props = defineProps<{ visible: boolean; title: string; identity: number }>()
const emit = defineEmits<{ 'update:visible': [value: boolean] }>()

const defaultGeom = { x: 10, y: 7, w: 76, h: 78 }
const lxmf = useLxmf(toRef(props, 'identity'))
const { scale, zoomIn, zoomOut } = useWinZoom('lxmf')

/* Draggable master/detail divider. Width persisted client-side. */
const RAIL_MIN = 180, RAIL_MAX = 520, LS_RAIL = 'lxmf.railW'
const railW = ref(Math.min(RAIL_MAX, Math.max(RAIL_MIN,
  Number(localStorage.getItem(LS_RAIL)) || 300)))
const dragging = ref(false)

function startResize(e: MouseEvent) {
  e.preventDefault()
  dragging.value = true
  const startX = e.clientX
  const startW = railW.value
  const onMove = (ev: MouseEvent) => {
    railW.value = Math.min(RAIL_MAX, Math.max(RAIL_MIN,
      startW + (ev.clientX - startX)))
  }
  const onUp = () => {
    dragging.value = false
    localStorage.setItem(LS_RAIL, String(railW.value))
    window.removeEventListener('mousemove', onMove)
    window.removeEventListener('mouseup', onUp)
  }
  window.addEventListener('mousemove', onMove)
  window.addEventListener('mouseup', onUp)
}

const showContact = ref(false)
const menuMsg = ref<Message | null>(null)

const draft = computed(() => lxmf.draftFor(lxmf.activePeer.value))
function setDraft(v: string) { lxmf.setDraft(lxmf.activePeer.value, v) }

function openPeer(peer: string) {
  lxmf.openPeer(peer)
  showContact.value = false
  lxmf.markConversationRead(peer)
}

async function onSend(content: string) {
  const peer = lxmf.activePeer.value
  if (!peer) return
  try {
    await lxmf.send(peer, content)
  } catch (e) {
    // Down identity / unprocessed sentinel: don't leave an unhandled
    // rejection. The guard above normally prevents reaching here.
    console.warn('lxmf send failed:', e instanceof Error ? e.message : e)
  }
}

function ratchetOf(peer: string): string {
  return lxmf.announces.value.find(a => a.hash === peer)?.ratchet ?? ''
}

function onDeleteConversation(peer: string) {
  if (!window.confirm('Delete this entire conversation? This cannot be undone.')) return
  lxmf.deleteConversation(peer)
  showContact.value = false
  if (lxmf.activePeer.value === peer) lxmf.activePeer.value = ''
}

async function copyMsg() {
  if (menuMsg.value) {
    try { await navigator.clipboard.writeText(menuMsg.value.content) } catch { /* ignore */ }
  }
  menuMsg.value = null
}
function cancelMsg() {
  if (menuMsg.value) lxmf.cancel(menuMsg.value.peer, menuMsg.value.key)
  menuMsg.value = null
}
function deleteMsg() {
  const m = menuMsg.value
  menuMsg.value = null
  if (m && window.confirm('Delete this message?')) lxmf.deleteMessage(m.peer, m.key)
}
function askDeleteMsg(m: Message) {
  if (m && window.confirm('Delete this message?')) lxmf.deleteMessage(m.peer, m.key)
}
</script>

<style scoped>
.msg-root { position: relative; height: 100%; display: flex; flex-direction: column;
            background: #1c1c1c; overflow: hidden; }
.idbar {
  display: flex; align-items: center; gap: 8px;
  padding: 6px 10px; border-bottom: 1px solid rgba(255,255,255,0.08);
}
.idlbl { color: #9a9a9a; font-size: 12px; }
.idsel {
  background: #2a2a2a; color: #e8e8e8; border: 1px solid rgba(255,255,255,0.12);
  border-radius: 6px; padding: 3px 8px; font-size: 12px; outline: none;
}
.noident { color: #9a9a9a; font-size: calc(13px * var(--rfs, 1)); padding: 24px; text-align: center; line-height: 1.5; }
.rfs-btn {
  display: inline-flex; align-items: center; justify-content: center;
  width: 18px; height: 18px; border-radius: 4px; font-size: 14px; font-weight: 700;
  color: rgba(255,255,255,0.5); cursor: pointer; font-family: system-ui; line-height: 1;
  user-select: none;
}
.rfs-btn:hover { color: rgba(255,255,255,0.9); background: rgba(255,255,255,0.1); }
.panes { flex: 1; display: flex; min-height: 0; }
.rail {
  flex: none;
  border-right: 1px solid rgba(255,255,255,0.08);
  overflow: hidden;
}
.splitter {
  flex: none; width: 6px; cursor: col-resize;
  margin: 0 -3px; z-index: 4;
  background: transparent;
}
.splitter:hover, .splitter.dragging {
  background: rgba(120,170,140,0.35);
}
.detail { flex: 1; min-width: 0; display: flex; flex-direction: column; }
.placeholder {
  flex: 1; display: flex; align-items: center; justify-content: center;
  color: #777; font-size: calc(13px * var(--rfs, 1)); font-style: italic;
}
.sheet-bg {
  position: absolute; inset: 0; background: rgba(0,0,0,0.45); z-index: 7;
  display: flex; align-items: flex-end; justify-content: center;
}
.sheet {
  width: 100%; max-width: 320px; margin: 12px;
  background: #262626; border-radius: 12px; overflow: hidden;
}
.sheet button {
  width: 100%; background: none; border: none;
  border-bottom: 1px solid rgba(255,255,255,0.07);
  color: #d8d8d8; padding: 12px; font-size: 14px; cursor: pointer;
}
.sheet button:last-child { border-bottom: none; }
.sheet button:hover { background: rgba(255,255,255,0.05); }
.sheet button.danger { color: #d98a8a; }
</style>
