/*
 * Copyright (c) Yann Collet, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */


/* *************************************
*  Compiler Options
***************************************/
#ifdef _MSC_VER   /* Visual */
#  pragma warning(disable : 4127)  /* disable: C4127: conditional expression is constant */
#  pragma warning(disable : 4204)  /* non-constant aggregate initializer */
#endif
#if defined(__MINGW32__) && !defined(_POSIX_SOURCE)
#  define _POSIX_SOURCE 1          /* disable %llu warnings with MinGW on Windows */
#endif

/*-*************************************
*  Includes
***************************************/
#include "platform.h"   /* Large Files support, SET_BINARY_MODE */
#include "util.h"       /* UTIL_getFileSize, UTIL_isRegularFile, UTIL_isSameFile */
#include <stdio.h>      /* fprintf, open, fdopen, fread, _fileno, stdin, stdout */
#include <stdlib.h>     /* malloc, free */
#include <string.h>     /* strcmp, strlen */
#include <fcntl.h>      /* O_WRONLY */
#include <assert.h>
#include <errno.h>      /* errno */
#include <limits.h>     /* INT_MAX */
#include <signal.h>
#include "timefn.h"     /* UTIL_getTime, UTIL_clockSpanMicro */

#if defined (_MSC_VER)
#  include <sys/stat.h>
#  include <io.h>
#endif

#include "../lib/common/mem.h"     /* U32, U64 */
#include "fileio.h"

#define ZSTD_STATIC_LINKING_ONLY   /* ZSTD_magicNumber, ZSTD_frameHeaderSize_max */
#include "../lib/zstd.h"
#include "../lib/zstd_errors.h"  /* ZSTD_error_frameParameter_windowTooLarge */

#if defined(ZSTD_GZCOMPRESS) || defined(ZSTD_GZDECOMPRESS)
#  include <zlib.h>
#  if !defined(z_const)
#    define z_const
#  endif
#endif

#if defined(ZSTD_LZMACOMPRESS) || defined(ZSTD_LZMADECOMPRESS)
#  include <lzma.h>
#endif

#define LZ4_MAGICNUMBER 0x184D2204
#if defined(ZSTD_LZ4COMPRESS) || defined(ZSTD_LZ4DECOMPRESS)
#  define LZ4F_ENABLE_OBSOLETE_ENUMS
#  include <lz4frame.h>
#  include <lz4.h>
#endif


/*-*************************************
*  Constants
***************************************/
#define ADAPT_WINDOWLOG_DEFAULT 23   /* 8 MB */
#define DICTSIZE_MAX (32 MB)   /* protection against large input (attack scenario) */

#define FNSPACE 30

/* Default file permissions 0666 (modulated by umask) */
#if !defined(_WIN32)
/* These macros aren't defined on windows. */
#define DEFAULT_FILE_PERMISSIONS (S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH|S_IWOTH)
#else
#define DEFAULT_FILE_PERMISSIONS (0666)
#endif

/*-*************************************
*  Macros
***************************************/
#define KB *(1 <<10)
#define MB *(1 <<20)
#define GB *(1U<<30)
#undef MAX
#define MAX(a,b) ((a)>(b) ? (a) : (b))

struct FIO_display_prefs_s {
    int displayLevel;   /* 0 : no display;  1: errors;  2: + result + interaction + warnings;  3: + progression;  4: + information */
    FIO_progressSetting_e progressSetting;
};

static FIO_display_prefs_t g_display_prefs = {2, FIO_ps_auto};

#define DISPLAY(...)         fprintf(stderr, __VA_ARGS__)
#define DISPLAYOUT(...)      fprintf(stdout, __VA_ARGS__)
#define DISPLAYLEVEL(l, ...) { if (g_display_prefs.displayLevel>=l) { DISPLAY(__VA_ARGS__); } }

static const U64 g_refreshRate = SEC_TO_MICRO / 6;
static UTIL_time_t g_displayClock = UTIL_TIME_INITIALIZER;

#define READY_FOR_UPDATE() ((g_display_prefs.progressSetting != FIO_ps_never) && UTIL_clockSpanMicro(g_displayClock) > g_refreshRate)
#define DELAY_NEXT_UPDATE() { g_displayClock = UTIL_getTime(); }
#define DISPLAYUPDATE(l, ...) {                              \
        if (g_display_prefs.displayLevel>=l && (g_display_prefs.progressSetting != FIO_ps_never)) { \
            if (READY_FOR_UPDATE() || (g_display_prefs.displayLevel>=4)) { \
                DELAY_NEXT_UPDATE();                         \
                DISPLAY(__VA_ARGS__);                        \
                if (g_display_prefs.displayLevel>=4) fflush(stderr);       \
    }   }   }

#undef MIN  /* in case it would be already defined */
#define MIN(a,b)    ((a) < (b) ? (a) : (b))


#define EXM_THROW(error, ...)                                             \
{                                                                         \
    DISPLAYLEVEL(1, "zstd: ");                                            \
    DISPLAYLEVEL(5, "Error defined at %s, line %i : \n", __FILE__, __LINE__); \
    DISPLAYLEVEL(1, "error %i : ", error);                                \
    DISPLAYLEVEL(1, __VA_ARGS__);                                         \
    DISPLAYLEVEL(1, " \n");                                               \
    exit(error);                                                          \
}

#define CHECK_V(v, f)                                \
    v = f;                                           \
    if (ZSTD_isError(v)) {                           \
        DISPLAYLEVEL(5, "%s \n", #f);                \
        EXM_THROW(11, "%s", ZSTD_getErrorName(v));   \
    }
#define CHECK(f) { size_t err; CHECK_V(err, f); }


/*-************************************
*  Signal (Ctrl-C trapping)
**************************************/
static const char* g_artefact = NULL;
static void INThandler(int sig)
{
    assert(sig==SIGINT); (void)sig;
#if !defined(_MSC_VER)
    signal(sig, SIG_IGN);  /* this invocation generates a buggy warning in Visual Studio */
#endif
    if (g_artefact) {
        assert(UTIL_isRegularFile(g_artefact));
        remove(g_artefact);
    }
    DISPLAY("\n");
    exit(2);
}
static void addHandler(char const* dstFileName)
{
    if (UTIL_isRegularFile(dstFileName)) {
        g_artefact = dstFileName;
        signal(SIGINT, INThandler);
    } else {
        g_artefact = NULL;
    }
}
/* Idempotent */
static void clearHandler(void)
{
    if (g_artefact) signal(SIGINT, SIG_DFL);
    g_artefact = NULL;
}


/*-*********************************************************
*  Termination signal trapping (Print debug stack trace)
***********************************************************/
#if defined(__has_feature) && !defined(BACKTRACE_ENABLE) /* Clang compiler */
#  if (__has_feature(address_sanitizer))
#    define BACKTRACE_ENABLE 0
#  endif /* __has_feature(address_sanitizer) */
#elif defined(__SANITIZE_ADDRESS__) && !defined(BACKTRACE_ENABLE) /* GCC compiler */
#  define BACKTRACE_ENABLE 0
#endif

#if !defined(BACKTRACE_ENABLE)
/* automatic detector : backtrace enabled by default on linux+glibc and osx */
#  if (defined(__linux__) && (defined(__GLIBC__) && !defined(__UCLIBC__))) \
     || (defined(__APPLE__) && defined(__MACH__))
#    define BACKTRACE_ENABLE 1
#  else
#    define BACKTRACE_ENABLE 0
#  endif
#endif

/* note : after this point, BACKTRACE_ENABLE is necessarily defined */


#if BACKTRACE_ENABLE

#include <execinfo.h>   /* backtrace, backtrace_symbols */

#define MAX_STACK_FRAMES    50

static void ABRThandler(int sig) {
    const char* name;
    void* addrlist[MAX_STACK_FRAMES];
    char** symbollist;
    int addrlen, i;

    switch (sig) {
        case SIGABRT: name = "SIGABRT"; break;
        case SIGFPE: name = "SIGFPE"; break;
        case SIGILL: name = "SIGILL"; break;
        case SIGINT: name = "SIGINT"; break;
        case SIGSEGV: name = "SIGSEGV"; break;
        default: name = "UNKNOWN";
    }

    DISPLAY("Caught %s signal, printing stack:\n", name);
    /* Retrieve current stack addresses. */
    addrlen = backtrace(addrlist, MAX_STACK_FRAMES);
    if (addrlen == 0) {
        DISPLAY("\n");
        return;
    }
    /* Create readable strings to each frame. */
    symbollist = backtrace_symbols(addrlist, addrlen);
    /* Print the stack trace, excluding calls handling the signal. */
    for (i = ZSTD_START_SYMBOLLIST_FRAME; i < addrlen; i++) {
        DISPLAY("%s\n", symbollist[i]);
    }
    free(symbollist);
    /* Reset and raise the signal so default handler runs. */
    signal(sig, SIG_DFL);
    raise(sig);
}
#endif

void FIO_addAbortHandler()
{
#if BACKTRACE_ENABLE
    signal(SIGABRT, ABRThandler);
    signal(SIGFPE, ABRThandler);
    signal(SIGILL, ABRThandler);
    signal(SIGSEGV, ABRThandler);
    signal(SIGBUS, ABRThandler);
#endif
}


/*-************************************************************
* Avoid fseek()'s 2GiB barrier with MSVC, macOS, *BSD, MinGW
***************************************************************/
#if defined(_MSC_VER) && _MSC_VER >= 1400
#   define LONG_SEEK _fseeki64
#   define LONG_TELL _ftelli64
#elif !defined(__64BIT__) && (PLATFORM_POSIX_VERSION >= 200112L) /* No point defining Large file for 64 bit */
#  define LONG_SEEK fseeko
#  define LONG_TELL ftello
#elif defined(__MINGW32__) && !defined(__STRICT_ANSI__) && !defined(__NO_MINGW_LFS) && defined(__MSVCRT__)
#   define LONG_SEEK fseeko64
#   define LONG_TELL ftello64
#elif defined(_WIN32) && !defined(__DJGPP__)
#   include <windows.h>
    static int LONG_SEEK(FILE* file, __int64 offset, int origin) {
        LARGE_INTEGER off;
        DWORD method;
        off.QuadPart = offset;
        if (origin == SEEK_END)
            method = FILE_END;
        else if (origin == SEEK_CUR)
            method = FILE_CURRENT;
        else
            method = FILE_BEGIN;

        if (SetFilePointerEx((HANDLE) _get_osfhandle(_fileno(file)), off, NULL, method))
            return 0;
        else
            return -1;
    }
    static __int64 LONG_TELL(FILE* file) {
        LARGE_INTEGER off, newOff;
        off.QuadPart = 0;
        newOff.QuadPart = 0;
        SetFilePointerEx((HANDLE) _get_osfhandle(_fileno(file)), off, &newOff, FILE_CURRENT);
        return newOff.QuadPart;
    }
#else
#   define LONG_SEEK fseek
#   define LONG_TELL ftell
#endif


/*-*************************************
*  Parameters: FIO_prefs_t
***************************************/

/* typedef'd to FIO_prefs_t within fileio.h */
struct FIO_prefs_s {

    /* Algorithm preferences */
    FIO_compressionType_t compressionType;
    U32 sparseFileSupport;   /* 0: no sparse allowed; 1: auto (file yes, stdout no); 2: force sparse */
    int dictIDFlag;
    int checksumFlag;
    int blockSize;
    int overlapLog;
    U32 adaptiveMode;
    U32 useRowMatchFinder;
    int rsyncable;
    int minAdaptLevel;
    int maxAdaptLevel;
    int ldmFlag;
    int ldmHashLog;
    int ldmMinMatch;
    int ldmBucketSizeLog;
    int ldmHashRateLog;
    size_t streamSrcSize;
    size_t targetCBlockSize;
    int srcSizeHint;
    int testMode;
    ZSTD_paramSwitch_e literalCompressionMode;

    /* IO preferences */
    U32 removeSrcFile;
    U32 overwrite;

    /* Computation resources preferences */
    unsigned memLimit;
    int nbWorkers;

    int excludeCompressedFiles;
    int patchFromMode;
    int contentSize;
    int allowBlockDevices;
};

/*-*************************************
*  Parameters: FIO_ctx_t
***************************************/

/* typedef'd to FIO_ctx_t within fileio.h */
struct FIO_ctx_s {

    /* file i/o info */
    int nbFilesTotal;
    int hasStdinInput;
    int hasStdoutOutput;

    /* file i/o state */
    int currFileIdx;
    int nbFilesProcessed;
    size_t totalBytesInput;
    size_t totalBytesOutput;
};


/*-*************************************
*  Parameters: Initialization
***************************************/

#define FIO_OVERLAP_LOG_NOTSET 9999
#define FIO_LDM_PARAM_NOTSET 9999


FIO_prefs_t* FIO_createPreferences(void)
{
    FIO_prefs_t* const ret = (FIO_prefs_t*)malloc(sizeof(FIO_prefs_t));
    if (!ret) EXM_THROW(21, "Allocation error : not enough memory");

    ret->compressionType = FIO_zstdCompression;
    ret->overwrite = 0;
    ret->sparseFileSupport = ZSTD_SPARSE_DEFAULT;
    ret->dictIDFlag = 1;
    ret->checksumFlag = 1;
    ret->removeSrcFile = 0;
    ret->memLimit = 0;
    ret->nbWorkers = 1;
    ret->blockSize = 0;
    ret->overlapLog = FIO_OVERLAP_LOG_NOTSET;
    ret->adaptiveMode = 0;
    ret->rsyncable = 0;
    ret->minAdaptLevel = -50;   /* initializing this value requires a constant, so ZSTD_minCLevel() doesn't work */
    ret->maxAdaptLevel = 22;   /* initializing this value requires a constant, so ZSTD_maxCLevel() doesn't work */
    ret->ldmFlag = 0;
    ret->ldmHashLog = 0;
    ret->ldmMinMatch = 0;
    ret->ldmBucketSizeLog = FIO_LDM_PARAM_NOTSET;
    ret->ldmHashRateLog = FIO_LDM_PARAM_NOTSET;
    ret->streamSrcSize = 0;
    ret->targetCBlockSize = 0;
    ret->srcSizeHint = 0;
    ret->testMode = 0;
    ret->literalCompressionMode = ZSTD_ps_auto;
    ret->excludeCompressedFiles = 0;
    ret->allowBlockDevices = 0;
    return ret;
}

FIO_ctx_t* FIO_createContext(void)
{
    FIO_ctx_t* const ret = (FIO_ctx_t*)malloc(sizeof(FIO_ctx_t));
    if (!ret) EXM_THROW(21, "Allocation error : not enough memory");

    ret->currFileIdx = 0;
    ret->hasStdinInput = 0;
    ret->hasStdoutOutput = 0;
    ret->nbFilesTotal = 1;
    ret->nbFilesProcessed = 0;
    ret->totalBytesInput = 0;
    ret->totalBytesOutput = 0;
    return ret;
}

void FIO_freePreferences(FIO_prefs_t* const prefs)
{
    free(prefs);
}

void FIO_freeContext(FIO_ctx_t* const fCtx)
{
    free(fCtx);
}


/*-*************************************
*  Parameters: Display Options
***************************************/

void FIO_setNotificationLevel(int level) { g_display_prefs.displayLevel=level; }

void FIO_setProgressSetting(FIO_progressSetting_e setting) { g_display_prefs.progressSetting = setting; }


/*-*************************************
*  Parameters: Setters
***************************************/

/* FIO_prefs_t functions */

void FIO_setCompressionType(FIO_prefs_t* const prefs, FIO_compressionType_t compressionType) { prefs->compressionType = compressionType; }

void FIO_overwriteMode(FIO_prefs_t* const prefs) { prefs->overwrite = 1; }

void FIO_setSparseWrite(FIO_prefs_t* const prefs, unsigned sparse) { prefs->sparseFileSupport = sparse; }

void FIO_setDictIDFlag(FIO_prefs_t* const prefs, int dictIDFlag) { prefs->dictIDFlag = dictIDFlag; }

void FIO_setChecksumFlag(FIO_prefs_t* const prefs, int checksumFlag) { prefs->checksumFlag = checksumFlag; }

void FIO_setRemoveSrcFile(FIO_prefs_t* const prefs, unsigned flag) { prefs->removeSrcFile = (flag>0); }

void FIO_setMemLimit(FIO_prefs_t* const prefs, unsigned memLimit) { prefs->memLimit = memLimit; }

void FIO_setNbWorkers(FIO_prefs_t* const prefs, int nbWorkers) {
#ifndef ZSTD_MULTITHREAD
    if (nbWorkers > 0) DISPLAYLEVEL(2, "Note : multi-threading is disabled \n");
#endif
    prefs->nbWorkers = nbWorkers;
}

void FIO_setExcludeCompressedFile(FIO_prefs_t* const prefs, int excludeCompressedFiles) { prefs->excludeCompressedFiles = excludeCompressedFiles; }

void FIO_setAllowBlockDevices(FIO_prefs_t* const prefs, int allowBlockDevices) { prefs->allowBlockDevices = allowBlockDevices; }

void FIO_setBlockSize(FIO_prefs_t* const prefs, int blockSize) {
    if (blockSize && prefs->nbWorkers==0)
        DISPLAYLEVEL(2, "Setting block size is useless in single-thread mode \n");
    prefs->blockSize = blockSize;
}

void FIO_setOverlapLog(FIO_prefs_t* const prefs, int overlapLog){
    if (overlapLog && prefs->nbWorkers==0)
        DISPLAYLEVEL(2, "Setting overlapLog is useless in single-thread mode \n");
    prefs->overlapLog = overlapLog;
}

void FIO_setAdaptiveMode(FIO_prefs_t* const prefs, unsigned adapt) {
    if ((adapt>0) && (prefs->nbWorkers==0))
        EXM_THROW(1, "Adaptive mode is not compatible with single thread mode \n");
    prefs->adaptiveMode = adapt;
}

void FIO_setUseRowMatchFinder(FIO_prefs_t* const prefs, int useRowMatchFinder) {
    prefs->useRowMatchFinder = useRowMatchFinder;
}

void FIO_setRsyncable(FIO_prefs_t* const prefs, int rsyncable) {
    if ((rsyncable>0) && (prefs->nbWorkers==0))
        EXM_THROW(1, "Rsyncable mode is not compatible with single thread mode \n");
    prefs->rsyncable = rsyncable;
}

void FIO_setStreamSrcSize(FIO_prefs_t* const prefs, size_t streamSrcSize) {
    prefs->streamSrcSize = streamSrcSize;
}

void FIO_setTargetCBlockSize(FIO_prefs_t* const prefs, size_t targetCBlockSize) {
    prefs->targetCBlockSize = targetCBlockSize;
}

void FIO_setSrcSizeHint(FIO_prefs_t* const prefs, size_t srcSizeHint) {
    prefs->srcSizeHint = (int)MIN((size_t)INT_MAX, srcSizeHint);
}

void FIO_setTestMode(FIO_prefs_t* const prefs, int testMode) {
    prefs->testMode = (testMode!=0);
}

void FIO_setLiteralCompressionMode(
        FIO_prefs_t* const prefs,
        ZSTD_paramSwitch_e mode) {
    prefs->literalCompressionMode = mode;
}

void FIO_setAdaptMin(FIO_prefs_t* const prefs, int minCLevel)
{
#ifndef ZSTD_NOCOMPRESS
    assert(minCLevel >= ZSTD_minCLevel());
#endif
    prefs->minAdaptLevel = minCLevel;
}

void FIO_setAdaptMax(FIO_prefs_t* const prefs, int maxCLevel)
{
    prefs->maxAdaptLevel = maxCLevel;
}

void FIO_setLdmFlag(FIO_prefs_t* const prefs, unsigned ldmFlag) {
    prefs->ldmFlag = (ldmFlag>0);
}

void FIO_setLdmHashLog(FIO_prefs_t* const prefs, int ldmHashLog) {
    prefs->ldmHashLog = ldmHashLog;
}

void FIO_setLdmMinMatch(FIO_prefs_t* const prefs, int ldmMinMatch) {
    prefs->ldmMinMatch = ldmMinMatch;
}

void FIO_setLdmBucketSizeLog(FIO_prefs_t* const prefs, int ldmBucketSizeLog) {
    prefs->ldmBucketSizeLog = ldmBucketSizeLog;
}


void FIO_setLdmHashRateLog(FIO_prefs_t* const prefs, int ldmHashRateLog) {
    prefs->ldmHashRateLog = ldmHashRateLog;
}

void FIO_setPatchFromMode(FIO_prefs_t* const prefs, int value)
{
    prefs->patchFromMode = value != 0;
}

void FIO_setContentSize(FIO_prefs_t* const prefs, int value)
{
    prefs->contentSize = value != 0;
}

/* FIO_ctx_t functions */

void FIO_setHasStdoutOutput(FIO_ctx_t* const fCtx, int value) {
    fCtx->hasStdoutOutput = value;
}

void FIO_setNbFilesTotal(FIO_ctx_t* const fCtx, int value)
{
    fCtx->nbFilesTotal = value;
}

void FIO_determineHasStdinInput(FIO_ctx_t* const fCtx, const FileNamesTable* const filenames) {
    size_t i = 0;
    for ( ; i < filenames->tableSize; ++i) {
        if (!strcmp(stdinmark, filenames->fileNames[i])) {
            fCtx->hasStdinInput = 1;
            return;
        }
    }
}

/*-*************************************
*  Functions
***************************************/
/** FIO_removeFile() :
 * @result : Unlink `fileName`, even if it's read-only */
static int FIO_removeFile(const char* path)
{
    stat_t statbuf;
    if (!UTIL_stat(path, &statbuf)) {
        DISPLAYLEVEL(2, "zstd: Failed to stat %s while trying to remove it\n", path);
        return 0;
    }
    if (!UTIL_isRegularFileStat(&statbuf)) {
        DISPLAYLEVEL(2, "zstd: Refusing to remove non-regular file %s\n", path);
        return 0;
    }
#if defined(_WIN32) || defined(WIN32)
    /* windows doesn't allow remove read-only files,
     * so try to make it writable first */
    if (!(statbuf.st_mode & _S_IWRITE)) {
        UTIL_chmod(path, &statbuf, _S_IWRITE);
    }
#endif
    return remove(path);
}

/** FIO_openSrcFile() :
 *  condition : `srcFileName` must be non-NULL. `prefs` may be NULL.
 * @result : FILE* to `srcFileName`, or NULL if it fails */
static FILE* FIO_openSrcFile(const FIO_prefs_t* const prefs, const char* srcFileName)
{
    stat_t statbuf;
    int allowBlockDevices = prefs != NULL ? prefs->allowBlockDevices : 0;
    assert(srcFileName != NULL);
    if (!strcmp (srcFileName, stdinmark)) {
        DISPLAYLEVEL(4,"Using stdin for input \n");
        SET_BINARY_MODE(stdin);
        return stdin;
    }

    if (!UTIL_stat(srcFileName, &statbuf)) {
        DISPLAYLEVEL(1, "zstd: can't stat %s : %s -- ignored \n",
                        srcFileName, strerror(errno));
        return NULL;
    }

    if (!UTIL_isRegularFileStat(&statbuf)
     && !UTIL_isFIFOStat(&statbuf)
     && !(allowBlockDevices && UTIL_isBlockDevStat(&statbuf))
    ) {
        DISPLAYLEVEL(1, "zstd: %s is not a regular file -- ignored \n",
                        srcFileName);
        return NULL;
    }

    {   FILE* const f = fopen(srcFileName, "rb");
        if (f == NULL)
            DISPLAYLEVEL(1, "zstd: %s: %s \n", srcFileName, strerror(errno));
        return f;
    }
}

/** FIO_openDstFile() :
 *  condition : `dstFileName` must be non-NULL.
 * @result : FILE* to `dstFileName`, or NULL if it fails */
static FILE*
FIO_openDstFile(FIO_ctx_t* fCtx, FIO_prefs_t* const prefs,
                const char* srcFileName, const char* dstFileName,
                const int mode)
{
    if (prefs->testMode) return NULL;  /* do not open file in test mode */

    assert(dstFileName != NULL);
    if (!strcmp (dstFileName, stdoutmark)) {
        DISPLAYLEVEL(4,"Using stdout for output \n");
        SET_BINARY_MODE(stdout);
        if (prefs->sparseFileSupport == 1) {
            prefs->sparseFileSupport = 0;
            DISPLAYLEVEL(4, "Sparse File Support is automatically disabled on stdout ; try --sparse \n");
        }
        return stdout;
    }

    /* ensure dst is not the same as src */
    if (srcFileName != NULL && UTIL_isSameFile(srcFileName, dstFileName)) {
        DISPLAYLEVEL(1, "zstd: Refusing to open an output file which will overwrite the input file \n");
        return NULL;
    }

    if (prefs->sparseFileSupport == 1) {
        prefs->sparseFileSupport = ZSTD_SPARSE_DEFAULT;
    }

    if (UTIL_isRegularFile(dstFileName)) {
        /* Check if destination file already exists */
#if !defined(_WIN32)
        /* this test does not work on Windows :
         * `NUL` and `nul` are detected as regular files */
        if (!strcmp(dstFileName, nulmark)) {
            EXM_THROW(40, "%s is unexpectedly categorized as a regular file",
                        dstFileName);
        }
#endif
        if (!prefs->overwrite) {
            if (g_display_prefs.displayLevel <= 1) {
                /* No interaction possible */
                DISPLAY("zstd: %s already exists; not overwritten  \n",
                        dstFileName);
                return NULL;
            }
            DISPLAY("zstd: %s already exists; ", dstFileName);
            if (UTIL_requireUserConfirmation("overwrite (y/n) ? ", "Not overwritten  \n", "yY", fCtx->hasStdinInput))
                return NULL;
        }
        /* need to unlink */
        FIO_removeFile(dstFileName);
    }

    {
#if defined(_WIN32)
        /* Windows requires opening the file as a "binary" file to avoid
         * mangling. This macro doesn't exist on unix. */
        const int openflags = O_WRONLY|O_CREAT|O_TRUNC|O_BINARY;
        const int fd = _open(dstFileName, openflags, mode);
        FILE* f = NULL;
        if (fd != -1) {
            f = _fdopen(fd, "wb");
        }
#else
        const int openflags = O_WRONLY|O_CREAT|O_TRUNC;
        const int fd = open(dstFileName, openflags, mode);
        FILE* f = NULL;
        if (fd != -1) {
            f = fdopen(fd, "wb");
        }
#endif
        if (f == NULL) {
            DISPLAYLEVEL(1, "zstd: %s: %s\n", dstFileName, strerror(errno));
        }
        return f;
    }
}

/*! FIO_createDictBuffer() :
 *  creates a buffer, pointed by `*bufferPtr`,
 *  loads `filename` content into it, up to DICTSIZE_MAX bytes.
 * @return : loaded size
 *  if fileName==NULL, returns 0 and a NULL pointer
 */
static size_t FIO_createDictBuffer(void** bufferPtr, const char* fileName, FIO_prefs_t* const prefs)
{
    FILE* fileHandle;
    U64 fileSize;

    assert(bufferPtr != NULL);
    *bufferPtr = NULL;
    if (fileName == NULL) return 0;

    DISPLAYLEVEL(4,"Loading %s as dictionary \n", fileName);
    fileHandle = fopen(fileName, "rb");
    if (fileHandle==NULL) EXM_THROW(31, "%s: %s", fileName, strerror(errno));

    fileSize = UTIL_getFileSize(fileName);
    if (fileSize == UTIL_FILESIZE_UNKNOWN) 
    	EXM_THROW(32, "This file format is not supported : Dictionary file %s\n", fileName);
    {
        size_t const dictSizeMax = prefs->patchFromMode ? prefs->memLimit : DICTSIZE_MAX;
        if (fileSize >  dictSizeMax) {
            EXM_THROW(32, "Dictionary file %s is too large (> %u bytes)",
                            fileName,  (unsigned)dictSizeMax);   /* avoid extreme cases */
        }
    }
    *bufferPtr = malloc((size_t)fileSize);
    if (*bufferPtr==NULL) EXM_THROW(34, "%s", strerror(errno));
    {   size_t const readSize = fread(*bufferPtr, 1, (size_t)fileSize, fileHandle);
        if (readSize != fileSize)
            EXM_THROW(35, "Error reading dictionary file %s : %s",
                    fileName, strerror(errno));
    }
    fclose(fileHandle);
    return (size_t)fileSize;
}



/* FIO_checkFilenameCollisions() :
 * Checks for and warns if there are any files that would have the same output path
 */
int FIO_checkFilenameCollisions(const char** filenameTable, unsigned nbFiles) {
    const char **filenameTableSorted, *prevElem, *filename;
    unsigned u;

    filenameTableSorted = (const char**) malloc(sizeof(char*) * nbFiles);
    if (!filenameTableSorted) {
        DISPLAY("Unable to malloc new str array, not checking for name collisions\n");
        return 1;
    }

    for (u = 0; u < nbFiles; ++u) {
        filename = strrchr(filenameTable[u], PATH_SEP);
        if (filename == NULL) {
            filenameTableSorted[u] = filenameTable[u];
        } else {
            filenameTableSorted[u] = filename+1;
        }
    }

    qsort((void*)filenameTableSorted, nbFiles, sizeof(char*), UTIL_compareStr);
    prevElem = filenameTableSorted[0];
    for (u = 1; u < nbFiles; ++u) {
        if (strcmp(prevElem, filenameTableSorted[u]) == 0) {
            DISPLAY("WARNING: Two files have same filename: %s\n", prevElem);
        }
        prevElem = filenameTableSorted[u];
    }

    free((void*)filenameTableSorted);
    return 0;
}

static const char*
extractFilename(const char* path, char separator)
{
    const char* search = strrchr(path, separator);
    if (search == NULL) return path;
    return search+1;
}

/* FIO_createFilename_fromOutDir() :
 * Takes a source file name and specified output directory, and
 * allocates memory for and returns a pointer to final path.
 * This function never returns an error (it may abort() in case of pb)
 */
static char*
FIO_createFilename_fromOutDir(const char* path, const char* outDirName, const size_t suffixLen)
{
    const char* filenameStart;
    char separator;
    char* result;

#if defined(_MSC_VER) || defined(__MINGW32__) || defined (__MSVCRT__) /* windows support */
    separator = '\\';
#else
    separator = '/';
#endif

    filenameStart = extractFilename(path, separator);
#if defined(_MSC_VER) || defined(__MINGW32__) || defined (__MSVCRT__) /* windows support */
    filenameStart = extractFilename(filenameStart, '/');  /* sometimes, '/' separator is also used on Windows (mingw+msys2) */
#endif

    result = (char*) calloc(1, strlen(outDirName) + 1 + strlen(filenameStart) + suffixLen + 1);
    if (!result) {
        EXM_THROW(30, "zstd: FIO_createFilename_fromOutDir: %s", strerror(errno));
    }

    memcpy(result, outDirName, strlen(outDirName));
    if (outDirName[strlen(outDirName)-1] == separator) {
        memcpy(result + strlen(outDirName), filenameStart, strlen(filenameStart));
    } else {
        memcpy(result + strlen(outDirName), &separator, 1);
        memcpy(result + strlen(outDirName) + 1, filenameStart, strlen(filenameStart));
    }

    return result;
}

/* FIO_highbit64() :
 * gives position of highest bit.
 * note : only works for v > 0 !
 */
static unsigned FIO_highbit64(unsigned long long v)
{
    unsigned count = 0;
    assert(v != 0);
    v >>= 1;
    while (v) { v >>= 1; count++; }
    return count;
}

static void FIO_adjustMemLimitForPatchFromMode(FIO_prefs_t* const prefs,
                                    unsigned long long const dictSize,
                                    unsigned long long const maxSrcFileSize)
{
    unsigned long long maxSize = MAX(prefs->memLimit, MAX(dictSize, maxSrcFileSize));
    unsigned const maxWindowSize = (1U << ZSTD_WINDOWLOG_MAX);
    if (maxSize == UTIL_FILESIZE_UNKNOWN)
        EXM_THROW(42, "Using --patch-from with stdin requires --stream-size");
    assert(maxSize != UTIL_FILESIZE_UNKNOWN);
    if (maxSize > maxWindowSize)
        EXM_THROW(42, "Can't handle files larger than %u GB\n", maxWindowSize/(1 GB));
    FIO_setMemLimit(prefs, (unsigned)maxSize);
}

/* FIO_removeMultiFilesWarning() :
 * Returns 1 if the console should abort, 0 if console should proceed.
 * This function handles logic when processing multiple files with -o, displaying the appropriate warnings/prompts.
 *
 * If -f is specified, or there is just 1 file, zstd will always proceed as usual.
 * If --rm is specified, there will be a prompt asking for user confirmation.
 *         If -f is specified with --rm, zstd will proceed as usual
 *         If -q is specified with --rm, zstd will abort pre-emptively
 *         If neither flag is specified, zstd will prompt the user for confirmation to proceed.
 * If --rm is not specified, then zstd will print a warning to the user (which can be silenced with -q).
 * However, if the output is stdout, we will always abort rather than displaying the warning prompt.
 */
static int FIO_removeMultiFilesWarning(FIO_ctx_t* const fCtx, const FIO_prefs_t* const prefs, const char* outFileName, int displayLevelCutoff)
{
    int error = 0;
    if (fCtx->nbFilesTotal > 1 && !prefs->overwrite) {
        if (g_display_prefs.displayLevel <= displayLevelCutoff) {
            if (prefs->removeSrcFile) {
                DISPLAYLEVEL(1, "zstd: Aborting... not deleting files and processing into dst: %s\n", outFileName);
                error =  1;
            }
        } else {
            if (!strcmp(outFileName, stdoutmark)) {
                DISPLAYLEVEL(2, "zstd: WARNING: all input files will be processed and concatenated into stdout. \n");
            } else {
                DISPLAYLEVEL(2, "zstd: WARNING: all input files will be processed and concatenated into a single output file: %s \n", outFileName);
            }
            DISPLAYLEVEL(2, "The concatenated output CANNOT regenerate the original directory tree. \n")
            if (prefs->removeSrcFile) {
                if (fCtx->hasStdoutOutput) {
                    DISPLAYLEVEL(1, "Aborting. Use -f if you really want to delete the files and output to stdout\n");
                    error = 1;
                } else {
                    error = g_display_prefs.displayLevel > displayLevelCutoff && UTIL_requireUserConfirmation("This is a destructive operation. Proceed? (y/n): ", "Aborting...", "yY", fCtx->hasStdinInput);
                }
            }
        }
    }
    return error;
}

#ifndef ZSTD_NOCOMPRESS

/* **********************************************************************
 *  Compression
 ************************************************************************/
typedef struct {
    FILE* srcFile;
    FILE* dstFile;
    void*  srcBuffer;
    size_t srcBufferSize;
    void*  dstBuffer;
    size_t dstBufferSize;
    void* dictBuffer;
    size_t dictBufferSize;
    const char* dictFileName;
    ZSTD_CStream* cctx;
} cRess_t;

/** ZSTD_cycleLog() :
 *  condition for correct operation : hashLog > 1 */
static U32 ZSTD_cycleLog(U32 hashLog, ZSTD_strategy strat)
{
    U32 const btScale = ((U32)strat >= (U32)ZSTD_btlazy2);
    assert(hashLog > 1);
    return hashLog - btScale;
}

static void FIO_adjustParamsForPatchFromMode(FIO_prefs_t* const prefs,
                                    ZSTD_compressionParameters* comprParams,
                                    unsigned long long const dictSize,
                                    unsigned long long const maxSrcFileSize,
                                    int cLevel)
{
    unsigned const fileWindowLog = FIO_highbit64(maxSrcFileSize) + 1;
    ZSTD_compressionParameters const cParams = ZSTD_getCParams(cLevel, (size_t)maxSrcFileSize, (size_t)dictSize);
    FIO_adjustMemLimitForPatchFromMode(prefs, dictSize, maxSrcFileSize);
    if (fileWindowLog > ZSTD_WINDOWLOG_MAX)
        DISPLAYLEVEL(1, "Max window log exceeded by file (compression ratio will suffer)\n");
    comprParams->windowLog = MAX(ZSTD_WINDOWLOG_MIN, MIN(ZSTD_WINDOWLOG_MAX, fileWindowLog));
    if (fileWindowLog > ZSTD_cycleLog(cParams.chainLog, cParams.strategy)) {
        if (!prefs->ldmFlag)
            DISPLAYLEVEL(1, "long mode automatically triggered\n");
        FIO_setLdmFlag(prefs, 1);
    }
    if (cParams.strategy >= ZSTD_btopt) {
        DISPLAYLEVEL(1, "[Optimal parser notes] Consider the following to improve patch size at the cost of speed:\n");
        DISPLAYLEVEL(1, "- Use --single-thread mode in the zstd cli\n");
        DISPLAYLEVEL(1, "- Set a larger targetLength (eg. --zstd=targetLength=4096)\n");
        DISPLAYLEVEL(1, "- Set a larger chainLog (eg. --zstd=chainLog=%u)\n", ZSTD_CHAINLOG_MAX);
        DISPLAYLEVEL(1, "Also consider playing around with searchLog and hashLog\n");
    }
}

static cRess_t FIO_createCResources(FIO_prefs_t* const prefs,
                                    const char* dictFileName, unsigned long long const maxSrcFileSize,
                                    int cLevel, ZSTD_compressionParameters comprParams) {
    cRess_t ress;
    memset(&ress, 0, sizeof(ress));

    DISPLAYLEVEL(6, "FIO_createCResources \n");
    ress.cctx = ZSTD_createCCtx();
    if (ress.cctx == NULL)
        EXM_THROW(30, "allocation error (%s): can't create ZSTD_CCtx",
                    strerror(errno));
    ress.srcBufferSize = ZSTD_CStreamInSize();
    ress.srcBuffer = malloc(ress.srcBufferSize);
    ress.dstBufferSize = ZSTD_CStreamOutSize();

    /* need to update memLimit before calling createDictBuffer
     * because of memLimit check inside it */
    if (prefs->patchFromMode) {
        unsigned long long const ssSize = (unsigned long long)prefs->streamSrcSize;
        FIO_adjustParamsForPatchFromMode(prefs, &comprParams, UTIL_getFileSize(dictFileName), ssSize > 0 ? ssSize : maxSrcFileSize, cLevel);
    }
    ress.dstBuffer = malloc(ress.dstBufferSize);
    ress.dictBufferSize = FIO_createDictBuffer(&ress.dictBuffer, dictFileName, prefs);   /* works with dictFileName==NULL */
    if (!ress.srcBuffer || !ress.dstBuffer)
        EXM_THROW(31, "allocation error : not enough memory");

    /* Advanced parameters, including dictionary */
    if (dictFileName && (ress.dictBuffer==NULL))
        EXM_THROW(32, "allocation error : can't create dictBuffer");
    ress.dictFileName = dictFileName;

    if (prefs->adaptiveMode && !prefs->ldmFlag && !comprParams.windowLog)
        comprParams.windowLog = ADAPT_WINDOWLOG_DEFAULT;

    CHECK( ZSTD_CCtx_setParameter(ress.cctx, ZSTD_c_contentSizeFlag, prefs->contentSize) );  /* always enable content size when available (note: supposed to be default) */
    CHECK( ZSTD_CCtx_setParameter(ress.cctx, ZSTD_c_dictIDFlag, prefs->dictIDFlag) );
    CHECK( ZSTD_CCtx_setParameter(ress.cctx, ZSTD_c_checksumFlag, prefs->checksumFlag) );
    /* compression level */
    CHECK( ZSTD_CCtx_setParameter(ress.cctx, ZSTD_c_compressionLevel, cLevel) );
    /* max compressed block size */
    CHECK( ZSTD_CCtx_setParameter(ress.cctx, ZSTD_c_targetCBlockSize, (int)prefs->targetCBlockSize) );
    /* source size hint */
    CHECK( ZSTD_CCtx_setParameter(ress.cctx, ZSTD_c_srcSizeHint, (int)prefs->srcSizeHint) );
    /* long distance matching */
    CHECK( ZSTD_CCtx_setParameter(ress.cctx, ZSTD_c_enableLongDistanceMatching, prefs->ldmFlag) );
    CHECK( ZSTD_CCtx_setParameter(ress.cctx, ZSTD_c_ldmHashLog, prefs->ldmHashLog) );
    CHECK( ZSTD_CCtx_setParameter(ress.cctx, ZSTD_c_ldmMinMatch, prefs->ldmMinMatch) );
    if (prefs->ldmBucketSizeLog != FIO_LDM_PARAM_NOTSET) {
        CHECK( ZSTD_CCtx_setParameter(ress.cctx, ZSTD_c_ldmBucketSizeLog, prefs->ldmBucketSizeLog) );
    }
    if (prefs->ldmHashRateLog != FIO_LDM_PARAM_NOTSET) {
        CHECK( ZSTD_CCtx_setParameter(ress.cctx, ZSTD_c_ldmHashRateLog, prefs->ldmHashRateLog) );
    }
    CHECK( ZSTD_CCtx_setParameter(ress.cctx, ZSTD_c_useRowMatchFinder, prefs->useRowMatchFinder));
    /* compression parameters */
    CHECK( ZSTD_CCtx_setParameter(ress.cctx, ZSTD_c_windowLog, (int)comprParams.windowLog) );
    CHECK( ZSTD_CCtx_setParameter(ress.cctx, ZSTD_c_chainLog, (int)comprParams.chainLog) );
    CHECK( ZSTD_CCtx_setParameter(ress.cctx, ZSTD_c_hashLog, (int)comprParams.hashLog) );
    CHECK( ZSTD_CCtx_setParameter(ress.cctx, ZSTD_c_searchLog, (int)comprParams.searchLog) );
    CHECK( ZSTD_CCtx_setParameter(ress.cctx, ZSTD_c_minMatch, (int)comprParams.minMatch) );
    CHECK( ZSTD_CCtx_setParameter(ress.cctx, ZSTD_c_targetLength, (int)comprParams.targetLength) );
    CHECK( ZSTD_CCtx_setParameter(ress.cctx, ZSTD_c_strategy, (int)comprParams.strategy) );
    CHECK( ZSTD_CCtx_setParameter(ress.cctx, ZSTD_c_literalCompressionMode, (int)prefs->literalCompressionMode) );
    CHECK( ZSTD_CCtx_setParameter(ress.cctx, ZSTD_c_enableDedicatedDictSearch, 1) );
    /* multi-threading */
#ifdef ZSTD_MULTITHREAD
    DISPLAYLEVEL(5,"set nb workers = %u \n", prefs->nbWorkers);
    CHECK( ZSTD_CCtx_setParameter(ress.cctx, ZSTD_c_nbWorkers, prefs->nbWorkers) );
    CHECK( ZSTD_CCtx_setParameter(ress.cctx, ZSTD_c_jobSize, prefs->blockSize) );
    if (prefs->overlapLog != FIO_OVERLAP_LOG_NOTSET) {
        DISPLAYLEVEL(3,"set overlapLog = %u \n", prefs->overlapLog);
        CHECK( ZSTD_CCtx_setParameter(ress.cctx, ZSTD_c_overlapLog, prefs->overlapLog) );
    }
    CHECK( ZSTD_CCtx_setParameter(ress.cctx, ZSTD_c_rsyncable, prefs->rsyncable) );
#endif
    /* dictionary */
    if (prefs->patchFromMode) {
        CHECK( ZSTD_CCtx_refPrefix(ress.cctx, ress.dictBuffer, ress.dictBufferSize) );
    } else {
        CHECK( ZSTD_CCtx_loadDictionary(ress.cctx, ress.dictBuffer, ress.dictBufferSize) );
    }

    return ress;
}

static void FIO_freeCResources(const cRess_t* const ress)
{
    free(ress->srcBuffer);
    free(ress->dstBuffer);
    free(ress->dictBuffer);
    ZSTD_freeCStream(ress->cctx);   /* never fails */
}


#ifdef ZSTD_GZCOMPRESS
static unsigned long long
FIO_compressGzFrame(const cRess_t* ress,  /* buffers & handlers are used, but not changed */
                    const char* srcFileName, U64 const srcFileSize,
                    int compressionLevel, U64* readsize)
{
    unsigned long long inFileSize = 0, outFileSize = 0;
    z_stream strm;

    if (compressionLevel > Z_BEST_COMPRESSION)
        compressionLevel = Z_BEST_COMPRESSION;

    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;

    {   int const ret = deflateInit2(&strm, compressionLevel, Z_DEFLATED,
                        15 /* maxWindowLogSize */ + 16 /* gzip only */,
                        8, Z_DEFAULT_STRATEGY); /* see http://www.zlib.net/manual.html */
        if (ret != Z_OK) {
            EXM_THROW(71, "zstd: %s: deflateInit2 error %d \n", srcFileName, ret);
    }   }

    strm.next_in = 0;
    strm.avail_in = 0;
    strm.next_out = (Bytef*)ress->dstBuffer;
    strm.avail_out = (uInt)ress->dstBufferSize;

    while (1) {
        int ret;
        if (strm.avail_in == 0) {
            size_t const inSize = fread(ress->srcBuffer, 1, ress->srcBufferSize, ress->srcFile);
            if (inSize == 0) break;
            inFileSize += inSize;
            strm.next_in = (z_const unsigned char*)ress->srcBuffer;
            strm.avail_in = (uInt)inSize;
        }
        ret = deflate(&strm, Z_NO_FLUSH);
        if (ret != Z_OK)
            EXM_THROW(72, "zstd: %s: deflate error %d \n", srcFileName, ret);
        {   size_t const cSize = ress->dstBufferSize - strm.avail_out;
            if (cSize) {
                if (fwrite(ress->dstBuffer, 1, cSize, ress->dstFile) != cSize)
                    EXM_THROW(73, "Write error : cannot write to output file : %s ", strerror(errno));
                outFileSize += cSize;
                strm.next_out = (Bytef*)ress->dstBuffer;
                strm.avail_out = (uInt)ress->dstBufferSize;
        }   }
        if (srcFileSize == UTIL_FILESIZE_UNKNOWN) {
            DISPLAYUPDATE(2, "\rRead : %u MB ==> %.2f%% ",
                            (unsigned)(inFileSize>>20),
                            (double)outFileSize/inFileSize*100)
        } else {
            DISPLAYUPDATE(2, "\rRead : %u / %u MB ==> %.2f%% ",
                            (unsigned)(inFileSize>>20), (unsigned)(srcFileSize>>20),
                            (double)outFileSize/inFileSize*100);
    }   }

    while (1) {
        int const ret = deflate(&strm, Z_FINISH);
        {   size_t const cSize = ress->dstBufferSize - strm.avail_out;
            if (cSize) {
                if (fwrite(ress->dstBuffer, 1, cSize, ress->dstFile) != cSize)
                    EXM_THROW(75, "Write error : %s ", strerror(errno));
                outFileSize += cSize;
                strm.next_out = (Bytef*)ress->dstBuffer;
                strm.avail_out = (uInt)ress->dstBufferSize;
        }   }
        if (ret == Z_STREAM_END) break;
        if (ret != Z_BUF_ERROR)
            EXM_THROW(77, "zstd: %s: deflate error %d \n", srcFileName, ret);
    }

    {   int const ret = deflateEnd(&strm);
        if (ret != Z_OK) {
            EXM_THROW(79, "zstd: %s: deflateEnd error %d \n", srcFileName, ret);
    }   }
    *readsize = inFileSize;
    return outFileSize;
}
#endif


#ifdef ZSTD_LZMACOMPRESS
static unsigned long long
FIO_compressLzmaFrame(cRess_t* ress,
                      const char* srcFileName, U64 const srcFileSize,
                      int compressionLevel, U64* readsize, int plain_lzma)
{
    unsigned long long inFileSize = 0, outFileSize = 0;
    lzma_stream strm = LZMA_STREAM_INIT;
    lzma_action action = LZMA_RUN;
    lzma_ret ret;

    if (compressionLevel < 0) compressionLevel = 0;
    if (compressionLevel > 9) compressionLevel = 9;

    if (plain_lzma) {
        lzma_options_lzma opt_lzma;
        if (lzma_lzma_preset(&opt_lzma, compressionLevel))
            EXM_THROW(81, "zstd: %s: lzma_lzma_preset error", srcFileName);
        ret = lzma_alone_encoder(&strm, &opt_lzma); /* LZMA */
        if (ret != LZMA_OK)
            EXM_THROW(82, "zstd: %s: lzma_alone_encoder error %d", srcFileName, ret);
    } else {
        ret = lzma_easy_encoder(&strm, compressionLevel, LZMA_CHECK_CRC64); /* XZ */
        if (ret != LZMA_OK)
            EXM_THROW(83, "zstd: %s: lzma_easy_encoder error %d", srcFileName, ret);
    }

    strm.next_in = 0;
    strm.avail_in = 0;
    strm.next_out = (BYTE*)ress->dstBuffer;
    strm.avail_out = ress->dstBufferSize;

    while (1) {
        if (strm.avail_in == 0) {
            size_t const inSize = fread(ress->srcBuffer, 1, ress->srcBufferSize, ress->srcFile);
            if (inSize == 0) action = LZMA_FINISH;
            inFileSize += inSize;
            strm.next_in = (BYTE const*)ress->srcBuffer;
            strm.avail_in = inSize;
        }

        ret = lzma_code(&strm, action);

        if (ret != LZMA_OK && ret != LZMA_STREAM_END)
            EXM_THROW(84, "zstd: %s: lzma_code encoding error %d", srcFileName, ret);
        {   size_t const compBytes = ress->dstBufferSize - strm.avail_out;
            if (compBytes) {
                if (fwrite(ress->dstBuffer, 1, compBytes, ress->dstFile) != compBytes)
                    EXM_THROW(85, "Write error : %s", strerror(errno));
                outFileSize += compBytes;
                strm.next_out = (BYTE*)ress->dstBuffer;
                strm.avail_out = ress->dstBufferSize;
        }   }
        if (srcFileSize == UTIL_FILESIZE_UNKNOWN)
            DISPLAYUPDATE(2, "\rRead : %u MB ==> %.2f%%",
                            (unsigned)(inFileSize>>20),
                            (double)outFileSize/inFileSize*100)
        else
            DISPLAYUPDATE(2, "\rRead : %u / %u MB ==> %.2f%%",
                            (unsigned)(inFileSize>>20), (unsigned)(srcFileSize>>20),
                            (double)outFileSize/inFileSize*100);
        if (ret == LZMA_STREAM_END) break;
    }

    lzma_end(&strm);
    *readsize = inFileSize;

    return outFileSize;
}
#endif

#ifdef ZSTD_LZ4COMPRESS

#if LZ4_VERSION_NUMBER <= 10600
#define LZ4F_blockLinked blockLinked
#define LZ4F_max64KB max64KB
#endif

static int FIO_LZ4_GetBlockSize_FromBlockId (int id) { return (1 << (8 + (2 * id))); }

static unsigned long long
FIO_compressLz4Frame(cRess_t* ress,
                     const char* srcFileName, U64 const srcFileSize,
                     int compressionLevel, int checksumFlag,
                     U64* readsize)
{
    const size_t blockSize = FIO_LZ4_GetBlockSize_FromBlockId(LZ4F_max64KB);
    unsigned long long inFileSize = 0, outFileSize = 0;

    LZ4F_preferences_t prefs;
    LZ4F_compressionContext_t ctx;

    LZ4F_errorCode_t const errorCode = LZ4F_createCompressionContext(&ctx, LZ4F_VERSION);
    if (LZ4F_isError(errorCode))
        EXM_THROW(31, "zstd: failed to create lz4 compression context");

    memset(&prefs, 0, sizeof(prefs));

    assert(blockSize <= ress->srcBufferSize);

    prefs.autoFlush = 1;
    prefs.compressionLevel = compressionLevel;
    prefs.frameInfo.blockMode = LZ4F_blockLinked;
    prefs.frameInfo.blockSizeID = LZ4F_max64KB;
    prefs.frameInfo.contentChecksumFlag = (contentChecksum_t)checksumFlag;
#if LZ4_VERSION_NUMBER >= 10600
    prefs.frameInfo.contentSize = (srcFileSize==UTIL_FILESIZE_UNKNOWN) ? 0 : srcFileSize;
#endif
    assert(LZ4F_compressBound(blockSize, &prefs) <= ress->dstBufferSize);

    {
        size_t readSize;
        size_t headerSize = LZ4F_compressBegin(ctx, ress->dstBuffer, ress->dstBufferSize, &prefs);
        if (LZ4F_isError(headerSize))
            EXM_THROW(33, "File header generation failed : %s",
                            LZ4F_getErrorName(headerSize));
        if (fwrite(ress->dstBuffer, 1, headerSize, ress->dstFile) != headerSize)
            EXM_THROW(34, "Write error : %s (cannot write header)", strerror(errno));
        outFileSize += headerSize;

        /* Read first block */
        readSize  = fread(ress->srcBuffer, (size_t)1, (size_t)blockSize, ress->srcFile);
        inFileSize += readSize;

        /* Main Loop */
        while (readSize>0) {
            size_t const outSize = LZ4F_compressUpdate(ctx,
                                        ress->dstBuffer, ress->dstBufferSize,
                                        ress->srcBuffer, readSize, NULL);
            if (LZ4F_isError(outSize))
                EXM_THROW(35, "zstd: %s: lz4 compression failed : %s",
                            srcFileName, LZ4F_getErrorName(outSize));
            outFileSize += outSize;
            if (srcFileSize == UTIL_FILESIZE_UNKNOWN) {
                DISPLAYUPDATE(2, "\rRead : %u MB ==> %.2f%%",
                                (unsigned)(inFileSize>>20),
                                (double)outFileSize/inFileSize*100)
            } else {
                DISPLAYUPDATE(2, "\rRead : %u / %u MB ==> %.2f%%",
                                (unsigned)(inFileSize>>20), (unsigned)(srcFileSize>>20),
                                (double)outFileSize/inFileSize*100);
            }

            /* Write Block */
            {   size_t const sizeCheck = fwrite(ress->dstBuffer, 1, outSize, ress->dstFile);
                if (sizeCheck != outSize)
                    EXM_THROW(36, "Write error : %s", strerror(errno));
            }

            /* Read next block */
            readSize  = fread(ress->srcBuffer, (size_t)1, (size_t)blockSize, ress->srcFile);
            inFileSize += readSize;
        }
        if (ferror(ress->srcFile)) EXM_THROW(37, "Error reading %s ", srcFileName);

        /* End of Stream mark */
        headerSize = LZ4F_compressEnd(ctx, ress->dstBuffer, ress->dstBufferSize, NULL);
        if (LZ4F_isError(headerSize))
            EXM_THROW(38, "zstd: %s: lz4 end of file generation failed : %s",
                        srcFileName, LZ4F_getErrorName(headerSize));

        {   size_t const sizeCheck = fwrite(ress->dstBuffer, 1, headerSize, ress->dstFile);
            if (sizeCheck != headerSize)
                EXM_THROW(39, "Write error : %s (cannot write end of stream)",
                            strerror(errno));
        }
        outFileSize += headerSize;
    }

    *readsize = inFileSize;
    LZ4F_freeCompressionContext(ctx);

    return outFileSize;
}
#endif


static unsigned long long
FIO_compressZstdFrame(FIO_ctx_t* const fCtx,
                      FIO_prefs_t* const prefs,
                      const cRess_t* ressPtr,
                      const char* srcFileName, U64 fileSize,
                      int compressionLevel, U64* readsize)
{
    cRess_t const ress = *ressPtr;
    FILE* const srcFile = ress.srcFile;
    FILE* const dstFile = ress.dstFile;
    U64 compressedfilesize = 0;
    ZSTD_EndDirective directive = ZSTD_e_continue;
    U64 pledgedSrcSize = ZSTD_CONTENTSIZE_UNKNOWN;

    /* stats */
    ZSTD_frameProgression previous_zfp_update = { 0, 0, 0, 0, 0, 0 };
    ZSTD_frameProgression previous_zfp_correction = { 0, 0, 0, 0, 0, 0 };
    typedef enum { noChange, slower, faster } speedChange_e;
    speedChange_e speedChange = noChange;
    unsigned flushWaiting = 0;
    unsigned inputPresented = 0;
    unsigned inputBlocked = 0;
    unsigned lastJobID = 0;
    UTIL_HumanReadableSize_t const file_hrs = UTIL_makeHumanReadableSize(fileSize);

    DISPLAYLEVEL(6, "compression using zstd format \n");

    /* init */
    if (fileSize != UTIL_FILESIZE_UNKNOWN) {
        pledgedSrcSize = fileSize;
        CHECK(ZSTD_CCtx_setPledgedSrcSize(ress.cctx, fileSize));
    } else if (prefs->streamSrcSize > 0) {
      /* unknown source size; use the declared stream size */
      pledgedSrcSize = prefs->streamSrcSize;
      CHECK( ZSTD_CCtx_setPledgedSrcSize(ress.cctx, prefs->streamSrcSize) );
    }

    {
        int windowLog;
        UTIL_HumanReadableSize_t windowSize;
        CHECK(ZSTD_CCtx_getParameter(ress.cctx, ZSTD_c_windowLog, &windowLog));
        if (windowLog == 0) {
            const ZSTD_compressionParameters cParams = ZSTD_getCParams(compressionLevel, fileSize, 0);
            windowLog = cParams.windowLog;
        }
        windowSize = UTIL_makeHumanReadableSize(MAX(1ULL, MIN(1ULL << windowLog, pledgedSrcSize)));
        DISPLAYLEVEL(4, "Decompression will require %.*f%s of memory\n", windowSize.precision, windowSize.value, windowSize.suffix);
    }
    (void)srcFileName;

    /* Main compression loop */
    do {
        size_t stillToFlush;
        /* Fill input Buffer */
        size_t const inSize = fread(ress.srcBuffer, (size_t)1, ress.srcBufferSize, srcFile);
        ZSTD_inBuffer inBuff = { ress.srcBuffer, inSize, 0 };
        DISPLAYLEVEL(6, "fread %u bytes from source \n", (unsigned)inSize);
        *readsize += inSize;

        if ((inSize == 0) || (*readsize == fileSize))
            directive = ZSTD_e_end;

        stillToFlush = 1;
        while ((inBuff.pos != inBuff.size)   /* input buffer must be entirely ingested */
            || (directive == ZSTD_e_end && stillToFlush != 0) ) {

            size_t const oldIPos = inBuff.pos;
            ZSTD_outBuffer outBuff = { ress.dstBuffer, ress.dstBufferSize, 0 };
            size_t const toFlushNow = ZSTD_toFlushNow(ress.cctx);
            CHECK_V(stillToFlush, ZSTD_compressStream2(ress.cctx, &outBuff, &inBuff, directive));

            /* count stats */
            inputPresented++;
            if (oldIPos == inBuff.pos) inputBlocked++;  /* input buffer is full and can't take any more : input speed is faster than consumption rate */
            if (!toFlushNow) flushWaiting = 1;

            /* Write compressed stream */
            DISPLAYLEVEL(6, "ZSTD_compress_generic(end:%u) => input pos(%u)<=(%u)size ; output generated %u bytes \n",
                            (unsigned)directive, (unsigned)inBuff.pos, (unsigned)inBuff.size, (unsigned)outBuff.pos);
            if (outBuff.pos) {
                size_t const sizeCheck = fwrite(ress.dstBuffer, 1, outBuff.pos, dstFile);
                if (sizeCheck != outBuff.pos)
                    EXM_THROW(25, "Write error : %s (cannot write compressed block)",
                                    strerror(errno));
                compressedfilesize += outBuff.pos;
            }

            /* display notification; and adapt compression level */
            if (READY_FOR_UPDATE()) {
                ZSTD_frameProgression const zfp = ZSTD_getFrameProgression(ress.cctx);
                double const cShare = (double)zfp.produced / (double)(zfp.consumed + !zfp.consumed/*avoid div0*/) * 100;
                UTIL_HumanReadableSize_t const buffered_hrs = UTIL_makeHumanReadableSize(zfp.ingested - zfp.consumed);
                UTIL_HumanReadableSize_t const consumed_hrs = UTIL_makeHumanReadableSize(zfp.consumed);
                UTIL_HumanReadableSize_t const produced_hrs = UTIL_makeHumanReadableSize(zfp.produced);

                /* display progress notifications */
                if (g_display_prefs.displayLevel >= 3) {
                    DISPLAYUPDATE(3, "\r(L%i) Buffered :%6.*f%4s - Consumed :%6.*f%4s - Compressed :%6.*f%4s => %.2f%% ",
                                compressionLevel,
                                buffered_hrs.precision, buffered_hrs.value, buffered_hrs.suffix,
                                consumed_hrs.precision, consumed_hrs.value, consumed_hrs.suffix,
                                produced_hrs.precision, produced_hrs.value, produced_hrs.suffix,
                                cShare );
                } else if (g_display_prefs.displayLevel >= 2 || g_display_prefs.progressSetting == FIO_ps_always) {
                    /* Require level 2 or forcibly displayed progress counter for summarized updates */
                    DISPLAYLEVEL(1, "\r%79s\r", "");    /* Clear out the current displayed line */
                    if (fCtx->nbFilesTotal > 1) {
                        size_t srcFileNameSize = strlen(srcFileName);
                        /* Ensure that the string we print is roughly the same size each time */
                        if (srcFileNameSize > 18) {
                            const char* truncatedSrcFileName = srcFileName + srcFileNameSize - 15;
                            DISPLAYLEVEL(1, "Compress: %u/%u files. Current: ...%s ",
                                        fCtx->currFileIdx+1, fCtx->nbFilesTotal, truncatedSrcFileName);
                        } else {
                            DISPLAYLEVEL(1, "Compress: %u/%u files. Current: %*s ",
                                        fCtx->currFileIdx+1, fCtx->nbFilesTotal, (int)(18-srcFileNameSize), srcFileName);
                        }
                    }
                    DISPLAYLEVEL(1, "Read:%6.*f%4s ", consumed_hrs.precision, consumed_hrs.value, consumed_hrs.suffix);
                    if (fileSize != UTIL_FILESIZE_UNKNOWN)
                        DISPLAYLEVEL(2, "/%6.*f%4s", file_hrs.precision, file_hrs.value, file_hrs.suffix);
                    DISPLAYLEVEL(1, " ==> %2.f%%", cShare);
                    DELAY_NEXT_UPDATE();
                }

                /* adaptive mode : statistics measurement and speed correction */
                if (prefs->adaptiveMode) {

                    /* check output speed */
                    if (zfp.currentJobID > 1) {  /* only possible if nbWorkers >= 1 */

                        unsigned long long newlyProduced = zfp.produced - previous_zfp_update.produced;
                        unsigned long long newlyFlushed = zfp.flushed - previous_zfp_update.flushed;
                        assert(zfp.produced >= previous_zfp_update.produced);
                        assert(prefs->nbWorkers >= 1);

                        /* test if compression is blocked
                         * either because output is slow and all buffers are full
                         * or because input is slow and no job can start while waiting for at least one buffer to be filled.
                         * note : exclude starting part, since currentJobID > 1 */
                        if ( (zfp.consumed == previous_zfp_update.consumed)   /* no data compressed : no data available, or no more buffer to compress to, OR compression is really slow (compression of a single block is slower than update rate)*/
                          && (zfp.nbActiveWorkers == 0)                       /* confirmed : no compression ongoing */
                          ) {
                            DISPLAYLEVEL(6, "all buffers full : compression stopped => slow down \n")
                            speedChange = slower;
                        }

                        previous_zfp_update = zfp;

                        if ( (newlyProduced > (newlyFlushed * 9 / 8))   /* compression produces more data than output can flush (though production can be spiky, due to work unit : (N==4)*block sizes) */
                          && (flushWaiting == 0)                        /* flush speed was never slowed by lack of production, so it's operating at max capacity */
                          ) {
                            DISPLAYLEVEL(6, "compression faster than flush (%llu > %llu), and flushed was never slowed down by lack of production => slow down \n", newlyProduced, newlyFlushed);
                            speedChange = slower;
                        }
                        flushWaiting = 0;
                    }

                    /* course correct only if there is at least one new job completed */
                    if (zfp.currentJobID > lastJobID) {
                        DISPLAYLEVEL(6, "compression level adaptation check \n")

                        /* check input speed */
                        if (zfp.currentJobID > (unsigned)(prefs->nbWorkers+1)) {   /* warm up period, to fill all workers */
                            if (inputBlocked <= 0) {
                                DISPLAYLEVEL(6, "input is never blocked => input is slower than ingestion \n");
                                speedChange = slower;
                            } else if (speedChange == noChange) {
                                unsigned long long newlyIngested = zfp.ingested - previous_zfp_correction.ingested;
                                unsigned long long newlyConsumed = zfp.consumed - previous_zfp_correction.consumed;
                                unsigned long long newlyProduced = zfp.produced - previous_zfp_correction.produced;
                                unsigned long long newlyFlushed  = zfp.flushed  - previous_zfp_correction.flushed;
                                previous_zfp_correction = zfp;
                                assert(inputPresented > 0);
                                DISPLAYLEVEL(6, "input blocked %u/%u(%.2f) - ingested:%u vs %u:consumed - flushed:%u vs %u:produced \n",
                                                inputBlocked, inputPresented, (double)inputBlocked/inputPresented*100,
                                                (unsigned)newlyIngested, (unsigned)newlyConsumed,
                                                (unsigned)newlyFlushed, (unsigned)newlyProduced);
                                if ( (inputBlocked > inputPresented / 8)     /* input is waiting often, because input buffers is full : compression or output too slow */
                                  && (newlyFlushed * 33 / 32 > newlyProduced)  /* flush everything that is produced */
                                  && (newlyIngested * 33 / 32 > newlyConsumed) /* input speed as fast or faster than compression speed */
                                ) {
                                    DISPLAYLEVEL(6, "recommend faster as in(%llu) >= (%llu)comp(%llu) <= out(%llu) \n",
                                                    newlyIngested, newlyConsumed, newlyProduced, newlyFlushed);
                                    speedChange = faster;
                                }
                            }
                            inputBlocked = 0;
                            inputPresented = 0;
                        }

                        if (speedChange == slower) {
                            DISPLAYLEVEL(6, "slower speed , higher compression \n")
                            compressionLevel ++;
                            if (compressionLevel > ZSTD_maxCLevel()) compressionLevel = ZSTD_maxCLevel();
                            if (compressionLevel > prefs->maxAdaptLevel) compressionLevel = prefs->maxAdaptLevel;
                            compressionLevel += (compressionLevel == 0);   /* skip 0 */
                            ZSTD_CCtx_setParameter(ress.cctx, ZSTD_c_compressionLevel, compressionLevel);
                        }
                        if (speedChange == faster) {
                            DISPLAYLEVEL(6, "faster speed , lighter compression \n")
                            compressionLevel --;
                            if (compressionLevel < prefs->minAdaptLevel) compressionLevel = prefs->minAdaptLevel;
                            compressionLevel -= (compressionLevel == 0);   /* skip 0 */
                            ZSTD_CCtx_setParameter(ress.cctx, ZSTD_c_compressionLevel, compressionLevel);
                        }
                        speedChange = noChange;

                        lastJobID = zfp.currentJobID;
                    }  /* if (zfp.currentJobID > lastJobID) */
                }  /* if (g_adaptiveMode) */
            }  /* if (READY_FOR_UPDATE()) */
        }  /* while ((inBuff.pos != inBuff.size) */
    } while (directive != ZSTD_e_end);

    if (ferror(srcFile)) {
        EXM_THROW(26, "Read error : I/O error");
    }
    if (fileSize != UTIL_FILESIZE_UNKNOWN && *readsize != fileSize) {
        EXM_THROW(27, "Read error : Incomplete read : %llu / %llu B",
                (unsigned long long)*readsize, (unsigned long long)fileSize);
    }

    return compressedfilesize;
}

/*! FIO_compressFilename_internal() :
 *  same as FIO_compressFilename_extRess(), with `ress.desFile` already opened.
 *  @return : 0 : compression completed correctly,
 *            1 : missing or pb opening srcFileName
 */
static int
FIO_compressFilename_internal(FIO_ctx_t* const fCtx,
                              FIO_prefs_t* const prefs,
                              cRess_t ress,
                              const char* dstFileName, const char* srcFileName,
                              int compressionLevel)
{
    UTIL_time_t const timeStart = UTIL_getTime();
    clock_t const cpuStart = clock();
    U64 readsize = 0;
    U64 compressedfilesize = 0;
    U64 const fileSize = UTIL_getFileSize(srcFileName);
    DISPLAYLEVEL(5, "%s: %llu bytes \n", srcFileName, (unsigned long long)fileSize);

    /* compression format selection */
    switch (prefs->compressionType) {
        default:
        case FIO_zstdCompression:
            compressedfilesize = FIO_compressZstdFrame(fCtx, prefs, &ress, srcFileName, fileSize, compressionLevel, &readsize);
            break;

        case FIO_gzipCompression:
#ifdef ZSTD_GZCOMPRESS
            compressedfilesize = FIO_compressGzFrame(&ress, srcFileName, fileSize, compressionLevel, &readsize);
#else
            (void)compressionLevel;
            EXM_THROW(20, "zstd: %s: file cannot be compressed as gzip (zstd compiled without ZSTD_GZCOMPRESS) -- ignored \n",
                            srcFileName);
#endif
            break;

        case FIO_xzCompression:
        case FIO_lzmaCompression:
#ifdef ZSTD_LZMACOMPRESS
            compressedfilesize = FIO_compressLzmaFrame(&ress, srcFileName, fileSize, compressionLevel, &readsize, prefs->compressionType==FIO_lzmaCompression);
#else
            (void)compressionLevel;
            EXM_THROW(20, "zstd: %s: file cannot be compressed as xz/lzma (zstd compiled without ZSTD_LZMACOMPRESS) -- ignored \n",
                            srcFileName);
#endif
            break;

        case FIO_lz4Compression:
#ifdef ZSTD_LZ4COMPRESS
            compressedfilesize = FIO_compressLz4Frame(&ress, srcFileName, fileSize, compressionLevel, prefs->checksumFlag, &readsize);
#else
            (void)compressionLevel;
            EXM_THROW(20, "zstd: %s: file cannot be compressed as lz4 (zstd compiled without ZSTD_LZ4COMPRESS) -- ignored \n",
                            srcFileName);
#endif
            break;
    }

    /* Status */
    fCtx->totalBytesInput += (size_t)readsize;
    fCtx->totalBytesOutput += (size_t)compressedfilesize;
    DISPLAYLEVEL(2, "\r%79s\r", "");
    if (g_display_prefs.displayLevel >= 2 &&
        !fCtx->hasStdoutOutput &&
        (g_display_prefs.displayLevel >= 3 || fCtx->nbFilesTotal <= 1)) {
        UTIL_HumanReadableSize_t hr_isize = UTIL_makeHumanReadableSize((U64) readsize);
        UTIL_HumanReadableSize_t hr_osize = UTIL_makeHumanReadableSize((U64) compressedfilesize);
        if (readsize == 0) {
            DISPLAYLEVEL(2,"%-20s :  (%6.*f%4s => %6.*f%4s, %s) \n",
                srcFileName,
                hr_isize.precision, hr_isize.value, hr_isize.suffix,
                hr_osize.precision, hr_osize.value, hr_osize.suffix,
                dstFileName);
        } else {
            DISPLAYLEVEL(2,"%-20s :%6.2f%%   (%6.*f%4s => %6.*f%4s, %s) \n",
                srcFileName,
                (double)compressedfilesize / (double)readsize * 100,
                hr_isize.precision, hr_isize.value, hr_isize.suffix,
                hr_osize.precision, hr_osize.value, hr_osize.suffix,
                dstFileName);
        }
    }

    /* Elapsed Time and CPU Load */
    {   clock_t const cpuEnd = clock();
        double const cpuLoad_s = (double)(cpuEnd - cpuStart) / CLOCKS_PER_SEC;
        U64 const timeLength_ns = UTIL_clockSpanNano(timeStart);
        double const timeLength_s = (double)timeLength_ns / 1000000000;
        double const cpuLoad_pct = (cpuLoad_s / timeLength_s) * 100;
        DISPLAYLEVEL(4, "%-20s : Completed in %.2f sec  (cpu load : %.0f%%)\n",
                        srcFileName, timeLength_s, cpuLoad_pct);
    }
    return 0;
}


/*! FIO_compressFilename_dstFile() :
 *  open dstFileName, or pass-through if ress.dstFile != NULL,
 *  then start compression with FIO_compressFilename_internal().
 *  Manages source removal (--rm) and file permissions transfer.
 *  note : ress.srcFile must be != NULL,
 *  so reach this function through FIO_compressFilename_srcFile().
 *  @return : 0 : compression completed correctly,
 *            1 : pb
 */
static int FIO_compressFilename_dstFile(FIO_ctx_t* const fCtx,
                                        FIO_prefs_t* const prefs,
                                        cRess_t ress,
                                        const char* dstFileName,
                                        const char* srcFileName,
                                        int compressionLevel)
{
    int closeDstFile = 0;
    int result;
    stat_t statbuf;
    int transferMTime = 0;
    assert(ress.srcFile != NULL);
    if (ress.dstFile == NULL) {
        int dstFilePermissions = DEFAULT_FILE_PERMISSIONS;
        if ( strcmp (srcFileName, stdinmark)
          && UTIL_stat(srcFileName, &statbuf)
          && UTIL_isRegularFileStat(&statbuf) ) {
            dstFilePermissions = statbuf.st_mode;
            transferMTime = 1;
        }

        closeDstFile = 1;
        DISPLAYLEVEL(6, "FIO_compressFilename_dstFile: opening dst: %s \n", dstFileName);
        ress.dstFile = FIO_openDstFile(fCtx, prefs, srcFileName, dstFileName, dstFilePermissions);
        if (ress.dstFile==NULL) return 1;  /* could not open dstFileName */
        /* Must only be added after FIO_openDstFile() succeeds.
         * Otherwise we may delete the destination file if it already exists,
         * and the user presses Ctrl-C when asked if they wish to overwrite.
         */
        addHandler(dstFileName);
    }

    result = FIO_compressFilename_internal(fCtx, prefs, ress, dstFileName, srcFileName, compressionLevel);

    if (closeDstFile) {
        FILE* const dstFile = ress.dstFile;
        ress.dstFile = NULL;

        clearHandler();

        DISPLAYLEVEL(6, "FIO_compressFilename_dstFile: closing dst: %s \n", dstFileName);
        if (fclose(dstFile)) { /* error closing dstFile */
            DISPLAYLEVEL(1, "zstd: %s: %s \n", dstFileName, strerror(errno));
            result=1;
        }
        if (transferMTime) {
            UTIL_utime(dstFileName, &statbuf);
        }
        if ( (result != 0)  /* operation failure */
          && strcmp(dstFileName, stdoutmark)  /* special case : don't remove() stdout */
          ) {
            FIO_removeFile(dstFileName); /* remove compression artefact; note don't do anything special if remove() fails */
        }
    }

    return result;
}

/* List used to compare file extensions (used with --exclude-compressed flag)
* Different from the suffixList and should only apply to ZSTD compress operationResult
*/
static const char *compressedFileExtensions[] = {
    ZSTD_EXTENSION,
    TZSTD_EXTENSION,
    GZ_EXTENSION,
    TGZ_EXTENSION,
    LZMA_EXTENSION,
    XZ_EXTENSION,
    TXZ_EXTENSION,
    LZ4_EXTENSION,
    TLZ4_EXTENSION,
    NULL
};

/*! FIO_compressFilename_srcFile() :
 *  @return : 0 : compression completed correctly,
 *            1 : missing or pb opening srcFileName
 */
static int
FIO_compressFilename_srcFile(FIO_ctx_t* const fCtx,
                             FIO_prefs_t* const prefs,
                             cRess_t ress,
                             const char* dstFileName,
                             const char* srcFileName,
                             int compressionLevel)
{
    int result;
    DISPLAYLEVEL(6, "FIO_compressFilename_srcFile: %s \n", srcFileName);

    /* ensure src is not a directory */
    if (UTIL_isDirectory(srcFileName)) {
        DISPLAYLEVEL(1, "zstd: %s is a directory -- ignored \n", srcFileName);
        return 1;
    }

    /* ensure src is not the same as dict (if present) */
    if (ress.dictFileName != NULL && UTIL_isSameFile(srcFileName, ress.dictFileName)) {
        DISPLAYLEVEL(1, "zstd: cannot use %s as an input file and dictionary \n", srcFileName);
        return 1;
    }

    /* Check if "srcFile" is compressed. Only done if --exclude-compressed flag is used
    * YES => ZSTD will skip compression of the file and will return 0.
    * NO => ZSTD will resume with compress operation.
    */
    if (prefs->excludeCompressedFiles == 1 && UTIL_isCompressedFile(srcFileName, compressedFileExtensions)) {
        DISPLAYLEVEL(4, "File is already compressed : %s \n", srcFileName);
        return 0;
    }

    ress.srcFile = FIO_openSrcFile(prefs, srcFileName);
    if (ress.srcFile == NULL) return 1;   /* srcFile could not be opened */

    result = FIO_compressFilename_dstFile(fCtx, prefs, ress, dstFileName, srcFileName, compressionLevel);

    fclose(ress.srcFile);
    ress.srcFile = NULL;
    if ( prefs->removeSrcFile   /* --rm */
      && result == 0       /* success */
      && strcmp(srcFileName, stdinmark)   /* exception : don't erase stdin */
      ) {
        /* We must clear the handler, since after this point calling it would
         * delete both the source and destination files.
         */
        clearHandler();
        if (FIO_removeFile(srcFileName))
            EXM_THROW(1, "zstd: %s: %s", srcFileName, strerror(errno));
    }
    return result;
}

static const char* checked_index(const char* options[], size_t length, size_t index) {
    assert(index < length);
    // Necessary to avoid warnings since -O3 will omit the above `assert`
    (void) length;
    return options[index];
}

#define INDEX(options, index) checked_index((options), sizeof(options)  / sizeof(char*), (index))

void FIO_displayCompressionParameters(const FIO_prefs_t* prefs) {
    static const char* formatOptions[5] = {ZSTD_EXTENSION, GZ_EXTENSION, XZ_EXTENSION,
                                           LZMA_EXTENSION, LZ4_EXTENSION};
    static const char* sparseOptions[3] = {" --no-sparse", "", " --sparse"};
    static const char* checkSumOptions[3] = {" --no-check", "", " --check"};
    static const char* rowMatchFinderOptions[3] = {"", " --no-row-match-finder", " --row-match-finder"};
    static const char* compressLiteralsOptions[3] = {"", " --compress-literals", " --no-compress-literals"};

    assert(g_display_prefs.displayLevel >= 4);

    DISPLAY("--format=%s", formatOptions[prefs->compressionType]);
    DISPLAY("%s", INDEX(sparseOptions, prefs->sparseFileSupport));
    DISPLAY("%s", prefs->dictIDFlag ? "" : " --no-dictID");
    DISPLAY("%s", INDEX(checkSumOptions, prefs->checksumFlag));
    DISPLAY(" --block-size=%d", prefs->blockSize);
    if (prefs->adaptiveMode)
        DISPLAY(" --adapt=min=%d,max=%d", prefs->minAdaptLevel, prefs->maxAdaptLevel);
    DISPLAY("%s", INDEX(rowMatchFinderOptions, prefs->useRowMatchFinder));
    DISPLAY("%s", prefs->rsyncable ? " --rsyncable" : "");
    if (prefs->streamSrcSize)
        DISPLAY(" --stream-size=%u", (unsigned) prefs->streamSrcSize);
    if (prefs->srcSizeHint)
        DISPLAY(" --size-hint=%d", prefs->srcSizeHint);
    if (prefs->targetCBlockSize)
        DISPLAY(" --target-compressed-block-size=%u", (unsigned) prefs->targetCBlockSize);
    DISPLAY("%s", INDEX(compressLiteralsOptions, prefs->literalCompressionMode));
    DISPLAY(" --memory=%u", prefs->memLimit ? prefs->memLimit : 128 MB);
    DISPLAY(" --threads=%d", prefs->nbWorkers);
    DISPLAY("%s", prefs->excludeCompressedFiles ? " --exclude-compressed" : "");
    DISPLAY(" --%scontent-size", prefs->contentSize ? "" : "no-");
    DISPLAY("\n");
}

#undef INDEX

int FIO_compressFilename(FIO_ctx_t* const fCtx, FIO_prefs_t* const prefs, const char* dstFileName,
                         const char* srcFileName, const char* dictFileName,
                         int compressionLevel, ZSTD_compressionParameters comprParams)
{
    cRess_t const ress = FIO_createCResources(prefs, dictFileName, UTIL_getFileSize(srcFileName), compressionLevel, comprParams);
    int const result = FIO_compressFilename_srcFile(fCtx, prefs, ress, dstFileName, srcFileName, compressionLevel);

#define DISPLAY_LEVEL_DEFAULT 2

    FIO_freeCResources(&ress);
    return result;
}

/* FIO_determineCompressedName() :
 * create a destination filename for compressed srcFileName.
 * @return a pointer to it.
 * This function never returns an error (it may abort() in case of pb)
 */
static const char*
FIO_determineCompressedName(const char* srcFileName, const char* outDirName, const char* suffix)
{
    static size_t dfnbCapacity = 0;
    static char* dstFileNameBuffer = NULL;   /* using static allocation : this function cannot be multi-threaded */
    char* outDirFilename = NULL;
    size_t sfnSize = strlen(srcFileName);
    size_t const srcSuffixLen = strlen(suffix);
    if (outDirName) {
        outDirFilename = FIO_createFilename_fromOutDir(srcFileName, outDirName, srcSuffixLen);
        sfnSize = strlen(outDirFilename);
        assert(outDirFilename != NULL);
    }

    if (dfnbCapacity <= sfnSize+srcSuffixLen+1) {
        /* resize buffer for dstName */
        free(dstFileNameBuffer);
        dfnbCapacity = sfnSize + srcSuffixLen + 30;
        dstFileNameBuffer = (char*)malloc(dfnbCapacity);
        if (!dstFileNameBuffer) {
            EXM_THROW(30, "zstd: %s", strerror(errno));
        }
    }
    assert(dstFileNameBuffer != NULL);

    if (outDirFilename) {
        memcpy(dstFileNameBuffer, outDirFilename, sfnSize);
        free(outDirFilename);
    } else {
        memcpy(dstFileNameBuffer, srcFileName, sfnSize);
    }
    memcpy(dstFileNameBuffer+sfnSize, suffix, srcSuffixLen+1 /* Include terminating null */);
    return dstFileNameBuffer;
}

static unsigned long long FIO_getLargestFileSize(const char** inFileNames, unsigned nbFiles)
{
    size_t i;
    unsigned long long fileSize, maxFileSize = 0;
    for (i = 0; i < nbFiles; i++) {
        fileSize = UTIL_getFileSize(inFileNames[i]);
        maxFileSize = fileSize > maxFileSize ? fileSize : maxFileSize;
    }
    return maxFileSize;
}

/* FIO_compressMultipleFilenames() :
 * compress nbFiles files
 * into either one destination (outFileName),
 * or into one file each (outFileName == NULL, but suffix != NULL),
 * or into a destination folder (specified with -O)
 */
int FIO_compressMultipleFilenames(FIO_ctx_t* const fCtx,
                                  FIO_prefs_t* const prefs,
                                  const char** inFileNamesTable,
                                  const char* outMirroredRootDirName,
                                  const char* outDirName,
                                  const char* outFileName, const char* suffix,
                                  const char* dictFileName, int compressionLevel,
                                  ZSTD_compressionParameters comprParams)
{
    int status;
    int error = 0;
    cRess_t ress = FIO_createCResources(prefs, dictFileName,
        FIO_getLargestFileSize(inFileNamesTable, (unsigned)fCtx->nbFilesTotal),
        compressionLevel, comprParams);

    /* init */
    assert(outFileName != NULL || suffix != NULL);
    if (outFileName != NULL) {   /* output into a single destination (stdout typically) */
        if (FIO_removeMultiFilesWarning(fCtx, prefs, outFileName, 1 /* displayLevelCutoff */)) {
            FIO_freeCResources(&ress);
            return 1;
        }
        ress.dstFile = FIO_openDstFile(fCtx, prefs, NULL, outFileName, DEFAULT_FILE_PERMISSIONS);
        if (ress.dstFile == NULL) {  /* could not open outFileName */
            error = 1;
        } else {
            for (; fCtx->currFileIdx < fCtx->nbFilesTotal; ++fCtx->currFileIdx) {
                status = FIO_compressFilename_srcFile(fCtx, prefs, ress, outFileName, inFileNamesTable[fCtx->currFileIdx], compressionLevel);
                if (!status) fCtx->nbFilesProcessed++;
                error |= status;
            }
            if (fclose(ress.dstFile))
                EXM_THROW(29, "Write error (%s) : cannot properly close %s",
                            strerror(errno), outFileName);
            ress.dstFile = NULL;
        }
    } else {
        if (outMirroredRootDirName)
            UTIL_mirrorSourceFilesDirectories(inFileNamesTable, (unsigned)fCtx->nbFilesTotal, outMirroredRootDirName);

        for (; fCtx->currFileIdx < fCtx->nbFilesTotal; ++fCtx->currFileIdx) {
            const char* const srcFileName = inFileNamesTable[fCtx->currFileIdx];
            const char* dstFileName = NULL;
            if (outMirroredRootDirName) {
                char* validMirroredDirName = UTIL_createMirroredDestDirName(srcFileName, outMirroredRootDirName);
                if (validMirroredDirName) {
                    dstFileName = FIO_determineCompressedName(srcFileName, validMirroredDirName, suffix);
                    free(validMirroredDirName);
                } else {
                    DISPLAYLEVEL(2, "zstd: --output-dir-mirror cannot compress '%s' into '%s' \n", srcFileName, outMirroredRootDirName);
                    error=1;
                    continue;
                }
            } else {
                dstFileName = FIO_determineCompressedName(srcFileName, outDirName, suffix);  /* cannot fail */
            }
            status = FIO_compressFilename_srcFile(fCtx, prefs, ress, dstFileName, srcFileName, compressionLevel);
            if (!status) fCtx->nbFilesProcessed++;
            error |= status;
        }

        if (outDirName)
            FIO_checkFilenameCollisions(inFileNamesTable , (unsigned)fCtx->nbFilesTotal);
    }

    if (fCtx->nbFilesProcessed >= 1 && fCtx->nbFilesTotal > 1 && fCtx->totalBytesInput != 0) {
        UTIL_HumanReadableSize_t hr_isize = UTIL_makeHumanReadableSize((U64) fCtx->totalBytesInput);
        UTIL_HumanReadableSize_t hr_osize = UTIL_makeHumanReadableSize((U64) fCtx->totalBytesOutput);

        DISPLAYLEVEL(2, "\r%79s\r", "");
        DISPLAYLEVEL(2, "%3d files compressed :%.2f%%   (%6.*f%4s => %6.*f%4s)\n",
                        fCtx->nbFilesProcessed,
                        (double)fCtx->totalBytesOutput/((double)fCtx->totalBytesInput)*100,
                        hr_isize.precision, hr_isize.value, hr_isize.suffix,
                        hr_osize.precision, hr_osize.value, hr_osize.suffix);
    }

    FIO_freeCResources(&ress);
    return error;
}

#endif /* #ifndef ZSTD_NOCOMPRESS */



#ifndef ZSTD_NODECOMPRESS

/* **************************************************************************
 *  Decompression
 ***************************************************************************/
typedef struct {
    void*  srcBuffer;
    size_t srcBufferSize;
    size_t srcBufferLoaded;
    void*  dstBuffer;
    size_t dstBufferSize;
    ZSTD_DStream* dctx;
    FILE*  dstFile;
} dRess_t;

static dRess_t FIO_createDResources(FIO_prefs_t* const prefs, const char* dictFileName)
{
    dRess_t ress;
    memset(&ress, 0, sizeof(ress));

    if (prefs->patchFromMode)
        FIO_adjustMemLimitForPatchFromMode(prefs, UTIL_getFileSize(dictFileName), 0 /* just use the dict size */);

    /* Allocation */
    ress.dctx = ZSTD_createDStream();
    if (ress.dctx==NULL)
        EXM_THROW(60, "Error: %s : can't create ZSTD_DStream", strerror(errno));
    CHECK( ZSTD_DCtx_setMaxWindowSize(ress.dctx, prefs->memLimit) );
    CHECK( ZSTD_DCtx_setParameter(ress.dctx, ZSTD_d_forceIgnoreChecksum, !prefs->checksumFlag));

    ress.srcBufferSize = ZSTD_DStreamInSize();
    ress.srcBuffer = malloc(ress.srcBufferSize);
    ress.dstBufferSize = ZSTD_DStreamOutSize();
    ress.dstBuffer = malloc(ress.dstBufferSize);
    if (!ress.srcBuffer || !ress.dstBuffer)
        EXM_THROW(61, "Allocation error : not enough memory");

    /* dictionary */
    {   void* dictBuffer;
        size_t const dictBufferSize = FIO_createDictBuffer(&dictBuffer, dictFileName, prefs);
        CHECK( ZSTD_initDStream_usingDict(ress.dctx, dictBuffer, dictBufferSize) );
        free(dictBuffer);
    }

    return ress;
}

static void FIO_freeDResources(dRess_t ress)
{
    CHECK( ZSTD_freeDStream(ress.dctx) );
    free(ress.srcBuffer);
    free(ress.dstBuffer);
}


/** FIO_fwriteSparse() :
*  @return : storedSkips,
*            argument for next call to FIO_fwriteSparse() or FIO_fwriteSparseEnd() */
static unsigned
FIO_fwriteSparse(FILE* file,
                 const void* buffer, size_t bufferSize,
                 const FIO_prefs_t* const prefs,
                 unsigned storedSkips)
{
    const size_t* const bufferT = (const size_t*)buffer;   /* Buffer is supposed malloc'ed, hence aligned on size_t */
    size_t bufferSizeT = bufferSize / sizeof(size_t);
    const size_t* const bufferTEnd = bufferT + bufferSizeT;
    const size_t* ptrT = bufferT;
    static const size_t segmentSizeT = (32 KB) / sizeof(size_t);   /* check every 32 KB */

    if (prefs->testMode) return 0;  /* do not output anything in test mode */

    if (!prefs->sparseFileSupport) {  /* normal write */
        size_t const sizeCheck = fwrite(buffer, 1, bufferSize, file);
        if (sizeCheck != bufferSize)
            EXM_THROW(70, "Write error : cannot write decoded block : %s",
                            strerror(errno));
        return 0;
    }

    /* avoid int overflow */
    if (storedSkips > 1 GB) {
        if (LONG_SEEK(file, 1 GB, SEEK_CUR) != 0)
            EXM_THROW(91, "1 GB skip error (sparse file support)");
        storedSkips -= 1 GB;
    }

    while (ptrT < bufferTEnd) {
        size_t nb0T;

        /* adjust last segment if < 32 KB */
        size_t seg0SizeT = segmentSizeT;
        if (seg0SizeT > bufferSizeT) seg0SizeT = bufferSizeT;
        bufferSizeT -= seg0SizeT;

        /* count leading zeroes */
        for (nb0T=0; (nb0T < seg0SizeT) && (ptrT[nb0T] == 0); nb0T++) ;
        storedSkips += (unsigned)(nb0T * sizeof(size_t));

        if (nb0T != seg0SizeT) {   /* not all 0s */
            size_t const nbNon0ST = seg0SizeT - nb0T;
            /* skip leading zeros */
            if (LONG_SEEK(file, storedSkips, SEEK_CUR) != 0)
                EXM_THROW(92, "Sparse skip error ; try --no-sparse");
            storedSkips = 0;
            /* write the rest */
            if (fwrite(ptrT + nb0T, sizeof(size_t), nbNon0ST, file) != nbNon0ST)
                EXM_THROW(93, "Write error : cannot write decoded block : %s",
                            strerror(errno));
        }
        ptrT += seg0SizeT;
    }

    {   static size_t const maskT = sizeof(size_t)-1;
        if (bufferSize & maskT) {
            /* size not multiple of sizeof(size_t) : implies end of block */
            const char* const restStart = (const char*)bufferTEnd;
            const char* restPtr = restStart;
            const char* const restEnd = (const char*)buffer + bufferSize;
            assert(restEnd > restStart && restEnd < restStart + sizeof(size_t));
            for ( ; (restPtr < restEnd) && (*restPtr == 0); restPtr++) ;
            storedSkips += (unsigned) (restPtr - restStart);
            if (restPtr != restEnd) {
                /* not all remaining bytes are 0 */
                size_t const restSize = (size_t)(restEnd - restPtr);
                if (LONG_SEEK(file, storedSkips, SEEK_CUR) != 0)
                    EXM_THROW(92, "Sparse skip error ; try --no-sparse");
                if (fwrite(restPtr, 1, restSize, file) != restSize)
                    EXM_THROW(95, "Write error : cannot write end of decoded block : %s",
                        strerror(errno));
                storedSkips = 0;
    }   }   }

    return storedSkips;
}

static void
FIO_fwriteSparseEnd(const FIO_prefs_t* const prefs, FILE* file, unsigned storedSkips)
{
    if (prefs->testMode) assert(storedSkips == 0);
    if (storedSkips>0) {
        assert(prefs->sparseFileSupport > 0);  /* storedSkips>0 implies sparse support is enabled */
        (void)prefs;   /* assert can be disabled, in which case prefs becomes unused */
        if (LONG_SEEK(file, storedSkips-1, SEEK_CUR) != 0)
            EXM_THROW(69, "Final skip error (sparse file support)");
        /* last zero must be explicitly written,
         * so that skipped ones get implicitly translated as zero by FS */
        {   const char lastZeroByte[1] = { 0 };
            if (fwrite(lastZeroByte, 1, 1, file) != 1)
                EXM_THROW(69, "Write error : cannot write last zero : %s", strerror(errno));
    }   }
}


/** FIO_passThrough() : just copy input into output, for compatibility with gzip -df mode
    @return : 0 (no error) */
static int FIO_passThrough(const FIO_prefs_t* const prefs,
                           FILE* foutput, FILE* finput,
                           void* buffer, size_t bufferSize,
                           size_t alreadyLoaded)
{
    size_t const blockSize = MIN(64 KB, bufferSize);
    size_t readFromInput;
    unsigned storedSkips = 0;

    /* assumption : ress->srcBufferLoaded bytes already loaded and stored within buffer */
    {   size_t const sizeCheck = fwrite(buffer, 1, alreadyLoaded, foutput);
        if (sizeCheck != alreadyLoaded) {
            DISPLAYLEVEL(1, "Pass-through write error : %s\n", strerror(errno));
            return 1;
    }   }

    do {
        readFromInput = fread(buffer, 1, blockSize, finput);
        storedSkips = FIO_fwriteSparse(foutput, buffer, readFromInput, prefs, storedSkips);
    } while (readFromInput == blockSize);
    if (ferror(finput)) {
        DISPLAYLEVEL(1, "Pass-through read error : %s\n", strerror(errno));
        return 1;
    }
    assert(feof(finput));

    FIO_fwriteSparseEnd(prefs, foutput, storedSkips);
    return 0;
}

/* FIO_zstdErrorHelp() :
 * detailed error message when requested window size is too large */
static void
FIO_zstdErrorHelp(const FIO_prefs_t* const prefs,
                  const dRess_t* ress,
                  size_t err, const char* srcFileName)
{
    ZSTD_frameHeader header;

    /* Help message only for one specific error */
    if (ZSTD_getErrorCode(err) != ZSTD_error_frameParameter_windowTooLarge)
        return;

    /* Try to decode the frame header */
    err = ZSTD_getFrameHeader(&header, ress->srcBuffer, ress->srcBufferLoaded);
    if (err == 0) {
        unsigned long long const windowSize = header.windowSize;
        unsigned const windowLog = FIO_highbit64(windowSize) + ((windowSize & (windowSize - 1)) != 0);
        assert(prefs->memLimit > 0);
        DISPLAYLEVEL(1, "%s : Window size larger than maximum : %llu > %u \n",
                        srcFileName, windowSize, prefs->memLimit);
        if (windowLog <= ZSTD_WINDOWLOG_MAX) {
            unsigned const windowMB = (unsigned)((windowSize >> 20) + ((windowSize & ((1 MB) - 1)) != 0));
            assert(windowSize < (U64)(1ULL << 52));   /* ensure now overflow for windowMB */
            DISPLAYLEVEL(1, "%s : Use --long=%u or --memory=%uMB \n",
                            srcFileName, windowLog, windowMB);
            return;
    }   }
    DISPLAYLEVEL(1, "%s : Window log larger than ZSTD_WINDOWLOG_MAX=%u; not supported \n",
                    srcFileName, ZSTD_WINDOWLOG_MAX);
}

/** FIO_decompressFrame() :
 *  @return : size of decoded zstd frame, or an error code
 */
#define FIO_ERROR_FRAME_DECODING   ((unsigned long long)(-2))
static unsigned long long
FIO_decompressZstdFrame(FIO_ctx_t* const fCtx, dRess_t* ress, FILE* finput,
                        const FIO_prefs_t* const prefs,
                        const char* srcFileName,
                        U64 alreadyDecoded)  /* for multi-frames streams */
{
    U64 frameSize = 0;
    U32 storedSkips = 0;

    /* display last 20 characters only */
    {   size_t const srcFileLength = strlen(srcFileName);
        if (srcFileLength>20) srcFileName += srcFileLength-20;
    }

    ZSTD_DCtx_reset(ress->dctx, ZSTD_reset_session_only);

    /* Header loading : ensures ZSTD_getFrameHeader() will succeed */
    {   size_t const toDecode = ZSTD_FRAMEHEADERSIZE_MAX;
        if (ress->srcBufferLoaded < toDecode) {
            size_t const toRead = toDecode - ress->srcBufferLoaded;
            void* const startPosition = (char*)ress->srcBuffer + ress->srcBufferLoaded;
            ress->srcBufferLoaded += fread(startPosition, 1, toRead, finput);
    }   }

    /* Main decompression Loop */
    while (1) {
        ZSTD_inBuffer  inBuff = { ress->srcBuffer, ress->srcBufferLoaded, 0 };
        ZSTD_outBuffer outBuff= { ress->dstBuffer, ress->dstBufferSize, 0 };
        size_t const readSizeHint = ZSTD_decompressStream(ress->dctx, &outBuff, &inBuff);
        const int displayLevel = (!fCtx->hasStdoutOutput || g_display_prefs.progressSetting == FIO_ps_always) ? 1 : 2;
        UTIL_HumanReadableSize_t const hrs = UTIL_makeHumanReadableSize(alreadyDecoded+frameSize);
        if (ZSTD_isError(readSizeHint)) {
            DISPLAYLEVEL(1, "%s : Decoding error (36) : %s \n",
                            srcFileName, ZSTD_getErrorName(readSizeHint));
            FIO_zstdErrorHelp(prefs, ress, readSizeHint, srcFileName);
            return FIO_ERROR_FRAME_DECODING;
        }

        /* Write block */
        storedSkips = FIO_fwriteSparse(ress->dstFile, ress->dstBuffer, outBuff.pos, prefs, storedSkips);
        frameSize += outBuff.pos;
        if (fCtx->nbFilesTotal > 1) {
            size_t srcFileNameSize = strlen(srcFileName);
            if (srcFileNameSize > 18) {
                const char* truncatedSrcFileName = srcFileName + srcFileNameSize - 15;
                DISPLAYUPDATE(displayLevel, "\rDecompress: %2u/%2u files. Current: ...%s : %.*f%s...    ",
                                fCtx->currFileIdx+1, fCtx->nbFilesTotal, truncatedSrcFileName, hrs.precision, hrs.value, hrs.suffix);
            } else {
                DISPLAYUPDATE(displayLevel, "\rDecompress: %2u/%2u files. Current: %s : %.*f%s...    ",
                            fCtx->currFileIdx+1, fCtx->nbFilesTotal, srcFileName, hrs.precision, hrs.value, hrs.suffix);
            }
        } else {
            DISPLAYUPDATE(displayLevel, "\r%-20.20s : %.*f%s...     ",
                            srcFileName, hrs.precision, hrs.value, hrs.suffix);
        }

        if (inBuff.pos > 0) {
            memmove(ress->srcBuffer, (char*)ress->srcBuffer + inBuff.pos, inBuff.size - inBuff.pos);
            ress->srcBufferLoaded -= inBuff.pos;
        }

        if (readSizeHint == 0) break;   /* end of frame */

        /* Fill input buffer */
        {   size_t const toDecode = MIN(readSizeHint, ress->srcBufferSize);  /* support large skippable frames */
            if (ress->srcBufferLoaded < toDecode) {
                size_t const toRead = toDecode - ress->srcBufferLoaded;   /* > 0 */
                void* const startPosition = (char*)ress->srcBuffer + ress->srcBufferLoaded;
                size_t const readSize = fread(startPosition, 1, toRead, finput);
                if (readSize==0) {
                    DISPLAYLEVEL(1, "%s : Read error (39) : premature end \n",
                                    srcFileName);
                    return FIO_ERROR_FRAME_DECODING;
                }
                ress->srcBufferLoaded += readSize;
    }   }   }

    FIO_fwriteSparseEnd(prefs, ress->dstFile, storedSkips);

    return frameSize;
}


#ifdef ZSTD_GZDECOMPRESS
static unsigned long long
FIO_decompressGzFrame(dRess_t* ress, FILE* srcFile,
                      const FIO_prefs_t* const prefs,
                      const char* srcFileName)
{
    unsigned long long outFileSize = 0;
    z_stream strm;
    int flush = Z_NO_FLUSH;
    int decodingError = 0;
    unsigned storedSkips = 0;

    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;
    strm.next_in = 0;
    strm.avail_in = 0;
    /* see http://www.zlib.net/manual.html */
    if (inflateInit2(&strm, 15 /* maxWindowLogSize */ + 16 /* gzip only */) != Z_OK)
        return FIO_ERROR_FRAME_DECODING;

    strm.next_out = (Bytef*)ress->dstBuffer;
    strm.avail_out = (uInt)ress->dstBufferSize;
    strm.avail_in = (uInt)ress->srcBufferLoaded;
    strm.next_in = (z_const unsigned char*)ress->srcBuffer;

    for ( ; ; ) {
        int ret;
        if (strm.avail_in == 0) {
            ress->srcBufferLoaded = fread(ress->srcBuffer, 1, ress->srcBufferSize, srcFile);
            if (ress->srcBufferLoaded == 0) flush = Z_FINISH;
            strm.next_in = (z_const unsigned char*)ress->srcBuffer;
            strm.avail_in = (uInt)ress->srcBufferLoaded;
        }
        ret = inflate(&strm, flush);
        if (ret == Z_BUF_ERROR) {
            DISPLAYLEVEL(1, "zstd: %s: premature gz end \n", srcFileName);
            decodingError = 1; break;
        }
        if (ret != Z_OK && ret != Z_STREAM_END) {
            DISPLAYLEVEL(1, "zstd: %s: inflate error %d \n", srcFileName, ret);
            decodingError = 1; break;
        }
        {   size_t const decompBytes = ress->dstBufferSize - strm.avail_out;
            if (decompBytes) {
                storedSkips = FIO_fwriteSparse(ress->dstFile, ress->dstBuffer, decompBytes, prefs, storedSkips);
                outFileSize += decompBytes;
                strm.next_out = (Bytef*)ress->dstBuffer;
                strm.avail_out = (uInt)ress->dstBufferSize;
            }
        }
        if (ret == Z_STREAM_END) break;
    }

    if (strm.avail_in > 0)
        memmove(ress->srcBuffer, strm.next_in, strm.avail_in);
    ress->srcBufferLoaded = strm.avail_in;
    if ( (inflateEnd(&strm) != Z_OK)  /* release resources ; error detected */
      && (decodingError==0) ) {
        DISPLAYLEVEL(1, "zstd: %s: inflateEnd error \n", srcFileName);
        decodingError = 1;
    }
    FIO_fwriteSparseEnd(prefs, ress->dstFile, storedSkips);
    return decodingError ? FIO_ERROR_FRAME_DECODING : outFileSize;
}
#endif


#ifdef ZSTD_LZMADECOMPRESS
static unsigned long long
FIO_decompressLzmaFrame(dRess_t* ress, FILE* srcFile,
                        const FIO_prefs_t* const prefs,
                        const char* srcFileName, int plain_lzma)
{
    unsigned long long outFileSize = 0;
    lzma_stream strm = LZMA_STREAM_INIT;
    lzma_action action = LZMA_RUN;
    lzma_ret initRet;
    int decodingError = 0;
    unsigned storedSkips = 0;

    strm.next_in = 0;
    strm.avail_in = 0;
    if (plain_lzma) {
        initRet = lzma_alone_decoder(&strm, UINT64_MAX); /* LZMA */
    } else {
        initRet = lzma_stream_decoder(&strm, UINT64_MAX, 0); /* XZ */
    }

    if (initRet != LZMA_OK) {
        DISPLAYLEVEL(1, "zstd: %s: %s error %d \n",
                        plain_lzma ? "lzma_alone_decoder" : "lzma_stream_decoder",
                        srcFileName, initRet);
        return FIO_ERROR_FRAME_DECODING;
    }

    strm.next_out = (BYTE*)ress->dstBuffer;
    strm.avail_out = ress->dstBufferSize;
    strm.next_in = (BYTE const*)ress->srcBuffer;
    strm.avail_in = ress->srcBufferLoaded;

    for ( ; ; ) {
        lzma_ret ret;
        if (strm.avail_in == 0) {
            ress->srcBufferLoaded = fread(ress->srcBuffer, 1, ress->srcBufferSize, srcFile);
            if (ress->srcBufferLoaded == 0) action = LZMA_FINISH;
            strm.next_in = (BYTE const*)ress->srcBuffer;
            strm.avail_in = ress->srcBufferLoaded;
        }
        ret = lzma_code(&strm, action);

        if (ret == LZMA_BUF_ERROR) {
            DISPLAYLEVEL(1, "zstd: %s: premature lzma end \n", srcFileName);
            decodingError = 1; break;
        }
        if (ret != LZMA_OK && ret != LZMA_STREAM_END) {
            DISPLAYLEVEL(1, "zstd: %s: lzma_code decoding error %d \n",
                            srcFileName, ret);
            decodingError = 1; break;
        }
        {   size_t const decompBytes = ress->dstBufferSize - strm.avail_out;
            if (decompBytes) {
                storedSkips = FIO_fwriteSparse(ress->dstFile, ress->dstBuffer, decompBytes, prefs, storedSkips);
                outFileSize += decompBytes;
                strm.next_out = (BYTE*)ress->dstBuffer;
                strm.avail_out = ress->dstBufferSize;
        }   }
        if (ret == LZMA_STREAM_END) break;
    }

    if (strm.avail_in > 0)
        memmove(ress->srcBuffer, strm.next_in, strm.avail_in);
    ress->srcBufferLoaded = strm.avail_in;
    lzma_end(&strm);
    FIO_fwriteSparseEnd(prefs, ress->dstFile, storedSkips);
    return decodingError ? FIO_ERROR_FRAME_DECODING : outFileSize;
}
#endif

#ifdef ZSTD_LZ4DECOMPRESS
static unsigned long long
FIO_decompressLz4Frame(dRess_t* ress, FILE* srcFile,
                       const FIO_prefs_t* const prefs,
                       const char* srcFileName)
{
    unsigned long long filesize = 0;
    LZ4F_errorCode_t nextToLoad;
    LZ4F_decompressionContext_t dCtx;
    LZ4F_errorCode_t const errorCode = LZ4F_createDecompressionContext(&dCtx, LZ4F_VERSION);
    int decodingError = 0;
    unsigned storedSkips = 0;

    if (LZ4F_isError(errorCode)) {
        DISPLAYLEVEL(1, "zstd: failed to create lz4 decompression context \n");
        return FIO_ERROR_FRAME_DECODING;
    }

    /* Init feed with magic number (already consumed from FILE* sFile) */
    {   size_t inSize = 4;
        size_t outSize= 0;
        MEM_writeLE32(ress->srcBuffer, LZ4_MAGICNUMBER);
        nextToLoad = LZ4F_decompress(dCtx, ress->dstBuffer, &outSize, ress->srcBuffer, &inSize, NULL);
        if (LZ4F_isError(nextToLoad)) {
            DISPLAYLEVEL(1, "zstd: %s: lz4 header error : %s \n",
                            srcFileName, LZ4F_getErrorName(nextToLoad));
            LZ4F_freeDecompressionContext(dCtx);
            return FIO_ERROR_FRAME_DECODING;
    }   }

    /* Main Loop */
    for (;nextToLoad;) {
        size_t readSize;
        size_t pos = 0;
        size_t decodedBytes = ress->dstBufferSize;

        /* Read input */
        if (nextToLoad > ress->srcBufferSize) nextToLoad = ress->srcBufferSize;
        readSize = fread(ress->srcBuffer, 1, nextToLoad, srcFile);
        if (!readSize) break;   /* reached end of file or stream */

        while ((pos < readSize) || (decodedBytes == ress->dstBufferSize)) {  /* still to read, or still to flush */
            /* Decode Input (at least partially) */
            size_t remaining = readSize - pos;
            decodedBytes = ress->dstBufferSize;
            nextToLoad = LZ4F_decompress(dCtx, ress->dstBuffer, &decodedBytes, (char*)(ress->srcBuffer)+pos, &remaining, NULL);
            if (LZ4F_isError(nextToLoad)) {
                DISPLAYLEVEL(1, "zstd: %s: lz4 decompression error : %s \n",
                                srcFileName, LZ4F_getErrorName(nextToLoad));
                decodingError = 1; nextToLoad = 0; break;
            }
            pos += remaining;

            /* Write Block */
            if (decodedBytes) {
                UTIL_HumanReadableSize_t hrs;
                storedSkips = FIO_fwriteSparse(ress->dstFile, ress->dstBuffer, decodedBytes, prefs, storedSkips);
                filesize += decodedBytes;
                hrs = UTIL_makeHumanReadableSize(filesize);
                DISPLAYUPDATE(2, "\rDecompressed : %.*f%s  ", hrs.precision, hrs.value, hrs.suffix);
            }

            if (!nextToLoad) break;
        }
    }
    /* can be out because readSize == 0, which could be an fread() error */
    if (ferror(srcFile)) {
        DISPLAYLEVEL(1, "zstd: %s: read error \n", srcFileName);
        decodingError=1;
    }

    if (nextToLoad!=0) {
        DISPLAYLEVEL(1, "zstd: %s: unfinished lz4 stream \n", srcFileName);
        decodingError=1;
    }

    LZ4F_freeDecompressionContext(dCtx);
    ress->srcBufferLoaded = 0; /* LZ4F will reach exact frame boundary */
    FIO_fwriteSparseEnd(prefs, ress->dstFile, storedSkips);

    return decodingError ? FIO_ERROR_FRAME_DECODING : filesize;
}
#endif



/** FIO_decompressFrames() :
 *  Find and decode frames inside srcFile
 *  srcFile presumed opened and valid
 * @return : 0 : OK
 *           1 : error
 */
static int FIO_decompressFrames(FIO_ctx_t* const fCtx,
                          dRess_t ress, FILE* srcFile,
                          const FIO_prefs_t* const prefs,
                          const char* dstFileName, const char* srcFileName)
{
    unsigned readSomething = 0;
    unsigned long long filesize = 0;
    assert(srcFile != NULL);

    /* for each frame */
    for ( ; ; ) {
        /* check magic number -> version */
        size_t const toRead = 4;
        const BYTE* const buf = (const BYTE*)ress.srcBuffer;
        if (ress.srcBufferLoaded < toRead)  /* load up to 4 bytes for header */
            ress.srcBufferLoaded += fread((char*)ress.srcBuffer + ress.srcBufferLoaded,
                                          (size_t)1, toRead - ress.srcBufferLoaded, srcFile);
        if (ress.srcBufferLoaded==0) {
            if (readSomething==0) {  /* srcFile is empty (which is invalid) */
                DISPLAYLEVEL(1, "zstd: %s: unexpected end of file \n", srcFileName);
                return 1;
            }  /* else, just reached frame boundary */
            break;   /* no more input */
        }
        readSomething = 1;   /* there is at least 1 byte in srcFile */
        if (ress.srcBufferLoaded < toRead) {
            DISPLAYLEVEL(1, "zstd: %s: unknown header \n", srcFileName);
            return 1;
        }
        if (ZSTD_isFrame(buf, ress.srcBufferLoaded)) {
            unsigned long long const frameSize = FIO_decompressZstdFrame(fCtx, &ress, srcFile, prefs, srcFileName, filesize);
            if (frameSize == FIO_ERROR_FRAME_DECODING) return 1;
            filesize += frameSize;
        } else if (buf[0] == 31 && buf[1] == 139) { /* gz magic number */
#ifdef ZSTD_GZDECOMPRESS
            unsigned long long const frameSize = FIO_decompressGzFrame(&ress, srcFile, prefs, srcFileName);
            if (frameSize == FIO_ERROR_FRAME_DECODING) return 1;
            filesize += frameSize;
#else
            DISPLAYLEVEL(1, "zstd: %s: gzip file cannot be uncompressed (zstd compiled without HAVE_ZLIB) -- ignored \n", srcFileName);
            return 1;
#endif
        } else if ((buf[0] == 0xFD && buf[1] == 0x37)  /* xz magic number */
                || (buf[0] == 0x5D && buf[1] == 0x00)) { /* lzma header (no magic number) */
#ifdef ZSTD_LZMADECOMPRESS
            unsigned long long const frameSize = FIO_decompressLzmaFrame(&ress, srcFile, prefs, srcFileName, buf[0] != 0xFD);
            if (frameSize == FIO_ERROR_FRAME_DECODING) return 1;
            filesize += frameSize;
#else
            DISPLAYLEVEL(1, "zstd: %s: xz/lzma file cannot be uncompressed (zstd compiled without HAVE_LZMA) -- ignored \n", srcFileName);
            return 1;
#endif
        } else if (MEM_readLE32(buf) == LZ4_MAGICNUMBER) {
#ifdef ZSTD_LZ4DECOMPRESS
            unsigned long long const frameSize = FIO_decompressLz4Frame(&ress, srcFile, prefs, srcFileName);
            if (frameSize == FIO_ERROR_FRAME_DECODING) return 1;
            filesize += frameSize;
#else
            DISPLAYLEVEL(1, "zstd: %s: lz4 file cannot be uncompressed (zstd compiled without HAVE_LZ4) -- ignored \n", srcFileName);
            return 1;
#endif
        } else if ((prefs->overwrite) && !strcmp (dstFileName, stdoutmark)) {  /* pass-through mode */
            return FIO_passThrough(prefs,
                                   ress.dstFile, srcFile,
                                   ress.srcBuffer, ress.srcBufferSize,
                                   ress.srcBufferLoaded);
        } else {
            DISPLAYLEVEL(1, "zstd: %s: unsupported format \n", srcFileName);
            return 1;
    }   }  /* for each frame */

    /* Final Status */
    fCtx->totalBytesOutput += (size_t)filesize;
    DISPLAYLEVEL(2, "\r%79s\r", "");
    /* No status message in pipe mode (stdin - stdout) or multi-files mode */
    if ((g_display_prefs.displayLevel >= 2 && fCtx->nbFilesTotal <= 1) ||
        g_display_prefs.displayLevel >= 3 ||
        g_display_prefs.progressSetting == FIO_ps_always) {
        DISPLAYLEVEL(1, "\r%-20s: %llu bytes \n", srcFileName, filesize);
    }

    return 0;
}

/** FIO_decompressDstFile() :
    open `dstFileName`,
    or path-through if ress.dstFile is already != 0,
    then start decompression process (FIO_decompressFrames()).
    @return : 0 : OK
              1 : operation aborted
*/
static int FIO_decompressDstFile(FIO_ctx_t* const fCtx,
                                 FIO_prefs_t* const prefs,
                                 dRess_t ress, FILE* srcFile,
                                 const char* dstFileName, const char* srcFileName)
{
    int result;
    stat_t statbuf;
    int releaseDstFile = 0;
    int transferMTime = 0;

    if ((ress.dstFile == NULL) && (prefs->testMode==0)) {
        int dstFilePermissions = DEFAULT_FILE_PERMISSIONS;
        if ( strcmp(srcFileName, stdinmark)   /* special case : don't transfer permissions from stdin */
          && UTIL_stat(srcFileName, &statbuf)
          && UTIL_isRegularFileStat(&statbuf) ) {
            dstFilePermissions = statbuf.st_mode;
            transferMTime = 1;
        }

        releaseDstFile = 1;

        ress.dstFile = FIO_openDstFile(fCtx, prefs, srcFileName, dstFileName, dstFilePermissions);
        if (ress.dstFile==NULL) return 1;

        /* Must only be added after FIO_openDstFile() succeeds.
         * Otherwise we may delete the destination file if it already exists,
         * and the user presses Ctrl-C when asked if they wish to overwrite.
         */
        addHandler(dstFileName);
    }

    result = FIO_decompressFrames(fCtx, ress, srcFile, prefs, dstFileName, srcFileName);

    if (releaseDstFile) {
        FILE* const dstFile = ress.dstFile;
        clearHandler();
        ress.dstFile = NULL;
        if (fclose(dstFile)) {
            DISPLAYLEVEL(1, "zstd: %s: %s \n", dstFileName, strerror(errno));
            result = 1;
        }

        if (transferMTime) {
            UTIL_utime(dstFileName, &statbuf);
        }

        if ( (result != 0)  /* operation failure */
          && strcmp(dstFileName, stdoutmark)  /* special case : don't remove() stdout */
          ) {
            FIO_removeFile(dstFileName);  /* remove decompression artefact; note: don't do anything special if remove() fails */
        }
    }

    return result;
}


/** FIO_decompressSrcFile() :
    Open `srcFileName`, transfer control to decompressDstFile()
    @return : 0 : OK
              1 : error
*/
static int FIO_decompressSrcFile(FIO_ctx_t* const fCtx, FIO_prefs_t* const prefs, dRess_t ress, const char* dstFileName, const char* srcFileName)
{
    FILE* srcFile;
    int result;

    if (UTIL_isDirectory(srcFileName)) {
        DISPLAYLEVEL(1, "zstd: %s is a directory -- ignored \n", srcFileName);
        return 1;
    }

    srcFile = FIO_openSrcFile(prefs, srcFileName);
    if (srcFile==NULL) return 1;
    ress.srcBufferLoaded = 0;

    result = FIO_decompressDstFile(fCtx, prefs, ress, srcFile, dstFileName, srcFileName);

    /* Close file */
    if (fclose(srcFile)) {
        DISPLAYLEVEL(1, "zstd: %s: %s \n", srcFileName, strerror(errno));  /* error should not happen */
        return 1;
    }
    if ( prefs->removeSrcFile  /* --rm */
      && (result==0)      /* decompression successful */
      && strcmp(srcFileName, stdinmark) ) /* not stdin */ {
        /* We must clear the handler, since after this point calling it would
         * delete both the source and destination files.
         */
        clearHandler();
        if (FIO_removeFile(srcFileName)) {
            /* failed to remove src file */
            DISPLAYLEVEL(1, "zstd: %s: %s \n", srcFileName, strerror(errno));
            return 1;
    }   }
    return result;
}



int FIO_decompressFilename(FIO_ctx_t* const fCtx, FIO_prefs_t* const prefs,
                           const char* dstFileName, const char* srcFileName,
                           const char* dictFileName)
{
    dRess_t const ress = FIO_createDResources(prefs, dictFileName);

    int const decodingError = FIO_decompressSrcFile(fCtx, prefs, ress, dstFileName, srcFileName);

    FIO_freeDResources(ress);
    return decodingError;
}

static const char *suffixList[] = {
    ZSTD_EXTENSION,
    TZSTD_EXTENSION,
#ifndef ZSTD_NODECOMPRESS
    ZSTD_ALT_EXTENSION,
#endif
#ifdef ZSTD_GZDECOMPRESS
    GZ_EXTENSION,
    TGZ_EXTENSION,
#endif
#ifdef ZSTD_LZMADECOMPRESS
    LZMA_EXTENSION,
    XZ_EXTENSION,
    TXZ_EXTENSION,
#endif
#ifdef ZSTD_LZ4DECOMPRESS
    LZ4_EXTENSION,
    TLZ4_EXTENSION,
#endif
    NULL
};

static const char *suffixListStr =
    ZSTD_EXTENSION "/" TZSTD_EXTENSION
#ifdef ZSTD_GZDECOMPRESS
    "/" GZ_EXTENSION "/" TGZ_EXTENSION
#endif
#ifdef ZSTD_LZMADECOMPRESS
    "/" LZMA_EXTENSION "/" XZ_EXTENSION "/" TXZ_EXTENSION
#endif
#ifdef ZSTD_LZ4DECOMPRESS
    "/" LZ4_EXTENSION "/" TLZ4_EXTENSION
#endif
;

/* FIO_determineDstName() :
 * create a destination filename from a srcFileName.
 * @return a pointer to it.
 * @return == NULL if there is an error */
static const char*
FIO_determineDstName(const char* srcFileName, const char* outDirName)
{
    static size_t dfnbCapacity = 0;
    static char* dstFileNameBuffer = NULL;   /* using static allocation : this function cannot be multi-threaded */
    size_t dstFileNameEndPos;
    char* outDirFilename = NULL;
    const char* dstSuffix = "";
    size_t dstSuffixLen = 0;

    size_t sfnSize = strlen(srcFileName);

    size_t srcSuffixLen;
    const char* const srcSuffix = strrchr(srcFileName, '.');
    if (srcSuffix == NULL) {
        DISPLAYLEVEL(1,
            "zstd: %s: unknown suffix (%s expected). "
            "Can't derive the output file name. "
            "Specify it with -o dstFileName. Ignoring.\n",
            srcFileName, suffixListStr);
        return NULL;
    }
    srcSuffixLen = strlen(srcSuffix);

    {
        const char** matchedSuffixPtr;
        for (matchedSuffixPtr = suffixList; *matchedSuffixPtr != NULL; matchedSuffixPtr++) {
            if (!strcmp(*matchedSuffixPtr, srcSuffix)) {
                break;
            }
        }

        /* check suffix is authorized */
        if (sfnSize <= srcSuffixLen || *matchedSuffixPtr == NULL) {
            DISPLAYLEVEL(1,
                "zstd: %s: unknown suffix (%s expected). "
                "Can't derive the output file name. "
                "Specify it with -o dstFileName. Ignoring.\n",
                srcFileName, suffixListStr);
            return NULL;
        }

        if ((*matchedSuffixPtr)[1] == 't') {
            dstSuffix = ".tar";
            dstSuffixLen = strlen(dstSuffix);
        }
    }

    if (outDirName) {
        outDirFilename = FIO_createFilename_fromOutDir(srcFileName, outDirName, 0);
        sfnSize = strlen(outDirFilename);
        assert(outDirFilename != NULL);
    }

    if (dfnbCapacity+srcSuffixLen <= sfnSize+1+dstSuffixLen) {
        /* allocate enough space to write dstFilename into it */
        free(dstFileNameBuffer);
        dfnbCapacity = sfnSize + 20;
        dstFileNameBuffer = (char*)malloc(dfnbCapacity);
        if (dstFileNameBuffer==NULL)
            EXM_THROW(74, "%s : not enough memory for dstFileName",
                      strerror(errno));
    }

    /* return dst name == src name truncated from suffix */
    assert(dstFileNameBuffer != NULL);
    dstFileNameEndPos = sfnSize - srcSuffixLen;
    if (outDirFilename) {
        memcpy(dstFileNameBuffer, outDirFilename, dstFileNameEndPos);
        free(outDirFilename);
    } else {
        memcpy(dstFileNameBuffer, srcFileName, dstFileNameEndPos);
    }

    /* The short tar extensions tzst, tgz, txz and tlz4 files should have "tar"
     * extension on decompression. Also writes terminating null. */
    strcpy(dstFileNameBuffer + dstFileNameEndPos, dstSuffix);
    return dstFileNameBuffer;

    /* note : dstFileNameBuffer memory is not going to be free */
}

int
FIO_decompressMultipleFilenames(FIO_ctx_t* const fCtx,
                                FIO_prefs_t* const prefs,
                                const char** srcNamesTable,
                                const char* outMirroredRootDirName,
                                const char* outDirName, const char* outFileName,
                                const char* dictFileName)
{
    int status;
    int error = 0;
    dRess_t ress = FIO_createDResources(prefs, dictFileName);

    if (outFileName) {
        if (FIO_removeMultiFilesWarning(fCtx, prefs, outFileName, 1 /* displayLevelCutoff */)) {
            FIO_freeDResources(ress);
            return 1;
        }
        if (!prefs->testMode) {
            ress.dstFile = FIO_openDstFile(fCtx, prefs, NULL, outFileName, DEFAULT_FILE_PERMISSIONS);
            if (ress.dstFile == 0) EXM_THROW(19, "cannot open %s", outFileName);
        }
        for (; fCtx->currFileIdx < fCtx->nbFilesTotal; fCtx->currFileIdx++) {
            status = FIO_decompressSrcFile(fCtx, prefs, ress, outFileName, srcNamesTable[fCtx->currFileIdx]);
            if (!status) fCtx->nbFilesProcessed++;
            error |= status;
        }
        if ((!prefs->testMode) && (fclose(ress.dstFile)))
            EXM_THROW(72, "Write error : %s : cannot properly close output file",
                        strerror(errno));
    } else {
        if (outMirroredRootDirName)
            UTIL_mirrorSourceFilesDirectories(srcNamesTable, (unsigned)fCtx->nbFilesTotal, outMirroredRootDirName);

        for (; fCtx->currFileIdx < fCtx->nbFilesTotal; fCtx->currFileIdx++) {   /* create dstFileName */
            const char* const srcFileName = srcNamesTable[fCtx->currFileIdx];
            const char* dstFileName = NULL;
            if (outMirroredRootDirName) {
                char* validMirroredDirName = UTIL_createMirroredDestDirName(srcFileName, outMirroredRootDirName);
                if (validMirroredDirName) {
                    dstFileName = FIO_determineDstName(srcFileName, validMirroredDirName);
                    free(validMirroredDirName);
                } else {
                    DISPLAYLEVEL(2, "zstd: --output-dir-mirror cannot decompress '%s' into '%s'\n", srcFileName, outMirroredRootDirName);
                }
            } else {
                dstFileName = FIO_determineDstName(srcFileName, outDirName);
            }
            if (dstFileName == NULL) { error=1; continue; }
            status = FIO_decompressSrcFile(fCtx, prefs, ress, dstFileName, srcFileName);
            if (!status) fCtx->nbFilesProcessed++;
            error |= status;
        }
        if (outDirName)
            FIO_checkFilenameCollisions(srcNamesTable , (unsigned)fCtx->nbFilesTotal);
    }

    if (fCtx->nbFilesProcessed >= 1  && fCtx->nbFilesTotal > 1 && fCtx->totalBytesOutput != 0)
        DISPLAYLEVEL(2, "%d files decompressed : %6zu bytes total \n", fCtx->nbFilesProcessed, fCtx->totalBytesOutput);

    FIO_freeDResources(ress);
    return error;
}

/* **************************************************************************
 *  .zst file info (--list command)
 ***************************************************************************/

typedef struct {
    U64 decompressedSize;
    U64 compressedSize;
    U64 windowSize;
    int numActualFrames;
    int numSkippableFrames;
    int decompUnavailable;
    int usesCheck;
    U32 nbFiles;
} fileInfo_t;

typedef enum {
  info_success=0,
  info_frame_error=1,
  info_not_zstd=2,
  info_file_error=3,
  info_truncated_input=4,
} InfoError;

#define ERROR_IF(c,n,...) {             \
    if (c) {                           \
        DISPLAYLEVEL(1, __VA_ARGS__);  \
        DISPLAYLEVEL(1, " \n");        \
        return n;                      \
    }                                  \
}

static InfoError
FIO_analyzeFrames(fileInfo_t* info, FILE* const srcFile)
{
    /* begin analyzing frame */
    for ( ; ; ) {
        BYTE headerBuffer[ZSTD_FRAMEHEADERSIZE_MAX];
        size_t const numBytesRead = fread(headerBuffer, 1, sizeof(headerBuffer), srcFile);
        if (numBytesRead < ZSTD_FRAMEHEADERSIZE_MIN(ZSTD_f_zstd1)) {
            if ( feof(srcFile)
              && (numBytesRead == 0)
              && (info->compressedSize > 0)
              && (info->compressedSize != UTIL_FILESIZE_UNKNOWN) ) {
                unsigned long long file_position = (unsigned long long) LONG_TELL(srcFile);
                unsigned long long file_size = (unsigned long long) info->compressedSize;
                ERROR_IF(file_position != file_size, info_truncated_input,
                  "Error: seeked to position %llu, which is beyond file size of %llu\n",
                  file_position,
                  file_size);
                break;  /* correct end of file => success */
            }
            ERROR_IF(feof(srcFile), info_not_zstd, "Error: reached end of file with incomplete frame");
            ERROR_IF(1, info_frame_error, "Error: did not reach end of file but ran out of frames");
        }
        {   U32 const magicNumber = MEM_readLE32(headerBuffer);
            /* Zstandard frame */
            if (magicNumber == ZSTD_MAGICNUMBER) {
                ZSTD_frameHeader header;
                U64 const frameContentSize = ZSTD_getFrameContentSize(headerBuffer, numBytesRead);
                if ( frameContentSize == ZSTD_CONTENTSIZE_ERROR
                  || frameContentSize == ZSTD_CONTENTSIZE_UNKNOWN ) {
                    info->decompUnavailable = 1;
                } else {
                    info->decompressedSize += frameContentSize;
                }
                ERROR_IF(ZSTD_getFrameHeader(&header, headerBuffer, numBytesRead) != 0,
                        info_frame_error, "Error: could not decode frame header");
                info->windowSize = header.windowSize;
                /* move to the end of the frame header */
                {   size_t const headerSize = ZSTD_frameHeaderSize(headerBuffer, numBytesRead);
                    ERROR_IF(ZSTD_isError(headerSize), info_frame_error, "Error: could not determine frame header size");
                    ERROR_IF(fseek(srcFile, ((long)headerSize)-((long)numBytesRead), SEEK_CUR) != 0,
                            info_frame_error, "Error: could not move to end of frame header");
                }

                /* skip all blocks in the frame */
                {   int lastBlock = 0;
                    do {
                        BYTE blockHeaderBuffer[3];
                        ERROR_IF(fread(blockHeaderBuffer, 1, 3, srcFile) != 3,
                                info_frame_error, "Error while reading block header");
                        {   U32 const blockHeader = MEM_readLE24(blockHeaderBuffer);
                            U32 const blockTypeID = (blockHeader >> 1) & 3;
                            U32 const isRLE = (blockTypeID == 1);
                            U32 const isWrongBlock = (blockTypeID == 3);
                            long const blockSize = isRLE ? 1 : (long)(blockHeader >> 3);
                            ERROR_IF(isWrongBlock, info_frame_error, "Error: unsupported block type");
                            lastBlock = blockHeader & 1;
                            ERROR_IF(fseek(srcFile, blockSize, SEEK_CUR) != 0,
                                    info_frame_error, "Error: could not skip to end of block");
                        }
                    } while (lastBlock != 1);
                }

                /* check if checksum is used */
                {   BYTE const frameHeaderDescriptor = headerBuffer[4];
                    int const contentChecksumFlag = (frameHeaderDescriptor & (1 << 2)) >> 2;
                    if (contentChecksumFlag) {
                        info->usesCheck = 1;
                        ERROR_IF(fseek(srcFile, 4, SEEK_CUR) != 0,
                                info_frame_error, "Error: could not skip past checksum");
                }   }
                info->numActualFrames++;
            }
            /* Skippable frame */
            else if ((magicNumber & ZSTD_MAGIC_SKIPPABLE_MASK) == ZSTD_MAGIC_SKIPPABLE_START) {
                U32 const frameSize = MEM_readLE32(headerBuffer + 4);
                long const seek = (long)(8 + frameSize - numBytesRead);
                ERROR_IF(LONG_SEEK(srcFile, seek, SEEK_CUR) != 0,
                        info_frame_error, "Error: could not find end of skippable frame");
                info->numSkippableFrames++;
            }
            /* unknown content */
            else {
                return info_not_zstd;
            }
        }  /* magic number analysis */
    }  /* end analyzing frames */
    return info_success;
}


static InfoError
getFileInfo_fileConfirmed(fileInfo_t* info, const char* inFileName)
{
    InfoError status;
    FILE* const srcFile = FIO_openSrcFile(NULL, inFileName);
    ERROR_IF(srcFile == NULL, info_file_error, "Error: could not open source file %s", inFileName);

    info->compressedSize = UTIL_getFileSize(inFileName);
    status = FIO_analyzeFrames(info, srcFile);

    fclose(srcFile);
    info->nbFiles = 1;
    return status;
}


/** getFileInfo() :
 *  Reads information from file, stores in *info
 * @return : InfoError status
 */
static InfoError
getFileInfo(fileInfo_t* info, const char* srcFileName)
{
    ERROR_IF(!UTIL_isRegularFile(srcFileName),
            info_file_error, "Error : %s is not a file", srcFileName);
    return getFileInfo_fileConfirmed(info, srcFileName);
}


static void
displayInfo(const char* inFileName, const fileInfo_t* info, int displayLevel)
{
    UTIL_HumanReadableSize_t const window_hrs = UTIL_makeHumanReadableSize(info->windowSize);
    UTIL_HumanReadableSize_t const compressed_hrs = UTIL_makeHumanReadableSize(info->compressedSize);
    UTIL_HumanReadableSize_t const decompressed_hrs = UTIL_makeHumanReadableSize(info->decompressedSize);
    double const ratio = (info->compressedSize == 0) ? 0 : ((double)info->decompressedSize)/(double)info->compressedSize;
    const char* const checkString = (info->usesCheck ? "XXH64" : "None");
    if (displayLevel <= 2) {
        if (!info->decompUnavailable) {
            DISPLAYOUT("%6d  %5d  %6.*f%4s  %8.*f%4s  %5.3f  %5s  %s\n",
                    info->numSkippableFrames + info->numActualFrames,
                    info->numSkippableFrames,
                    compressed_hrs.precision, compressed_hrs.value, compressed_hrs.suffix,
                    decompressed_hrs.precision, decompressed_hrs.value, decompressed_hrs.suffix,
                    ratio, checkString, inFileName);
        } else {
            DISPLAYOUT("%6d  %5d  %6.*f%4s                       %5s  %s\n",
                    info->numSkippableFrames + info->numActualFrames,
                    info->numSkippableFrames,
                    compressed_hrs.precision, compressed_hrs.value, compressed_hrs.suffix,
                    checkString, inFileName);
        }
    } else {
        DISPLAYOUT("%s \n", inFileName);
        DISPLAYOUT("# Zstandard Frames: %d\n", info->numActualFrames);
        if (info->numSkippableFrames)
            DISPLAYOUT("# Skippable Frames: %d\n", info->numSkippableFrames);
        DISPLAYOUT("Window Size: %.*f%s (%llu B)\n",
                   window_hrs.precision, window_hrs.value, window_hrs.suffix,
                   (unsigned long long)info->windowSize);
        DISPLAYOUT("Compressed Size: %.*f%s (%llu B)\n",
                    compressed_hrs.precision, compressed_hrs.value, compressed_hrs.suffix,
                    (unsigned long long)info->compressedSize);
        if (!info->decompUnavailable) {
            DISPLAYOUT("Decompressed Size: %.*f%s (%llu B)\n",
                    decompressed_hrs.precision, decompressed_hrs.value, decompressed_hrs.suffix,
                    (unsigned long long)info->decompressedSize);
            DISPLAYOUT("Ratio: %.4f\n", ratio);
        }
        DISPLAYOUT("Check: %s\n", checkString);
        DISPLAYOUT("\n");
    }
}

static fileInfo_t FIO_addFInfo(fileInfo_t fi1, fileInfo_t fi2)
{
    fileInfo_t total;
    memset(&total, 0, sizeof(total));
    total.numActualFrames = fi1.numActualFrames + fi2.numActualFrames;
    total.numSkippableFrames = fi1.numSkippableFrames + fi2.numSkippableFrames;
    total.compressedSize = fi1.compressedSize + fi2.compressedSize;
    total.decompressedSize = fi1.decompressedSize + fi2.decompressedSize;
    total.decompUnavailable = fi1.decompUnavailable | fi2.decompUnavailable;
    total.usesCheck = fi1.usesCheck & fi2.usesCheck;
    total.nbFiles = fi1.nbFiles + fi2.nbFiles;
    return total;
}

static int
FIO_listFile(fileInfo_t* total, const char* inFileName, int displayLevel)
{
    fileInfo_t info;
    memset(&info, 0, sizeof(info));
    {   InfoError const error = getFileInfo(&info, inFileName);
        switch (error) {
            case info_frame_error:
                /* display error, but provide output */
                DISPLAYLEVEL(1, "Error while parsing \"%s\" \n", inFileName);
                break;
            case info_not_zstd:
                DISPLAYOUT("File \"%s\" not compressed by zstd \n", inFileName);
                if (displayLevel > 2) DISPLAYOUT("\n");
                return 1;
            case info_file_error:
                /* error occurred while opening the file */
                if (displayLevel > 2) DISPLAYOUT("\n");
                return 1;
            case info_truncated_input:
                DISPLAYOUT("File \"%s\" is truncated \n", inFileName);
                if (displayLevel > 2) DISPLAYOUT("\n");
                return 1;
            case info_success:
            default:
                break;
        }

        displayInfo(inFileName, &info, displayLevel);
        *total = FIO_addFInfo(*total, info);
        assert(error == info_success || error == info_frame_error);
        return (int)error;
    }
}

int FIO_listMultipleFiles(unsigned numFiles, const char** filenameTable, int displayLevel)
{
    /* ensure no specified input is stdin (needs fseek() capability) */
    {   unsigned u;
        for (u=0; u<numFiles;u++) {
            ERROR_IF(!strcmp (filenameTable[u], stdinmark),
                    1, "zstd: --list does not support reading from standard input");
    }   }

    if (numFiles == 0) {
        if (!IS_CONSOLE(stdin)) {
            DISPLAYLEVEL(1, "zstd: --list does not support reading from standard input \n");
        }
        DISPLAYLEVEL(1, "No files given \n");
        return 1;
    }

    if (displayLevel <= 2) {
        DISPLAYOUT("Frames  Skips  Compressed  Uncompressed  Ratio  Check  Filename\n");
    }
    {   int error = 0;
        fileInfo_t total;
        memset(&total, 0, sizeof(total));
        total.usesCheck = 1;
        /* --list each file, and check for any error */
        {   unsigned u;
            for (u=0; u<numFiles;u++) {
                error |= FIO_listFile(&total, filenameTable[u], displayLevel);
        }   }
        if (numFiles > 1 && displayLevel <= 2) {   /* display total */
            UTIL_HumanReadableSize_t const compressed_hrs = UTIL_makeHumanReadableSize(total.compressedSize);
            UTIL_HumanReadableSize_t const decompressed_hrs = UTIL_makeHumanReadableSize(total.decompressedSize);
            double const ratio = (total.compressedSize == 0) ? 0 : ((double)total.decompressedSize)/(double)total.compressedSize;
            const char* const checkString = (total.usesCheck ? "XXH64" : "");
            DISPLAYOUT("----------------------------------------------------------------- \n");
            if (total.decompUnavailable) {
                DISPLAYOUT("%6d  %5d  %6.*f%4s                       %5s  %u files\n",
                        total.numSkippableFrames + total.numActualFrames,
                        total.numSkippableFrames,
                        compressed_hrs.precision, compressed_hrs.value, compressed_hrs.suffix,
                        checkString, (unsigned)total.nbFiles);
            } else {
                DISPLAYOUT("%6d  %5d  %6.*f%4s  %8.*f%4s  %5.3f  %5s  %u files\n",
                        total.numSkippableFrames + total.numActualFrames,
                        total.numSkippableFrames,
                        compressed_hrs.precision, compressed_hrs.value, compressed_hrs.suffix,
                        decompressed_hrs.precision, decompressed_hrs.value, decompressed_hrs.suffix,
                        ratio, checkString, (unsigned)total.nbFiles);
        }   }
        return error;
    }
}


#endif /* #ifndef ZSTD_NODECOMPRESS */
