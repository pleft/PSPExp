/*
 * The file manager's work on the file system, apart from what it looks like
 * (files.c): folder listings, and the long jobs (copy, move, delete) run on
 * a thread with progress the view polls and a cancel it can set.
 */
#ifndef PSPEXP_FILES_OPS_H
#define PSPEXP_FILES_OPS_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

#define FOPS_PATH 1024

typedef struct FEntry {
    char name[256];
    int is_dir, is_link, is_up;   /* is_up: the ".." row */
    uint64_t size;
    time_t mtime;
    mode_t mode;
    int marked;                   /* the view's */
} FEntry;

typedef struct FList { FEntry *e; int n, cap; } FList;

enum { FSORT_NAME, FSORT_SIZE, FSORT_DATE };

/* dir's entries: ".." first (not at "/"), then folders, then files, each
 * sorted; hidden ones (".name") only with show_hidden. 0, or -1 with errno */
int  fops_list(const char *dir, int show_hidden, int sort, FList *out);
void fops_free(FList *l);

void fops_join(char *out, size_t n, const char *dir, const char *name);
/* "/a/b/c" -> "/a/b"; returns the part removed ("c"), in a static buffer */
const char *fops_parent(char *path);
/* 1536 -> "1.5 KB" */
void fops_human(uint64_t bytes, char *out, size_t n);
/* the name has no '/' and is not "." or ".." */
int  fops_name_ok(const char *name);

int  fops_mkdir(const char *path, char *err, size_t en);
int  fops_rename(const char *from, const char *to, char *err, size_t en);

/* ---- jobs ---------------------------------------------------------------- */

enum { FJOB_COPY, FJOB_MOVE, FJOB_DELETE };

typedef struct FJob {
    /* in */
    int kind;
    char src[FOPS_PATH], dst[FOPS_PATH];   /* folders; dst unused for delete */
    char (*names)[256]; int nnames;         /* in src */
    /* progress, written by the job */
    volatile uint64_t bytes_done, bytes_total;
    volatile int files_done, files_total;
    char current[256];
    volatile int cancel;                    /* set by the view */
    volatile int done;                      /* 1 when finished */
    char error[256];                        /* "" when all went well */
    void *thread;
} FJob;

/* starts job on its own thread; the view polls done */
int  fjob_start(FJob *job);
/* after done: the thread joined, the names freed */
void fjob_finish(FJob *job);
/* the same job, run here (tests) */
void fjob_run(FJob *job);

/* names that would be replaced in dst (copy / move): how many, the first */
int  fops_conflicts(const char *dst, char (*names)[256], int n, char *first, size_t fn);

#endif
