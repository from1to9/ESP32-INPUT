#include "passport_wifi.h"
#include <stdio.h>
#include <string.h>

const char *passport_wifi_security_name(passport_wifi_security_t value)
{
    return value == PW_OPEN ? "Open" : value == PW_WPA2 ? "WPA2" : "Unsupported";
}
const char *passport_wifi_phase_name(passport_wifi_phase_t value)
{
    return (const char *const[]){"Offline", "Switching network", "Connecting", "Associated / no IP", "LAN IP assigned"}[value];
}
void passport_wifi_ssid_text(const passport_ssid_t *ssid, char *out, size_t capacity)
{
    bool printable = ssid->length != 0;
    for (unsigned i = 0; i < ssid->length; ++i) printable &= ssid->bytes[i] >= 32 && ssid->bytes[i] <= 126;
    if (!ssid->length) { snprintf(out, capacity, "Hidden SSID"); return; }
    if (printable) { snprintf(out, capacity, "%.*s", ssid->length, ssid->bytes); return; }
    size_t used = snprintf(out, capacity, "hex:");
    for (unsigned i = 0; i < ssid->length && used + 2 < capacity; ++i) used += snprintf(out + used, capacity - used, "%02X", ssid->bytes[i]);
}
passport_name_key_t passport_wifi_ap_name_key(const passport_wifi_row_t *row)
{
    passport_name_key_t key = {.kind = PN_SCAN_AP, .ap = {.length = row->ssid.length, .auth = row->auth}};
    memcpy(key.ap.ssid, row->ssid.bytes, sizeof(key.ap.ssid));
    memcpy(key.ap.bssid, row->bssid, sizeof(key.ap.bssid));
    return key;
}

unsigned passport_wifi_profile_matches(const passport_wifi_state_t *snapshot, const passport_wifi_row_t *row,
                                       uint32_t *profile_id)
{
    unsigned matches = 0;
    *profile_id = 0;
    if (!snapshot->storage_known || row->ssid.length > sizeof(row->ssid.bytes)) return 0;
    for (unsigned i = 0; i < snapshot->profile_count; ++i) {
        const passport_wifi_profile_t *profile = &snapshot->profiles[i];
        if (profile->security != row->security || profile->ssid.length != row->ssid.length ||
            memcmp(profile->ssid.bytes, row->ssid.bytes, row->ssid.length)) continue;
        ++matches;
        *profile_id = profile->id;
    }
    if (matches != 1) *profile_id = 0;
    return matches;
}

passport_name_result_t passport_wifi_scan_label(const passport_wifi_state_t *snapshot,
                                               const passport_wifi_row_t *row, char *label)
{
    passport_name_key_t key = passport_wifi_ap_name_key(row);
    passport_name_result_t result = passport_names_lookup(&key, label);
    if (result != PN_NOT_FOUND) return result;
    uint32_t profile;
    if (passport_wifi_profile_matches(snapshot, row, &profile) != 1) return PN_NOT_FOUND;
    key = (passport_name_key_t){.kind = PN_WIFI_PROFILE, .profile = profile};
    return passport_names_lookup(&key, label);
}
