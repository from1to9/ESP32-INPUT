// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "control_json.h"
typedef struct {
    const char *key;
    const cs_node_t *schema;
    cp_scope_mask_t scope;
    const char *default_json, *apply_policy, *persistence;
    bool sensitive;
} cp_config_info_t;
const cs_node_t *cp_argument_schema(cp_operation_id_t);
size_t cp_config_count(void);
const cp_config_info_t *cp_config_at(size_t);
const cp_config_info_t *cp_config_find(const char *);
