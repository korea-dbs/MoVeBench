/*
** 2012 January 20
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
**
** An in-memory key/value storage subsystem that presents the interfadce
** defined by kv.h
*/
#include "sqliteInt.h"
#include "lsm.h"
#include <lz4.h>
#include <time.h>
#include <zlib.h>

/* Forward declarations of objects */
typedef struct KVLsm KVLsm;
typedef struct KVLsmCsr KVLsmCsr;

/*
** An instance of an open connection to an LSM store.  A subclass of KVStore.
*/
struct KVLsm {
  KVStore base;                   /* Base class, must be first */
  lsm_db *pDb;                    /* LSM database handle */
  lsm_cursor *pCsr;               /* LSM cursor holding read-trans open */
};

/*
** An instance of an open cursor pointing into an LSM store.  A subclass
** of KVCursor.
*/
struct KVLsmCsr {
  KVCursor base;                  /* Base class. Must be first */
  lsm_cursor *pCsr;               /* LSM cursor handle */
};

/*
** zlib-backed page compression for the LSM storage engine. The LSM layer
** persists iId in database checkpoints, so keep this value stable.
*/
#define KVLSM_COMPRESSION_ZLIB_ID 100
#define KVLSM_COMPRESSION_LZ4_ID 101

double g_lsmCompressMs = 0.0;
double g_lsmUncompressMs = 0.0;
int g_lsmCompressCalls = 0;
int g_lsmUncompressCalls = 0;
long long g_lsmCompressInBytes = 0;
long long g_lsmCompressOutBytes = 0;
long long g_lsmUncompressInBytes = 0;
long long g_lsmUncompressOutBytes = 0;

static double kvlsmMsBetween(struct timespec *p0, struct timespec *p1){
  return (p1->tv_sec - p0->tv_sec)*1000.0 + (p1->tv_nsec - p0->tv_nsec)/1e6;
}

static int kvlsmZlibBound(void *pCtx, int nSrc){
  UNUSED_PARAMETER(pCtx);
  return (int)compressBound((uLong)nSrc);
}

static int kvlsmZlibCompress(
  void *pCtx,
  char *aOut, int *pnOut,
  const char *aIn, int nIn
){
  uLongf nOut = (uLongf)*pnOut;
  int rc;
  struct timespec t0, t1;
  UNUSED_PARAMETER(pCtx);

  clock_gettime(CLOCK_MONOTONIC, &t0);
  rc = compress((Bytef*)aOut, &nOut, (const Bytef*)aIn, (uLong)nIn);
  clock_gettime(CLOCK_MONOTONIC, &t1);
  g_lsmCompressMs += kvlsmMsBetween(&t0, &t1);
  g_lsmCompressCalls++;
  g_lsmCompressInBytes += nIn;
  g_lsmCompressOutBytes += (rc==Z_OK) ? (long long)nOut : 0;
  if( rc!=Z_OK ){
    return LSM_ERROR;
  }

  *pnOut = (int)nOut;
  return LSM_OK;
}

static int kvlsmZlibUncompress(
  void *pCtx,
  char *aOut, int *pnOut,
  const char *aIn, int nIn
){
  uLongf nOut = (uLongf)*pnOut;
  int rc;
  struct timespec t0, t1;
  UNUSED_PARAMETER(pCtx);

  clock_gettime(CLOCK_MONOTONIC, &t0);
  rc = uncompress((Bytef*)aOut, &nOut, (const Bytef*)aIn, (uLong)nIn);
  clock_gettime(CLOCK_MONOTONIC, &t1);
  g_lsmUncompressMs += kvlsmMsBetween(&t0, &t1);
  g_lsmUncompressCalls++;
  g_lsmUncompressInBytes += nIn;
  g_lsmUncompressOutBytes += (rc==Z_OK) ? (long long)nOut : 0;
  if( rc!=Z_OK ){
    return LSM_ERROR;
  }

  *pnOut = (int)nOut;
  return LSM_OK;
}

static lsm_compress kvlsmZlibCompression = {
  0,                              /* pCtx */
  KVLSM_COMPRESSION_ZLIB_ID,      /* iId */
  kvlsmZlibBound,                 /* xBound */
  kvlsmZlibCompress,              /* xCompress */
  kvlsmZlibUncompress,            /* xUncompress */
  0                               /* xFree */
};

static int kvlsmLz4Bound(void *pCtx, int nSrc){
  UNUSED_PARAMETER(pCtx);
  return LZ4_compressBound(nSrc);
}

static int kvlsmLz4Compress(
  void *pCtx,
  char *aOut, int *pnOut,
  const char *aIn, int nIn
){
  int nOut;
  struct timespec t0, t1;
  UNUSED_PARAMETER(pCtx);

  clock_gettime(CLOCK_MONOTONIC, &t0);
  nOut = LZ4_compress_default(aIn, aOut, nIn, *pnOut);
  clock_gettime(CLOCK_MONOTONIC, &t1);
  g_lsmCompressMs += kvlsmMsBetween(&t0, &t1);
  g_lsmCompressCalls++;
  g_lsmCompressInBytes += nIn;
  g_lsmCompressOutBytes += nOut>0 ? nOut : 0;
  if( nOut<=0 ){
    return LSM_ERROR;
  }

  *pnOut = nOut;
  return LSM_OK;
}

static int kvlsmLz4Uncompress(
  void *pCtx,
  char *aOut, int *pnOut,
  const char *aIn, int nIn
){
  int nOut;
  struct timespec t0, t1;
  UNUSED_PARAMETER(pCtx);

  clock_gettime(CLOCK_MONOTONIC, &t0);
  nOut = LZ4_decompress_safe(aIn, aOut, nIn, *pnOut);
  clock_gettime(CLOCK_MONOTONIC, &t1);
  g_lsmUncompressMs += kvlsmMsBetween(&t0, &t1);
  g_lsmUncompressCalls++;
  g_lsmUncompressInBytes += nIn;
  g_lsmUncompressOutBytes += nOut>=0 ? nOut : 0;
  if( nOut<0 ){
    return LSM_ERROR;
  }

  *pnOut = nOut;
  return LSM_OK;
}

static lsm_compress kvlsmLz4Compression = {
  0,                              /* pCtx */
  KVLSM_COMPRESSION_LZ4_ID,       /* iId */
  kvlsmLz4Bound,                  /* xBound */
  kvlsmLz4Compress,               /* xCompress */
  kvlsmLz4Uncompress,             /* xUncompress */
  0                               /* xFree */
};

static int kvlsmConfigureCompression(lsm_db *pDb, const char *zName){
  const char *zVal = sqlite4_uri_parameter(zName, "lsm_compression");
  if( zVal==0 ){
    zVal = sqlite4_uri_parameter(zName, "compression");
  }
  if( zVal==0 || sqlite4_stricmp(zVal, "none")==0 || sqlite4_stricmp(zVal, "0")==0 ){
    return LSM_OK;
  }
  if( sqlite4_stricmp(zVal, "zlib")==0 || sqlite4_stricmp(zVal, "1")==0 ){
    return lsm_config(pDb, LSM_CONFIG_SET_COMPRESSION, &kvlsmZlibCompression);
  }
  if( sqlite4_stricmp(zVal, "lz4")==0 || sqlite4_stricmp(zVal, "2")==0 ){
    return lsm_config(pDb, LSM_CONFIG_SET_COMPRESSION, &kvlsmLz4Compression);
  }
  return LSM_ERROR;
}
  
/*
** Begin a transaction or subtransaction.
**
** If iLevel==1 then begin an outermost read transaction.
**
** If iLevel==2 then begin an outermost write transaction.
**
** If iLevel>2 then begin a nested write transaction.
**
** iLevel may not be less than 1.  After this routine returns successfully
** the transaction level will be equal to iLevel.  The transaction level
** must be at least 1 to read and at least 2 to write.
*/
static int kvlsmBegin(KVStore *pKVStore, int iLevel){
  int rc = SQLITE4_OK;
  KVLsm *p = (KVLsm *)pKVStore;

  assert( iLevel>0 );
  if( p->pCsr==0 ){
    rc = lsm_csr_open(p->pDb, &p->pCsr);
  }
  if( rc==SQLITE4_OK && iLevel>=2 && iLevel>=pKVStore->iTransLevel ){
    rc = lsm_begin(p->pDb, iLevel-1);
  }

  if( rc==SQLITE4_OK ){
    pKVStore->iTransLevel = SQLITE4_MAX(iLevel, pKVStore->iTransLevel);
  }else if( pKVStore->iTransLevel==0 ){
    lsm_csr_close(p->pCsr);
    p->pCsr = 0;
  }

  return rc;
}

/*
** Commit a transaction or subtransaction.
**
** Make permanent all changes back through the most recent xBegin 
** with the iLevel+1.  If iLevel==0 then make all changes permanent.
** The argument iLevel will always be less than the current transaction
** level when this routine is called.
**
** Commit is divided into two phases.  A rollback is still possible after
** phase one completes.  In this implementation, phase one is a no-op since
** phase two cannot fail.
**
** After this routine returns successfully, the transaction level will be 
** equal to iLevel.
*/
static int kvlsmCommitPhaseOne(KVStore *pKVStore, int iLevel){
  return SQLITE4_OK;
}
static int kvlsmCommitPhaseTwo(KVStore *pKVStore, int iLevel){
  int rc = SQLITE4_OK;
  KVLsm *p = (KVLsm *)pKVStore;

  if( pKVStore->iTransLevel>iLevel ){
    if( pKVStore->iTransLevel>=2 ){
      rc = lsm_commit(p->pDb, SQLITE4_MAX(0, iLevel-1));
    }
    if( iLevel==0 ){
      lsm_csr_close(p->pCsr);
      p->pCsr = 0;
    }
    if( rc==SQLITE4_OK ){
      pKVStore->iTransLevel = iLevel;
    }
  }
  return rc;
}

/*
** Rollback a transaction or subtransaction.
**
** Revert all uncommitted changes back through the most recent xBegin or 
** xCommit with the same iLevel.  If iLevel==0 then back out all uncommited
** changes.
**
** After this routine returns successfully, the transaction level will be
** equal to iLevel.
*/
static int kvlsmRollback(KVStore *pKVStore, int iLevel){
  int rc = SQLITE4_OK;
  KVLsm *p = (KVLsm *)pKVStore;

  if( pKVStore->iTransLevel>=iLevel ){
    if( pKVStore->iTransLevel>=2 ){
      rc = lsm_rollback(p->pDb, SQLITE4_MAX(0, iLevel-1));
    }
    if( iLevel==0 ){
      lsm_csr_close(p->pCsr);
      p->pCsr = 0;
    }
    if( rc==SQLITE4_OK ){
      pKVStore->iTransLevel = iLevel;
    }
  }
  return rc;
}

/*
** Revert a transaction back to what it was when it started.
*/
static int kvlsmRevert(KVStore *pKVStore, int iLevel){
  return SQLITE4_OK;
}

/*
** Implementation of the xReplace(X, aKey, nKey, aData, nData) method.
**
** Insert or replace the entry with the key aKey[0..nKey-1].  The data for
** the new entry is aData[0..nData-1].  Return SQLITE4_OK on success or an
** error code if the insert fails.
**
** The inputs aKey[] and aData[] are only valid until this routine
** returns.  If the storage engine needs to keep that information
** long-term, it will need to make its own copy of these values.
**
** A transaction will always be active when this routine is called.
*/
static int kvlsmReplace(
  KVStore *pKVStore,
  const KVByteArray *aKey, KVSize nKey,
  const KVByteArray *aData, KVSize nData
){
  KVLsm *p = (KVLsm *)pKVStore;
  return lsm_insert(p->pDb, (void *)aKey, nKey, (void *)aData, nData);
}

/*
** Create a new cursor object.
*/
static int kvlsmOpenCursor(KVStore *pKVStore, KVCursor **ppKVCursor){
  int rc = SQLITE4_OK;
  KVLsm *p = (KVLsm *)pKVStore;
  KVLsmCsr *pCsr;

  pCsr = (KVLsmCsr *)sqlite4_malloc(pKVStore->pEnv, sizeof(KVLsmCsr));
  if( pCsr==0 ){
    rc = SQLITE4_NOMEM;
  }else{
    memset(pCsr, 0, sizeof(KVLsmCsr));
    rc = lsm_csr_open(p->pDb, &pCsr->pCsr);

    if( rc==SQLITE4_OK ){
      pCsr->base.pStore = pKVStore;
      pCsr->base.pStoreVfunc = pKVStore->pStoreVfunc;
    }else{
      sqlite4_free(pCsr->base.pEnv, pCsr);
      pCsr = 0;
    }
  }

  *ppKVCursor = (KVCursor*)pCsr;
  return rc;
}

/*
** Reset a cursor
*/
static int kvlsmReset(KVCursor *pKVCursor){
  return SQLITE4_OK;
}

/*
** Destroy a cursor object
*/
static int kvlsmCloseCursor(KVCursor *pKVCursor){
  KVLsmCsr *pCsr = (KVLsmCsr *)pKVCursor;
  lsm_csr_close(pCsr->pCsr);
  sqlite4_free(pCsr->base.pEnv, pCsr);
  return SQLITE4_OK;
}

/*
** Move a cursor to the next non-deleted node.
*/
static int kvlsmNextEntry(KVCursor *pKVCursor){
  int rc;
  KVLsmCsr *pCsr = (KVLsmCsr *)pKVCursor;

  if( lsm_csr_valid(pCsr->pCsr)==0 ) return SQLITE4_NOTFOUND;
  rc = lsm_csr_next(pCsr->pCsr);
  if( rc==LSM_OK && lsm_csr_valid(pCsr->pCsr)==0 ){
    rc = SQLITE4_NOTFOUND;
  }
  return rc;
}

/*
** Move a cursor to the previous non-deleted node.
*/
static int kvlsmPrevEntry(KVCursor *pKVCursor){
  int rc;
  KVLsmCsr *pCsr = (KVLsmCsr *)pKVCursor;

  if( lsm_csr_valid(pCsr->pCsr)==0 ) return SQLITE4_NOTFOUND;
  rc = lsm_csr_prev(pCsr->pCsr);
  if( rc==LSM_OK && lsm_csr_valid(pCsr->pCsr)==0 ){
    rc = SQLITE4_NOTFOUND;
  }
  return rc;
}

/*
** Seek a cursor.
*/
static int kvlsmSeek(
  KVCursor *pKVCursor, 
  const KVByteArray *aKey,
  KVSize nKey,
  int dir
){
  int rc;
  KVLsmCsr *pCsr = (KVLsmCsr *)pKVCursor;

  assert( dir==0 || dir==1 || dir==-1 || dir==-2 );
  assert( LSM_SEEK_EQ==0 && LSM_SEEK_GE==1 && LSM_SEEK_LE==-1 );
  assert( LSM_SEEK_LEFAST==-2 );

  rc = lsm_csr_seek(pCsr->pCsr, (void *)aKey, nKey, dir);
  if( rc==SQLITE4_OK ){
    if( lsm_csr_valid(pCsr->pCsr)==0 ){
      rc = SQLITE4_NOTFOUND;
    }else{
      const void *pDbKey;
      int nDbKey;

      rc = lsm_csr_key(pCsr->pCsr, &pDbKey, &nDbKey);
      if( rc==SQLITE4_OK && (nDbKey!=nKey || memcmp(pDbKey, aKey, nKey)) ){
        rc = SQLITE4_INEXACT;
      }
    }
  }

  return rc;
}

/*
** Delete the entry that the cursor is pointing to.
**
** Though the entry is "deleted", it still continues to exist as a
** phantom.  Subsequent xNext or xPrev calls will work, as will
** calls to xKey and xData, thought the result from xKey and xData
** are undefined.
*/
static int kvlsmDelete(KVCursor *pKVCursor){
  int rc;
  const void *pKey;
  int nKey;
  KVLsmCsr *pCsr = (KVLsmCsr *)pKVCursor;

  assert( lsm_csr_valid(pCsr->pCsr) );
  rc = lsm_csr_key(pCsr->pCsr, &pKey, &nKey);
  if( rc==SQLITE4_OK ){
    rc = lsm_delete(((KVLsm *)(pKVCursor->pStore))->pDb, pKey, nKey);
  }

  return SQLITE4_OK;
}

/*
** Return the key of the node the cursor is pointing to.
*/
static int kvlsmKey(
  KVCursor *pKVCursor,         /* The cursor whose key is desired */
  const KVByteArray **paKey,   /* Make this point to the key */
  KVSize *pN                   /* Make this point to the size of the key */
){
  KVLsmCsr *pCsr = (KVLsmCsr *)pKVCursor;
  if( 0==lsm_csr_valid(pCsr->pCsr) ) return SQLITE4_DONE;
  return lsm_csr_key(pCsr->pCsr, (const void **)paKey, (int *)pN);
}

/*
** Return the data of the node the cursor is pointing to.
*/
static int kvlsmData(
  KVCursor *pKVCursor,         /* The cursor from which to take the data */
  KVSize ofst,                 /* Offset into the data to begin reading */
  KVSize n,                    /* Number of bytes requested */
  const KVByteArray **paData,  /* Pointer to the data written here */
  KVSize *pNData               /* Number of bytes delivered */
){
  KVLsmCsr *pCsr = (KVLsmCsr *)pKVCursor;
  int rc;
  void *pData;
  int nData;

  rc = lsm_csr_value(pCsr->pCsr, (const void **)&pData, &nData);
  if( rc==SQLITE4_OK ){
    if( n<0 ){
      *paData = pData;
      *pNData = nData;
    }else{
      int nOut = n;
      if( (ofst+n)>nData ) nOut = nData - ofst;
      if( nOut<0 ) nOut = 0;

      *paData = &((u8 *)pData)[n];
      *pNData = nOut;
    }
  }

  return rc;
}

/*
** Destructor for the entire in-memory storage tree.
*/
static int kvlsmClose(KVStore *pKVStore){
  KVLsm *p = (KVLsm *)pKVStore;

  /* If there is an active transaction, roll it back. The important
  ** part is that the read-transaction cursor is closed. Otherwise, the
  ** call to lsm_close() below will fail.  */
  kvlsmRollback(pKVStore, 0);
  assert( p->pCsr==0 );

  lsm_close(p->pDb);
  sqlite4_free(p->base.pEnv, p);
  return SQLITE4_OK;
}

static int kvlsmControl(KVStore *pKVStore, int op, void *pArg){
  int rc = SQLITE4_OK;
  KVLsm *p = (KVLsm *)pKVStore;

  switch( op ){
    case SQLITE4_KVCTRL_LSM_HANDLE: {
      lsm_db **ppOut = (lsm_db **)pArg;
      *ppOut = p->pDb;
      break;
    }

    case SQLITE4_KVCTRL_SYNCHRONOUS: {
      int *peSafety = (int *)pArg;
      int eParam = *peSafety + 1;
      lsm_config(p->pDb, LSM_CONFIG_SAFETY, &eParam);
      *peSafety = eParam-1;
      break;
    }

    case SQLITE4_KVCTRL_LSM_FLUSH: {
      lsm_flush(p->pDb);
      break;
    }

    case SQLITE4_KVCTRL_LSM_MERGE: {
      int nPage = *(int*)pArg;
      int nWrite = 0;
      lsm_work(p->pDb, 0, nPage, &nWrite);
      *(int*)pArg = nWrite;
      break;
    }

    case SQLITE4_KVCTRL_LSM_CHECKPOINT: {
      lsm_checkpoint(p->pDb, 0);
      break;
    }


    default:
      rc = SQLITE4_NOTFOUND;
      break;
  }

  return rc;
}

static int kvlsmGetMeta(KVStore *pKVStore, unsigned int *piVal){
  KVLsm *p = (KVLsm *)pKVStore;
  return lsm_get_user_version(p->pDb, piVal);
}

static int kvlsmPutMeta(KVStore *pKVStore, unsigned int iVal){
  KVLsm *p = (KVLsm *)pKVStore;
  return lsm_set_user_version(p->pDb, iVal);
}

typedef struct PragmaCtx PragmaCtx;
struct PragmaCtx {
  lsm_db *pDb;
  int ePragma;
  int eConfig;
};

#define KVLSM_LSM_FLUSH      1
#define KVLSM_LSM_WORK       2
#define KVLSM_LSM_CHECKPOINT 3
#define KVLSM_LSM_CONFIG     4

static void kvlsmPragmaDestroy(void *p){
  sqlite4_free(0, p);
}

static void kvlsmPragma(sqlite4_context *ctx, int nArg, sqlite4_value **apArg){
  PragmaCtx *p = (PragmaCtx *)sqlite4_context_appdata(ctx);
  int rc = SQLITE4_OK;

  switch( p->ePragma ){
    case KVLSM_LSM_FLUSH:
      if( nArg!=0 ) goto wrong_num_args;
      rc = lsm_flush(p->pDb);
      break;

    case KVLSM_LSM_WORK:
      if( nArg!=2 ){
        goto wrong_num_args;
      }else{
        int nMerge = sqlite4_value_int(apArg[0]);
        int nWrite = sqlite4_value_int(apArg[1]);
        rc = lsm_work(p->pDb, nMerge, nWrite, &nWrite);
        sqlite4_result_int(ctx, nWrite);
      }
      break;

    case KVLSM_LSM_CHECKPOINT: {
      int nKB;
      if( nArg!=0 ) goto wrong_num_args;
      rc = lsm_checkpoint(p->pDb, &nKB);
      sqlite4_result_int(ctx, nKB);
      break;
    }

    case KVLSM_LSM_CONFIG: {
      int iVal = -1;
      if( nArg>1 ) goto wrong_num_args;
      if( nArg==1 ){
        iVal = sqlite4_value_int(apArg[0]);
      }
      rc = lsm_config(p->pDb, p->eConfig, &iVal);
      sqlite4_result_int(ctx, iVal);
      break;
    }
  }

  if( rc!=SQLITE4_OK ){
    sqlite4_result_error_code(ctx, rc);
  }
  return;

 wrong_num_args:
  sqlite4_result_error(ctx, "wrong number of arguments", -1);
}

static int kvlsmGetMethod(
  sqlite4_kvstore *pKVStore, 
  const char *zMethod, 
  void **ppArg,
  void (**pxFunc)(sqlite4_context *, int, sqlite4_value **),
  void (**pxDestroy)(void *)
){
  KVLsm *pLsm = (KVLsm *)pKVStore;
  PragmaCtx *p;
  int ePragma = 0;
  int eConfig = 0;
  struct ConfigPragma {
    const char *zName;
    int eConfig;
  } aConfigPragma[] = {
    { "lsm_autoflush",          LSM_CONFIG_AUTOFLUSH },
    { "page_size",              LSM_CONFIG_PAGE_SIZE },
    { "lsm_safety",             LSM_CONFIG_SAFETY },
    { "lsm_block_size",         LSM_CONFIG_BLOCK_SIZE },
    { "lsm_autowork",           LSM_CONFIG_AUTOWORK },
    { "lsm_mmap",               LSM_CONFIG_MMAP },
    { "lsm_use_log",            LSM_CONFIG_USE_LOG },
    { "lsm_automerge",          LSM_CONFIG_AUTOMERGE },
    { "lsm_max_freelist",       LSM_CONFIG_MAX_FREELIST },
    { "lsm_multiple_processes", LSM_CONFIG_MULTIPLE_PROCESSES },
    { "lsm_autocheckpoint",     LSM_CONFIG_AUTOCHECKPOINT },
    { "lsm_readonly",           LSM_CONFIG_READONLY }
  };
  int i;

  if( 0==sqlite4_strnicmp(zMethod, "lsm_flush", 9) ){
    ePragma = KVLSM_LSM_FLUSH;
  }
  else if( 0==sqlite4_strnicmp(zMethod, "lsm_work", 8) ){
    ePragma = KVLSM_LSM_WORK;
  }
  else if( 0==sqlite4_strnicmp(zMethod, "lsm_checkpoint", 14) ){
    ePragma = KVLSM_LSM_CHECKPOINT;
  }else{
    for(i=0; i<ArraySize(aConfigPragma); i++){
      if( 0==sqlite4_stricmp(zMethod, aConfigPragma[i].zName) ){
        ePragma = KVLSM_LSM_CONFIG;
        eConfig = aConfigPragma[i].eConfig;
        break;
      }
    }
    if( ePragma==0 ) return SQLITE4_NOTFOUND;
  }

  p = sqlite4_malloc(0, sizeof(PragmaCtx));
  if( p==0 ) return SQLITE4_NOMEM;
  p->ePragma = ePragma;
  p->eConfig = eConfig;
  p->pDb = pLsm->pDb;

  *ppArg = (void *)p;
  *pxFunc = kvlsmPragma;
  *pxDestroy = kvlsmPragmaDestroy;
  return SQLITE4_OK;
}

/*
** Create a new in-memory storage engine and return a pointer to it.
*/
int sqlite4KVStoreOpenLsm(
  sqlite4_env *pEnv,          /* Run-time environment */
  KVStore **ppKVStore,        /* OUT: write the new KVStore here */
  const char *zName,          /* Name of the file to open */
  unsigned openFlags          /* Flags */
){

  /* Virtual methods for an LSM data store */
  static const KVStoreMethods kvlsmMethods = {
    1,                            /* iVersion */
    sizeof(KVStoreMethods),       /* szSelf */
    kvlsmReplace,                 /* xReplace */
    kvlsmOpenCursor,              /* xOpenCursor */
    kvlsmSeek,                    /* xSeek */
    kvlsmNextEntry,               /* xNext */
    kvlsmPrevEntry,               /* xPrev */
    kvlsmDelete,                  /* xDelete */
    kvlsmKey,                     /* xKey */
    kvlsmData,                    /* xData */
    kvlsmReset,                   /* xReset */
    kvlsmCloseCursor,             /* xCloseCursor */
    kvlsmBegin,                   /* xBegin */
    kvlsmCommitPhaseOne,          /* xCommitPhaseOne */
    kvlsmCommitPhaseTwo,          /* xCommitPhaseTwo */
    kvlsmRollback,                /* xRollback */
    kvlsmRevert,                  /* xRevert */
    kvlsmClose,                   /* xClose */
    kvlsmControl,                 /* xControl */
    kvlsmGetMeta,                 /* xGetMeta */
    kvlsmPutMeta,                 /* xPutMeta */
    kvlsmGetMethod                /* xGetMethod */
  };

  KVLsm *pNew;
  int rc = SQLITE4_OK;

  pNew = (KVLsm *)sqlite4_malloc(pEnv, sizeof(KVLsm));
  if( pNew==0 ){
    rc = SQLITE4_NOMEM;
  }else{
    struct Config {
      const char *zParam;
      int eParam;
    } aConfig[] = {
      { "lsm_mmap", LSM_CONFIG_MMAP },
      { "page_size", LSM_CONFIG_PAGE_SIZE },
      { "lsm_block_size", LSM_CONFIG_BLOCK_SIZE },
      { "lsm_multiple_processes", LSM_CONFIG_MULTIPLE_PROCESSES },
      { "lsm_automerge", LSM_CONFIG_AUTOMERGE }
    };

    memset(pNew, 0, sizeof(KVLsm));
    pNew->base.pStoreVfunc = &kvlsmMethods;
    pNew->base.pEnv = pEnv;
    rc = lsm_new(0, &pNew->pDb);
    if( rc==SQLITE4_OK ){
      int i;
      for(i=0; i<ArraySize(aConfig); i++){
        const char *zVal = sqlite4_uri_parameter(zName, aConfig[i].zParam);
        if( zVal ){
          int nVal = sqlite4Atoi(zVal);
          lsm_config(pNew->pDb, aConfig[i].eParam, &nVal);
        }
      }

      rc = kvlsmConfigureCompression(pNew->pDb, zName);
      if( rc==SQLITE4_OK ){
        rc = lsm_open(pNew->pDb, zName);
      }
    }

    if( rc!=SQLITE4_OK ){
      lsm_close(pNew->pDb);
      sqlite4_free(pEnv, pNew);
      pNew = 0;
    }
  }

  *ppKVStore = (KVStore*)pNew;
  return rc;
}
