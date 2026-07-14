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
import { ref, reactive, computed, watch,
         type ComputedRef, type Ref, type WritableComputedRef } from 'vue'
import { useDeviceStore } from 'spangap-browser/stores/device'
import { useMenuStore } from 'spangap-browser/stores/menu'
import LxmfPanel from '../panels/LxmfPanel.vue'
import { registerApp } from 'spangap-browser/lib/apps'
import { registerWindowMount } from 'spangap-browser/lib/windowMounts'
/* Import cycle with the wrapper panel (it reads the per-identity records and
 * useLxmf from here) — benign: both sides only touch the other's bindings
 * from inside functions, never at module-eval time. */
import MessagesWindows from '../panels/MessagesWindows.vue'

/* ── Composition-layer wiring (pattern mirrors modules/rnsd.ts) ──────── */

/** Each usable identity gets its own Messages window (no in-window identity
 *  picker). These maps are keyed by identity slot `n`; the MessagesWindows
 *  wrapper (registered as a window mount below) renders one window per usable
 *  identity (or a single FALLBACK_ID window when there are none, so the
 *  "create an identity" guidance is still reachable). */
export const FALLBACK_ID = -1
export const messagesVisibleById = reactive<Record<number, boolean>>({})
export const messagesFocusById = reactive<Record<number, number>>({})

/* Menu action for an identity's window: only ever show + raise, never hide. */
export function showMessages(n: number = FALLBACK_ID) {
  messagesVisibleById[n] = true
  messagesFocusById[n] = (messagesFocusById[n] ?? 0) + 1
}

/* ── Client-local view cursor (§3.3) ────────────────────────────────────
 * activeIdentity is NOT s.lxmf.cli.selected_id — the CLI and a browser
 * tab legitimately view different identities. Persisted only to
 * localStorage, never to the device. activePeer is the shared selection
 * the composition layer turns into a pane or a screen (§2). */
const LS_IDENT = 'lxmf.activeIdentity'
const _activeIdentity = ref<number>(
  Number(localStorage.getItem(LS_IDENT) ?? -1),
)
watch(_activeIdentity, (n) => { localStorage.setItem(LS_IDENT, String(n)) })

/* Selection + composer drafts are now scoped per identity slot `n`, since
 * each identity has its own window viewing its own conversations. The active
 * peer naturally resets when you look at a different identity (a fresh slot).
 *
 * Composer drafts: in-memory, per-peer, NEVER persisted (§4). Switching
 * threads and back preserves typed text without ever writing a stage=draft
 * record. Reactive so the bound `draft` computed re-tracks keystrokes. */
const _activePeerById = reactive<Record<number, string>>({})
const _draftsById = reactive<Record<number, Record<string, string>>>({})

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

/* ── Nomad page links in message text (mirror of nomad's lxmf@<hash>) ─────
 * A message may quote a Nomad page URL "<32hex>:/path" (e.g.
 * a8d2…338:/page/index.mu). We render those as tappable links; a tap writes
 * the shared `nomad.url_web` sentinel (with a nonce so a repeat tap re-fires)
 * and the nomad module brings its browser window forward on that page. This
 * is the exact reverse of nomad's openLxmf → lxmf.url_web; decoupled — we only
 * write the var, nomad reacts. */
const NOMAD_HASH_RE = /^[0-9a-f]{32}$/

export interface MsgSegment { text: string; link: { hash: string; path: string } | null }

/** Split message content into plain-text runs and Nomad-link runs. A link is a
 *  32-hex node hash + ":" + a "/"-rooted page path. Trailing sentence
 *  punctuation is kept outside the link. */
export function segmentMessage(content: string): MsgSegment[] {
  const re = /([0-9a-fA-F]{32}):(\/\S+)/g
  const out: MsgSegment[] = []
  let last = 0
  let m: RegExpExecArray | null
  while ((m = re.exec(content))) {
    // Don't match inside a longer hex run (e.g. a 64-hex id).
    if (m.index > 0 && /[0-9a-fA-F]/.test(content[m.index - 1]!)) continue
    const path = m[2]!.replace(/[.,;:!?)\]}'"]+$/, '')
    if (!path) continue
    const start = m.index
    const end = start + 33 + path.length          // 32 hex + ':' + path
    if (start > last) out.push({ text: content.slice(last, start), link: null })
    out.push({ text: content.slice(start, end), link: { hash: m[1]!.toLowerCase(), path } })
    last = end
    re.lastIndex = end
  }
  if (last < content.length) out.push({ text: content.slice(last), link: null })
  return out
}

/** Open a Nomad page URL tapped in a message (writes the shared sentinel). */
export function openNomad(hash: string, path: string) {
  const h = hash.trim().toLowerCase()
  if (!NOMAD_HASH_RE.test(h)) return
  const p = (path || '/page/index.mu').trim()
  useDeviceStore().sendJson(nest('nomad.url_web', `${h}:${p}|${Date.now()}`))
}

/* ── The composable ─────────────────────────────────────────────────── */

export interface UseLxmf {
  activeIdentity: typeof _activeIdentity
  activePeer: WritableComputedRef<string>
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

/** `identity` pins this instance to one slot (a Messages window passes its
 *  own slot). Omitted ⇒ the shared client-local cursor `_activeIdentity`
 *  (used by the settings panel and announces view). */
export function useLxmf(identity?: number | Ref<number>): UseLxmf {
  const device = useDeviceStore()
  const pinned = identity !== undefined

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

  // For the shared cursor only: default activeIdentity to the lowest *usable*
  // slot; fall back to the lowest existing slot so history stays viewable.
  // A pinned instance keeps its fixed slot and skips this.
  if (!pinned) {
    watch([identities, usableIdentities], ([ids, usable]) => {
      if (usable.length && !usable.some(i => i.n === _activeIdentity.value))
        _activeIdentity.value = usable[0]!.n
      else if (!usable.length && ids.length &&
               !ids.some(i => i.n === _activeIdentity.value))
        _activeIdentity.value = ids[0]!.n
    }, { immediate: true })
  }

  const activeId = computed(() =>
    pinned ? (typeof identity === 'number' ? identity : identity!.value)
           : _activeIdentity.value)
  const activeIdentityUp = computed(() =>
    usableIdentities.value.some(i => i.n === activeId.value))

  /* Selection for this instance's identity slot (writable). */
  const activePeer: WritableComputedRef<string> = computed({
    get: () => _activePeerById[activeId.value] ?? '',
    set: (v: string) => { _activePeerById[activeId.value] = v },
  })

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
    /* Announce stamps are device monotonic seconds-since-boot (an offline
     * device has no wall clock). sys.boot_time is the wall-clock instant the
     * device booted, so boot_time + mono = real Unix time we can age against
     * Date.now(). 0 (unknown) until the device clock is valid; sort on the raw
     * monotonic value so newest-first order holds even in that window. */
    const boot = num(device.get('sys.boot_time'))
    return Object.keys(raw).map((hash) => {
      // "<last_s>|<cost>|<hops>|<ratchet>|<name>"
      const [last, cost, hops, ratchet, ...rest] = str(raw[hash]).split('|')
      return {
        hash,
        mono: num(last),
        cost: num(cost),
        hops: num(hops, HOPS_UNKNOWN),
        ratchet: str(ratchet),
        name: rest.join('|'),
      }
    }).sort((a, b) => b.mono - a.mono).map((a): Announce => ({
      hash: a.hash,
      name: a.name,
      lastSeen: boot > 0 && a.mono > 0 ? boot + a.mono : 0,
      cost: a.cost,
      hops: a.hops,
      ratchet: a.ratchet,
    }))
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
      // Per-conversation read watermark (ts up to which read); unread = later inbound.
      const readTs = num(device.get(`s.lxmf.id.${activeId.value}.contacts.${peer}.read_ts`) ?? 0)
      let last: Message | null = null
      let unread = 0, count = 0
      for (const key of Object.keys(thread)) {
        const m = readMsg(peer, key, thread[key] ?? {})
        if (m.stage === 'draft') continue // never rendered (§4/§6)
        count++
        if (m.dir === 'in' && m.ts > readTs) unread++
        if (!last || m.ts >= last.ts) last = m
      }
      if (count === 0) continue
      out.push({ peer, name: displayName(peer), last, ts: last?.ts ?? 0, unread, count })
    }
    return out.sort((a, b) => b.ts - a.ts)
  })

  const activeConversation = computed<{ day: string; messages: Message[] }[]>(() => {
    const peer = activePeer.value
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
      thread: opts?.thread ?? '', stage: 'queued',
      ts: Math.floor(Date.now() / 1000),
    }
    if (opts?.method) rec.method = opts.method
    const data = nest(`s.lxmf.id.${n}.msgs.${peer}.${key}`, rec)
    if (_draftsById[n]) delete _draftsById[n]![peer]
    // The record is written optimistically as `queued` so the bubble
    // appears the instant we send (MessageBubble already renders queued
    // as the in-flight "…" chip). Settling on the stage no longer works —
    // we just wrote a non-draft stage ourselves — so key completion off a
    // firmware-owned field instead: `message_id` (batched with the real
    // stage in fw), or a `failed` stage on the early-return error paths.
    // This keeps the queue pacing honest so a rapid second send can't
    // clobber the single cmd.send sentinel before firmware reads it.
    const recPath = `s.lxmf.id.${n}.msgs.${peer}.${key}`
    const processed = () =>
      !!str(device.get(`${recPath}.message_id`)) ||
      str(device.get(`${recPath}.stage`)) === 'failed'
    await sendQ('send').enqueue(`${peer}/${key}`, { data, settle: processed })
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

  /** One per-conversation watermark write, never a set() per message: record
   *  the newest message ts as "read up to here". Unread is derived from it. */
  function markConversationRead(peer: string) {
    const n = activeId.value
    const thread = device.get(`s.lxmf.id.${n}.msgs.${peer}`) ?? {}
    let newest = 0
    for (const key of Object.keys(thread)) {
      const r = thread[key] ?? {}
      if (num(r.ts) > newest) newest = num(r.ts)
    }
    if (newest <= 0) return
    const cur = num(device.get(`s.lxmf.id.${n}.contacts.${peer}.read_ts`) ?? 0)
    if (newest > cur) device.sendJson(nest(`s.lxmf.id.${n}.contacts.${peer}.read_ts`, newest))
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

  function openPeer(peer: string) { activePeer.value = peer }
  const draftFor = (peer: string) => _draftsById[activeId.value]?.[peer] ?? ''
  const setDraft = (peer: string, text: string) => {
    let d = _draftsById[activeId.value]
    if (!d) { d = {}; _draftsById[activeId.value] = d }
    if (text) d[peer] = text
    else delete d[peer]
  }
  const contactOf = (peer: string) => contacts.value[peer] ?? null

  return {
    activeIdentity: _activeIdentity,
    activePeer,
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
  const lx = useLxmf()

  /* A contact tapped in the nomad web browser arrives as `lxmf.url_web`
   * (written by the nomad module). Bring the right identity's Messages window
   * forward and open the conversation. Decoupled from nomad — we only read the
   * shared var; the firmware lxmf task issues the path request off the same key.
   * Clear it after so a repeat tap on the same contact re-fires. */
  const device = useDeviceStore()
  watch(() => str(device.get('lxmf.url_web')), (raw) => {
    /* "<hash>[:<nonce>]" — the nonce makes every tap a fresh value, so the
     * key is never consumed (an unset raced the sync and ate the hash). */
    const peer = raw.trim().toLowerCase().split(':')[0] ?? ''
    if (!/^[0-9a-f]{32}$/.test(peer)) return
    const usable = lx.usableIdentities.value
    const n = usable.find(i => i.n === lx.activeIdentity.value)?.n
      ?? usable[0]?.n ?? FALLBACK_ID
    lx.activeIdentity.value = n
    lx.openPeer(peer)
    showMessages(n)
  })

  /* Top-level "LXMF Messages" menu. With ≤1 usable identity it's a single
   * action (opens that identity's window). With >1, each identity becomes
   * its own item opening its own window — so the set of items is rebuilt
   * (unregister/register) whenever the usable-identity list changes. */
  let regIds: number[] = []
  let regSingle = false
  watch(lx.usableIdentities, (usable) => {
    for (const n of regIds) menu.unregister(`lxmf/id-${n}`)
    regIds = []
    if (regSingle) { menu.unregister('lxmf/messages'); regSingle = false }

    menu.setMenu('lxmf', { label: 'LXMF Messages', placement: 2 })
    if (usable.length > 1) {
      for (const id of usable) {
        menu.register(`lxmf/id-${id.n}`, id.displayName,
          { type: 'action', action: () => showMessages(id.n) })
        regIds.push(id.n)
      }
    } else {
      const n = usable[0]?.n ?? FALLBACK_ID
      menu.register('lxmf/messages', 'LXMF Messages',
        { type: 'action', action: () => showMessages(n) })
      regSingle = true
    }
  }, { immediate: true })

  /* Settings → Mesh Network → LXMF Messages (the LXMF settings panel). */
  menu.register('settings/mesh/lxmf', 'LXMF Messages', { type: 'panel', component: LxmfPanel }, { placement: 3 })

  /* Dock app: the messenger. (Announces has no separate app — the announce
   * stream lives inside the LXMF window.) */
  registerApp({ id: 'lxmf', label: 'LXMF', icon: 'lxmf', placement: 5,
                open: () => {
                  /* Open the active identity's window (or the first usable one);
                   * MessagesWindows only mounts a window per usable identity,
                   * so defaulting to FALLBACK_ID would target an unmounted
                   * window when identities exist. showMessages bumps the focus
                   * token, which raises an already-open window to the front. */
                  const usable = lx.usableIdentities.value
                  const active = lx.activeIdentity.value
                  const n = usable.length
                    ? (usable.some(i => i.n === active) ? active : usable[0]!.n)
                    : FALLBACK_ID
                  showMessages(n)
                },
                isOpen: () => Object.values(messagesVisibleById).some(Boolean) })

  /* The windows themselves: a bare mount — MessagesWindows owns its own v-for
   * over usable identities and the per-identity visible/focus records. */
  registerWindowMount({ id: 'lxmf-messages', component: MessagesWindows })
}
