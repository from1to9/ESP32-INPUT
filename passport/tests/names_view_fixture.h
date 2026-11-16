#pragma once
#include "../main/passport_names.h"
#include <string.h>

#define LONG_DISPLAY_LABEL "WWWWWWWWWWWWWWWW"
#define MAX_BYTES_LABEL "••••••••••••••••"
static bool label_storage_known = true;

passport_name_catalog_t passport_names_catalog(passport_name_domain_t domain)
{
    (void)domain;
    return (passport_name_catalog_t){.known = label_storage_known};
}
passport_name_result_t passport_names_lookup(const passport_name_key_t *key, char *label)
{
    label[0] = 0;
    if (!label_storage_known) return PN_UNCERTAIN;
    if (key->kind == PN_HID_BOND && (key->bond[0] == 1 || key->bond[0] == 2)) strcpy(label, LONG_DISPLAY_LABEL);
    if (key->kind == PN_WIFI_PROFILE && key->profile == 1) strcpy(label, "Studio profile");
    if (key->kind == PN_SCAN_AP && key->ap.bssid[5] == 2) {
        const char *ap_label = key->ap.length != sizeof(key->ap.ssid) ? "Exact AP" :
            key->ap.auth == 3 ? LONG_DISPLAY_LABEL : MAX_BYTES_LABEL;
        strcpy(label, ap_label);
    }
    return label[0] ? PN_OK : PN_NOT_FOUND;
}
