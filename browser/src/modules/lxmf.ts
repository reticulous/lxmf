/**
 * lxmf — headless state layer for the LXMF messaging client.
 *
 * This is the ONLY module that touches `useDeviceStore()` for messaging.
 * Views bind to `useLxmf()`; they never read the device store, never
 * know whether they are a pane or a screen, and emit intent rather than
 * mutate layout. See docs/plans/lxmf-browser-client.md §2/§3.
 *
 * Everything is storage: reads are `computed` over the reactive mirror
 * (multi-frontend coherence is free); writes are storage patches. The
 * `s.lxmf.*` schema is documented in reticulous/docs/lxmf.md and
 * docs/internals/lxmf.md — this file maps it onto reactive state.
 */
import { ref, reactive, computed, watch, type ComputedRef } from 'vue'
import { useDeviceStore } from 'diptych-browser/stores/device'
import { useMenuStore } from 'diptych-browser/stores/menu'
import LxmfPanel from '../panels/LxmfPanel.vue'

/* ── Composition-layer wiring (pattern mirrors modules/rnsd.ts) ──────── */

/** Visibility refs for the Status floating windows; MainLayout binds to them. */
export const messagesVisible = ref(false)
export const announcesVisible = ref(false)

/* ── Client-local view cursor (§3.3) ────────────────────────────────────
 * activeIdentity is NOT s.lxmf.cli.selected_id — the CLI and a browser
 * tab legitimately view different identities. Persisted only to
 * localStorage, never to the device. activePeer is the shared selection
 * the composition layer turns into a pane or a screen (§2). */
const LS_IDENT = 'lxmf.activeIdentity'
const _activeIdentity = ref<number>(
  Number(localStorage.getItem(LS_IDENT) ?? -1),
)
const _activePeer = ref<string>('')
watch(_activeIdentity, (n) => {
  localStorage.setItem(LS_IDENT, String(n))
  _activePeer.value = '' // selection does not survive an identity switch
})

/* Composer drafts: in-memory, per-peer, NEVER persisted (§4). Switching
 * threads and back preserves typed text without ever writing a
 * stage=draft record. Reactive so the bound `draft` computed re-tracks
 * keystrokes — a plain Map is not reactive and the model would stay
 * stale (Send would see empty content). */
const _composerDrafts = reactive<Record<string, string>>({})

/* ── Types ──────────────────────────────────────────────────────────── */

export type Stage =
  | 'draft' | 'queued' | 'sending' | 'sent'
  | 'delivered' | 'failed' | 'cancelled' | 'received'

export interface Message {
  key: string
  peer: string
  dir: 'in' | 'out'
  stage: Stage
  title: string
  content: string
  thread: string
  ts: number
  read: boolean
  attempts: number
  lastError: string
  messageId: string
}

export interface Conversation {
  peer: string
  name: string
  last: Message | null
  ts: number
  unread: number
  count: number
}

export interface Identity {
  n: number
  label: string
  displayName: string
  up: boolean
  destHash: string
  enabled: boolean
}

export interface Contact {
  peer: string
  displayName: string
  nick: string
  trust: number
  lastSeen: number
}

export interface Announce {
  hash: string
  name: string
  lastSeen: number
  cost: number
  hops: number
  ratchet: string
}

export interface Reachability {
  lastSeenS: number
  hops: number
}

/* ── Pure helpers (reused verbatim by the on-device port) ───────────── */

const HOPS_UNKNOWN = 128 /* µR PATHFINDER_M */

export function shortHash(peer: string): string {
  return peer ? `${peer.slice(0, 8)}…` : '(unknown)'
}

/** Deterministic identicon descriptor. Hue from the first hash bytes,
 *  glyph = up-to-two initials of the resolved name (hex prefix if
 *  unnamed). Legible at 24 px, identical on a 320×240 screen. */
export function peerAvatar(peer: string, name: string): { hue: number; glyph: string } {
  let h = 0
  for (let i = 0; i < Math.min(peer.length, 8); i++) h = (h * 31 + peer.charCodeAt(i)) >>> 0
  const named = name && !name.startsWith('(')
  const glyph = named
    ? name.trim().split(/\s+/).slice(0, 2).map(w => w[0]!.toUpperCase()).join('')
    : peer.slice(0, 2).toUpperCase()
  return { hue: h % 360, glyph: glyph || '?' }
}

function num(v: unknown, d = 0): number { const n = Number(v); return Number.isFinite(n) ? n : d }
function str(v: unknown): string { return v == null ? '' : String(v) }

/* ── Per-sentinel command queues (§3.2) ─────────────────────────────────
 * cmd.send / cmd.cancel / cmd.delete / cmd.announce are four distinct
 * firmware-cleared keys. One small queue PER key PER identity: the next
 * write for that key waits until the mirror shows the key undefined
 * (firmware deleted it = done), event-driven on the reactive store with
 * a timeout fallback. Different cmd kinds never block each other. */

type Patch = Record<string, any>

function deepAssign(dst: Patch, src: Patch): Patch {
  for (const k of Object.keys(src)) {
    const v = src[k]
    if (v && typeof v === 'object' && !Array.isArray(v)) {
      if (!dst[k] || typeof dst[k] !== 'object') dst[k] = {}
      deepAssign(dst[k], v)
    } else dst[k] = v
  }
  return dst
}

/** Build a nested object from a dot path: ("lxmf.id.0.cmd.send", "x")
 *  → {lxmf:{id:{0:{cmd:{send:"x"}}}}}. */
function nest(path: string, val: any): Patch {
  const parts = path.split('.')
  const root: Patch = {}
  let cur = root
  for (let i = 0; i < parts.length - 1; i++) cur = (cur[parts[i]!] = {})
  cur[parts[parts.length - 1]!] = val
  return root
}

/* The firmware *deletes* the cmd sentinel when it processes it, but it
 * lives in the ephemeral `lxmf.*` namespace whose deletions are NOT
 * propagated back over the storage DC (the ~1 Hz republish merges, it
 * never sends an explicit null). So "sentinel disappeared" is
 * unobservable from the browser and must NOT be the completion signal —
 * doing so produced a false "send failed" on sends that actually
 * succeeded (firmware logged `msg → sent`). Instead we resolve on the
 * reliably-synced *effect* (the `s.lxmf.*` record's stage / existence),
 * and a write with no observable effect just paces by a short settle so
 * a rapid same-kind sentinel can't clobber an unprocessed one. We never
 * reject: the message's stage is the UI source of truth, and a spurious
 * error is worse than none. */
const CMD_MAX_WAIT_MS = 8000
const CMD_SETTLE_MS = 600

interface EnqueueOpts {
  data?: Patch
  /** Reactive predicate: true once the firmware's effect is visible. */
  settle?: () => boolean
  maxWaitMs?: number
}

class CmdQueue {
  private chain: Promise<void> = Promise.resolve()
  constructor(private keyPath: string) {}

  enqueue(value: string, opts: EnqueueOpts = {}): Promise<void> {
    const run = () => new Promise<void>((resolve) => {
      const device = useDeviceStore()
      const patch: Patch = opts.data ? structuredClone(opts.data) : {}
      deepAssign(patch, nest(this.keyPath, value))
      device.sendJson(patch)

      const settle = opts.settle
      if (!settle) { setTimeout(resolve, CMD_SETTLE_MS); return }
      if (settle()) { setTimeout(resolve, 0); return }
      let done = false
      const finish = () => {
        if (done) return
        done = true
        stop()
        clearTimeout(timer)
        resolve()
      }
      const stop = watch(() => settle(), (ok) => { if (ok) finish() })
      const timer = setTimeout(finish, opts.maxWaitMs ?? CMD_MAX_WAIT_MS)
    })
    // Serialize same-kind actions; never wedge the chain.
    const next = this.chain.then(run, run)
    this.chain = next.catch(() => {})
    return next
  }
}

/* Queues are keyed by full sentinel path so they are per-identity too. */
const _queues = new Map<string, CmdQueue>()
function queue(path: string): CmdQueue {
  let q = _queues.get(path)
  if (!q) _queues.set(path, (q = new CmdQueue(path)))
  return q
}

function rand4(): string {
  return Math.floor(Math.random() * 0x10000).toString(16).padStart(4, '0')
}

/* ── The composable ─────────────────────────────────────────────────── */

export interface UseLxmf {
  activeIdentity: typeof _activeIdentity
  activePeer: typeof _activePeer
  identities: ComputedRef<Identity[]>
  usableIdentities: ComputedRef<Identity[]>
  activeIdentityUp: ComputedRef<boolean>
  conversations: ComputedRef<Conversation[]>
  activeConversation: ComputedRef<{ day: string; messages: Message[] }[]>
  contacts: ComputedRef<Record<string, Contact>>
  announces: ComputedRef<Announce[]>
  peerDirectory: ComputedRef<{ peer: string; name: string; known: boolean }[]>
  unreadTotal: ComputedRef<number>
  displayName: (peer: string) => string
  reachability: (peer: string) => Reachability | null
  contactOf: (peer: string) => Contact | null
  draftFor: (peer: string) => string
  setDraft: (peer: string, text: string) => void
  openPeer: (peer: string) => void
  send: (peer: string, content: string, opts?: { method?: string; thread?: string }) => Promise<void>
  resend: (peer: string, key: string) => Promise<void>
  cancel: (peer: string, key: string) => Promise<void>
  deleteMessage: (peer: string, key: string) => Promise<void>
  deleteConversation: (peer: string) => Promise<void>
  markConversationRead: (peer: string) => void
  announceNow: () => Promise<void>
  createIdentity: (label: string) => Promise<void>
  importIdentity: (privHex: string) => Promise<void>
  destroyIdentity: (n: number) => Promise<void>
  setEnabled: (n: number, on: boolean) => void
}

export function useLxmf(): UseLxmf {
  const device = useDeviceStore()

  const idTree = computed<Record<string, any>>(() => device.get('s.lxmf.id') ?? {})
  const liveTree = computed<Record<string, any>>(() => device.get('lxmf.id') ?? {})

  const identities = computed<Identity[]>(() =>
    Object.keys(idTree.value)
      .map(Number).filter(n => !Number.isNaN(n)).sort((a, b) => a - b)
      .map((n) => {
        const s = idTree.value[n] ?? {}
        const live = liveTree.value[n] ?? {}
        return {
          n,
          label: str(s.label),
          displayName: str(s.display_name) || str(s.label) || `identity ${n}`,
          up: num(live.up) === 1,
          destHash: str(live.dest_hash),
          enabled: num(s.enabled, 1) === 1,   // default on; absent ⇒ enabled
        }
      }))

  /** Only identities the firmware has actually brought up can send or
   *  receive. A config-only slot (e.g. `s.lxmf.id.N.*` written by a test
   *  or import with no `secrets.lxmf.id.N.privkey`) is NOT usable —
   *  presenting it as one lets a send hang on an unprocessable sentinel. */
  const usableIdentities = computed<Identity[]>(() =>
    identities.value.filter(i => i.up && i.destHash))

  // Default activeIdentity to the lowest *usable* slot; fall back to the
  // lowest existing slot only so history is still viewable.
  watch([identities, usableIdentities], ([ids, usable]) => {
    if (usable.length && !usable.some(i => i.n === _activeIdentity.value))
      _activeIdentity.value = usable[0]!.n
    else if (!usable.length && ids.length &&
             !ids.some(i => i.n === _activeIdentity.value))
      _activeIdentity.value = ids[0]!.n
  }, { immediate: true })

  const activeId = computed(() => _activeIdentity.value)
  const activeIdentityUp = computed(() =>
    usableIdentities.value.some(i => i.n === _activeIdentity.value))

  const contacts = computed<Record<string, Contact>>(() => {
    const raw = device.get(`s.lxmf.id.${activeId.value}.contacts`) ?? {}
    const out: Record<string, Contact> = {}
    for (const peer of Object.keys(raw)) {
      const c = raw[peer] ?? {}
      out[peer] = {
        peer,
        displayName: str(c.display_name),
        nick: str(c.nick),
        trust: num(c.trust),
        lastSeen: num(c.last_seen),
      }
    }
    return out
  })

  const announces = computed<Announce[]>(() => {
    const raw = device.get('lxmf.announces') ?? {}
    return Object.keys(raw).map((hash) => {
      // "<last_s>|<cost>|<hops>|<ratchet>|<name>"
      const [last, cost, hops, ratchet, ...rest] = str(raw[hash]).split('|')
      return {
        hash,
        name: rest.join('|'),
        lastSeen: num(last),
        cost: num(cost),
        hops: num(hops, HOPS_UNKNOWN),
        ratchet: str(ratchet),
      }
    }).sort((a, b) => b.lastSeen - a.lastSeen)
  })

  /** Memoized snapshot-pure resolver (§3.1, §9): recomputed only when
   *  contacts/announces change, never per row — no rail thrash. */
  const nameMap = computed<Map<string, string>>(() => {
    const m = new Map<string, string>()
    for (const a of announces.value) if (a.name) m.set(a.hash, a.name)
    for (const c of Object.values(contacts.value))
      if (c.displayName || c.nick) m.set(c.peer, c.displayName || c.nick)
    return m
  })
  const displayName = (peer: string) =>
    nameMap.value.get(peer) ?? shortHash(peer)

  /* Signal model: a peer we've exchanged messages with (any stored
   * in/out message) automatically becomes a contact, and the contact
   * carries the durable name in synced storage — so it survives announce
   * churn and is coherent across frontends. We own
   * contacts.<peer>.{hash,display_name} (firmware owns trust/last_seen,
   * disjoint). Non-conversation peers are intentionally NOT persisted —
   * their name may age out with the announce, which is acceptable. */
  const convPeers = () =>
    Object.keys(device.get(`s.lxmf.id.${activeId.value}.msgs`) ?? {})
  watch(
    () => convPeers().map(p =>
      `${p}:${nameMap.value.get(p) ?? ''}:${contacts.value[p]?.displayName ?? ''}`
    ).join('|'),
    () => {
      const n = activeId.value
      const patch: Patch = {}
      let any = false
      for (const peer of convPeers()) {
        const c = contacts.value[peer]
        const name = nameMap.value.get(peer)
        if (!c) {                                   // exchange ⇒ contact
          deepAssign(patch, nest(`s.lxmf.id.${n}.contacts.${peer}.hash`, peer))
          any = true
        }
        if (name && !(c && c.displayName)) {        // promote resolved name
          deepAssign(patch,
            nest(`s.lxmf.id.${n}.contacts.${peer}.display_name`, name))
          any = true
        }
      }
      if (any) device.sendJson(patch)
    },
    { immediate: true },
  )

  const reachMap = computed<Map<string, Reachability>>(() => {
    const m = new Map<string, Reachability>()
    for (const a of announces.value) m.set(a.hash, { lastSeenS: a.lastSeen, hops: a.hops })
    return m
  })
  const reachability = (peer: string) => reachMap.value.get(peer) ?? null

  function readMsg(peer: string, key: string, r: any): Message {
    return {
      key, peer,
      dir: str(r.dir) === 'in' ? 'in' : 'out',
      stage: (str(r.stage) || 'queued') as Stage,
      title: str(r.title),
      content: str(r.content),
      thread: str(r.thread),
      ts: num(r.ts),
      read: num(r.read) === 1,
      attempts: num(r.attempts),
      lastError: str(r.last_error),
      messageId: str(r.message_id),
    }
  }

  const conversations = computed<Conversation[]>(() => {
    const msgs = device.get(`s.lxmf.id.${activeId.value}.msgs`) ?? {}
    const out: Conversation[] = []
    for (const peer of Object.keys(msgs)) {
      const thread = msgs[peer] ?? {}
      let last: Message | null = null
      let unread = 0, count = 0
      for (const key of Object.keys(thread)) {
        const m = readMsg(peer, key, thread[key] ?? {})
        if (m.stage === 'draft') continue // never rendered (§4/§6)
        count++
        if (m.dir === 'in' && !m.read) unread++
        if (!last || m.ts >= last.ts) last = m
      }
      if (count === 0) continue
      out.push({ peer, name: displayName(peer), last, ts: last?.ts ?? 0, unread, count })
    }
    return out.sort((a, b) => b.ts - a.ts)
  })

  const activeConversation = computed<{ day: string; messages: Message[] }[]>(() => {
    const peer = _activePeer.value
    if (!peer) return []
    const thread = device.get(`s.lxmf.id.${activeId.value}.msgs.${peer}`) ?? {}
    const msgs = Object.keys(thread)
      .map(key => readMsg(peer, key, thread[key] ?? {}))
      .filter(m => m.stage !== 'draft')
      .sort((a, b) => a.ts - b.ts)
    const buckets: { day: string; messages: Message[] }[] = []
    for (const m of msgs) {
      const day = new Date(m.ts * 1000).toLocaleDateString(undefined,
        { weekday: 'long', month: 'short', day: 'numeric' })
      const tail = buckets[buckets.length - 1]
      if (tail && tail.day === day) tail.messages.push(m)
      else buckets.push({ day, messages: [m] })
    }
    return buckets
  })

  const peerDirectory = computed(() => {
    const seen = new Set<string>()
    const rows: { peer: string; name: string; known: boolean }[] = []
    for (const c of Object.values(contacts.value)) {
      seen.add(c.peer)
      rows.push({ peer: c.peer, name: displayName(c.peer), known: true })
    }
    for (const a of announces.value) {
      if (seen.has(a.hash)) continue
      seen.add(a.hash)
      rows.push({ peer: a.hash, name: displayName(a.hash), known: false })
    }
    return rows
  })

  const unreadTotal = computed(() =>
    conversations.value.reduce((s, c) => s + c.unread, 0))

  /* ── Action verbs (the only writers) ──────────────────────────────── */

  function sendQ(kind: 'send' | 'cancel' | 'delete' | 'announce') {
    return queue(`lxmf.id.${activeId.value}.cmd.${kind}`)
  }

  async function send(peer: string, content: string,
                      opts?: { method?: string; thread?: string }) {
    const n = activeId.value
    // Fail fast on a down/keyless identity rather than writing a
    // sentinel nothing will ever process (6 s CmdQueue timeout).
    if (!activeIdentityUp.value)
      throw new Error('identity not connected — cannot send')
    const key = `o_${Date.now()}_${rand4()}`
    const rec: Patch = {
      dir: 'out', peer, title: '', content,
      thread: opts?.thread ?? '', stage: 'draft',
      ts: Math.floor(Date.now() / 1000),
    }
    if (opts?.method) rec.method = opts.method
    const data = nest(`s.lxmf.id.${n}.msgs.${peer}.${key}`, rec)
    delete _composerDrafts[peer]
    const stageOf = () =>
      str(device.get(`s.lxmf.id.${n}.msgs.${peer}.${key}.stage`))
    // Done once the firmware moves the record off our optimistic draft.
    await sendQ('send').enqueue(`${peer}/${key}`,
      { data, settle: () => { const s = stageOf(); return !!s && s !== 'draft' } })
  }

  const resend = (peer: string, key: string) => {
    const n = activeId.value
    const stageOf = () =>
      str(device.get(`s.lxmf.id.${n}.msgs.${peer}.${key}.stage`))
    return sendQ('send').enqueue(`${peer}/${key}`,
      { settle: () => { const s = stageOf(); return !!s && s !== 'failed' } })
  }
  const cancel = (peer: string, key: string) => {
    const n = activeId.value
    return sendQ('cancel').enqueue(`${peer}/${key}`, {
      settle: () => ['cancelled', 'failed', 'sent', 'delivered'].includes(
        str(device.get(`s.lxmf.id.${n}.msgs.${peer}.${key}.stage`))),
    })
  }
  const deleteMessage = (peer: string, key: string) => {
    const n = activeId.value
    return sendQ('delete').enqueue(`${peer}/${key}`, {
      settle: () =>
        device.get(`s.lxmf.id.${n}.msgs.${peer}.${key}`) === undefined,
    })
  }
  const deleteConversation = (peer: string) => {
    const n = activeId.value
    return sendQ('delete').enqueue(peer, {
      settle: () => device.get(`s.lxmf.id.${n}.msgs.${peer}`) === undefined,
    })
  }
  const announceNow = () => sendQ('announce').enqueue('1')

  /** Local-only, no sentinel: one patch flipping every inbound-unread
   *  key in the thread (§3.2 — never a set() per message). */
  function markConversationRead(peer: string) {
    const n = activeId.value
    const thread = device.get(`s.lxmf.id.${n}.msgs.${peer}`) ?? {}
    const patch: Patch = {}
    let any = false
    for (const key of Object.keys(thread)) {
      const r = thread[key] ?? {}
      if (str(r.dir) === 'in' && num(r.read) !== 1) {
        deepAssign(patch, nest(`s.lxmf.id.${n}.msgs.${peer}.${key}.read`, 1))
        any = true
      }
    }
    if (any) device.sendJson(patch)
  }

  const createIdentity = (label: string) =>
    queue('lxmf.cmd.identity_new').enqueue(label || 'main')
  const importIdentity = (privHex: string) =>
    queue('lxmf.cmd.identity_import').enqueue(privHex.trim())
  const destroyIdentity = (n: number) =>
    queue('lxmf.cmd.identity_destroy').enqueue(String(n))
  /* Plain config write — no sentinel. The firmware reads
   * s.lxmf.id.<n>.enabled live (idEnabled) on every announce / send /
   * inbound, so the flip takes effect without a cmd round-trip. */
  const setEnabled = (n: number, on: boolean) =>
    device.set(`s.lxmf.id.${n}.enabled`, on ? 1 : 0)

  function openPeer(peer: string) { _activePeer.value = peer }
  const draftFor = (peer: string) => _composerDrafts[peer] ?? ''
  const setDraft = (peer: string, text: string) => {
    if (text) _composerDrafts[peer] = text
    else delete _composerDrafts[peer]
  }
  const contactOf = (peer: string) => contacts.value[peer] ?? null

  return {
    activeIdentity: _activeIdentity,
    activePeer: _activePeer,
    identities, usableIdentities, activeIdentityUp,
    conversations, activeConversation, contacts, announces,
    peerDirectory, unreadTotal,
    displayName, reachability, contactOf, draftFor, setDraft, openPeer,
    send, resend, cancel, deleteMessage, deleteConversation,
    markConversationRead, announceNow,
    createIdentity, importIdentity, destroyIdentity, setEnabled,
  }
}

/* ── Menu registration (pattern from modules/rnsd.ts) ───────────────── */

export function registerLxmf() {
  const menu = useMenuStore()

  menu.register('status', 'Status', 20, [
    { id: 'status.messages', label: 'LXMF', type: 'action', order: 5,
      action: () => { messagesVisible.value = !messagesVisible.value } },
    { id: 'status.announces', label: 'Announces', type: 'action', order: 6,
      action: () => { announcesVisible.value = !announcesVisible.value } },
  ])

  menu.register('settings', 'Settings', 10, [
    { id: 'reticulum', label: 'Reticulum', type: 'submenu', order: 30,
      children: [
        { id: 'reticulum.lxmf', label: 'LXMF', type: 'panel', order: 20,
          component: LxmfPanel },
      ],
    },
  ])
}
