// SPDX-License-Identifier: Apache-2.0
static void touch_context_checks(void)
{
    app_view_t original = s_state;
    s_state.mode = INPUT_MOUSE;
    s_state.overlay = APP_OVERLAY_NONE;
    s_state.locked = s_state.screen_off = s_state.remote_owned = false;
    ++s_state.ui_revision;
    render();
    app_ui_touch_context_t context = app_ui_touch_context();
    assert(context.eligible_pad && context.epoch == s_state.epoch && context.ui_revision == s_state.ui_revision);
    assert(context.x1 == 12 && context.y1 == 64 && context.x2 == 407 && context.y2 == 355);
    ui_memory_stats_t before = ui_memory_stats();
    uint64_t flushes = s_flush_count;
    clear_intents();
    for (unsigned i = 0; i < 1000; ++i) {
        app_ui_touch_context_t next = app_ui_touch_context();
        assert(next.eligible_pad && next.epoch == context.epoch && next.ui_revision == context.ui_revision);
        assert(next.x1 == context.x1 && next.y1 == context.y1 && next.x2 == context.x2 && next.y2 == context.y2);
    }
    ui_memory_stats_t after = ui_memory_stats();
    assert(after.allocations == before.allocations && after.reallocations == before.reallocations && after.frees == before.frees);
    assert(s_flush_count == flushes && s_intent_count == 0);
    s_state.mode = INPUT_KEYBOARD; render(); assert(!app_ui_touch_context().eligible_pad);
    s_state.mode = INPUT_AUDIO; render(); assert(!app_ui_touch_context().eligible_pad);
    s_state.mode = INPUT_MOUSE;
    s_state.remote_owned = true; render(); assert(!app_ui_touch_context().eligible_pad);
    s_state.remote_owned = false;
    s_state.locked = true; render(); assert(!app_ui_touch_context().eligible_pad);
    s_state.locked = false;
    s_state.screen_off = true; render(); assert(!app_ui_touch_context().eligible_pad);
    s_state.screen_off = false;
    s_state.overlay = APP_OVERLAY_SETTINGS; s_state.settings_page = APP_SETTINGS_INPUT;
    ++s_state.ui_revision; render(); assert(!app_ui_touch_context().eligible_pad);
    pointer(120, 342, true);
    s_state.overlay = APP_OVERLAY_NONE; ++s_state.ui_revision;
    render();
    assert(!app_ui_touch_context().eligible_pad); /* Old panel still covers the pad. */
    pointer(120, 342, false); render();
    assert(app_ui_touch_context().eligible_pad);
    assert(app_ui_touch_context().ui_revision == s_state.ui_revision);
    /* A live private editor remains private even over Mouse. Exercise real
     * pointer typing and the deferred close while its last key is held. */
    open_editor(APP_LOCAL_EDIT_WIFI_PASSPHRASE, NULL);
    s_state.mode = INPUT_MOUSE; ++s_state.ui_revision; render();
    assert(!app_ui_touch_context().eligible_pad);
    editor_type_text("test-only-secret");
    assert(ui_private_password_contains("test-only-secret"));
    assert(!app_ui_touch_context().eligible_pad);
    pointer(120, 342, true);
    s_state.overlay = APP_OVERLAY_NONE; ++s_state.ui_revision; render();
    assert(!app_ui_touch_context().eligible_pad);
    pointer(120, 342, false); render();
    assert(app_ui_touch_context().eligible_pad && ui_private_buffers_cleared());
    finish_editor();
    s_state = original; render(); clear_intents();
    puts("PASS: read-only visible pad context, exact bounds, stale covering panels and authority-free allocation/flush-free queries");
}
