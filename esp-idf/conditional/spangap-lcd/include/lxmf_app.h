/**
 * lxmf_app.h — the LXMessenger launcher program as a boot-registered Service.
 *
 * LxmfApp is an LcdApp (hence a Service): the straddle's `services:` entry
 * points the generated boot code at this header, which constructs an LxmfApp and
 * registers it. LcdApp::onInit installs its launcher tile; appInit() does the
 * boot-task wiring (Settings pane, the lxmf.url_lcd open-URL subscription).
 *
 * The class is declared here (global, no namespace — the codebase disambiguates
 * by the lxmf* symbol prefix) so the trampoline TU can `new LxmfApp()`; the
 * methods are defined out-of-line in lxmf_lcd.cpp, where the file-static UI state
 * lives. Compiled only under conditional/spangap-lcd/, so it exists only when the
 * lcd straddle is staged — matching the entry's `when: spangap/spangap-lcd` gate.
 */
#pragma once

#include "lcd_app.h"   /* LcdApp (a Service) */
#include "lvgl.h"      /* lv_obj_t */

/** The LXMessenger. onCreate builds the three-screen program; cleanup on
 *  eviction is handled by the layer's own onLayerDelete (it nulls every handle
 *  so a late storage change early-returns), so no onClose is needed. appInit()
 *  (boot task) registers the Settings pane and the lxmf.url_lcd subscription. */
class LxmfApp : public LcdApp {
public:
    LxmfApp();
    void onCreate(lv_obj_t* root) override;

protected:
    void appInit() override;
};
