// SPDX-License-Identifier: Apache-2.0
#include "ui_trust_review_fixture.h"
static void apply_trust_intents(void)
{
    for (size_t i = 0; i < s_intent_count; ++i)
        if (s_intents[i].action == APP_API_COMMISSIONING) ui_trust_fixture_handle(&s_state, &s_intents[i]);
    clear_intents(); render();
}
static void trust_review_checks(void)
{
    app_view_t original = s_state;
    s_state.locked = s_state.screen_off = s_state.remote_owned = false;
    s_state.capture_indicator_required = s_state.audio_active = s_state.listening = false;
    const char *names[] = {"normal", "maximum", "package", "minimal"};
    for (unsigned kind = 0; kind < 4; ++kind) {
        ui_trust_fixture_open(&s_state, (ui_trust_fixture_kind_t)kind);
        assert(s_state.api.review_complete && s_state.api.review_page_count > 0);
        unsigned pages = s_state.api.review_page_count;
        for (unsigned page = 0; page < pages; ++page) {
            assert(s_state.api.review_page == page);
            char name[64]; snprintf(name, sizeof(name), "trust-authority-%s-%02u", names[kind], page + 1);
            screenshot(name);
            assert(has_visible_label(lv_screen_active(), s_state.api.review_pages[page]));
            clear_intents(); tap_control("Approve identity");
            if (page + 1 < pages) {
                assert(action_count(APP_API_COMMISSIONING) == 0);
                clear_intents(); tap_control("Next");
                assert(last_action(APP_API_COMMISSIONING)->api.page == page + 1);
                apply_trust_intents();
            } else {
                assert(last_action(APP_API_COMMISSIONING)->api.operation == APP_API_REVIEW_APPROVE);
                unsigned approvals = ui_trust_fixture_approvals(); apply_trust_intents();
                assert(ui_trust_fixture_approvals() == approvals + 1);
            }
        }
        ui_trust_fixture_finish(&s_state);
    }
    ui_trust_fixture_open(&s_state, UI_TRUST_NORMAL);
    for (unsigned field = 0; field < 6; ++field) {
        ui_trust_fixture_change(&s_state, field); assert(!s_state.api.review_complete && !s_state.api.can_approve);
        char name[64]; snprintf(name, sizeof(name), "trust-authority-incomplete-%u", field); screenshot(name);
        clear_intents(); tap_control("Approve identity"); assert(action_count(APP_API_COMMISSIONING) == 0);
    }
    ui_trust_fixture_change(&s_state, 6);
    while (s_state.api.review_page + 1 < s_state.api.review_page_count) {
        render(); clear_intents(); tap_control("Next"); apply_trust_intents();
    }
    render(); clear_intents(); pointer(240, 448, true);
    ui_trust_fixture_change(&s_state, 0); render(); pointer(240, 448, false);
    assert(action_count(APP_API_COMMISSIONING) == 0);
    ui_trust_fixture_finish(&s_state);
    s_state = original; render(); clear_intents();
    puts("PASS: production trust Review formatter + actual LVGL pages, complete authority, sequential local approval, replacement and missing fields");
}
