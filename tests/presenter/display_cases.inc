// SPDX-License-Identifier: Apache-2.0
static void display_signal(lv_event_code_t code)
{
    assert(s_test_display.callback);
    lv_event_t event = {.display = &s_test_display, .code = code};
    s_lvgl_owner = true;
    s_test_display.callback(&event);
    s_lvgl_owner = false;
}

static void render_indicator(const app_view_t *view)
{
    s_test_indicator = view->capture_indicator_required && !view->screen_off ?
        (app_ui_capture_indicator_t){.armed = true, .capture_generation = view->capture_indicator_generation,
            .indicator_revision = view->ui_revision, .x1 = 280, .y1 = 8, .x2 = 395, .y2 = 55} :
        (app_ui_capture_indicator_t){0};
}

static app_view_t setup_display_indicator(void)
{
    atomic_store(&s_clock_us, 100000);
    initialize();
    s_board = (board_display_receipt_t){.installed = true, .callback_attached = true, .display_generation = 8};
    s_lvgl_owner = true;
    assert(app_presenter_bind_display(&s_test_display) == ESP_OK);
    s_lvgl_owner = false;
    s_render_hook = render_indicator;
    return (app_view_t){.epoch = 3, .ui_revision = 7, .capture_indicator_required = true,
        .capture_indicator_generation = 23};
}

static void observed_complete(void)
{
    s_board.coverage_complete = s_board.indicator_transfer_confirmed = true;
    s_board.transfers = s_board.completed_transfers = 2;
    s_board.completed_us = (uint64_t)atomic_load(&s_clock_us);
}

static void complete_refresh_without_poll(void)
{
    display_signal(LV_EVENT_REFR_START);
    assert(s_board.required.x0 == 280 && s_board.required.y0 == 8 &&
        s_board.required.x1 == 396 && s_board.required.y1 == 56);
    observed_complete();
    display_signal(LV_EVENT_REFR_READY);
}

static void complete_refresh(void)
{
    complete_refresh_without_poll();
    assert(app_presenter_indicator_status().usable);
}

static void display_first_proof_survives_unpolled_refresh(void)
{
    app_view_t view = setup_display_indicator();
    assert(app_presenter_publish(&view) == APP_PRESENTER_ACCEPTED); tick();
    complete_refresh_without_poll();
    atomic_store(&s_clock_us, 120000);
    display_signal(LV_EVENT_REFR_START); display_signal(LV_EVENT_REFR_READY);
    assert(!s_board.transfers && !s_board.indicator_transfer_confirmed);
    atomic_store(&s_clock_us, 130000);
    display_signal(LV_EVENT_REFR_START); display_signal(LV_EVENT_REFR_READY);
    atomic_store(&s_clock_us, 140000);
    display_signal(LV_EVENT_REFR_START);
    /* One completed draw outside the marker cannot create a new proof. */
    s_board.transfers = s_board.completed_transfers = 1;
    s_board.completed_us = 140000;
    display_signal(LV_EVENT_REFR_READY);
    app_presenter_indicator_status_t status = app_presenter_indicator_status();
    assert(status.usable && status.indicator_transfer_confirmed && status.owner_alive);
    assert(status.refresh_sequence == 1 && status.completed_us == 100000);
    assert(s_begins == 4 && s_ends == 4);
}

static void newer_proof_before_status_lock(void)
{
    atomic_store(&s_clock_us, 110000);
    complete_refresh_without_poll(); tick();
}

static void revoke_before_status_lock(void)
{
    ++s_board.invalidation_revision; s_board.indicator_transfer_confirmed = false;
    tick();
}

static void revoke_and_reprove_before_status_lock(void)
{
    revoke_before_status_lock();
    newer_proof_before_status_lock();
}

static void display_copied_receipt_cannot_replace_newer_observation(void)
{
    app_view_t view = setup_display_indicator();
    assert(app_presenter_publish(&view) == APP_PRESENTER_ACCEPTED); tick();
    complete_refresh_without_poll();
    s_receipt_hook = newer_proof_before_status_lock;
    app_presenter_indicator_status_t status = app_presenter_indicator_status();
    assert(status.usable && status.refresh_sequence == 2 && status.completed_us == 110000);
    s_receipt_hook = revoke_and_reprove_before_status_lock;
    status = app_presenter_indicator_status();
    assert(!status.usable || status.refresh_sequence == 3);
    status = app_presenter_indicator_status();
    assert(status.usable && status.refresh_sequence == 3);
    s_receipt_hook = revoke_before_status_lock;
    assert(!app_presenter_indicator_status().usable);
    assert(!app_presenter_indicator_status().usable);
}

static void display_unpolled_invalid_receipts_never_become_proof(void)
{
    app_view_t view = setup_display_indicator();
    assert(app_presenter_publish(&view) == APP_PRESENTER_ACCEPTED); tick();
    for (unsigned defect = 0; defect < 8; ++defect) {
        display_signal(LV_EVENT_REFR_START); observed_complete();
        if (defect == 0) s_board.completed_transfers = 1;
        if (defect == 1) s_board.coverage_complete = false;
        if (defect == 2) s_board.indicator_transfer_confirmed = false;
        if (defect == 3) --s_board.required.x1;
        if (defect == 4) ++s_board.token.capture_generation;
        if (defect == 5) s_board.callback_attached = false;
        if (defect == 6) s_board.association_lost = true;
        if (defect == 7) s_end_error = ESP_FAIL;
        display_signal(LV_EVENT_REFR_READY);
        tick(); /* Owner gets the sealed bad receipt before a fresh begin. */
        s_end_error = ESP_OK;
        display_signal(LV_EVENT_REFR_START);
        /* Restore the seam only after begin has cleared the rejected record. */
        s_board.callback_attached = true; s_board.association_lost = false;
        display_signal(LV_EVENT_REFR_READY);
        assert(!app_presenter_indicator_status().usable);
    }
}

static void display_first_late_proof_survives_unpolled_refresh(void)
{
    app_view_t view = setup_display_indicator();
    assert(app_presenter_publish(&view) == APP_PRESENTER_ACCEPTED); tick();
    display_signal(LV_EVENT_REFR_START); display_signal(LV_EVENT_REFR_READY);
    atomic_store(&s_clock_us, 110000);
    observed_complete();
    atomic_store(&s_clock_us, 120000);
    display_signal(LV_EVENT_REFR_START); display_signal(LV_EVENT_REFR_READY);
    app_presenter_indicator_status_t status = app_presenter_indicator_status();
    assert(status.usable && status.refresh_sequence == 1 && status.completed_us == 110000);
    assert(s_begins == 2 && s_ends == 2);
}

static void display_unpolled_first_proof_revocations(void)
{
    app_view_t view = setup_display_indicator();
    assert(app_presenter_publish(&view) == APP_PRESENTER_ACCEPTED); tick();
    complete_refresh_without_poll();
    ++s_board.invalidation_revision; s_board.indicator_transfer_confirmed = false;
    display_signal(LV_EVENT_REFR_START); display_signal(LV_EVENT_REFR_READY);
    assert(!app_presenter_indicator_status().usable);

    complete_refresh_without_poll();
    view.screen_off = true; ++view.ui_revision;
    assert(app_presenter_publish(&view) == APP_PRESENTER_ACCEPTED); tick();
    display_signal(LV_EVENT_REFR_START); display_signal(LV_EVENT_REFR_READY);
    assert(!app_presenter_indicator_status().indicator_armed && !app_presenter_indicator_status().usable);

    view.screen_off = false; ++view.ui_revision;
    assert(app_presenter_publish(&view) == APP_PRESENTER_ACCEPTED); tick();
    complete_refresh_without_poll();
    ++view.capture_indicator_generation; ++view.ui_revision;
    assert(app_presenter_publish(&view) == APP_PRESENTER_ACCEPTED); tick();
    display_signal(LV_EVENT_REFR_START); display_signal(LV_EVENT_REFR_READY);
    app_presenter_indicator_status_t status = app_presenter_indicator_status();
    assert(status.capture_generation == view.capture_indicator_generation && !status.usable);

    complete_refresh_without_poll();
    ++s_board.display_generation; ++s_board.invalidation_revision;
    s_board.indicator_transfer_confirmed = false;
    unsigned repaints = s_repaints; tick();
    assert(s_repaints == repaints + 1);
    display_signal(LV_EVENT_REFR_START); display_signal(LV_EVENT_REFR_READY);
    status = app_presenter_indicator_status();
    assert(status.display_generation == s_board.display_generation && !status.usable);
}

static void display_binding_failures(void)
{
    initialize();
    s_lvgl_owner = true;
    assert(app_presenter_bind_display(NULL) == ESP_ERR_INVALID_ARG);
    assert(app_presenter_bind_display(&s_test_display) == ESP_ERR_INVALID_STATE);
    s_board.installed = s_board.callback_attached = true;
    assert(app_presenter_bind_display(&s_test_display) == ESP_ERR_INVALID_STATE);
    s_board.display_generation = 8;
    s_test_display.x = 1;
    assert(app_presenter_bind_display(&s_test_display) == ESP_ERR_INVALID_ARG);
    s_test_display.x = 0; s_test_display.rotation = LV_DISPLAY_ROTATION_90;
    assert(app_presenter_bind_display(&s_test_display) == ESP_ERR_INVALID_ARG);
    s_test_display.rotation = LV_DISPLAY_ROTATION_0;
    assert(app_presenter_bind_display(&s_test_display) == ESP_OK);
    assert(app_presenter_bind_display(&s_test_display) == ESP_ERR_INVALID_STATE);
    s_lvgl_owner = false;
    assert(app_presenter_indicator_status().bound && !app_presenter_indicator_status().usable);
}

static void display_requires_real_complete_receipt(void)
{
    app_view_t view = setup_display_indicator();
    assert(app_presenter_publish(&view) == APP_PRESENTER_ACCEPTED);
    assert(!app_presenter_indicator_status().indicator_armed);
    tick();
    assert(app_presenter_indicator_status().indicator_armed && !app_presenter_indicator_status().usable);
    display_signal(LV_EVENT_FLUSH_FINISH);
    assert(!app_presenter_indicator_status().usable && s_begins == 0 && s_ends == 0);
    display_signal(LV_EVENT_REFR_START);
    observed_complete();
    assert(!app_presenter_indicator_status().usable); /* Not sealed yet. */
    display_signal(LV_EVENT_REFR_READY);
    s_board.completed_transfers = 1;
    assert(!app_presenter_indicator_status().usable);
    s_board.completed_transfers = 2; s_board.coverage_complete = false;
    assert(!app_presenter_indicator_status().usable);
    s_board.coverage_complete = true; s_board.indicator_transfer_confirmed = false;
    assert(!app_presenter_indicator_status().usable);
    s_board.indicator_transfer_confirmed = true; --s_board.required.x1;
    assert(!app_presenter_indicator_status().usable);
    ++s_board.required.x1;
    app_presenter_indicator_status_t status = app_presenter_indicator_status();
    assert(status.usable && status.indicator_transfer_confirmed && status.owner_alive);
    assert(status.completed_us == 100000 && status.capture_generation == 23 && status.indicator_revision == 7);
    assert(status.refresh_sequence == 1 && s_begins == 1 && s_ends == 1);
}

static void display_late_completion_and_failure(void)
{
    app_view_t view = setup_display_indicator();
    assert(app_presenter_publish(&view) == APP_PRESENTER_ACCEPTED); tick();
    display_signal(LV_EVENT_REFR_START); display_signal(LV_EVENT_REFR_READY);
    assert(!app_presenter_indicator_status().usable);
    atomic_store(&s_clock_us, 110000);
    observed_complete();
    assert(app_presenter_indicator_status().usable);
    s_board.association_lost = true;
    assert(!app_presenter_indicator_status().usable);
    s_board.association_lost = false;
    s_begin_error = ESP_FAIL;
    display_signal(LV_EVENT_REFR_START);
    display_signal(LV_EVENT_REFR_READY);
    assert(!app_presenter_indicator_status().usable && s_ends == 1);
    s_begin_error = ESP_OK;
    s_end_error = ESP_FAIL;
    display_signal(LV_EVENT_REFR_START); observed_complete(); display_signal(LV_EVENT_REFR_READY);
    assert(!app_presenter_indicator_status().usable);
}

static void display_idle_proof_and_owner_liveness(void)
{
    app_view_t view = setup_display_indicator();
    assert(app_presenter_publish(&view) == APP_PRESENTER_ACCEPTED); tick(); complete_refresh();
    atomic_store(&s_clock_us, 300000); tick();
    assert(s_renders == 1 && s_begins == 1);
    app_presenter_indicator_status_t status = app_presenter_indicator_status();
    assert(status.usable && status.completed_us == 100000 && status.owner_heartbeat_ms == 300);
    /* Empty/unrelated work cannot renew proof; existing unchanged pixels may remain. */
    display_signal(LV_EVENT_REFR_START); display_signal(LV_EVENT_REFR_READY);
    status = app_presenter_indicator_status();
    assert(status.usable && status.refresh_sequence == 1 && status.completed_us == 100000);
    atomic_store(&s_clock_us, 551000);
    for (unsigned i = 0; i < 10000; ++i) assert(app_presenter_publish(&view) == APP_PRESENTER_ACCEPTED);
    status = app_presenter_indicator_status();
    assert(status.indicator_transfer_confirmed && !status.owner_alive && !status.usable);
    assert(status.owner_heartbeat_ms == 300 && status.heartbeat_age_ms == 251);
    tick();
    status = app_presenter_indicator_status();
    assert(status.usable && status.completed_us == 100000 && status.owner_heartbeat_ms == 551);
    atomic_store(&s_clock_us, 500000);
    assert(!app_presenter_indicator_status().clock_valid && !app_presenter_indicator_status().usable);
}

static void display_superseded_hidden_and_recreated(void)
{
    app_view_t view = setup_display_indicator();
    assert(app_presenter_publish(&view) == APP_PRESENTER_ACCEPTED); tick(); complete_refresh();
    board_display_receipt_t old = s_board;
    ++view.capture_indicator_generation; ++view.ui_revision;
    assert(app_presenter_publish(&view) == APP_PRESENTER_ACCEPTED); tick();
    s_board = old;
    assert(!app_presenter_indicator_status().usable);
    complete_refresh();
    uint64_t current_sequence = app_presenter_indicator_status().refresh_sequence;
    old.invalidation_revision = s_board.invalidation_revision;
    s_board = old;
    assert(app_presenter_indicator_status().usable && app_presenter_indicator_status().refresh_sequence == current_sequence);
    view.screen_off = true; ++view.ui_revision;
    assert(app_presenter_publish(&view) == APP_PRESENTER_ACCEPTED); tick();
    assert(!app_presenter_indicator_status().usable && !app_presenter_indicator_status().indicator_armed);
    view.screen_off = false; ++view.ui_revision;
    assert(app_presenter_publish(&view) == APP_PRESENTER_ACCEPTED); tick();
    assert(!app_presenter_indicator_status().usable);
    complete_refresh();
    ++s_board.display_generation;
    assert(!app_presenter_indicator_status().usable);
    display_signal(LV_EVENT_DELETE);
    assert(!app_presenter_indicator_status().bound && !app_presenter_indicator_status().usable);
    s_lvgl_owner = true;
    assert(app_presenter_bind_display(&s_test_display) == ESP_OK);
    s_lvgl_owner = false;
    assert(!app_presenter_indicator_status().usable);
    complete_refresh();
    s_test_display.rotation = LV_DISPLAY_ROTATION_90;
    display_signal(LV_EVENT_REFR_START);
    assert(!app_presenter_indicator_status().usable);
}

static void display_revocation_and_live_generation(void)
{
    app_view_t view = setup_display_indicator();
    assert(app_presenter_publish(&view) == APP_PRESENTER_ACCEPTED); tick(); complete_refresh();
    ++s_board.invalidation_revision; s_board.indicator_transfer_confirmed = false;
    assert(!app_presenter_indicator_status().usable);
    display_signal(LV_EVENT_REFR_START); display_signal(LV_EVENT_REFR_READY);
    assert(!app_presenter_indicator_status().usable);
    complete_refresh();
    /* No status read between revocation and the next empty refresh. */
    ++s_board.invalidation_revision; s_board.indicator_transfer_confirmed = false;
    display_signal(LV_EVENT_REFR_START); display_signal(LV_EVENT_REFR_READY);
    assert(!app_presenter_indicator_status().usable);
    complete_refresh();
    ++s_board.display_generation; ++s_board.invalidation_revision;
    assert(!app_presenter_indicator_status().usable);
    unsigned repaints = s_repaints; tick();
    assert(s_repaints == repaints + 1 && app_presenter_indicator_status().display_generation == s_board.display_generation);
    assert(!app_presenter_indicator_status().usable);
    complete_refresh();
}
