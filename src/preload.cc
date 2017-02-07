/*
 * Copyright (c) 2016-2017 Carnegie Mellon University.
 *
 * All rights reserved.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file. See the AUTHORS file for names of contributors.
 */

#include <errno.h>
#include <assert.h>
#include <dirent.h>
#include <dlfcn.h>
#include <string.h>
#include <pthread.h>
#include <sys/stat.h>

#include <string>

#include "shuffle_internal.h"
#include "preload_internal.h"

#include "preload.h"

/* XXX: VPIC is usually a single-threaded process but mutex may be
 * needed if VPIC is running with openmp.
 */
static maybe_mutex_t maybe_mtx = MAYBE_MUTEX_INITIALIZER;

/* global context */
preload_ctx_t pctx = { 0 };


/* number of MPI barriers invoked by app */
static int num_barriers = 0;

/* number of epoches generated */
static int num_epochs = 0;


/*
 * we use the address of fake_dirptr as a fake DIR* with opendir/closedir
 */
static int fake_dirptr = 0;

/*
 * next_functions: libc replacement functions we are providing to the preloader.
 */
struct next_functions {
    /* functions we need */
    int (*MPI_Init)(int *argc, char ***argv);
    int (*MPI_Finalize)(void);
    int (*MPI_Barrier)(MPI_Comm comm);
    int (*mkdir)(const char *path, mode_t mode);
    DIR *(*opendir)(const char *filename);
    int (*closedir)(DIR *dirp);
    FILE *(*fopen)(const char *filename, const char *mode);
    size_t (*fwrite)(const void *ptr, size_t size, size_t nitems, FILE *stream);
    int (*fclose)(FILE *stream);

    /* for error catching we do these */
    int (*feof)(FILE *stream);
    int (*ferror)(FILE *stream);
    void (*clearerr)(FILE *stream);
    size_t (*fread)(void *ptr, size_t size, size_t nitems, FILE *stream);
    int (*fseek)(FILE *stream, long offset, int whence);
    long (*ftell)(FILE *stream);
};

static struct next_functions nxt = { 0 };

/*
 * this once is used to trigger the init of the preload library...
 */
static pthread_once_t init_once = PTHREAD_ONCE_INIT;

/* helper: must_getnextdlsym: get next symbol or fail */
static void must_getnextdlsym(void **result, const char *symbol)
{
    *result = dlsym(RTLD_NEXT, symbol);
    if (*result == NULL) msg_abort(symbol);
}

/*
 * preload_init: called via init_once.   if this fails we are sunk, so
 * we'll abort the process....
 */
static void preload_init()
{
    must_getnextdlsym(reinterpret_cast<void **>(&nxt.MPI_Init), "MPI_Init");
    must_getnextdlsym(reinterpret_cast<void **>(&nxt.MPI_Finalize), "MPI_Finalize");
    must_getnextdlsym(reinterpret_cast<void **>(&nxt.MPI_Barrier), "MPI_Barrier");
    must_getnextdlsym(reinterpret_cast<void **>(&nxt.mkdir), "mkdir");
    must_getnextdlsym(reinterpret_cast<void **>(&nxt.opendir), "opendir");
    must_getnextdlsym(reinterpret_cast<void **>(&nxt.closedir), "closedir");
    must_getnextdlsym(reinterpret_cast<void **>(&nxt.fopen), "fopen");
    must_getnextdlsym(reinterpret_cast<void **>(&nxt.fwrite), "fwrite");
    must_getnextdlsym(reinterpret_cast<void **>(&nxt.fclose), "fclose");

    must_getnextdlsym(reinterpret_cast<void **>(&nxt.feof), "feof");
    must_getnextdlsym(reinterpret_cast<void **>(&nxt.ferror), "ferror");
    must_getnextdlsym(reinterpret_cast<void **>(&nxt.clearerr), "clearerr");
    must_getnextdlsym(reinterpret_cast<void **>(&nxt.fread), "fread");
    must_getnextdlsym(reinterpret_cast<void **>(&nxt.fseek), "fseek");
    must_getnextdlsym(reinterpret_cast<void **>(&nxt.ftell), "ftell");

    pctx.logfd = -1;
    pctx.isdeltafs = new std::set<FILE*>;

    pctx.deltafs_root = getenv("PRELOAD_Deltafs_root");
    if (!pctx.deltafs_root) pctx.deltafs_root = DEFAULT_DELTAFS_ROOT;
    pctx.len_deltafs_root = strlen(pctx.deltafs_root);

    /* deltafs root:
     *   - any non-null path,
     *   - not "/", and
     *   - not ending in "/"
     */
    if ( pctx.len_deltafs_root == 0
            || (pctx.len_deltafs_root == 1 && pctx.deltafs_root[0] == '/')
            || pctx.deltafs_root[pctx.len_deltafs_root - 1] == '/' )
        msg_abort("bad deltafs_root");

    /* obtain the path to plfsdir */
    pctx.plfsdir = getenv("PRELOAD_Plfsdir");

    /* plfsdir:
     *   - if null, no plfsdir will ever be created
     *   - otherwise, it will be created and opened at MPI_Init
     */
    if (pctx.plfsdir == NULL) {
        if (pctx.deltafs_root[0] != '/') {
            /* default to deltafs_root if deltafs_root is relative */
            pctx.plfsdir = pctx.deltafs_root;
        }
    }
    if (pctx.plfsdir != NULL) {
        pctx.len_plfsdir = strlen(pctx.plfsdir);
    }

    pctx.plfsfd = -1;

    pctx.local_root = getenv("PRELOAD_Local_root");
    if (!pctx.local_root) pctx.local_root = DEFAULT_LOCAL_ROOT;
    pctx.len_local_root = strlen(pctx.local_root);

    /* local root:
     *   - any non-null path,
     *   - not "/",
     *   - starting with "/", and
     *   - not ending in "/"
     */
    if ( pctx.len_local_root == 0 || pctx.len_local_root == 1
            || pctx.local_root[0] != '/'
            || pctx.local_root[pctx.len_local_root - 1] == '/' )
        msg_abort("bad local_root");

    if (is_envset("PRELOAD_Bypass_shuffle"))
        pctx.mode |= BYPASS_SHUFFLE;
    if (is_envset("PRELOAD_Bypass_placement"))
        pctx.mode |= BYPASS_PLACEMENT;

    if (is_envset("PRELOAD_Bypass_deltafs_plfsdir"))
        pctx.mode |= BYPASS_DELTAFS_PLFSDIR;
    if (is_envset("PRELOAD_Bypass_deltafs_namespace"))
        pctx.mode |= BYPASS_DELTAFS_NAMESPACE;
    if (is_envset("PRELOAD_Bypass_deltafs"))
        pctx.mode |= BYPASS_DELTAFS;

    if (is_envset("PRELOAD_Skip_monitoring"))
        pctx.nomon = 1;
    if (is_envset("PRELOAD_Enable_verbose_error"))
        pctx.verbose = 1;
    if (is_envset("PRELOAD_Testing"))
        pctx.testin = 1;

    /* XXXCDC: additional init can go here or MPI_Init() */
}

/*
 * claim_path: look at path to see if we can claim it
 */
static bool claim_path(const char *path, bool *exact)
{

    if (strncmp(pctx.deltafs_root, path, pctx.len_deltafs_root) != 0) {
        return(false);
    } else if (path[pctx.len_deltafs_root] != '/' &&
            path[pctx.len_deltafs_root] != '\0') {
        return(false);
    }

    /* if we've just got pctx.root, caller may convert it to a "/" */
    *exact = (path[pctx.len_deltafs_root] == '\0');
    return(true);
}

/*
 * under_plfsdir: if a given path is a plfsdir or plfsdir files
 */
static bool under_plfsdir(const char* path)
{
    if (pctx.plfsdir == NULL) {
        return(false);
    } else if (strncmp(pctx.plfsdir, path, pctx.len_plfsdir) != 0) {
        return(false);
    } else if (path[pctx.len_plfsdir] != '/' &&
            path[pctx.len_plfsdir] != '\0') {
        return(false);
    } else {
        return(true);
    }
}

/*
 * claim_FILE: look at FILE* and see if we claim it
 */
static bool claim_FILE(FILE *stream)
{
    std::set<FILE *>::iterator it;
    bool rv;

    must_maybelockmutex(&maybe_mtx);
    assert(pctx.isdeltafs != NULL);
    it = pctx.isdeltafs->find(stream);
    rv = (it != pctx.isdeltafs->end());
    must_maybeunlock(&maybe_mtx);

    return(rv);
}

namespace {
/*
 * fake_file is a replacement for FILE* that we use to accumulate all the
 * VPIC particle data before sending it to the shuffle layer (on fclose).
 *
 * we assume only one thread is writing to the file at a time, so we
 * do not put a mutex on it.
 *
 * we ignore out of memory errors.
 */
class fake_file {
  private:
    std::string path_;       /* path of particle file (malloc'd c++) */
    char data_[64];          /* enough for one VPIC particle */
    char *dptr_;             /* ptr to next free space in data_ */
    size_t resid_;           /* residual */

  public:
    explicit fake_file(const char *path) :
        path_(path), dptr_(data_), resid_(sizeof(data_)) {};

    /* returns the actual number of bytes added. */
    size_t add_data(const void *toadd, size_t len) {
        int n = (len > resid_) ? resid_ : len;
        if (n) {
            memcpy(dptr_, toadd, n);
            dptr_ += n;
            resid_ -= n;
        }
        return(n);
    }

    /* get data length */
    size_t size() {
        return sizeof(data_) - resid_;
    }

    /* recover filename. */
    const char *file_name()  {
        return path_.c_str();
    }

    /* get data */
    char *data() {
        return data_;
    }
};
} // namespace

/*
 * here are the actual override functions from libc...
 */
extern "C" {

/*
 * MPI_Init
 */
int MPI_Init(int *argc, char ***argv)
{
    bool exact;
    const char* stripped;
    time_t now;
    char buf[50];
    char tmp[100];
    char path[PATH_MAX];
    char conf[100];
    int rank;
    int rv;
    int n;

    rv = pthread_once(&init_once, preload_init);
    if (rv) msg_abort("MPI_Init:pthread_once");

    rv = nxt.MPI_Init(argc, argv);
    if (rv == MPI_SUCCESS) {
        MPI_Comm_rank(MPI_COMM_WORLD, &rank);
        pctx.rank = rank;
    } else {
        return(rv);
    }

#if MPI_VERSION < 3
    if (rank == 0) {
        warn("using non-recent MPI release: some features disabled\n-- "
                "MPI ver 3 is suggested in production mode");
    }
#endif

#ifndef NDEBUG
    if (rank == 0) {
        warn("assertions enabled: code unnecessarily slow\n-- recompile with "
                "\"-DNDEBUG\" to disable assertions");
    }
#endif

    if (pctx.testin) {
        if (rank == 0) {
            warn("testing mode: code unnecessarily slow\n-- rerun with "
                    "\"export PRELOAD_Testing=0\" to "
                    "disable testing");
        }

        snprintf(path, sizeof(path), "/tmp/vpic-preload-%d.log", rank);

        pctx.logfd = open(path, O_WRONLY | O_CREAT | O_TRUNC |
                O_APPEND, 0777);

        if (pctx.logfd == -1) {
            msg_abort("cannot open log");
        } else {
            now = time(NULL);
            n = snprintf(tmp, sizeof(tmp), "%s\n--- trace ---\n",
                    ctime_r(&now, buf));
            n = write(pctx.logfd, tmp, n);

            errno = 0;
        }
    }

    if (rank == 0) {
        info("initializing ...");
    }

    if (!IS_BYPASS_SHUFFLE(pctx.mode)) {
        shuffle_init();

        nxt.MPI_Barrier(MPI_COMM_WORLD);

        if (rank == 0) {
            info("shuffle layer ready");
        }
    }

    /* pre-create plfsdirs if there is any */
    if (pctx.plfsdir != NULL) {
        if (!claim_path(pctx.plfsdir, &exact)) {
            msg_abort("plfsdir out of deltafs");  /* Oops!! */
        }

        /* relative paths we pass through; absolute we strip off prefix */

        if (pctx.plfsdir[0] != '/') {
            stripped = pctx.plfsdir;
        } else if (!exact) {
            stripped = pctx.plfsdir + pctx.len_deltafs_root;
        } else {
            msg_abort("bad plfsdir");
        }

        if (rank == 0) {
            if (IS_BYPASS_DELTAFS_NAMESPACE(pctx.mode) ||
                    IS_BYPASS_DELTAFS(pctx.mode)) {
                snprintf(path, sizeof(path), "%s/%s", pctx.local_root,
                        stripped);
                rv = nxt.mkdir(path, 0777);
            } else if (!IS_BYPASS_DELTAFS_PLFSDIR(pctx.mode)) {
                rv = deltafs_mkdir(stripped, 0777 | DELTAFS_DIR_PLFS_STYLE);
            } else {
                rv = deltafs_mkdir(stripped, 0777);
            }

            if (rv != 0) {
                msg_abort("cannot make plfsdir");
            }
        }

        /* so everyone sees the dir created */
        nxt.MPI_Barrier(MPI_COMM_WORLD);

        /* everyone opens it */
        if (IS_BYPASS_DELTAFS_NAMESPACE(pctx.mode)) {
            snprintf(path, sizeof(path), "%s/%s", pctx.local_root, stripped);
            snprintf(conf, sizeof(conf), "rank=%d", rank);

            pctx.plfsh = deltafs_plfsdir_create(path, conf);
            if (pctx.plfsh == NULL) {
                msg_abort("cannot open plfsdir");
            } else if (rank == 0) {
                info("LW plfs dir opened");
            }
        } else if (!IS_BYPASS_DELTAFS_PLFSDIR(pctx.mode) &&
                !IS_BYPASS_DELTAFS(pctx.mode)) {
            pctx.plfsfd = deltafs_open(stripped, O_WRONLY | O_DIRECTORY, 0);
            if (pctx.plfsfd == -1) {
                msg_abort("cannot open plfsdir");
            } else if (rank == 0) {
                info("plfs dir opened");
            }
        }

        nxt.MPI_Barrier(MPI_COMM_WORLD);
    }

    trace("MPI init done");

    return(rv);
}

/*
 * MPI_Barrier
 */
int MPI_Barrier(MPI_Comm comm)
{
    int rv = nxt.MPI_Barrier(comm);

    num_barriers++;

    return(rv);
}

/*
 * MPI_Finalize
 */
int MPI_Finalize(void)
{
    int rv;

    rv = pthread_once(&init_once, preload_init);
    if (rv) msg_abort("MPI_Finalize:pthread_once");

    trace("MPI finalizing ... ");

    if (pctx.rank == 0) info("finalizing ... ");

    if (!IS_BYPASS_SHUFFLE(pctx.mode)) {
        /* ensures all peer messages are handled */
        nxt.MPI_Barrier(MPI_COMM_WORLD);
        shuffle_destroy();

        if (pctx.rank == 0) {
            info("shuffle layer closed");
        }
    }

    /* all writes done, time to close all plfsdirs */
    if (pctx.plfsh != NULL) {
        deltafs_plfsdir_close(pctx.plfsh);
        pctx.plfsh = NULL;

        if (pctx.rank == 0) {
            info("LW plfs dir closed");
        }
    }

    if (pctx.plfsfd != -1) {
        deltafs_close(pctx.plfsfd);
        pctx.plfsfd = -1;

        if (pctx.rank == 0) {
            info("plfs dir closed");
        }
    }

    /* close log file*/
    if (pctx.logfd != -1) {
        close(pctx.logfd);
        pctx.logfd = -1;
    }

    /* !!! OK !!! */
    if (pctx.rank == 0) info("all done");
    rv = nxt.MPI_Finalize();
    return(rv);
}

/*
 * mkdir
 */
int mkdir(const char *dir, mode_t mode)
{
    bool exact;
    const char *stripped;
    char path[PATH_MAX];
    int rv;

    rv = pthread_once(&init_once, preload_init);
    if (rv) msg_abort("mkdir:pthread_once");

    if (!claim_path(dir, &exact)) {
        return(nxt.mkdir(dir, mode));
    } else if (under_plfsdir(dir)) {
        return(0);  /* plfsdirs are pre-created at MPI_Init */
    }

    /* relative paths we pass through; absolute we strip off prefix */

    if (*dir != '/') {
        stripped = dir;
    } else {
        stripped = (exact) ? "/" : (dir + pctx.len_deltafs_root);
    }

    if (IS_BYPASS_DELTAFS_NAMESPACE(pctx.mode) ||
            IS_BYPASS_DELTAFS(pctx.mode)) {
        snprintf(path, sizeof(path), "%s/%s", pctx.local_root, stripped);
        rv = nxt.mkdir(path, mode);
    } else {
        rv = deltafs_mkdir(stripped, mode);
    }

    if (rv != 0) {
        if (pctx.verbose) {
            error("mkdir:deltafs_mkdir");
        }
    }

    return(rv);
}

/*
 * opendir
 */
DIR *opendir(const char *dir)
{
    bool exact;
    const char *stripped;
    DIR* rv;

    int ret = pthread_once(&init_once, preload_init);
    if (ret) msg_abort("opendir:pthread_once");

    if (!claim_path(dir, &exact)) {
        return(nxt.opendir(dir));
    } else if (!under_plfsdir(dir)) {
        return(NULL);  /* not supported */
    }

    num_epochs++;
    mon_reinit(&mctx);  /* reset mon stats */
    mctx.epoch_start = now_micros();
    mctx.epoch_seq = num_epochs;

    trace("epoch stamped");

    /* relative paths we pass through; absolute we strip off prefix */

    if (*dir != '/') {
        stripped = dir;
    } else {
        stripped = (exact) ? "/" : (dir + pctx.len_deltafs_root);
    }

    /* return a fake DIR* since we don't actually open */
    rv = reinterpret_cast<DIR*>(&fake_dirptr);

    trace("now flush data ... ");

    if (IS_BYPASS_DELTAFS_NAMESPACE(pctx.mode)) {
        if (pctx.plfsh != NULL) {
            deltafs_plfsdir_epoch_flush(pctx.plfsh, NULL);
        } else {
            msg_abort("plfs not opened");
        }

    } else if (!IS_BYPASS_DELTAFS_PLFSDIR(pctx.mode) &&
            !IS_BYPASS_DELTAFS(pctx.mode)) {
        if (pctx.plfsfd != -1 ) {
            deltafs_epoch_flush(pctx.plfsfd, NULL);
        } else {
            msg_abort("plfs not opened");
        }

    } else {
        /* no op */
    }

    /*
     * this ensures all writes from the new epoch will go into a
     * new write buffer
     */
    nxt.MPI_Barrier(MPI_COMM_WORLD);

    trace("epoch begins");

    return(rv);
}

/*
 * closedir
 */
int closedir(DIR *dirp)
{
    mon_ctx_t sum;
    int rv;

    rv = pthread_once(&init_once, preload_init);
    if (rv) msg_abort("closedir:pthread_once");

    if (dirp != reinterpret_cast<DIR*>(&fake_dirptr)) {
        return(nxt.closedir(dirp));

    } else {  /* deltafs */

        if (!pctx.nomon) {
            mctx.dura = now_micros() - mctx.epoch_start;
            if (pctx.testin) {
                if (pctx.logfd != -1) {
                    mon_dumpstate(pctx.logfd, &mctx);
                }
            }

            if (pctx.rank == 0) info("merging epoch mon stats ...");

            mon_reduce(&mctx, &sum);

            if (pctx.rank == 0) {
                info("dumping epoch mon stats ...");
                mon_dumpstate(fileno(stderr), &sum);
                info("dumping done");
            }
        }

        trace("epoch ends");

        return(0);
    }
}

/*
 * fopen
 */
FILE *fopen(const char *fname, const char *mode)
{
    bool exact;
    const char *stripped;
    FILE* rv;

    int ret = pthread_once(&init_once, preload_init);
    if (ret) msg_abort("fopen:pthread_once");

    if (!claim_path(fname, &exact)) {
        return(nxt.fopen(fname, mode));
    } else if (!under_plfsdir(fname)) {
        return(NULL);  /* XXX: support this */
    }

    /* relative paths we pass through; absolute we strip off prefix */

    if (*fname != '/') {
        stripped = fname;
    } else {
        stripped = (exact) ? "/" : (fname + pctx.len_deltafs_root);
    }

    /* allocate a fake FILE* and put it in the set */
    fake_file *ff = new fake_file(stripped);
    rv = reinterpret_cast<FILE*>(ff);

    must_maybelockmutex(&maybe_mtx);
    assert(pctx.isdeltafs != NULL);
    pctx.isdeltafs->insert(rv);
    must_maybeunlock(&maybe_mtx);

    return(rv);
}

/*
 * fwrite
 */
size_t fwrite(const void *ptr, size_t size, size_t nitems, FILE *stream)
{
    int rv;

    rv = pthread_once(&init_once, preload_init);
    if (rv) msg_abort("fwrite:pthread_once");

    if (!claim_FILE(stream)) {
        return(nxt.fwrite(ptr, size, nitems, stream));
    }

    fake_file *ff = reinterpret_cast<fake_file*>(stream);
    size_t cnt = ff->add_data(ptr, size * nitems);

    /*
     * fwrite returns number of items written.  it can return a short
     * object count on error.
     */

    return(cnt / size);    /* truncates on error */
}

/*
 * fclose.   returns EOF on error.
 */
int fclose(FILE *stream)
{
    int rv;

    rv = pthread_once(&init_once, preload_init);
    if (rv) msg_abort("fclose:pthread_once");

    if (!claim_FILE(stream)) {
        return(nxt.fclose(stream));
    }

    fake_file *ff = reinterpret_cast<fake_file*>(stream);

    if (IS_BYPASS_SHUFFLE(pctx.mode)) {
        /*
         * preload_write() will check if
         *   - BYPASS_DELTAFS_PLFSDIR
         *   - BYPASS_DELTAFS
         */
        rv = mon_preload_write(ff->file_name(), ff->data(), ff->size(),
                &mctx);
        if (rv != 0) {
            if (pctx.verbose) {
                error("fclose:preload_write");
            }
        }
    } else {
        /*
         * shuffle_write() will check if
         *   - BYPASS_PLACEMENT
         */
        rv = mon_shuffle_write(ff->file_name(), ff->data(), ff->size(),
                &mctx);
        if (rv != 0) {
            if (pctx.verbose) {
                error("fclose:shuffle_write");
            }
        }
    }

    must_maybelockmutex(&maybe_mtx);
    assert(pctx.isdeltafs != NULL);
    pctx.isdeltafs->erase(stream);
    must_maybeunlock(&maybe_mtx);

    delete ff;

    return(rv);
}

/*
 * preload_write
 */
int preload_write(const char *fn, char *data, size_t len)
{
    int rv;
    char path[PATH_MAX];
    ssize_t n;
    int fd;

    /* Return 0 on success, or EOF on errors */

    rv = EOF;

    if (IS_BYPASS_DELTAFS_NAMESPACE(pctx.mode)) {
        if (pctx.plfsh == NULL) {
            msg_abort("plfsdir not opened");
        }

        rv = deltafs_plfsdir_append(pctx.plfsh, fn + pctx.len_plfsdir + 1,
                data, len);

        if (rv != 0) {
            rv = EOF;
        }

    } else if (IS_BYPASS_DELTAFS(pctx.mode)) {
        snprintf(path, sizeof(path), "%s/%s", pctx.local_root, fn);
        fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0666);

        if (fd != -1) {
            n = write(fd, data, len);
            if (n == len) {
                rv = 0;
            }
            close(fd);
        }
    } else {
        if (pctx.plfsfd == -1) {
            msg_abort("plfsdir not opened");
        }

        fd = deltafs_openat(pctx.plfsfd, fn + pctx.len_plfsdir + 1,
                O_WRONLY | O_CREAT | O_APPEND, 0666);

        if (fd != -1) {
            n = deltafs_write(fd, data, len);
            if (n == len) {
                rv = 0;
            }
            deltafs_close(fd);
        }
    }

    return(rv);
}

/*
 * the rest of these we do not override for deltafs.   if we get a
 * deltafs FILE*, we've got a serious problem and we abort...
 */

/*
 * feof
 */
int feof(FILE *stream)
{
    int rv;
    rv = pthread_once(&init_once, preload_init);
    if (rv) msg_abort("feof:pthread_once");

    if (!claim_FILE(stream)) {
        return(nxt.feof(stream));
    }

    errno = ENOTSUP;
    msg_abort("feof!");
    return(0);
}

/*
 * ferror
 */
int ferror(FILE *stream)
{
    int rv;
    rv = pthread_once(&init_once, preload_init);
    if (rv) msg_abort("ferror:pthread_once");

    if (!claim_FILE(stream)) {
        return(nxt.ferror(stream));
    }

    errno = ENOTSUP;
    msg_abort("ferror!");
    return(0);
}

/*
 * clearerr
 */
void clearerr(FILE *stream)
{
    int rv;
    rv = pthread_once(&init_once, preload_init);
    if (rv) msg_abort("clearerr:pthread_once");

    if (!claim_FILE(stream)) {
        nxt.clearerr(stream);
        return;
    }

    errno = ENOTSUP;
    msg_abort("clearerr!");
}

/*
 * fread
 */
size_t fread(void *ptr, size_t size, size_t nitems, FILE *stream)
{
    int rv;
    rv = pthread_once(&init_once, preload_init);
    if (rv) msg_abort("fread:pthread_once");

    if (!claim_FILE(stream)) {
        return(nxt.fread(ptr, size, nitems, stream));
    }

    errno = ENOTSUP;
    msg_abort("fread!");
    return(0);
}

/*
 * fseek
 */
int fseek(FILE *stream, long offset, int whence)
{
    int rv;
    rv = pthread_once(&init_once, preload_init);
    if (rv) msg_abort("fseek:pthread_once");

    if (!claim_FILE(stream)) {
        return(nxt.fseek(stream, offset, whence));
    }

    errno = ENOTSUP;
    msg_abort("fseek!");
    return(0);
}

/*
 * ftell
 */
long ftell(FILE *stream)
{
    int rv;
    rv = pthread_once(&init_once, preload_init);
    if (rv) msg_abort("ftell:pthread_once");

    if (!claim_FILE(stream)) {
        return(nxt.ftell(stream));
    }

    errno = ENOTSUP;
    msg_abort("ftell!");
    return(0);
}

}   /* extern "C" */
