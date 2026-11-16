#pragma once
#include "passport_names.h"

// One owning task per domain. Readers use the public snapshot interface.
void passport_names_load(passport_name_domain_t domain);
bool passport_names_take(passport_name_domain_t domain, passport_name_change_t *change);
void passport_names_finish(passport_name_domain_t domain, passport_name_result_t result);
passport_name_result_t passport_names_apply(const passport_name_change_t *change);
passport_name_result_t passport_names_remove(const passport_name_key_t *key);
passport_name_result_t passport_names_prune(passport_name_domain_t domain,
                                            bool (*retained)(const passport_name_key_t *));
bool passport_names_bond_id(const uint8_t *material, size_t length, uint8_t id[PASSPORT_BOND_ID_BYTES]);
