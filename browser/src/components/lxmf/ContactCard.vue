<!-- Per-peer detail overlay. The `trust` flag renders as a Signal-style
     Verified badge; the destination hash + ratchet are the
     safety-number analog (compare-to-verify, no QR in v1 — plan §4/§6).
     Presentational: data in, intent out. -->
<template>
  <div class="card">
    <div class="chead">
      <button class="x" title="Close" @click="emit('close')">
        <q-icon :name="matArrowBack" size="20px" />
      </button>
      <span>Contact</span>
    </div>

    <div class="body">
      <!-- Action row. Each button is its own text's width — they are a row of
           verbs, not a stack of full-width choices. -->
      <div class="actions">
        <button class="act danger" @click="emit('delete-conversation', peer)">Delete</button>
        <div class="pingwrap">
          <button class="act" @click="onPing">Ping</button>
          <!-- Result popover. A touch anywhere — the backdrop below or the
               popover itself — dismisses it. -->
          <div v-if="showPing" class="pingpop" @click="showPing = false">
            <template v-if="!ping || ping.state === 'probing'">Probing…</template>
            <template v-else-if="ping.state === 'path'">Finding a path…</template>
            <template v-else>
              <!-- The probe's own outcome, then what the radio measured. The
                   reading rides every outcome: it is a measurement of the link,
                   not of the probe, and it is worth most where the probe came
                   back empty. -->
              <div v-if="ping.state === 'ok'" class="pingrtt">
                <!-- A link measures the round trip, not the path, so it has no
                     hop count to state. Saying "0 hops" would be a made-up
                     number where there is simply no answer. -->
                {{ ping.rttMs }} ms<template v-if="ping.hops > 0">
                  · {{ ping.hops }} hop{{ ping.hops === 1 ? '' : 's' }}</template>
              </div>
              <div v-else class="pingrtt">{{ pingFailText }}</div>
              <div v-for="l in linkLines" :key="l" class="pingrow">{{ l }}</div>
            </template>
          </div>
        </div>
      </div>
      <div v-if="showPing" class="pingbg" @click="showPing = false"></div>

      <div class="hero">
        <PeerAvatar :peer="peer" :name="name" :size="72" />
        <div class="hname">{{ name }}</div>
        <div v-if="verified" class="verified">
          <q-icon :name="matVerifiedUser" size="15px" /> Verified
        </div>
        <div v-else class="unverified">Not verified</div>
      </div>

      <div v-if="reachLine" class="reach">{{ reachLine }}</div>

      <!-- Grouped in fours for eye comparison; the copy button still yields
           the bare hex (paste targets want it unspaced). -->
      <div class="sect">Destination hash</div>
      <div class="addr">
        <span class="addrhex">{{ groupedHash }}</span>
        <button class="copy" :title="copied ? 'Copied' : 'Copy'" @click="copyHash">
          <q-icon :name="copied ? matCheck : matContentCopy" size="15px" />
        </button>
      </div>

      <!-- Per-contact propagation node: offered as the second option of the
           resend dialog for this contact's messages. -->
      <div class="sect">Propagation node</div>
      <select class="pnsel" :value="pnSelValue" @change="onPnSelect">
        <option value="">None</option>
        <option v-for="node in pnNodes" :key="node.hash" :value="node.hash">
          {{ node.name || node.hash.slice(0, 8) + '…' }} (ours)
        </option>
        <option v-if="pnIsForeign" :value="contactPn">
          {{ contactPn.slice(0, 8) }}… (custom)
        </option>
        <option value="~custom">Other…</option>
      </select>
      <div v-if="pnCustom" class="pncustom">
        <input v-model="pnCustomHash" class="pnfld" placeholder="32-hex node dest hash" />
        <button class="pnset" :disabled="!validCustomPn" @click="applyCustomPn">Set</button>
      </div>

      <template v-if="ratchet">
        <div class="sect">Ratchet</div>
        <div class="sn small">{{ ratchet }}</div>
      </template>

    </div>
  </div>
</template>

<script setup lang="ts">
import { computed, ref } from 'vue'
import { matArrowBack, matVerifiedUser, matContentCopy, matCheck }
  from '@quasar/extras/material-icons'
import PeerAvatar from './PeerAvatar.vue'
import { hasDest, pingLinkLines, peerMeasOf,
         type Contact, type PingResult, type PnNode, type Reachability }
  from '../../modules/lxmf'

const props = defineProps<{
  peer: string
  name: string
  contact: Contact | null
  reach: Reachability | null
  ratchet: string
  pnNodes: PnNode[]
  ping: PingResult | null
}>()
const emit = defineEmits<{
  close: []
  'delete-conversation': [peer: string]
  'set-pn': [peer: string, hash: string]
  ping: [peer: string]
}>()

/* ── Ping ──
 * The button starts a probe and opens the popover; the popover then just
 * renders whatever the firmware has published for this peer, so a re-press is
 * a fresh measurement rather than a second dialog. */
const showPing = ref(false)
function onPing() {
  showPing.value = true
  emit('ping', props.peer)
}

/* The radio's own reading of the link, published beside the probe's round trip:
 * one line per direction SUPE has measured, or — for a peer outside the
 * protocol, which states no power and so can have no path loss — the level this
 * radio read and the power it sent at. Rendered by the module so
 * this popover, the device's screen and its console cannot drift into
 * describing the same measurement three different ways. */
const linkLines = computed(() => pingLinkLines(peerMeasOf(props.peer)))

/* What became of the probe, never a verdict on the contact. The probe is a link
 * establishment, so these are that link's outcomes: `no-route` is the dial
 * failing, `no-proof` a probe on a link already open going unanswered. */
const pingFailText = computed(() => {
  switch (props.ping?.state) {
    case 'no-proof':  return 'Delivered, but no proof came back.'
    case 'no-route':  return 'No route to this contact.'
    case 'timeout':   return 'No answer.'
    case 'cancelled': return 'Probe cancelled.'
    default:          return 'Probe failed.'
  }
})

/* ── Per-contact propagation node ── */
const contactPn = computed(() => {
  const v = props.contact?.pn ?? ''
  return hasDest(v) ? v.toLowerCase() : ''
})
const pnIsForeign = computed(() =>
  !!contactPn.value && !props.pnNodes.some(n => n.hash === contactPn.value))
const pnCustom = ref(false)
const pnCustomHash = ref('')
const validCustomPn = computed(() => /^[0-9a-f]{32}$/i.test(pnCustomHash.value.trim()))
const pnSelValue = computed(() => (pnCustom.value ? '~custom' : contactPn.value))
function onPnSelect(e: Event) {
  const v = (e.target as HTMLSelectElement).value
  if (v === '~custom') { pnCustom.value = true; return }
  pnCustom.value = false
  emit('set-pn', props.peer, v)
}
function applyCustomPn() {
  emit('set-pn', props.peer, pnCustomHash.value.trim().toLowerCase())
  pnCustom.value = false
  pnCustomHash.value = ''
}

const verified = computed(() => (props.contact?.trust ?? 0) >= 1)


const copied = ref(false)
let copiedTimer: ReturnType<typeof setTimeout> | undefined
async function copyHash() {
  try { await navigator.clipboard.writeText(props.peer) } catch { /* ignore */ }
  copied.value = true
  clearTimeout(copiedTimer)
  copiedTimer = setTimeout(() => { copied.value = false }, 1500)
}

const groupedHash = computed(() =>
  (props.peer.match(/.{1,4}/g) ?? []).join(' '))

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
  return `Last heard on the mesh ${age}${hops}`
})
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
.hero { text-align: center; margin-bottom: 14px; display: flex;
        flex-direction: column; align-items: center; gap: 6px; }
.hname { color: #e8e8e8; font-size: calc(17px * var(--rfs, 1)); font-weight: 600; margin-top: 6px; }
.verified {
  display: inline-flex; align-items: center; gap: 4px;
  color: #6fb98f; font-size: calc(12px * var(--rfs, 1));
}
.unverified { color: #888; font-size: calc(12px * var(--rfs, 1)); }
.reach { text-align: center; color: #8a8a8a; font-size: calc(12px * var(--rfs, 1)); margin-bottom: 12px; }
.sect {
  color: #aaa; font-size: calc(12px * var(--rfs, 1)); text-transform: uppercase;
  letter-spacing: 0.05em; margin: 16px 0 6px;
}
.sn {
  font-family: 'JetBrains Mono', 'Menlo', monospace; font-size: calc(13px * var(--rfs, 1));
  color: #c8d8c8; background: #232323; border-radius: 8px;
  padding: 10px 12px; word-break: break-word; line-height: 1.6;
}
.sn.small { font-size: calc(11px * var(--rfs, 1)); color: #9a9a9a; }
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
/* Action row: buttons sized to their own text, left-aligned, above everything
 * else on the page. */
.actions { display: flex; gap: 8px; align-items: center; margin-bottom: 14px; }
.act {
  flex: none; background: none;
  border: 1px solid rgba(255,255,255,0.18); color: #d8d8d8; border-radius: 8px;
  padding: 5px 12px; font-size: calc(13px * var(--rfs, 1)); cursor: pointer;
  white-space: nowrap;
}
.act:hover { background: rgba(255,255,255,0.08); }
.act.danger { border-color: #a05656; color: #d98a8a; }
.act.danger:hover { background: rgba(160,86,86,0.15); }

/* The popover hangs off the Ping button, over a full-card backdrop that catches
 * the dismissing touch. */
.pingwrap { position: relative; flex: none; }
.pingbg { position: absolute; inset: 0; z-index: 8; }
.pingpop {
  position: absolute; top: calc(100% + 6px); left: 0; z-index: 9;
  min-width: 230px; background: #2b2b2b; border: 1px solid rgba(255,255,255,0.14);
  border-radius: 8px; padding: 9px 11px;
  box-shadow: 0 6px 18px rgba(0,0,0,0.45);
  color: #d8d8d8; font-size: calc(12px * var(--rfs, 1)); line-height: 1.5;
}
.pingrtt { color: #e8e8e8; font-weight: 600; margin-bottom: 4px; }
/* A measurement line wraps rather than running off the popover: it carries a
   loss, a signal-to-noise, a power in two units and an age, and truncating any
   of those would drop the half a reader came for. */
.pingrow { white-space: normal; }
.pingrow + .pingrow { margin-top: 3px; }
.pnsel {
  width: 100%; background: #2a2a2a; color: #e8e8e8;
  border: 1px solid rgba(255,255,255,0.12); border-radius: 6px;
  padding: 7px 10px; font-size: calc(13px * var(--rfs, 1)); outline: none;
}
.pncustom { display: flex; gap: 8px; margin-top: 8px; }
.pnfld {
  flex: 1; background: #2a2a2a; color: #e8e8e8;
  border: 1px solid rgba(255,255,255,0.12); border-radius: 6px;
  padding: 6px 10px; font-size: calc(11px * var(--rfs, 1));
  font-family: 'JetBrains Mono', monospace; outline: none;
}
.pnset {
  background: #3a5d47; border: none; color: #eaffea;
  border-radius: 6px; padding: 6px 12px; font-size: calc(12px * var(--rfs, 1));
  cursor: pointer;
}
.pnset:disabled { background: #333; color: #777; cursor: default; }
</style>
