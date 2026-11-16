#include <assert.h>
#include <stdio.h>
#include "../main/passport_names.c"
#include "../main/passport_names_text.c"

typedef struct { const char *key; unsigned char data[2200]; size_t length; } fake_blob_t;
static fake_blob_t flash[] = {{.key = "hid"}, {.key = "wifi"}, {.key = "bond_secret"}};
static fake_blob_t pending_blob;
static bool fail_write, fail_commit;
static unsigned writes, random_calls;

static fake_blob_t *find_blob(const char *key)
{
    for (unsigned i = 0; i < sizeof(flash) / sizeof(flash[0]); ++i) {
        if (!strcmp(flash[i].key, key)) return &flash[i];
    }
    abort();
}
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle)
{
    assert(!strcmp(name, NAMESPACE));
    *handle = mode + 1;
    return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *data, size_t *length)
{
    assert(handle == 1);
    fake_blob_t *blob = find_blob(key);
    if (!blob->length) return ESP_ERR_NVS_NOT_FOUND;
    if (*length < blob->length) return ESP_ERR_INVALID_SIZE;
    memcpy(data, blob->data, blob->length);
    *length = blob->length;
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t length)
{
    assert(handle == 2 && length <= sizeof(pending_blob.data));
    ++writes;
    if (fail_write) return ESP_FAIL;
    pending_blob.key = key;
    memcpy(pending_blob.data, data, length);
    pending_blob.length = length;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(handle == 2 && pending_blob.length);
    if (fail_commit) return ESP_FAIL;
    *find_blob(pending_blob.key) = pending_blob;
    return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { (void)handle; pending_blob.length = 0; }
void esp_fill_random(void *data, size_t size) { memset(data, ++random_calls, size); }
void explicit_bzero(void *data, size_t size) { memset(data, 0, size); }
const mbedtls_md_info_t *mbedtls_md_info_from_type(int type) { assert(type == MBEDTLS_MD_SHA256); static const int md; return &md; }
int mbedtls_md_hmac(const mbedtls_md_info_t *md, const unsigned char *key, size_t key_length,
                   const unsigned char *message, size_t length, unsigned char *digest)
{
    assert(md && key_length == 32 && length);
    for (unsigned i = 0; i < 32; ++i) digest[i] = key[i] ^ message[i % length];
    return 0;
}
const lv_font_t lv_font_montserrat_12 = 12, lv_font_montserrat_14 = 14;
bool lv_font_get_glyph_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *out, uint32_t scalar, uint32_t next)
{
    assert((*font == 12 || *font == 14) && !next);
    out->is_placeholder = false;
    return (scalar >= 32 && scalar < 127) || scalar == 0xB0 || scalar == 0x2022;
}

static void boot(void)
{
    for (unsigned n = PN_BLE; n <= PN_WIFI; ++n) {
        catalog_t *c = &catalogs[n];
        c->known = c->uncertain = c->taken = false;
        c->ticket = c->completed = c->revision = c->count = 0;
    }
    memset(bond_secret, 0, sizeof(bond_secret));
    passport_names_load(PN_BLE);
    passport_names_load(PN_WIFI);
}

static passport_name_change_t change_for(passport_name_key_t key, const char *text)
{
    passport_name_change_t change = {.key = key, .revision = passport_names_catalog(key_domain(&key)).revision,
                                     .clear = !text};
    if (text) snprintf(change.label, sizeof(change.label), "%s", text);
    return change;
}

static passport_name_result_t submit_and_service(const passport_name_change_t *change)
{
    passport_name_domain_t domain = key_domain(&change->key);
    uint32_t ticket;
    passport_name_result_t admitted = passport_names_submit(change, &ticket);
    if (admitted != PN_OK) return admitted;
    assert(passport_names_result(domain, ticket) == PN_PENDING);
    assert(!passport_names_take(domain == PN_BLE ? PN_WIFI : PN_BLE, &catalogs[domain].queued));
    uint32_t another;
    assert(passport_names_submit(change, &another) == PN_BUSY);
    passport_name_change_t current;
    assert(passport_names_take(domain, &current));
    assert(!passport_names_take(domain, &current));
    passport_names_finish(domain, passport_names_apply(&current));
    return passport_names_result(domain, ticket);
}

static void test_text(void)
{
    assert(passport_names_text_valid("Front desk °", sizeof("Front desk °") - 1));
    char full[49] = "••••••••••••••••";
    assert(strlen(full) == 48 && passport_names_text_valid(full, 48));
    assert(!passport_names_text_valid(full, 49));
    static const char *const invalid[] = {"", "abcdefghijklmnopq", "\xc0\xaf", "\xe0\x80\x80", "\xed\xa0\x80",
        "\xf4\x90\x80\x80", "\x80", "\xc2", "\xe2\x80\xae", "\xe2\x81\xa6", "\x7f", "\n", "\xe5\xad\x97"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        assert(!passport_names_text_valid(invalid[i], strlen(invalid[i])));
    }
    assert(!passport_names_text_valid("A\0B", 3));
}

static bool keep_access_points(const passport_name_key_t *key) { return key->kind == PN_SCAN_AP; }

int main(void)
{
    test_text();
    boot();
    assert(random_calls == 1 && passport_names_catalog(PN_BLE).known && passport_names_catalog(PN_WIFI).known);
    uint8_t material[] = {17, 3, 37}, bond_id[PASSPORT_BOND_ID_BYTES];
    passport_name_key_t bond = {.kind = PN_HID_BOND};
    assert(passport_names_bond_id(material, sizeof(material), bond.bond));
    passport_name_change_t change = change_for(bond, "Desk host");
    assert(submit_and_service(&change) == PN_OK && submit_and_service(&change) == PN_STALE);
    uint32_t revision = passport_names_catalog(PN_BLE).revision;
    boot();
    assert(random_calls == 1 && passport_names_bond_id(material, sizeof(material), bond_id));
    assert(!memcmp(bond_id, bond.bond, sizeof(bond_id)));
    passport_name_entry_t entry;
    assert(passport_names_entry(PN_BLE, revision, 0, &entry) == PN_OK && !strcmp(entry.label, "Desk host"));
    material[2]++;
    assert(passport_names_bond_id(material, sizeof(material), bond_id) && memcmp(bond_id, bond.bond, sizeof(bond_id)));

    passport_name_key_t profile = {.kind = PN_WIFI_PROFILE, .profile = 37};
    change = change_for(profile, "Studio");
    assert(submit_and_service(&change) == PN_OK);
    passport_name_key_t ap = {.kind = PN_SCAN_AP, .ap = {.ssid = "AP", .length = 2, .auth = 3}};
    for (unsigned n = 1; n <= PASSPORT_AP_NAMES; ++n) {
        ap.ap.bssid[5] = n;
        change = change_for(ap, "Duplicate name");
        assert(submit_and_service(&change) == PN_OK);
    }
    ap.ap.auth = 4;
    change = change_for(ap, "New auth");
    assert(submit_and_service(&change) == PN_FULL);
    char label[PASSPORT_NAME_BYTES + 1];
    assert(passport_names_lookup(&ap, label) == PN_NOT_FOUND && !label[0]);
    ap.ap.auth = 3;
    assert(passport_names_lookup(&ap, label) == PN_OK);
    assert(passport_names_prune(PN_WIFI, keep_access_points) == PN_OK);
    assert(passport_names_catalog(PN_WIFI).count == PASSPORT_AP_NAMES);
    assert(passport_names_lookup(&profile, label) == PN_NOT_FOUND);
    boot();
    assert(passport_names_lookup(&ap, label) == PN_OK && !strcmp(label, "Duplicate name"));
    change = change_for(ap, NULL);
    fail_commit = true;
    assert(submit_and_service(&change) == PN_UNCERTAIN);
    unsigned attempted = writes;
    assert(submit_and_service(&change) == PN_UNCERTAIN && writes == attempted);
    assert(passport_names_lookup(&ap, label) == PN_UNCERTAIN && !label[0]);
    assert(!passport_names_catalog(PN_WIFI).known && passport_names_catalog(PN_BLE).known);
    fail_commit = false;
    boot();
    assert(submit_and_service(&change) == PN_OK && passport_names_lookup(&ap, label) == PN_NOT_FOUND);
    fail_write = true;
    change = change_for(bond, NULL);
    assert(submit_and_service(&change) == PN_UNCERTAIN);
    fail_write = false;
    find_blob("wifi")->data[0] = 0;
    boot();
    assert(!passport_names_catalog(PN_WIFI).known && passport_names_catalog(PN_BLE).known);
    find_blob("bond_secret")->length = 0;
    boot();
    assert(!passport_names_catalog(PN_BLE).known && random_calls == 1);
    puts("PASS: label encoding, exact keys, capacity, stale mutations, reboot restore, and uncertain write isolation");
}
