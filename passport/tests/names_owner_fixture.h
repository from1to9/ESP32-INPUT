#pragma once
#include "../main/passport_names_owner.h"
#include <string.h>

static bool name_queued;
static passport_name_change_t queued_name;
static passport_name_result_t name_result, name_remove_result;
static passport_name_key_t removed_name;
static unsigned name_applied, name_removals;

void passport_names_load(passport_name_domain_t domain) { (void)domain; }
bool passport_names_take(passport_name_domain_t domain, passport_name_change_t *change)
{
    (void)domain;
    if (!name_queued) return false;
    *change = queued_name;
    name_queued = false;
    return true;
}
void passport_names_finish(passport_name_domain_t domain, passport_name_result_t result)
{
    (void)domain;
    name_result = result;
}
passport_name_result_t passport_names_apply(const passport_name_change_t *change)
{
    assert(change->revision);
    ++name_applied;
    return PN_OK;
}
passport_name_result_t passport_names_remove(const passport_name_key_t *key)
{
    removed_name = *key;
    ++name_removals;
    return name_remove_result;
}
passport_name_result_t passport_names_prune(passport_name_domain_t domain,
                                            bool (*retained)(const passport_name_key_t *))
{
    (void)domain;
    assert(retained);
    return PN_OK;
}
bool passport_names_bond_id(const uint8_t *material, size_t length, uint8_t id[PASSPORT_BOND_ID_BYTES])
{
    assert(length == 36);
    memset(id, 0, PASSPORT_BOND_ID_BYTES);
    id[0] = material[2];
    id[1] = material[20];
    return true;
}
bool passport_names_equal(const passport_name_key_t *a, const passport_name_key_t *b)
{
    assert(a->kind == PN_SCAN_AP && b->kind == PN_SCAN_AP);
    return a->ap.length == b->ap.length && a->ap.auth == b->ap.auth &&
           !memcmp(a->ap.bssid, b->ap.bssid, sizeof(a->ap.bssid)) &&
           !memcmp(a->ap.ssid, b->ap.ssid, a->ap.length);
}
passport_name_result_t passport_names_lookup(const passport_name_key_t *key, char *label)
{
    if (key->kind == PN_SCAN_AP && key->ap.bssid[5] == 11) strcpy(label, "Exact AP");
    else if (key->kind == PN_WIFI_PROFILE && key->profile == 1) strcpy(label, "Profile label");
    else label[0] = 0;
    return label[0] ? PN_OK : PN_NOT_FOUND;
}
