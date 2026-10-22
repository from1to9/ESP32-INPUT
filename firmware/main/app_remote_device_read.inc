// SPDX-License-Identifier: Apache-2.0
/* Coordinator-only encoders. Network workers receive copied bridge caches. */
#include <math.h>
static const char remote_device_retention_manifest[] =
    "{\"schema\":1,\"reset\":[\"device_preferences\",\"ble_hid_bonds\",\"wifi_profiles\",\"hid_target_preferences\"],"
    "\"retain\":[\"control_trust\",\"classic_bonds\",\"firmware\",\"ota\",\"models\",\"recordings\","
    "\"unknown_namespaces\",\"external_nand\",\"factory_backups\"],\"restart\":false}";
static void remote_device_manifest_digest(char digest[65])
{
    uint8_t hash[32]; cp_sha256(remote_device_retention_manifest, sizeof(remote_device_retention_manifest) - 1U, hash);
    remote_device_hex(hash, sizeof(hash), digest);
}
static bool remote_device_json(char *json, size_t capacity, size_t *used, const char *format, ...)
{
    if (*used >= capacity) return false;
    va_list args; va_start(args, format);
    int written = vsnprintf(json + *used, capacity - *used, format, args);
    va_end(args);
    if (written < 0 || (size_t)written >= capacity - *used) { json[0] = 0; *used = capacity; return false; }
    *used += (size_t)written; return true;
}
static cp_error_t remote_device_page(const char *resource, unsigned *cursor, unsigned *limit)
{
    static const char root[] = "/api/v1/bluetooth/peers";
    if (strncmp(resource, root, sizeof(root) - 1U)) return CP_NOT_IMPLEMENTED;
    const char *p = resource + sizeof(root) - 1U;
    *cursor = 0; *limit = 8;
    bool cursor_seen = false, limit_seen = false;
    if (!*p) return CP_OK;
    if (*p++ != '?') return CP_NOT_IMPLEMENTED;
    while (*p) {
        unsigned *value;
        if (!strncmp(p, "cursor=", 7) && !cursor_seen) { value = cursor; cursor_seen = true; p += 7; }
        else if (!strncmp(p, "limit=", 6) && !limit_seen) { value = limit; limit_seen = true; p += 6; }
        else return CP_INVALID;
        *value = 0;
        if (*p < '0' || *p > '9') return CP_INVALID;
        while (*p >= '0' && *p <= '9') {
            if (*value > 100U) return CP_INVALID;
            *value = *value * 10U + (unsigned)(*p++ - '0');
        }
        if (!*p) break;
        if (*p++ != '&' || !*p) return CP_INVALID;
    }
    return *limit && *limit <= 8U && *cursor <= BLE_HID_MAX_BONDS + BT_AUDIO_CLASSIC_BONDS_MAX ? CP_OK : CP_INVALID;
}
static cp_error_t remote_device_peers_read(const char *resource, char *json, size_t capacity, size_t *length)
{
    unsigned cursor, limit;
    cp_error_t error = remote_device_page(resource, &cursor, &limit);
    if (error != CP_OK) return error;
    ble_hid_snapshot_t link = ble_hid_snapshot();
    static EXT_RAM_BSS_ATTR ble_hid_bond_list_t ble;
    ble_hid_copy_bond_list(&ble);
    static EXT_RAM_BSS_ATTR bt_audio_bond_list_t classic;
    bt_audio_copy_bond_list(&classic);
    ble_hid_target_snapshot_t target = ble_hid_target_snapshot();
    if (ble.count > BLE_HID_MAX_BONDS || classic.count > BT_AUDIO_CLASSIC_BONDS_MAX) return CP_INTERNAL;
    unsigned total = (unsigned)ble.count + classic.count;
    if (cursor > total) return CP_STALE;
    size_t used = 0;
    (void)remote_device_json(json, capacity, &used,
        "{\"ble_revision\":\"%" PRIu64 "\",\"classic_revision\":\"%" PRIu64 "\",\"target_revision\":\"%" PRIu64 "\","
        "\"ble_known\":%s,\"ble_updating\":%s,\"classic_known\":%s,\"classic_refreshing\":%s,\"connected\":%s,"
        "\"current_match_known\":%s,\"target_durable\":%s,\"total\":%u,\"peers\":[",
        ble.revision, classic.revision, target.revision, ble.known ? "true":"false", ble.updating ? "true":"false",
        classic.known ? "true":"false", classic.refreshing ? "true":"false", link.connected ? "true":"false",
        link.connected && ble.known && ble.current_match_known ? "true":"false", target.durable ? "true":"false", total);
    unsigned end = cursor + limit < total ? cursor + limit : total;
    for (unsigned i = cursor; i < end; ++i) {
        if (i < ble.count) {
            const ble_hid_bond_t *bond = &ble.bonds[i]; char id[65], encoded[68];
            if (bond->bond_id_known) { remote_device_hex(bond->bond_id, sizeof(bond->bond_id), id); (void)snprintf(encoded, sizeof(encoded), "\"%s\"", id); }
            else (void)snprintf(encoded, sizeof(encoded), "%s", "null");
            (void)remote_device_json(json, capacity, &used,
                "%s{\"transport\":\"ble_hid\",\"peer_id\":%s,\"name\":null,\"order\":%u,\"selected\":%s,\"current\":%s,\"selectable\":%s}",
                i == cursor ? "":",", encoded, i, ble.known && target.available && bond->is_selected ? "true":"false",
                ble.known && link.connected && ble.current_match_known && bond->is_current ? "true":"false",
                ble.known && !ble.updating && bond->selection_supported && bond->bond_id_known && bond->address_type_known ? "true":"false");
        } else {
            const bt_audio_bond_t *bond = &classic.bonds[i - ble.count]; char id[13]; remote_device_hex(bond->address, sizeof(bond->address), id);
            (void)remote_device_json(json, capacity, &used,
                "%s{\"transport\":\"classic_audio\",\"peer_id\":\"%s\",\"name\":null,\"order\":null,\"selected\":false,"
                "\"hfp_current\":%s,\"a2dp_current\":%s}", i == cursor ? "":",", id,
                classic.known && bond->hfp_current ? "true":"false", classic.known && bond->a2dp_current ? "true":"false");
        }
    }
    if (!remote_device_json(json, capacity, &used, "],\"next_cursor\":%u}", end < total ? end : 0)) return CP_TOO_LARGE;
    *length = used; return CP_OK;
}
static cp_error_t remote_device_state_json(const cp_identity_t *identity, char *json, size_t capacity, size_t *length)
{
    if (!identity || !(identity->scopes & CP_SCOPE_VIEW) || !json || !capacity || !length) return CP_FORBIDDEN;
    settings_store_status_t storage = settings_store_status(); board_power_status_t power = {0};
    (void)board_power_read(&power); char digest[65]; remote_device_manifest_digest(digest);
    char percent[8] = "null", voltage[12] = "null", current[12] = "null", temperature[24] = "null";
    if (power.percent_valid && power.percent <= 100U) (void)snprintf(percent, sizeof(percent), "%u", power.percent);
    if (power.voltage_valid) (void)snprintf(voltage, sizeof(voltage), "%u", power.voltage_mv);
    if (power.current_valid) (void)snprintf(current, sizeof(current), "%d", power.current_ma);
    if (power.temperature_valid && isfinite(power.temperature_c)) (void)snprintf(temperature, sizeof(temperature), "%.2f", (double)power.temperature_c);
    size_t used = 0;
    (void)remote_device_json(json, capacity, &used,
        "{\"settings_revision\":\"%" PRIu64 "\",\"maintenance_revision\":\"%" PRIu64 "\",\"persistence\":{\"available\":%s,\"loaded\":%s,\"pending\":%s,"
        "\"requested_revision\":\"%" PRIu64 "\",\"saved_revision\":\"%" PRIu64 "\",\"error\":%d},"
        "\"power\":{\"gauge_available\":%s,\"battery_present\":%s,\"external_power\":%s,\"charging\":%s,\"full\":%s,"
        "\"percent\":%s,\"voltage_mv\":%s,\"current_ma\":%s,\"temperature_c\":%s,\"observed_us\":\"%" PRIu64 "\",\"error\":%d},"
        "\"factory_reset\":{\"retained_manifest_digest\":\"%s\",\"manifest\":%s},\"approval\":",
        s_settings_revision, s_remote_device_maintenance_revision, storage.available ? "true":"false", storage.loaded ? "true":"false", storage.pending ? "true":"false",
        storage.requested_revision, storage.saved_revision, storage.error, power.gauge_available ? "true":"false",
        power.battery_present == BOARD_TRUTH_UNKNOWN ? "null":power.battery_present == BOARD_TRUTH_YES ? "true":"false",
        power.external_power == BOARD_TRUTH_UNKNOWN ? "null":power.external_power == BOARD_TRUTH_YES ? "true":"false",
        power.charging == BOARD_TRUTH_UNKNOWN ? "null":power.charging == BOARD_TRUTH_YES ? "true":"false",
        power.full == BOARD_TRUTH_UNKNOWN ? "null":power.full == BOARD_TRUTH_YES ? "true":"false",
        percent, voltage, current, temperature, power.timestamp_us, power.error, digest, remote_device_retention_manifest);
    bool found = false;
    for (size_t i = 0; i < sizeof(s_remote_device_reviews)/sizeof(s_remote_device_reviews[0]); ++i) {
        const app_remote_device_approval_t *a = &s_remote_device_reviews[i].approval;
        if (!s_remote_device_reviews[i].used || a->cancelled || a->consumed || s_now_ms >= a->expires_ms ||
            !remote_device_id_equal(a->owner.principal_id, identity->principal_id, CP_ID_BYTES) ||
            !remote_device_id_equal(a->owner.session_id, identity->session_id, CP_ID_BYTES)) continue;
        remote_device_hex(a->action_digest, sizeof(a->action_digest), digest);
        (void)remote_device_json(json, capacity, &used,
            "{\"operation_id\":\"%s\",\"operation\":%u,\"digest\":\"%s\",\"revision\":\"%" PRIu64 "\","
            "\"expires_ms\":\"%" PRIu64 "\",\"physical_required\":%s,\"physical_satisfied\":%s,\"confirmed\":%s}",
            a->id, (unsigned)a->operation, digest, a->resource_revision, a->expires_ms,
            a->physical_required ? "true":"false", a->physical_satisfied ? "true":"false", a->confirmed ? "true":"false");
        found = true; break;
    }
    if (!remote_device_json(json, capacity, &used, "%s}", found ? "":"null")) return CP_TOO_LARGE;
    *length = used; return CP_OK;
}
