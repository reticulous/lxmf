<!-- Settings → Mesh Network → LXMF Messages. The identity admin surface (create /
     import / destroy) and config knobs — deliberately NOT the chat
     window (plan §1). Identity destroy is irreversible and wipes the
     secret + all of that identity's storage, so it double-confirms. -->
<template>
  <div class="q-gutter-y-md">
    <PanelHeading title="LXMF" />

    <div class="text-caption text-grey-5">Identities</div>
    <div v-if="lxmf.identities.value.length === 0" class="none">
      No identities. Without one this device is a transport-only node
      (it relays the mesh but has no mailbox). Create or import one below.
    </div>
    <div v-for="id in lxmf.identities.value" :key="id.n" class="ident">
      <div class="irow">
        <div>
          <div class="ilabel">
            {{ id.displayName }}
            <span class="slot">slot {{ id.n }}</span>
            <span class="up" :class="{ live: id.up }">{{ id.up ? 'up' : 'down' }}</span>
          </div>
          <div class="ihash">{{ id.destHash || '(announcing…)' }}</div>
        </div>
        <div class="iact">
          <q-toggle
            :model-value="id.enabled"
            dense size="xs" color="green-5"
            :title="id.enabled ? 'Enabled — participating' : 'Disabled — dark on mesh'"
            @update:model-value="v => lxmf.setEnabled(id.n, v)"
          />
          <button class="del" @click="destroy(id.n)">Destroy</button>
        </div>
      </div>
    </div>

    <div class="text-caption text-grey-5">Add identity</div>
    <div class="add">
      <input v-model="newLabel" class="fld" placeholder="Display name" />
      <button class="act" :disabled="busy" @click="create">Create new</button>
    </div>
    <div class="add">
      <input v-model="importHex" class="fld mono" placeholder="128-hex private key" />
      <button class="act" :disabled="busy || !validHex" @click="doImport">Import</button>
    </div>
    <div v-if="msg" class="msg">{{ msg }}</div>

    <q-separator dark />

    <SettingSlider label="Re-announce interval (s)" k="s.lxmf.announce_interval_s"
                   :min="0" :max="21600" :step="300" />
    <div class="text-caption text-grey-5">0 = announce on demand only.</div>
    <SettingSlider label="Announce catalogue cap" k="s.lxmf.max_announces"
                   :min="256" :max="8192" :step="256" />

    <SettingSlider label="Advertised stamp cost" k="s.lxmf.stamp_cost"
                   :min="0" :max="18" :step="1" />
    <div class="text-caption text-grey-5">
      Proof-of-work cost (bits) we ask senders to pay. 0 = advertise none.
      Higher costs take senders longer to compute.
    </div>
    <SettingToggle label="Generate outbound stamps" k="s.lxmf.generate_stamps" />
    <div class="text-caption text-grey-5">
      Pay a peer's advertised proof-of-work cost when sending. No cost
      advertised → no work, no delay.
    </div>
    <SettingToggle label="Require inbound stamps" k="s.lxmf.enforce_stamps" />
    <div class="text-caption text-grey-5">
      Drop incoming messages that lack a valid stamp for our advertised cost.
    </div>

    <SettingToggle label="Notification sound" k="s.lxmf.sound_enabled" />
    <div class="text-caption text-grey-5">
      Play a sound out the speaker when a message arrives. Requires audio
      hardware; the sound file is set in <code>s.lxmf.sound</code>.
    </div>

    <q-separator dark />

    <div class="text-caption text-grey-5">Propagation nodes</div>
    <div class="text-caption text-grey-5">
      Classic LXMF store-and-forward nodes. Messages can be resent through
      one from a message's detail page; nodes with “check” on are polled
      for messages held for your identities.
    </div>
    <div v-if="lxmf.pnNodes.value.length === 0" class="none">
      No propagation nodes configured.
    </div>
    <div v-for="(node, i) in lxmf.pnNodes.value" :key="node.hash" class="ident">
      <div class="irow">
        <div class="pnmain">
          <input
            class="fld pnname"
            :value="node.name"
            placeholder="(unnamed)"
            @change="e => lxmf.pnSetName(i, (e.target as HTMLInputElement).value)"
          />
          <div class="ihash">{{ node.hash }}</div>
          <div v-if="pnStatusLine(node.hash)" class="pnstat">{{ pnStatusLine(node.hash) }}</div>
        </div>
        <div class="iact">
          <label class="chk" title="Check this node for held messages">
            <input type="checkbox" :checked="node.check"
                   @change="e => lxmf.pnSetCheck(i, (e.target as HTMLInputElement).checked)" />
            check
          </label>
          <button class="mv" :disabled="i === 0" title="Move up"
                  @click="lxmf.pnMove(i, -1)">↑</button>
          <button class="mv" :disabled="i === lxmf.pnNodes.value.length - 1" title="Move down"
                  @click="lxmf.pnMove(i, 1)">↓</button>
          <button class="del" title="Remove" @click="removePn(i)">✕</button>
        </div>
      </div>
    </div>
    <div class="add">
      <input v-model="pnHash" class="fld mono" placeholder="32-hex node dest hash" />
      <input v-model="pnName" class="fld pnaddname" placeholder="Name (optional)" />
      <button class="act" :disabled="!validPnHash" @click="addPn">Add</button>
    </div>
    <div class="add">
      <button class="act" :disabled="!lxmf.pnNodes.value.some(n => n.check)" @click="checkNow">
        Check for messages now
      </button>
      <span v-if="syncing" class="pnstat">checking {{ syncing.slice(0, 8) }}…</span>
    </div>
    <SettingSlider label="Check interval (s)" k="s.lxmf.pn.check_interval_s"
                   :min="0" :max="21600" :step="300" />
    <div class="text-caption text-grey-5">
      How often the checked nodes are polled. 0 = only when asked.
    </div>
  </div>
</template>

<script setup lang="ts">
import { computed, ref } from 'vue'
import { useDeviceStore } from 'spangap-browser/stores/device'
import { useLxmf } from '../modules/lxmf'

const lxmf = useLxmf()
const newLabel = ref('')
const importHex = ref('')
const busy = ref(false)
const msg = ref('')

const validHex = computed(() => /^[0-9a-f]{128}$/i.test(importHex.value.trim()))

/* ── Propagation nodes ── */
const pnHash = ref('')
const pnName = ref('')
const validPnHash = computed(() => /^[0-9a-f]{32}$/i.test(pnHash.value.trim()))
const syncing = computed(() =>
  String(useDeviceStore().get('lxmf.pn.sync') ?? ''))

function addPn() {
  try { lxmf.pnAdd(pnHash.value, pnName.value) }
  catch (e) { msg.value = e instanceof Error ? e.message : 'failed'; return }
  pnHash.value = ''
  pnName.value = ''
}
function removePn(i: number) {
  const node = lxmf.pnNodes.value[i]
  if (!node) return
  if (!window.confirm(`Remove propagation node ${node.name || node.hash}?`)) return
  lxmf.pnRemove(i)
}
function checkNow() {
  lxmf.pnSyncNow()
}
function pnStatusLine(hash: string): string {
  const st = lxmf.pnStatus(hash)
  if (!st || !st.lastCheckS) return ''
  const when = new Date(st.lastCheckS * 1000).toLocaleTimeString()
  return st.lastErr ? `last check ${when}: ${st.lastErr}`
                    : `last check ${when}: ${st.lastGot} message(s)`
}

async function run(fn: () => Promise<void>, ok: string) {
  busy.value = true
  msg.value = ''
  try { await fn(); msg.value = ok }
  catch (e) { msg.value = e instanceof Error ? e.message : 'failed' }
  finally { busy.value = false }
}

function create() {
  const label = newLabel.value.trim() || 'main'
  run(() => lxmf.createIdentity(label), `Created “${label}”.`)
  newLabel.value = ''
}
function doImport() {
  const hex = importHex.value.trim()
  run(() => lxmf.importIdentity(hex), 'Identity imported.')
  importHex.value = ''
}
function destroy(n: number) {
  if (!window.confirm(
    `Destroy identity in slot ${n}? This wipes its private key and all ` +
    `of its messages and contacts. This cannot be undone.`)) return
  run(() => lxmf.destroyIdentity(n), `Destroyed slot ${n}.`)
}
</script>

<style scoped>
.none { color: #9a9a9a; font-size: 13px; line-height: 1.4; }
.ident { background: #232323; border-radius: 8px; padding: 8px 10px; margin-bottom: 6px; }
.irow { display: flex; align-items: center; justify-content: space-between; gap: 10px; }
.ilabel { color: #e8e8e8; font-size: 13px; font-weight: 600; }
.slot { color: #888; font-weight: 400; font-size: 11px; margin-left: 6px; }
.up { font-size: 11px; margin-left: 8px; color: #a06868; }
.up.live { color: #6fb98f; }
.ihash {
  color: #9a9a9a; font-size: 11px; margin-top: 2px;
  font-family: 'JetBrains Mono', monospace; word-break: break-all;
}
.iact { display: flex; align-items: center; gap: 8px; flex: none; }
.del {
  background: none; border: 1px solid #a05656; color: #d98a8a;
  border-radius: 5px; padding: 2px 10px; font-size: 12px; cursor: pointer;
}
.del:hover { background: rgba(160,86,86,0.15); }
.add { display: flex; gap: 8px; align-items: center; }
.fld {
  flex: 1; background: #2a2a2a; color: #e8e8e8;
  border: 1px solid rgba(255,255,255,0.12); border-radius: 6px;
  padding: 7px 10px; font-size: 13px; outline: none;
}
.fld.mono { font-family: 'JetBrains Mono', monospace; font-size: 11px; }
.pnmain { flex: 1; min-width: 0; }
.pnname { width: 160px; margin-bottom: 3px; padding: 3px 8px; font-size: 12px; }
.pnaddname { max-width: 160px; }
.pnstat { color: #8a9a8a; font-size: 11px; margin-top: 2px; }
.chk {
  display: inline-flex; align-items: center; gap: 4px;
  color: #b8c0b8; font-size: 12px; cursor: pointer; user-select: none;
}
.mv {
  background: none; border: 1px solid rgba(255,255,255,0.18); color: #b8b8b8;
  border-radius: 5px; padding: 2px 7px; font-size: 12px; cursor: pointer;
}
.mv:disabled { opacity: 0.3; cursor: default; }
.mv:not(:disabled):hover { background: rgba(255,255,255,0.08); }
.fld:focus { border-color: rgba(120,170,140,0.6); }
.act {
  background: #3a5d47; border: none; color: #eaffea;
  border-radius: 6px; padding: 7px 12px; font-size: 13px; cursor: pointer;
}
.act:disabled { background: #333; color: #777; cursor: default; }
.act:not(:disabled):hover { background: #46704f; }
.msg { color: #9fb8d8; font-size: 12px; }
</style>
