// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "app_ui.h"
typedef enum { UI_TRUST_NORMAL, UI_TRUST_PUBLIC_CAPACITY, UI_TRUST_PACKAGE_CAPACITY, UI_TRUST_MINIMAL } ui_trust_fixture_kind_t;
void ui_trust_fixture_open(app_view_t *, ui_trust_fixture_kind_t);
void ui_trust_fixture_handle(app_view_t *, const app_intent_t *);
void ui_trust_fixture_change(app_view_t *, unsigned invalid_field);
void ui_trust_fixture_finish(app_view_t *);
unsigned ui_trust_fixture_approvals(void);
