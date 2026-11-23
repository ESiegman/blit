/*
 * libfst's configuration, as blit compiles it (fstapi.c reads this file when
 * FST_INCLUDE_CONFIG is defined). The other files here are libfst's own
 * src/ and LICENSE, unchanged, from https://github.com/gtkwave/libfst at
 * commit b2681f927a092a21b42ba1f3b2a9af831deaf58d (12 Nov 2026); to update
 * them, copy that folder's files over these and say the new commit here and
 * in THIRD_PARTY_NOTICES.md.
 *
 * No parallel writer (blit only reads), so no HAVE_LIBPTHREAD.
 */
#ifndef BLIT_FST_CONFIG_H
#define BLIT_FST_CONFIG_H

/* (What libfst's own build finds on Linux and macOS.) */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define HAVE_FSEEKO 1
#define HAVE_REALPATH 1

#endif
