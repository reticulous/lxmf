/**
 * lxmf_tiny.h — total-unread LXMF page on the tiny OLED.
 *
 * The when:-gated tinylcd slice of lxmf (conditional/tinylcd/): only compiled
 * when spangap/tinylcd is staged. One page, one number: unread messages
 * summed across every identity's conversations. See lxmf_tiny.cpp.
 */
#pragma once

#include "service.h"

class LxmfTinyPage : public Service {
public:
    void onInit() override;
};
