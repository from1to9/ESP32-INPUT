// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stddef.h>
#define __containerof(pointer, type, member) ((type *)((char *)(pointer) - offsetof(type, member)))
