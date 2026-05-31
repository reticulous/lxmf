/**
 * lxmf_lcd.cpp — on-device "LXMessenger" launcher program (LVGL).
 *
 * Models the web UI LXMF messenger, split into two screens within the one
 * program layer (mirroring the Settings program's page model):
 *   - Contacts: a list of conversations (peers with messages), one compact line
 *     each — name + last-message preview. Tapping one opens its thread.
 *   - Thread:   a back chevron + peer name + a scroll-to-bottom chevron (top
 *     bar), the message bubbles (in left / out right), and a compose row
 *     (textarea + Send) that sits at the *end of the chat* and scrolls with it
 *     rather than being pinned to the bottom. The thread runs fullscreen (no
 *     system status bar) for maximum height.
 *
 * Layout is tuned for density: 1px vertical padding throughout, a smaller font,
 * single-line conversation summaries. Message/preview/name text is run through
 * printable() — control bytes and any codepoint the font can't draw are dropped,
 * so unrenderable unicode never leaves placeholder boxes (printable codepoints
 * the font does carry, e.g. accented Latin, pass through unchanged).
 *
 * Storage is the API (same keys the browser uses):
 *   s.lxmf.id.<n>.msgs.<peer>.<key>.{dir,content,ts,read,stage}   messages
 *   s.lxmf.id.<n>.contacts.<peer>.display_name                    names
 *   lxmf.id.<n>.up                                                identity live
 *   lxmf.id.<n>.cmd.send = "<peer>/<key>"                         send sentinel
 * Everything runs on the lcd task; storage subscriptions are dispatched there,
 * so we touch LVGL straight from the change callback.
 */
#include "sdkconfig.h"

#if CONFIG_SPANGAP_LCD

#include "lcd.h"
#include "storage.h"
#include "compat.h"

#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>

namespace {

/* ---- look & feel (dense) ---- */

/* Compact + accented: smaller than the 14 default, and (unlike LVGL's stock
 * montserrat) carries umlauts/accents so message text isn't reduced to boxes. */
const lv_font_t* const kFont = &lv_font_montserrat_12_latin;

const int HDR_H = 20;   /* thread top bar height */

/* Keep only what the UI font can actually draw: strip C0/C1 control bytes and any
 * codepoint with no glyph (emoji, CJK, …). Valid UTF-8 multibyte sequences whose
 * glyph the font carries (accented Latin, if present) pass through unchanged.
 * `oneLine` folds CR/LF/TAB to a single space (previews); else newlines stay. */
std::string printable(std::string_view in, bool oneLine) {
    std::string out;
    out.reserve(in.size());
    size_t i = 0, n = in.size();
    while (i < n) {
        uint8_t b = (uint8_t)in[i];
        uint32_t cp; size_t len;
        if      (b < 0x80)          { cp = b;        len = 1; }
        else if ((b & 0xE0) == 0xC0){ cp = b & 0x1F; len = 2; }
        else if ((b & 0xF0) == 0xE0){ cp = b & 0x0F; len = 3; }
        else if ((b & 0xF8) == 0xF0){ cp = b & 0x07; len = 4; }
        else { i++; continue; }                       /* stray continuation / invalid lead */
        if (i + len > n) break;                        /* truncated tail */
        bool ok = true;
        for (size_t k = 1; k < len; k++) {
            uint8_t c = (uint8_t)in[i + k];
            if ((c & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (c & 0x3F);
        }
        if (!ok) { i++; continue; }
        size_t at = i;
        i += len;

        if (cp == '\n' || cp == '\r' || cp == '\t') { out += oneLine ? ' ' : (char)cp; continue; }
        if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F)) continue;   /* C0 / DEL / C1 */
        lv_font_glyph_dsc_t g;
        if (!lv_font_get_glyph_dsc(kFont, &g, cp, 0) || g.is_placeholder) continue;
        out.append(in.data() + at, len);
    }
    return out;
}

lv_obj_t* mkLabel(lv_obj_t* parent, const std::string& txt, lv_color_t color) {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, kFont, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, txt.c_str());
    return l;
}

/* ---- model (lcd-task-only, no locks) ---- */

struct Msg {
    std::string peer, key, content, stage;
    long ts = 0;
    bool in = false, read = false;
};

int               g_id = -1;            /* active identity index, -1 = none */
std::string       g_msgsPrefix;         /* "s.lxmf.id.N.msgs" */
std::vector<Msg>  g_msgs;               /* all non-draft messages for g_id */
std::vector<std::string> g_convPeers;   /* peer per contacts row (click index) */
std::string       g_curPeer;            /* peer of the open thread */
bool              g_subscribed = false;

lv_obj_t* s_layer    = nullptr;
lv_obj_t* s_contacts = nullptr;         /* conversations list screen */
lv_obj_t* s_thread   = nullptr;         /* conversation screen (built once, reused) */
lv_obj_t* s_msgList  = nullptr;         /* scroll container inside s_thread */
lv_obj_t* s_bubbles  = nullptr;         /* bubble column (cleaned+rebuilt) inside s_msgList */
lv_obj_t* s_threadName = nullptr;       /* header peer-name label */
lv_obj_t* s_compose  = nullptr;         /* compose textarea (last child of s_msgList) */

void rebuildContacts();
void rebuildThread();

/* ---- identity ---- */

void ensureId() {
    g_id = -1;
    for (int n = 0; n < 4; n++) {
        char k[40];
        snprintf(k, sizeof k, "lxmf.id.%d.up", n);
        if (storageGetInt(k, 0) != 1) continue;
        snprintf(k, sizeof k, "lxmf.id.%d.dest_hash", n);
        if (storageGetStr(k, "").empty()) continue;   /* usable = up AND has a dest (mirrors web) */
        g_id = n; break;
    }
    g_msgsPrefix = (g_id >= 0) ? ("s.lxmf.id." + std::to_string(g_id) + ".msgs") : "";
}

std::string peerName(const std::string& peer) {
    char k[96];
    snprintf(k, sizeof k, "s.lxmf.id.%d.contacts.%s.display_name", g_id, peer.c_str());
    std::string n = storageGetStr(k, "");
    if (!n.empty()) return printable(n, true);
    return peer.substr(0, 8) + "...";
}

/* ---- message store: rebuilt from storage via a leaf walk ---- */

void msgCb(const char* key, const char* val) {
    /* key = "s.lxmf.id.N.msgs.<peer>.<mkey>.<field>" */
    size_t plen = g_msgsPrefix.size();
    if (strncmp(key, g_msgsPrefix.c_str(), plen) != 0) return;
    const char* rest = key + plen;
    if (*rest == '.') rest++;
    const char* d1 = strchr(rest, '.'); if (!d1) return;
    std::string peer(rest, d1 - rest);
    const char* mk = d1 + 1;
    const char* d2 = strchr(mk, '.'); if (!d2) return;
    std::string mkey(mk, d2 - mk);
    const char* field = d2 + 1;
    if (strchr(field, '.')) return;     /* deeper than a leaf field */

    Msg* m = nullptr;
    for (auto& x : g_msgs) if (x.peer == peer && x.key == mkey) { m = &x; break; }
    if (!m) { g_msgs.emplace_back(); m = &g_msgs.back(); m->peer = peer; m->key = mkey; }

    if      (!strcmp(field, "dir"))     m->in   = (val && !strcmp(val, "in"));
    else if (!strcmp(field, "content")) m->content = val ? val : "";
    else if (!strcmp(field, "ts"))      m->ts   = val ? atol(val) : 0;
    else if (!strcmp(field, "read"))    m->read = (val && atoi(val) != 0);
    else if (!strcmp(field, "stage"))   m->stage = val ? val : "";
}

void refreshMsgs() {
    g_msgs.clear();
    if (g_id < 0) return;
    storageForEach(g_msgsPrefix.c_str(), msgCb);
    g_msgs.erase(std::remove_if(g_msgs.begin(), g_msgs.end(),
                                [](const Msg& m) { return m.stage == "draft"; }),
                 g_msgs.end());
}

/* ---- compose / send ---- */

void sendMessage(const std::string& peer, const std::string& text) {
    if (g_id < 0 || peer.empty() || text.empty()) return;
    static unsigned seq = 0;
    char key[40];
    snprintf(key, sizeof key, "o_%u_%u", (unsigned)millis(), ++seq);
    char base[120];
    snprintf(base, sizeof base, "s.lxmf.id.%d.msgs.%s.%s", g_id, peer.c_str(), key);
    char k[200];
    auto setf = [&](const char* f, const char* v) {
        snprintf(k, sizeof k, "%s.%s", base, f);
        storageSet(k, v);
    };
    setf("dir", "out");
    setf("peer", peer.c_str());
    setf("title", "");
    setf("content", text.c_str());
    setf("thread", "");
    setf("stage", "draft");
    snprintf(k, sizeof k, "%s.ts", base);
    storageSet(k, (int)time(nullptr));
    /* Send sentinel (ephemeral): "<peer>/<key>". The lxmf task drives the stage. */
    char sentinel[48], payload[80];
    snprintf(sentinel, sizeof sentinel, "lxmf.id.%d.cmd.send", g_id);
    snprintf(payload, sizeof payload, "%s/%s", peer.c_str(), key);
    storageSet(sentinel, payload);
}

void onSend(lv_event_t*) {
    if (!s_compose || g_curPeer.empty()) return;
    const char* t = lv_textarea_get_text(s_compose);
    if (t && *t) {
        sendMessage(g_curPeer, t);
        lv_textarea_set_text(s_compose, "");
    }
}

void markRead(const std::string& peer) {
    for (auto& m : g_msgs) {
        if (m.peer == peer && m.in && !m.read) {
            char k[200];
            snprintf(k, sizeof k, "s.lxmf.id.%d.msgs.%s.%s.read", g_id, peer.c_str(), m.key.c_str());
            storageSet(k, 1);
            m.read = true;
        }
    }
}

/* ---- scrolling ---- */

void scrollThreadBottom(bool anim) {
    if (!s_msgList) return;
    lv_obj_update_layout(s_msgList);
    lv_obj_scroll_to_y(s_msgList, LV_COORD_MAX, anim ? LV_ANIM_ON : LV_ANIM_OFF);
}

/* ---- navigation ---- */

void showContacts() {
    lcdProgramFullscreen(false);            /* status bar back for the list screen */
    if (s_thread) lv_obj_add_flag(s_thread, LV_OBJ_FLAG_HIDDEN);
    if (s_contacts) {
        rebuildContacts();
        lv_obj_remove_flag(s_contacts, LV_OBJ_FLAG_HIDDEN);
    }
    g_curPeer.clear();
}

void buildThreadShell() {
    s_thread = lv_obj_create(s_layer);
    lv_obj_remove_style_all(s_thread);
    lv_obj_set_size(s_thread, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_thread, lv_color_hex(0x10141a), 0);
    lv_obj_set_style_bg_opa(s_thread, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(s_thread, LV_FLEX_FLOW_COLUMN);

    /* Header: back chevron + peer name + scroll-to-bottom chevron. */
    lv_obj_t* hdr = lv_obj_create(s_thread);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, lv_pct(100), HDR_H);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(0x222b38), 0);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);

    lv_obj_t* back = mkLabel(hdr, LV_SYMBOL_LEFT, lv_color_white());
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(back, 12);
    lv_obj_add_event_cb(back, [](lv_event_t*) { showContacts(); }, LV_EVENT_CLICKED, nullptr);

    s_threadName = mkLabel(hdr, "", lv_color_white());
    lv_obj_align(s_threadName, LV_ALIGN_LEFT_MID, 28, 0);

    lv_obj_t* down = mkLabel(hdr, LV_SYMBOL_DOWN, lv_color_hex(0xc0c8d0));
    lv_obj_align(down, LV_ALIGN_RIGHT_MID, -6, 0);
    lv_obj_add_flag(down, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(down, 12);
    lv_obj_add_event_cb(down, [](lv_event_t*) { scrollThreadBottom(true); }, LV_EVENT_CLICKED, nullptr);

    /* Scroll area (grows to fill): bubble column + the compose row at its end. */
    s_msgList = lv_obj_create(s_thread);
    lv_obj_remove_style_all(s_msgList);
    lv_obj_set_width(s_msgList, lv_pct(100));
    lv_obj_set_flex_grow(s_msgList, 1);
    lv_obj_set_flex_flow(s_msgList, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(s_msgList, 4, 0);
    lv_obj_set_style_pad_ver(s_msgList, 1, 0);
    lv_obj_set_style_pad_row(s_msgList, 3, 0);   /* gap between last message and the input */

    s_bubbles = lv_obj_create(s_msgList);
    lv_obj_remove_style_all(s_bubbles);
    lv_obj_set_width(s_bubbles, lv_pct(100));
    lv_obj_set_height(s_bubbles, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_bubbles, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_bubbles, 1, 0);
    lv_obj_remove_flag(s_bubbles, LV_OBJ_FLAG_SCROLLABLE);

    /* Compose row: textarea + Send. Last child of the scroll area, so it follows
     * the final message and scrolls off the top rather than sticking to the bottom. */
    lv_obj_t* comp = lv_obj_create(s_msgList);
    lv_obj_remove_style_all(comp);
    lv_obj_set_width(comp, lv_pct(100));
    lv_obj_set_height(comp, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(comp, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(comp, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(comp, 1, 0);
    lv_obj_set_style_pad_column(comp, 4, 0);
    lv_obj_remove_flag(comp, LV_OBJ_FLAG_SCROLLABLE);

    s_compose = lv_textarea_create(comp);
    lv_textarea_set_one_line(s_compose, true);
    lv_textarea_set_placeholder_text(s_compose, "Message");
    lv_obj_set_style_text_font(s_compose, kFont, 0);
    lv_obj_set_style_pad_ver(s_compose, 1, 0);
    lv_obj_set_style_pad_hor(s_compose, 4, 0);
    lv_obj_set_flex_grow(s_compose, 1);
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), s_compose);
    lv_obj_add_event_cb(s_compose, onSend, LV_EVENT_READY, nullptr);                 /* Enter sends */
    lv_obj_add_event_cb(s_compose, [](lv_event_t*) { scrollThreadBottom(false); },   /* typing -> bottom */
                        LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_t* send = lv_button_create(comp);
    lv_obj_set_style_pad_ver(send, 1, 0);
    lv_obj_set_style_pad_hor(send, 6, 0);
    lv_obj_t* sl = lv_label_create(send);
    lv_obj_set_style_text_font(sl, kFont, 0);
    lv_label_set_text(sl, "Send");
    lv_obj_center(sl);
    lv_obj_add_event_cb(send, onSend, LV_EVENT_CLICKED, nullptr);
}

void openThread(const std::string& peer) {
    g_curPeer = peer;
    if (!s_thread) buildThreadShell();
    lv_label_set_text(s_threadName, peerName(peer).c_str());
    if (s_contacts) lv_obj_add_flag(s_contacts, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_thread, LV_OBJ_FLAG_HIDDEN);
    lcdProgramFullscreen(true);             /* immersive chat: no status bar */
    markRead(peer);
    rebuildThread();
}

void onContactClick(lv_event_t* e) {
    size_t idx = (size_t)(intptr_t)lv_event_get_user_data(e);
    if (idx < g_convPeers.size()) openThread(g_convPeers[idx]);
}

/* ---- rendering ---- */

void addBubble(const Msg& m) {
    lv_obj_t* row = lv_obj_create(s_bubbles);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, m.in ? LV_FLEX_ALIGN_START : LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* bub = lv_obj_create(row);
    lv_obj_remove_style_all(bub);
    lv_obj_set_width(bub, LV_SIZE_CONTENT);
    lv_obj_set_height(bub, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(bub, 240, 0);          /* fixed px — a pct of a content-sized chain collapses */
    lv_obj_set_style_bg_color(bub, m.in ? lv_color_hex(0x2a313a) : lv_color_hex(0x2563a0), 0);
    lv_obj_set_style_bg_opa(bub, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bub, 6, 0);
    lv_obj_set_style_pad_ver(bub, 1, 0);
    lv_obj_set_style_pad_hor(bub, 6, 0);
    lv_obj_set_flex_flow(bub, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(bub, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* c = mkLabel(bub, printable(m.content, false), lv_color_white());
    lv_label_set_long_mode(c, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_max_width(c, 228, 0);            /* wrap within the bubble (240 - padding) */

    char tbuf[8] = "";
    if (m.ts > 0) {
        time_t tt = m.ts;
        struct tm tmv {};
        localtime_r(&tt, &tmv);
        strftime(tbuf, sizeof tbuf, "%H:%M", &tmv);
    }
    mkLabel(bub, tbuf, lv_color_hex(0xc0c8d0));
}

void rebuildThread() {
    if (!s_bubbles || g_curPeer.empty()) return;
    lv_obj_clean(s_bubbles);
    std::vector<const Msg*> ms;
    for (auto& m : g_msgs) if (m.peer == g_curPeer) ms.push_back(&m);
    std::sort(ms.begin(), ms.end(), [](const Msg* a, const Msg* b) { return a->ts < b->ts; });
    for (auto* m : ms) addBubble(*m);
    scrollThreadBottom(false);                          /* newest + compose at bottom */
}

struct Conv { std::string peer, preview; long ts = 0; int unread = 0; };

void rebuildContacts() {
    if (!s_contacts) return;
    lv_obj_clean(s_contacts);
    g_convPeers.clear();

    if (g_id < 0) {
        mkLabel(s_contacts, "No active identity.\nCreate one in the web UI.", lv_color_hex(0x8a93a0));
        return;
    }

    std::vector<Conv> convs;
    for (auto& m : g_msgs) {
        Conv* c = nullptr;
        for (auto& x : convs) if (x.peer == m.peer) { c = &x; break; }
        if (!c) { convs.push_back({ m.peer, "", 0, 0 }); c = &convs.back(); }
        if (m.ts >= c->ts) { c->ts = m.ts; c->preview = (m.in ? "" : "You: ") + m.content; }
        if (m.in && !m.read) c->unread++;
    }
    std::sort(convs.begin(), convs.end(), [](const Conv& a, const Conv& b) { return a.ts > b.ts; });

    if (convs.empty()) {
        mkLabel(s_contacts, "No conversations yet.", lv_color_hex(0x8a93a0));
        return;
    }

    for (size_t i = 0; i < convs.size(); i++) {
        const Conv& c = convs[i];
        g_convPeers.push_back(c.peer);

        /* Two stacked lines: sender, then a one-line preview below it. Stacking
         * (not inline) keeps a tall, reliable tap target even when the message is
         * short — an inline preview collapses the row to a single thin line. */
        lv_obj_t* row = lv_button_create(s_contacts);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x20262e), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 4, 0);
        lv_obj_set_style_pad_ver(row, 1, 0);
        lv_obj_set_style_pad_hor(row, 6, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(row, 1, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(row, onContactClick, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), row);

        std::string title = peerName(c.peer);
        if (c.unread > 0) title += " (" + std::to_string(c.unread) + ")";
        mkLabel(row, title, c.unread > 0 ? lv_color_hex(0x6cc06c) : lv_color_white());

        /* LONG_DOT ellipsizes only within the label's own size, so bound both:
         * full row width + exactly one line tall → a long preview is clipped to a
         * single "…"-terminated line instead of wrapping. */
        lv_obj_t* pv = mkLabel(row, printable(c.preview, true), lv_color_hex(0x8a93a0));
        lv_label_set_long_mode(pv, LV_LABEL_LONG_DOT);
        lv_obj_set_width(pv, lv_pct(100));
        lv_obj_set_height(pv, lv_font_get_line_height(kFont));
    }
}

/* ---- live updates: storage change -> refresh whatever's shown ---- */

void onStorageChange(const char*, const char*) {
    if (!s_layer) return;
    if (g_id < 0) ensureId();          /* identity may have come up after open */
    refreshMsgs();
    if (s_thread && !lv_obj_has_flag(s_thread, LV_OBJ_FLAG_HIDDEN)) rebuildThread();
    else                                                            rebuildContacts();
}

/* ---- entry point (lcd task, on first open / relaid layer) ---- */

void lxmfApp(void* arg) {
    s_layer  = static_cast<lv_obj_t*>(arg);
    s_thread = nullptr; s_msgList = nullptr; s_bubbles = nullptr; s_compose = nullptr; s_threadName = nullptr;
    g_curPeer.clear();
    ensureId();

    s_contacts = lv_obj_create(s_layer);
    lv_obj_remove_style_all(s_contacts);
    lv_obj_set_size(s_contacts, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_contacts, lv_color_hex(0x10141a), 0);
    lv_obj_set_style_bg_opa(s_contacts, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(s_contacts, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(s_contacts, 4, 0);
    lv_obj_set_style_pad_ver(s_contacts, 1, 0);
    lv_obj_set_style_pad_row(s_contacts, 1, 0);

    refreshMsgs();
    rebuildContacts();

    if (!g_subscribed) {
        storageSubscribeChanges("s.lxmf.id", onStorageChange);   /* msgs + contacts */
        storageSubscribeChanges("lxmf.id",   onStorageChange);   /* identity .up edge */
        g_subscribed = true;
    }
}

/* ---- Settings → Reticulum → LXMF (admin pane, mirrors the web LxmfPanel) ---- */

void lxmfSettingsPane(void* arg) {
    lv_obj_t* p = static_cast<lv_obj_t*>(arg);
    lcdSettingSection(p, "LXMF");
    lcdSettingSlider (p, "Announce (s)", "s.lxmf.announce_interval_s", 0, 21600);
    lcdSettingSlider (p, "Announce cap", "s.lxmf.max_announces", 256, 8192);

    lcdSettingSection(p, "Identities");
    /* Only existing slots; literal keys (the helpers store keys by pointer). */
    if (storageExists("s.lxmf.id.0.label")) {
        lcdSettingValue (p, "0 dest",    "lxmf.id.0.dest_hash");
        lcdSettingSwitch(p, "0 enabled", "s.lxmf.id.0.enabled");
    }
    if (storageExists("s.lxmf.id.1.label")) {
        lcdSettingValue (p, "1 dest",    "lxmf.id.1.dest_hash");
        lcdSettingSwitch(p, "1 enabled", "s.lxmf.id.1.enabled");
    }
    if (storageExists("s.lxmf.id.2.label")) {
        lcdSettingValue (p, "2 dest",    "lxmf.id.2.dest_hash");
        lcdSettingSwitch(p, "2 enabled", "s.lxmf.id.2.enabled");
    }
    if (storageExists("s.lxmf.id.3.label")) {
        lcdSettingValue (p, "3 dest",    "lxmf.id.3.dest_hash");
        lcdSettingSwitch(p, "3 enabled", "s.lxmf.id.3.enabled");
    }

    lcdSettingSection(p, "Add identity");
    /* Type a name + Enter -> writes the ephemeral create sentinel. */
    lcdSettingText   (p, "New (name)", "lxmf.cmd.identity_new");
}

}  // namespace

/* Register the LXMessenger launcher program. Call once at startup (lcd build).
 * extern "C": main.cpp's app_main is extern "C", so its forward decl resolves to
 * a C symbol — match it here so the linker finds the definition. */
extern "C" void lxmfLcdRegister(void) {
    lcdRegister("LXMF", "rns", lxmfApp);
    lcdRegisterSettings("Reticulum/LXMF", "LXMF", lxmfSettingsPane);
}

#endif /* CONFIG_SPANGAP_LCD */
