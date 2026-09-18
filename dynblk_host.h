/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DYNBLK_HOST_H
#define DYNBLK_HOST_H
#include "dynblk_engine.h"
struct de_host {
    int directory, *files;
    unsigned int file_slots;
    bool readonly, direct, strict;
    char parent[4096], error[512];
    void *bounce;
    db_u64 sync_count, read_bytes, write_bytes;
};
int dh_init(struct de_host *, struct de_io *, const char *, bool, bool, bool);
void dh_close(struct de_host *);
#endif
