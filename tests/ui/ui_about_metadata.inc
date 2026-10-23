// SPDX-License-Identifier: Apache-2.0
/* About metadata is provided by the coordinator; all values here are fixtures. */
static lv_obj_t *about_value_at(lv_obj_t *object, int x, int y)
{
    if (lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN)) return NULL;
    if (lv_obj_check_type(object, &lv_label_class)) {
        lv_area_t area;
        lv_obj_get_coords(object, &area);
        if (area.x1 == x && area.y1 == y) return object;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); ++i) {
        lv_obj_t *found = about_value_at(lv_obj_get_child(object, (int32_t)i), x, y);
        if (found) return found;
    }
    return NULL;
}

static void about_value(unsigned row, const char *expected)
{
    lv_obj_t *value = about_value_at(lv_screen_active(), 148, 64 + (int)row * 24);
    assert(value && !strcmp(lv_label_get_text(value), expected));
}

static void about_repository(const char *expected)
{
    lv_obj_t *value = about_value_at(lv_screen_active(), 12, 232);
    assert(value && !strcmp(lv_label_get_text(value), expected));
    assert(lv_label_get_long_mode(value) == LV_LABEL_LONG_WRAP);
    lv_point_t size;
    lv_text_get_size(&size, expected, lv_obj_get_style_text_font(value, 0),
        lv_obj_get_style_text_letter_space(value, 0), lv_obj_get_style_text_line_space(value, 0),
        lv_obj_get_content_width(value), LV_TEXT_FLAG_NONE);
    assert(size.y <= lv_obj_get_content_height(value));
}

static void about_metadata_checks(void)
{
    app_view_t original = s_state;
    s_state.mode = INPUT_MOUSE; s_state.overlay = APP_OVERLAY_SETTINGS;
    s_state.settings_page = APP_SETTINGS_HOME;
    s_state.locked = s_state.screen_off = s_state.remote_owned = false;
    ++s_state.ui_revision; render(); clear_intents(); tap_control("About");
    assert(action_count(APP_SETTINGS_PAGE) == 1 && last_action(APP_SETTINGS_PAGE)->x == APP_SETTINGS_ABOUT);
    assert(s_state.settings_page == APP_SETTINGS_HOME);
    s_state.settings_page = APP_SETTINGS_ABOUT;
    strcpy(s_state.developer, "from1to9");
    strcpy(s_state.repository_url, "https://github.com/from1to9/ESP32-INPUT");
    strcpy(s_state.git_commit, "0123456789abcdef0123456789abcdef01234567");
    strcpy(s_state.firmware_version, "fixture-1.2.3");
    strcpy(s_state.build_id, "ELF-artifact-7654321");
    strcpy(s_state.board_revision, "Test board");
    strcpy(s_state.device_id, "Fixture device");
    s_state.uptime_seconds = 3782;
    const app_git_state_t states[] = {APP_GIT_CLEAN, APP_GIT_DIRTY, APP_GIT_UNKNOWN, (app_git_state_t)99};
    const char *names[] = {"clean", "modified", "unknown-state", "invalid-state"};
    const char *expected[] = {"0123456789ab / Clean", "0123456789ab / Modified", "0123456789ab / Unknown", "0123456789ab / Unknown"};
    for (unsigned i = 0; i < 4; ++i) {
        s_state.git_state = states[i]; ++s_state.ui_revision;
        char scene[64]; snprintf(scene, sizeof(scene), "about-metadata-%s", names[i]); screenshot(scene);
        about_value(0, "fixture-1.2.3"); about_value(1, "ELF-artifact-7654321");
        about_value(2, "Test board"); about_value(3, "Fixture device");
        about_value(4, "from1to9"); about_value(5, expected[i]);
        about_repository("https://github.com/from1to9/ESP32-INPUT");
        assert(has_visible_label(lv_screen_active(), "Build artifact ID"));
        assert(has_visible_label(lv_screen_active(), "Uptime  1h 03m"));
        assert_rect(control("Back"), 12, 424, 456, 48);
        assert_rect(control("Close"), 404, 8, 64, 48);
        clear_intents(); tap(240, 252);
        assert(s_intent_count == action_count(APP_TOUCH_BEGIN) + action_count(APP_TOUCH_END));
    }
    /* The bounded maximum is wrapped, never silently shortened. */
    strcpy(s_state.repository_url, "https://example.com/");
    size_t prefix = strlen(s_state.repository_url);
    memset(s_state.repository_url + prefix, 'W', sizeof(s_state.repository_url) - prefix - 1);
    s_state.repository_url[sizeof(s_state.repository_url) - 1] = 0;
    screenshot("about-metadata-long-repository"); about_repository(s_state.repository_url);
    strcpy(s_state.repository_url, "https://git.example.com:8443/team/project");
    screenshot("about-metadata-repository-port"); about_repository(s_state.repository_url);
    const char *valid_urls[] = {"https://example.com:1/repo", "https://example.com:65535/repo",
        "https://example.com:00001/repo", "https://Git-1.Example.com/a%20b/%2F",
        "https://example.com/a-._~!$&'()*+,;=:@/b"};
    for (unsigned i = 0; i < sizeof(valid_urls) / sizeof(valid_urls[0]); ++i) {
        strcpy(s_state.repository_url, valid_urls[i]); render(); about_repository(valid_urls[i]);
    }
    s_state.developer[0] = s_state.repository_url[0] = s_state.git_commit[0] = 0;
    s_state.git_state = APP_GIT_CLEAN;
    screenshot("about-metadata-empty");
    about_value(4, "Unknown"); about_value(5, "Unknown"); about_repository("Unavailable");
    /* Reject unterminated copies without converting them to apparently valid prefixes. */
    memset(s_state.developer, 'D', sizeof(s_state.developer));
    memset(s_state.repository_url, 'R', sizeof(s_state.repository_url));
    memset(s_state.git_commit, 'a', sizeof(s_state.git_commit));
    screenshot("about-metadata-unterminated");
    about_value(4, "Unknown"); about_value(5, "Unknown"); about_repository("Unavailable");
    assert(s_state.developer[31] == 'D' && s_state.repository_url[95] == 'R' && s_state.git_commit[40] == 'a');
    strcpy(s_state.developer, "Bad\nname");
    strcpy(s_state.repository_url, "https:///missing-host");
    strcpy(s_state.git_commit, "0123456789abcdef0123456789abcdef0123456z");
    screenshot("about-metadata-malformed");
    about_value(4, "Unknown"); about_value(5, "Unknown"); about_repository("Unavailable");
    const char *bad_urls[] = {"javascript:alert(1)", "https://github..com/a/b", "https://-github.com/a/b",
        "https://github.com-/a/b", "https://github.com/a b", "https://github.com/a%", "https://github.com/a%zz",
        "https://user:secret@example.com/repo", "https://user@example.com/repo",
        "https://example.com/repo?token=secret", "https://example.com/repo?", "https://example.com/repo#branch",
        "https://example.com/repo#", " https://example.com/repo", "https://example.com/repo ",
        "https://example.com:/repo", "https://example.com:0/repo", "https://example.com:65536/repo",
        "https://example.com:000001/repo", "https://example.com:abc/repo", "https://example.com/repo|branch",
        "https://[::1]/repo", "https://example.com/repo\\branch", "https://example.com/repo{branch}",
        "https://example.com./repo", "https://localhost/repo", "HTTPS://example.com/repo",
        "https://example.com", "https://example.com/",
        "https://aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.com/repo"};
    for (unsigned i = 0; i < sizeof(bad_urls) / sizeof(bad_urls[0]); ++i) {
        strcpy(s_state.repository_url, bad_urls[i]); render(); about_repository("Unavailable");
    }
    const char *bad_developers[] = {" leading", "trailing ", "\tcontrol", "Non-ASCII \xc3\xa9"};
    for (unsigned i = 0; i < sizeof(bad_developers) / sizeof(bad_developers[0]); ++i) {
        strcpy(s_state.developer, bad_developers[i]); render(); about_value(4, "Unknown");
    }
    strcpy(s_state.developer, "Example Developer"); render(); about_value(4, "Example Developer");
    strcpy(s_state.git_commit, "0123456789ab"); render(); about_value(5, "Unknown");
    clear_intents(); tap_control("Back");
    assert(action_count(APP_SETTINGS_PAGE) == 1 && last_action(APP_SETTINGS_PAGE)->x == APP_SETTINGS_HOME);
    clear_intents(); tap_control("Close"); assert(action_count(APP_CLOSE) == 1);
    s_state = original; render(); clear_intents();
    puts("PASS: bounded About metadata, complete repository wrapping, distinct artifact/Git identity, truthful invalid snapshots and actual navigation hits");
}
