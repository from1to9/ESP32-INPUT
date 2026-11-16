// SPDX-License-Identifier: Apache-2.0
/* Included by the control runtime. The worker owns the scratch snapshots and
 * is the only writer of the label catalog. */
typedef struct {
    ble_hid_bond_list_t ble;
    bt_audio_bond_list_t classic;
    app_wifi_profiles_snapshot_t wifi;
} display_name_owners_t;
static EXT_RAM_BSS_ATTR display_name_owners_t label_owners;

static cp_error_t display_name_command(const cp_command_t *command, app_display_name_entry_t *entry)
{
    char kind[20], id[APP_DISPLAY_NAME_ID_CAPACITY];
    if (!cp_command_string(command, "kind", kind, sizeof(kind)) ||
        !cp_command_string(command, "id", id, sizeof(id))) return CP_INVALID;
    entry->kind = app_display_name_parse_kind(kind);
    if (!app_display_name_parse_id(entry->kind, id, entry->id)) return CP_INVALID;
    cp_scope_mask_t required = entry->kind == APP_DISPLAY_NAME_HID_BOND || entry->kind == APP_DISPLAY_NAME_CLASSIC_BOND ?
        CP_SCOPE_CONNECTIONS_MANAGE : CP_SCOPE_WIFI_CONFIGURE;
    if (!(command->identity.scopes & required)) return CP_FORBIDDEN;
    if (command->operation == CP_OP_DISPLAY_NAME_SET &&
        !cp_command_string(command, "label", entry->label, sizeof(entry->label))) return CP_INVALID;
    return CP_OK;
}
static unsigned display_name_owner_matches(const display_name_owners_t *owners, const app_display_name_entry_t *entry)
{
    unsigned matches = 0;
    if (entry->kind == APP_DISPLAY_NAME_HID_BOND) {
        for (unsigned i = 0; i < owners->ble.count; ++i) {
            const ble_hid_bond_t *row = &owners->ble.bonds[i];
            matches += row->bond_id_known && !memcmp(row->bond_id, entry->id, sizeof(entry->id));
        }
    } else if (entry->kind == APP_DISPLAY_NAME_CLASSIC_BOND) {
        for (unsigned i = 0; i < owners->classic.count; ++i) {
            const bt_audio_bond_t *row = &owners->classic.bonds[i];
            if (!row->bond_id_known) continue;
            uint8_t id[APP_DISPLAY_NAME_ID_BYTES];
            app_display_name_classic_identity(row->address, row->bond_id.bytes, id);
            matches += !memcmp(id, entry->id, sizeof(id));
        }
    } else if (entry->kind == APP_DISPLAY_NAME_WIFI_PROFILE) {
        for (unsigned i = 0; i < owners->wifi.count; ++i) {
            uint8_t id[APP_DISPLAY_NAME_ID_BYTES];
            matches += app_display_name_profile_identity(owners->wifi.profiles[i].id, id) &&
                !memcmp(id, entry->id, sizeof(id));
        }
    }
    return matches;
}
static cp_error_t display_name_check_owner(const cp_command_t *command, app_display_name_entry_t *entry)
{
    if (entry->kind == APP_DISPLAY_NAME_WIFI_AP) {
        int64_t generation, index;
        if (!cp_command_integer(command, "scan_generation", &generation) || generation <= 0 || generation > UINT32_MAX ||
            !cp_command_integer(command, "scan_index", &index) || index < 0 || index >= APP_WIFI_SCAN_CAPACITY)
            return CP_INVALID;
        app_wifi_scan_record_t row;
        uint8_t id[APP_DISPLAY_NAME_ID_BYTES];
        if (app_wifi_get_scan_record((uint32_t)generation, (uint16_t)index, &row) != ESP_OK ||
            !app_display_name_ap_identity((const uint8_t *)row.ssid, row.ssid_length, row.bssid, row.sdk_authmode, id) ||
            memcmp(id, entry->id, sizeof(id))) return CP_STALE;
        app_wifi_profiles_snapshot(&label_owners.wifi);
        if (!label_owners.wifi.known || label_owners.wifi.uncertain || label_owners.wifi.operation_pending) return CP_UNAVAILABLE;
        if (app_display_name_scan_profiles(&label_owners.wifi, &row, id)) return CP_INVALID;
        entry->ssid_length = row.ssid_length;
        memcpy(entry->ssid, row.ssid, row.ssid_length);
        return CP_OK;
    }
    uint64_t wanted, current;
    if (!argument_u64(command, "owner_revision", &wanted)) return CP_INVALID;
    if (entry->kind == APP_DISPLAY_NAME_HID_BOND) {
        ble_hid_copy_bond_list(&label_owners.ble);
        if (!label_owners.ble.known || label_owners.ble.updating) return CP_UNAVAILABLE;
        current = label_owners.ble.revision;
    } else if (entry->kind == APP_DISPLAY_NAME_CLASSIC_BOND) {
        if (!bt_audio_classic_forget_supported()) return CP_UNAVAILABLE;
        bt_audio_copy_bond_list(&label_owners.classic);
        if (!label_owners.classic.known || label_owners.classic.refreshing) return CP_UNAVAILABLE;
        current = label_owners.classic.revision;
    } else {
        app_wifi_profiles_snapshot(&label_owners.wifi);
        if (!label_owners.wifi.known || label_owners.wifi.uncertain || label_owners.wifi.operation_pending) return CP_UNAVAILABLE;
        current = label_owners.wifi.revision;
    }
    return wanted == current && display_name_owner_matches(&label_owners, entry) == 1 ? CP_OK : CP_STALE;
}
static void display_name_apply(work_t *work)
{
    app_display_name_entry_t entry = {0};
    const cp_command_t *command = &work->command;
    bool clear = command->operation == CP_OP_DISPLAY_NAME_CLEAR;
    cp_error_t error = display_name_command(command, &entry);
    if (error == CP_OK && !clear) error = display_name_check_owner(command, &entry);
    uint64_t revision = 0;
    if (error == CP_OK) error = app_display_names_change(&entry, command->resource_revision, clear, &revision);
    work->result.state = error == CP_OK ? CP_RESULT_APPLIED : CP_RESULT_FAILED;
    work->result.error = error;
    work->result.resource_revision = error == CP_OK ? revision : app_display_names_status().revision;
    const char *reason = error == CP_OK ? (clear ? "label_cleared" : "label_saved") :
        error == CP_STORAGE_UNCERTAIN ? "label_storage_uncertain" : error == CP_STALE ? "label_identity_or_revision_changed" :
        error == CP_BUSY ? "label_capacity_reached" : "label_change_rejected";
    (void)snprintf(work->result.reason, sizeof(work->result.reason), "%s", reason);
    work->completion_pending = true;
    wipe(work->command.args_json, sizeof(work->command.args_json));
}

typedef struct { bool ble, classic, wifi; } display_name_authority_t;
static bool display_name_removed(void *context, const app_display_name_entry_t *entry)
{
    const display_name_authority_t *known = context;
    bool complete = entry->kind == APP_DISPLAY_NAME_HID_BOND ? known->ble :
        entry->kind == APP_DISPLAY_NAME_CLASSIC_BOND ? known->classic :
        entry->kind == APP_DISPLAY_NAME_WIFI_PROFILE && known->wifi;
    return complete && !display_name_owner_matches(&label_owners, entry);
}
static void display_name_cleanup_service(void)
{
    static uint64_t next_ms;
    static uint64_t last_labels, last_ble, last_classic, last_wifi;
    static unsigned last_authority;
    uint64_t now = now_ms();
    if (now < next_ms) return;
    app_display_names_status_t state = app_display_names_status();
    if (!state.known) {
        next_ms = now + 30000U;
        app_display_names_load();
        return;
    }
    next_ms = now + 1000U;
    if (!state.count) return;
    ble_hid_copy_bond_list(&label_owners.ble);
    bt_audio_copy_bond_list(&label_owners.classic);
    app_wifi_profiles_snapshot(&label_owners.wifi);
    display_name_authority_t known = {
        .ble = label_owners.ble.known && !label_owners.ble.updating,
        .classic = label_owners.classic.known && !label_owners.classic.refreshing && bt_audio_classic_forget_supported(),
        .wifi = label_owners.wifi.known && !label_owners.wifi.uncertain && !label_owners.wifi.operation_pending
    };
    for (unsigned i = 0; known.ble && i < label_owners.ble.count; ++i)
        known.ble = label_owners.ble.bonds[i].bond_id_known;
    for (unsigned i = 0; known.classic && i < label_owners.classic.count; ++i)
        known.classic = label_owners.classic.bonds[i].bond_id_known;
    unsigned authority = (known.ble ? 1U : 0U) | (known.classic ? 2U : 0U) | (known.wifi ? 4U : 0U);
    if (state.revision == last_labels && authority == last_authority &&
        label_owners.ble.revision == last_ble && label_owners.classic.revision == last_classic &&
        label_owners.wifi.revision == last_wifi && !state.cleanup_pending) return;
    last_labels = state.revision;
    last_ble = label_owners.ble.revision;
    last_classic = label_owners.classic.revision;
    last_wifi = label_owners.wifi.revision;
    last_authority = authority;
    app_display_names_cleanup(display_name_removed, &known);
}
