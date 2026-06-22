<!-- Settings → Mesh Network → LXMF Messages. The identity admin surface (create /
     import / destroy) and config knobs — deliberately NOT the chat
     window (plan §1). Identity destroy is irreversible and wipes the
     secret + all of that identity's storage, so it double-confirms. -->
<template>
  <div class="q-gutter-y-md">
    <PanelHeading title="LXMF" />

    <SettingSlider label="Re-announce interval (s)" k="s.lxmf.announce_interval_s"
                   :min="0" :max="21600" :step="300" />
    <div class="text-caption text-grey-5">0 = announce on demand only.</div>
    <SettingSlider label="Announce catalogue cap" k="s.lxmf.max_announces"
                   :min="256" :max="8192" :step="256" />

    <SettingToggle label="Generate outbound stamps" k="s.lxmf.generate_stamps" />
    <div class="text-caption text-grey-5">
      Pay a peer's advertised proof-of-work cost when sending. No cost
      advertised → no work, no delay.
    </div>
    <SettingToggle label="Require inbound stamps" k="s.lxmf.enforce_stamps" />
    <div class="text-caption text-grey-5">
      Drop incoming messages that lack a valid stamp for our advertised cost.
    </div>

    <q-separator dark />

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

    <q-separator dark />

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
  </div>
</template>

<script setup lang="ts">
import { computed, ref } from 'vue'
import { useLxmf } from '../modules/lxmf'

const lxmf = useLxmf()
const newLabel = ref('')
const importHex = ref('')
const busy = ref(false)
const msg = ref('')

const validHex = computed(() => /^[0-9a-f]{128}$/i.test(importHex.value.trim()))

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
.fld:focus { border-color: rgba(120,170,140,0.6); }
.act {
  background: #3a5d47; border: none; color: #eaffea;
  border-radius: 6px; padding: 7px 12px; font-size: 13px; cursor: pointer;
}
.act:disabled { background: #333; color: #777; cursor: default; }
.act:not(:disabled):hover { background: #46704f; }
.msg { color: #9fb8d8; font-size: 12px; }
</style>
