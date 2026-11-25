/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "kode.h"

unsigned g_kode_inits;

void kode_init(void)
{
    g_kode_inits++;
}
