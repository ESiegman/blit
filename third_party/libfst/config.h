/*
 * libfst's configuration, as blit compiles it (fstapi.c reads this file when
 * FST_INCLUDE_CONFIG is defined). The other files here are libfst's own
 * src/ and LICENSE, unchanged, from https://github.com/gtkwave/libfst at
 * commit b2681f927a092a21b42ba1f3b2a9af831deaf58d (12 Nov 2026); to update
 * them, copy that folder's files over these and say the new commit here and
 * in THIRD_PARTY_NOTICES.md.
 *
 * No parallel writer (blit only reads), so no HAVE_LIBPTHREAD.
 *
 * A file that is cut short or damaged must not take the editor down, and
 * libfst's reader, given one, can end the process or never finish (Nov
 * 2026, every byte of real files changed in turn, tests/timeline_test.cpp):
 *
 * - its own checks end it: abort() after a security check, exit(255) when a
 *   block does not uncompress;
 * - a length read from the file is believed and allocated, and the
 *   allocation's failure not checked, so the next write is to address 0;
 *   or the allocation, of a terabyte, succeeds, and is written out whole
 *   to a file of its own;
 * - a name in a hierarchy cut short is read until a 0 that never comes, at
 *   the end of the file.
 *
 * So, without a change to libfst's own files, each of these goes back
 * instead to where the reader (src/wavefst.cpp) last called blit_fst_catch,
 * which says the file is damaged: abort() and exit(), an allocation of
 * some bytes that fails or that is more than the file could hold (as
 * blit_fst_catch is told: no block of an FST, however it is compressed,
 * comes to more than a few thousand times its size), and a million reads
 * in a row at the end of a file. The files libfst opened for it are kept
 * track of, and blit_fst_release closes those still open; what it had
 * allocated is not freed (its state cannot be trusted to free it): a
 * damaged file costs a little memory, not the editor. With no
 * blit_fst_catch in force (another program built from these sources) each
 * is as libfst has it.
 */
#ifndef BLIT_FST_CONFIG_H
#define BLIT_FST_CONFIG_H

/* (What libfst's own build finds on Linux and macOS.) */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define HAVE_FSEEKO 1
#define HAVE_REALPATH 1

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <zlib.h>

static _Thread_local jmp_buf *blit_fst_jump;
static _Thread_local size_t blit_fst_most;
static _Thread_local unsigned long blit_fst_eofs;

/* Where libfst's checks go back to, on this thread (NULL: nowhere), and
   the most it may allocate at once. */
void blit_fst_catch(jmp_buf *to, size_t most);
void blit_fst_catch(jmp_buf *to, size_t most)
{
    blit_fst_jump = to;
    blit_fst_most = most;
    blit_fst_eofs = 0;
}

/* The files (and gzip readers) libfst opens while blit_fst_catch is in
   force, until it closes them. */
#define BLIT_FST_FILES 16
static _Thread_local FILE *blit_fst_files[BLIT_FST_FILES];
static _Thread_local gzFile blit_fst_gz[BLIT_FST_FILES];

static void blit_fst_keep(void **list, void *p)
{
    if (!p || !blit_fst_jump) {
        return;
    }
    for (int i = 0; i < BLIT_FST_FILES; i++) {
        if (!list[i]) {
            list[i] = p;
            return;
        }
    }
}

static void blit_fst_drop(void **list, void *p)
{
    for (int i = 0; i < BLIT_FST_FILES; i++) {
        if (list[i] == p) {
            list[i] = NULL;
        }
    }
}

static FILE *blit_fst_fopen(const char *name, const char *mode)
{
    FILE *f = fopen(name, mode);
    blit_fst_keep((void **)blit_fst_files, f);
    return f;
}

static FILE *blit_fst_tmpfile(void)
{
    FILE *f = tmpfile();
    blit_fst_keep((void **)blit_fst_files, f);
    return f;
}

static int blit_fst_fclose(FILE *f)
{
    blit_fst_drop((void **)blit_fst_files, f);
    return fclose(f);
}

static gzFile blit_fst_gzdopen(int fd, const char *mode)
{
    gzFile g = gzdopen(fd, mode);
    blit_fst_keep((void **)blit_fst_gz, g);
    return g;
}

static int blit_fst_gzclose(gzFile g)
{
    blit_fst_drop((void **)blit_fst_gz, g);
    return gzclose(g);
}

/* After libfst came back to blit_fst_catch's place: the files it left
   open, closed. */
void blit_fst_release(void);
void blit_fst_release(void)
{
    for (int i = 0; i < BLIT_FST_FILES; i++) {
        if (blit_fst_files[i]) {
            fclose(blit_fst_files[i]);
            blit_fst_files[i] = NULL;
        }
        if (blit_fst_gz[i]) {
            gzclose(blit_fst_gz[i]);
            blit_fst_gz[i] = NULL;
        }
    }
}

static _Noreturn void blit_fst_fail(int code)
{
    if (blit_fst_jump) {
        longjmp(*blit_fst_jump, 1);
    }
    if (code < 0) {
        abort();
    }
    exit(code);
}

static void *blit_fst_held(void *p, size_t n)
{
    if (!p && n && blit_fst_jump) {
        longjmp(*blit_fst_jump, 1);
    }
    return p;
}

static void blit_fst_size(size_t n, size_t size)
{
    if (blit_fst_jump && size && n > blit_fst_most / size) {
        longjmp(*blit_fst_jump, 1);
    }
}

static void *blit_fst_malloc(size_t n)
{
    blit_fst_size(n, 1);
    return blit_fst_held(malloc(n), n);
}

static void *blit_fst_calloc(size_t n, size_t size)
{
    blit_fst_size(n, size);
    return blit_fst_held(calloc(n, size), n * size);
}

static void *blit_fst_realloc(void *p, size_t n)
{
    blit_fst_size(n, 1);
    return blit_fst_held(realloc(p, n), n);
}

static int blit_fst_fgetc(FILE *f)
{
    int c = fgetc(f);
    if (c != EOF) {
        blit_fst_eofs = 0;
    } else if (++blit_fst_eofs > 1000000 && blit_fst_jump) {
        longjmp(*blit_fst_jump, 1);
    }
    return c;
}

#define abort() blit_fst_fail(-1)
#define exit(code) blit_fst_fail(code)
#define malloc(n) blit_fst_malloc(n)
#define calloc(n, size) blit_fst_calloc((n), (size))
#define realloc(p, n) blit_fst_realloc((p), (n))
#define fgetc(f) blit_fst_fgetc(f)
#define fopen(name, mode) blit_fst_fopen((name), (mode))
#define tmpfile() blit_fst_tmpfile()
#define fclose(f) blit_fst_fclose(f)
#define gzdopen(fd, mode) blit_fst_gzdopen((fd), (mode))
#define gzclose(g) blit_fst_gzclose(g)

#endif
