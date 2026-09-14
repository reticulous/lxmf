<!-- Per-message detail overlay: every field we store about a message. Opened by
     clicking a bubble. Presentational: data in, close out. -->
<template>
  <div class="card">
    <div class="chead">
      <button class="x" title="Close" @click="emit('close')">
        <q-icon :name="matArrowBack" size="20px" />
      </button>
      <span>Message details</span>
    </div>

    <div class="body">
      <div class="hero">
        <span class="dir" :class="m.dir">{{ m.dir === 'in' ? 'Incoming' : 'Outgoing' }}</span>
        <span class="status">{{ statusName }}</span>
      </div>

      <!-- The failure our proxy stopped on. Said here and not in the
           conversation, where one ✕ is the whole story a reader wants — and
           said as the PROXY's verdict, because the one thing this must never
           be read as is trouble reaching the proxy. -->
      <template v-if="proxyGaveUpWith">
        <div class="sect">Our proxy gave up</div>
        <div class="sn text">It tried and stopped: {{ proxyGaveUpWith }}</div>
      </template>

      <template v-if="m.title">
        <div class="sect">Title</div>
        <div class="sn text">{{ m.title }}</div>
      </template>

      <div class="sect">Content</div>
      <div class="sn text">{{ m.content || '—' }}</div>

      <div class="sect">Peer</div>
      <div class="addr">
        <span class="addrhex">{{ peerName }}<br>{{ grouped(m.peer) }}</span>
        <button class="copy" :title="copiedKey === 'peer' ? 'Copied' : 'Copy'" @click="copy('peer', m.peer)">
          <q-icon :name="copiedKey === 'peer' ? matCheck : matContentCopy" size="15px" />
        </button>
      </div>

      <div class="sect">Message ID</div>
      <div class="addr">
        <span class="addrhex">{{ grouped(m.messageId) || '—' }}</span>
        <button v-if="m.messageId" class="copy" :title="copiedKey === 'mid' ? 'Copied' : 'Copy'"
                @click="copy('mid', m.messageId)">
          <q-icon :name="copiedKey === 'mid' ? matCheck : matContentCopy" size="15px" />
        </button>
      </div>

      <template v-if="isReply">
        <div class="sect">In reply to</div>
        <div class="sn small">{{ grouped(m.replyTo!) }}</div>
        <!-- The fragment as it arrived, whether or not it is one the
             conversation will draw — that needs it to occur in the message
             above, and this is where an unverifiable quote can still be read. -->
        <template v-if="m.replyQuote">
          <div class="sect">Quoting</div>
          <div class="sn small">{{ m.replyQuote }}</div>
        </template>
      </template>

      <div class="sect">Details</div>
      <div class="kv">
        <div class="k">Timestamp</div><div class="v">{{ sentTime }}</div>
        <template v-if="m.dir === 'out'">
          <div class="k">Method</div><div class="v">{{ m.method || 'auto' }}</div>
          <div class="k">Attempts</div><div class="v">{{ triesLabel }}</div>
        </template>
        <template v-else>
          <div class="k">Read</div><div class="v">{{ m.read ? 'yes' : 'no' }}</div>
        </template>
      </div>

      <!-- "…" only where a press opens a dialog. With one thing it can mean,
           the button says the thing. -->
      <button v-if="m.dir === 'out'" class="resend" @click="openResend">
        {{ resendOptions.length === 1 && resendOptions[0].via === RESEND_RETRY
             ? 'Try again now' : 'Resend…' }}
      </button>
      <div v-if="resendDone" class="done">{{ resendDone }}</div>
    </div>

    <!-- Resend dialog: direct first, the contact's node second (when set),
         then our propagation nodes. -->
    <div v-if="resendOpen" class="dlg-bg" @click="resendOpen = false">
      <div class="dlg" @click.stop>
        <div class="dtitle">Resend message</div>
        <label v-for="opt in resendOptions" :key="opt.via" class="ropt">
          <input type="radio" name="resend-via" :value="opt.via" v-model="resendVia" />
          <span>{{ opt.label }}</span>
        </label>
        <div class="dbtns">
          <button class="go" @click="doResend">Resend</button>
          <button class="cancel" @click="resendOpen = false">Cancel</button>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { computed, ref } from 'vue'
import { matArrowBack, matContentCopy, matCheck } from '@quasar/extras/material-icons'
import { type Message, type PnNode, lxmfStatusName, LxmfStatus,
         hasDest, LXMF_TRIES_GAVEUP, RESEND_RETRY } from '../../modules/lxmf'

const props = defineProps<{
  m: Message
  peerName: string
  pnNodes: PnNode[]
  contactPn: string
  /** This identity's mail belongs to a proxy server. */
  proxied?: boolean
  /** A conversation link to this peer is up, so this device can carry it. */
  linkUp?: boolean
}>()
const emit = defineEmits<{
  close: []
  /** via = '' → resend directly, RESEND_RETRY → ask our proxy to try again
   *  without moving the body, else the 32-hex propagation node. */
  resend: [m: Message, via: string]
}>()

/* ── Resend ──
 *
 * A dialog is for choosing, so it appears only when there is a choice. Proxied
 * with no link open there is exactly one thing a press can mean, and asking
 * which of one is a question with no information in it. */
const resendOpen = ref(false)
const resendVia = ref('')
const resendDone = ref('')       /* the little "here is what I did" notice */
let doneTimer: ReturnType<typeof setTimeout> | undefined

const resendOptions = computed(() => {
  const opts: { via: string; label: string }[] = []
  /* Proxied, this device does not send: the server does. The direct option
   * comes back only when a link to the recipient is open, which is the one
   * case where this device can carry the message itself. */
  if (props.proxied) {
    if (props.linkUp) opts.push({ via: '', label: 'Directly, over the open link' })
    opts.push({ via: RESEND_RETRY, label: 'Ask your proxy to try again now' })
  } else {
    opts.push({ via: '', label: 'Directly' })
  }
  const cpn = hasDest(props.contactPn) ? props.contactPn.toLowerCase() : ''
  if (cpn) {
    const known = props.pnNodes.find(n => n.hash === cpn)
    opts.push({
      via: cpn,
      label: `Contact’s node — ${known?.name || cpn.slice(0, 8) + '…'}`,
    })
  }
  for (const n of props.pnNodes) {
    if (n.hash === cpn) continue
    opts.push({ via: n.hash, label: `Node ${n.name || n.hash.slice(0, 8) + '…'}` })
  }
  return opts
})

function fire(via: string) {
  emit('resend', props.m, via)
  /* The one action with nothing to watch afterwards says so itself: no message
   * leaves this device, so the conversation shows nothing happening for as long
   * as the proxy takes. */
  if (via === RESEND_RETRY) {
    resendDone.value = 'Asked your proxy to try again now.'
    clearTimeout(doneTimer)
    doneTimer = setTimeout(() => { resendDone.value = '' }, 2600)
  }
}

function openResend() {
  const opts = resendOptions.value
  if (opts.length === 1) { fire(opts[0].via); return }
  resendVia.value = opts[0].via
  resendOpen.value = true
}
function doResend() {
  resendOpen.value = false
  fire(resendVia.value)
}

const statusName = computed(() => lxmfStatusName(props.m.status))

/* The status the proxy itself stopped on, named — empty unless this message
 * actually ended that way, so the section it fills does not appear on messages
 * the proxy never gave up on. */
const proxyGaveUpWith = computed(() =>
  props.m.status === LxmfStatus.OurProxyGaveUp && props.m.proxyStatus
    ? (lxmfStatusName(props.m.proxyStatus) || 'an unnamed failure')
    : '')

const grouped = (hex: string) => (hex.match(/.{1,4}/g) ?? []).join(' ')

const isReply = computed(() => {
  const r = props.m.replyTo ?? ''
  return r.length > 0 && !/^0+$/.test(r)
})

const sentTime = computed(() =>
  props.m.ts ? new Date(props.m.ts * 1000).toLocaleString() : '—')
const triesLabel = computed(() =>
  props.m.tries === LXMF_TRIES_GAVEUP ? 'gave up' : String(props.m.tries))

const copiedKey = ref('')
let copiedTimer: ReturnType<typeof setTimeout> | undefined
async function copy(key: string, val: string) {
  try { await navigator.clipboard.writeText(val) } catch { /* ignore */ }
  copiedKey.value = key
  clearTimeout(copiedTimer)
  copiedTimer = setTimeout(() => { copiedKey.value = '' }, 1500)
}
</script>

<style scoped>
.card { position: absolute; inset: 0; background: #1c1c1c; z-index: 6;
        display: flex; flex-direction: column; }
.chead {
  display: flex; align-items: center; gap: 8px;
  padding: 8px 10px; border-bottom: 1px solid rgba(255,255,255,0.08);
  color: #e8e8e8; font-weight: 600; font-size: calc(14px * var(--rfs, 1));
}
.x { background: none; border: none; color: #9a9a9a; cursor: pointer; padding: 2px; }
.body { flex: 1; overflow-y: auto; padding: 16px; }
.hero { display: flex; align-items: center; gap: 8px; margin-bottom: 6px; }
.dir { font-weight: 600; font-size: calc(15px * var(--rfs, 1)); color: #e8e8e8; }
.dir.in  { color: #9ec9ff; }
.dir.out { color: #9fe0b0; }
.status { color: #8a8a8a; font-size: calc(12px * var(--rfs, 1)); text-transform: uppercase; letter-spacing: 0.04em; }
.bars i:nth-child(1) { height: 4px; }
.bars i:nth-child(2) { height: 6px; }
.bars i:nth-child(3) { height: 8px; }
.bars i:nth-child(4) { height: 10px; }
.sect {
  color: #aaa; font-size: calc(12px * var(--rfs, 1)); text-transform: uppercase;
  letter-spacing: 0.05em; margin: 16px 0 6px;
}
.sn {
  font-family: 'JetBrains Mono', 'Menlo', monospace; font-size: calc(13px * var(--rfs, 1));
  color: #c8d8c8; background: #232323; border-radius: 8px;
  padding: 10px 12px; word-break: break-word; line-height: 1.6;
}
.sn.text { font-family: inherit; color: #e8e8e8; white-space: pre-wrap; }
.sn.small { font-size: calc(11px * var(--rfs, 1)); color: #9a9a9a; }
.kv {
  display: grid; grid-template-columns: max-content 1fr; gap: 4px 14px;
  background: #232323; border-radius: 8px; padding: 10px 12px;
}
.k { color: #8a8a8a; font-size: calc(12px * var(--rfs, 1)); }
.v { color: #e0e0e0; font-size: calc(12px * var(--rfs, 1)); word-break: break-word; }
.v.mono { font-family: 'JetBrains Mono', 'Menlo', monospace; }
.addr {
  display: flex; align-items: center; gap: 8px;
  background: #232323; border-radius: 8px; padding: 8px 8px 8px 12px;
}
.addrhex {
  flex: 1; min-width: 0;
  font-family: 'JetBrains Mono', 'Menlo', monospace; font-size: calc(12px * var(--rfs, 1));
  color: #c8d8c8; line-height: 1.5;
}
.copy {
  flex: none; background: none; border: none; color: #9a9a9a;
  cursor: pointer; padding: 4px; border-radius: 5px;
}
.copy:hover { background: rgba(255,255,255,0.08); color: #cfcfcf; }
.resend {
  margin-top: 18px; width: 100%; background: #3a5d47; border: none;
  color: #eaffea; border-radius: 8px; padding: 9px;
  font-size: calc(13px * var(--rfs, 1)); cursor: pointer;
}
.resend:hover { background: #46704f; }
/* The receipt for an action with nothing else to show for itself. */
.done {
  margin-top: 8px; color: #9ecf9e; text-align: center;
  font-size: calc(12px * var(--rfs, 1));
}
.dlg-bg {
  position: absolute; inset: 0; background: rgba(0,0,0,0.5); z-index: 8;
  display: flex; align-items: center; justify-content: center;
}
.dlg {
  width: 100%; max-width: 320px; margin: 16px; background: #262626;
  border-radius: 12px; padding: 14px;
}
.dtitle {
  color: #e8e8e8; font-weight: 600; font-size: calc(14px * var(--rfs, 1));
  margin-bottom: 10px;
}
.ropt {
  display: flex; align-items: center; gap: 8px; padding: 7px 2px;
  color: #d8d8d8; font-size: calc(13px * var(--rfs, 1)); cursor: pointer;
}
.dbtns { display: flex; gap: 8px; margin-top: 12px; }
.dbtns button {
  flex: 1; border: none; border-radius: 8px; padding: 8px;
  font-size: calc(13px * var(--rfs, 1)); cursor: pointer;
}
.dbtns .go { background: #3a5d47; color: #eaffea; }
.dbtns .go:hover { background: #46704f; }
.dbtns .cancel { background: #333; color: #c8c8c8; }
</style>
