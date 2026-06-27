// /*
// ** compact_db.c
// **
// ** Manually run LSM compaction on a LSMoVe database file.
// ** Use this after bulk inserts when LSM_DFLT_AUTOWORK=0.
// **
// ** Build:
// **   gcc -O2 compact_db.c -I. -Isrc/ -L. -lsqlite4 -lpthread -lm -lz -llz4 -o compact_db
// **
// ** Usage:
// **   ./compact_db /mnt/nvme0/mydb.db [none|zlib|lz4]
// */
// #include <stdio.h>
// #include <stdlib.h>
// #include <string.h>
// #include "src/lsm.h"
// #include <lz4.h>
// #include <zlib.h>

// #define COMPACT_COMPRESSION_ZLIB_ID 100
// #define COMPACT_COMPRESSION_LZ4_ID 101

// static int compactZlibBound(void *pCtx, int nSrc){
//   (void)pCtx;
//   return (int)compressBound((uLong)nSrc);
// }

// static int compactZlibCompress(
//   void *pCtx,
//   char *aOut, int *pnOut,
//   const char *aIn, int nIn
// ){
//   uLongf nOut = (uLongf)*pnOut;
//   int rc;
//   (void)pCtx;

//   rc = compress((Bytef*)aOut, &nOut, (const Bytef*)aIn, (uLong)nIn);
//   if( rc!=Z_OK ) return LSM_ERROR;

//   *pnOut = (int)nOut;
//   return LSM_OK;
// }

// static int compactZlibUncompress(
//   void *pCtx,
//   char *aOut, int *pnOut,
//   const char *aIn, int nIn
// ){
//   uLongf nOut = (uLongf)*pnOut;
//   int rc;
//   (void)pCtx;

//   rc = uncompress((Bytef*)aOut, &nOut, (const Bytef*)aIn, (uLong)nIn);
//   if( rc!=Z_OK ) return LSM_ERROR;

//   *pnOut = (int)nOut;
//   return LSM_OK;
// }

// static lsm_compress compactZlibCompression = {
//   0,
//   COMPACT_COMPRESSION_ZLIB_ID,
//   compactZlibBound,
//   compactZlibCompress,
//   compactZlibUncompress,
//   0
// };

// static int compactLz4Bound(void *pCtx, int nSrc){
//   (void)pCtx;
//   return LZ4_compressBound(nSrc);
// }

// static int compactLz4Compress(
//   void *pCtx,
//   char *aOut, int *pnOut,
//   const char *aIn, int nIn
// ){
//   int nOut;
//   (void)pCtx;

//   nOut = LZ4_compress_default(aIn, aOut, nIn, *pnOut);
//   if( nOut<=0 ) return LSM_ERROR;

//   *pnOut = nOut;
//   return LSM_OK;
// }

// static int compactLz4Uncompress(
//   void *pCtx,
//   char *aOut, int *pnOut,
//   const char *aIn, int nIn
// ){
//   int nOut;
//   (void)pCtx;

//   nOut = LZ4_decompress_safe(aIn, aOut, nIn, *pnOut);
//   if( nOut<0 ) return LSM_ERROR;

//   *pnOut = nOut;
//   return LSM_OK;
// }

// static lsm_compress compactLz4Compression = {
//   0,
//   COMPACT_COMPRESSION_LZ4_ID,
//   compactLz4Bound,
//   compactLz4Compress,
//   compactLz4Uncompress,
//   0
// };

// int main(int argc, char **argv){
//   lsm_db *pDb = 0;
//   lsm_env *pEnv = lsm_default_env();
//   int nWritten = 0;
//   int nTotal = 0;
//   int rc;

//   if( argc < 2 ){
//     fprintf(stderr, "Usage: %s <database_file> [none|zlib|lz4]\n", argv[0]);
//     return 1;
//   }

//   rc = lsm_new(pEnv, &pDb);
//   if( rc != 0 ){
//     fprintf(stderr, "lsm_new failed: %d\n", rc);
//     return 1;
//   }

//   if( argc >= 3 && strcmp(argv[2], "zlib")==0 ){
//     rc = lsm_config(pDb, LSM_CONFIG_SET_COMPRESSION, &compactZlibCompression);
//     if( rc != 0 ){
//       fprintf(stderr, "lsm_config compression failed: %d\n", rc);
//       lsm_close(pDb);
//       return 1;
//     }
//   }else if( argc >= 3 && strcmp(argv[2], "lz4")==0 ){
//     rc = lsm_config(pDb, LSM_CONFIG_SET_COMPRESSION, &compactLz4Compression);
//     if( rc != 0 ){
//       fprintf(stderr, "lsm_config compression failed: %d\n", rc);
//       lsm_close(pDb);
//       return 1;
//     }
//   }

//   rc = lsm_open(pDb, argv[1]);
//   if( rc != 0 ){
//     fprintf(stderr, "lsm_open failed: %d\n", rc);
//     lsm_close(pDb);
//     return 1;
//   }

//   /* Show DB structure before compaction */
//   {
//     char *zInfo = 0;
//     lsm_info(pDb, LSM_INFO_DB_STRUCTURE, &zInfo);
//     fprintf(stderr, "Before: %s\n", zInfo ? zInfo : "(null)");
//     lsm_free(pEnv, zInfo);
//   }

//   /* Phase 1: Merge with pDb->nMerge to do bulk merge work */
//   fprintf(stderr, "Compacting %s (phase 1: merge)\n", argv[1]);
//   do {
//     rc = lsm_work(pDb, 0, 4096, &nWritten);
//     if( rc != 0 ){
//       fprintf(stderr, "lsm_work failed: %d\n", rc);
//       break;
//     }
//     nTotal += nWritten;
//     if( nWritten > 0 ){
//       fprintf(stderr, "  %d KB written so far\n", nTotal);
//     }
//   } while( nWritten > 0 );

//   fprintf(stderr, "Phase 1 done. Total: %d KB written.\n", nTotal);

//   /* Phase 2: Compact to single segment with nMerge=1.
//   ** This is required before lsm_reclaim can safely move blocks,
//   ** because the redirect array is shared across ALL segments. */
//   if( rc == 0 ){
//     fprintf(stderr, "Phase 2: compact to single segment\n");
//     nTotal = 0;
//     do {
//       rc = lsm_work(pDb, 1, 4096, &nWritten);
//       if( rc != 0 ){
//         fprintf(stderr, "lsm_work(nMerge=1) failed: %d\n", rc);
//         break;
//       }
//       nTotal += nWritten;
//       if( nWritten > 0 ){
//         fprintf(stderr, "  %d KB written so far\n", nTotal);
//       }
//     } while( nWritten > 0 );
//     fprintf(stderr, "Phase 2 done. Total: %d KB written.\n", nTotal);
//   }

//   /* Show DB structure after merge */
//   {
//     char *zInfo = 0;
//     lsm_info(pDb, LSM_INFO_DB_STRUCTURE, &zInfo);
//     fprintf(stderr, "After merge: %s\n", zInfo ? zInfo : "(null)");
//     lsm_free(pEnv, zInfo);
//   }

//   /* Reclaim free space */
//   fprintf(stderr, "Reclaiming free space\n");
//   nTotal = 0;
//   do {
//     rc = lsm_reclaim(pDb, 4096, &nWritten);
//     if( rc != 0 ){
//       fprintf(stderr, "lsm_reclaim failed: %d\n", rc);
//       break;
//     }
//     nTotal += nWritten;
//     if( nWritten > 0 ){
//       fprintf(stderr, "  %d KB relocated so far\n", nTotal);
//     }
//   } while( nWritten > 0 );

//   fprintf(stderr, "Reclaim done. Total: %d KB relocated.\n", nTotal);

//   /* Show final DB structure */
//   {
//     char *zInfo = 0;
//     lsm_info(pDb, LSM_INFO_DB_STRUCTURE, &zInfo);
//     fprintf(stderr, "Final:  %s\n", zInfo ? zInfo : "(null)");
//     lsm_free(pEnv, zInfo);
//   }

//   lsm_close(pDb);
//   return 0;
// }


// --------- work from here ---------
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/lsm.h"

#include <lz4.h>
#include <zlib.h>

#define COMPACT_COMPRESSION_ZLIB_ID 100
#define COMPACT_COMPRESSION_LZ4_ID 101
#define RECLAIM_STEP_KB 4096
#define RECLAIM_MAX_ITER 100000

static int compactZlibBound(void *pCtx, int nSrc){
  (void)pCtx;
  return (int)compressBound((uLong)nSrc);
}

static int compactZlibCompress(
  void *pCtx,
  char *aOut, int *pnOut,
  const char *aIn, int nIn
){
  uLongf nOut = (uLongf)*pnOut;
  int rc;
  (void)pCtx;

  rc = compress((Bytef*)aOut, &nOut, (const Bytef*)aIn, (uLong)nIn);
  if( rc!=Z_OK ) return LSM_ERROR;

  *pnOut = (int)nOut;
  return LSM_OK;
}

static int compactZlibUncompress(
  void *pCtx,
  char *aOut, int *pnOut,
  const char *aIn, int nIn
){
  uLongf nOut = (uLongf)*pnOut;
  int rc;
  (void)pCtx;

  rc = uncompress((Bytef*)aOut, &nOut, (const Bytef*)aIn, (uLong)nIn);
  if( rc!=Z_OK ) return LSM_ERROR;

  *pnOut = (int)nOut;
  return LSM_OK;
}

static lsm_compress compactZlibCompression = {
  0,
  COMPACT_COMPRESSION_ZLIB_ID,
  compactZlibBound,
  compactZlibCompress,
  compactZlibUncompress,
  0
};

static int compactLz4Bound(void *pCtx, int nSrc){
  (void)pCtx;
  return LZ4_compressBound(nSrc);
}

static int compactLz4Compress(
  void *pCtx,
  char *aOut, int *pnOut,
  const char *aIn, int nIn
){
  int nOut;
  (void)pCtx;

  nOut = LZ4_compress_default(aIn, aOut, nIn, *pnOut);
  if( nOut<=0 ) return LSM_ERROR;

  *pnOut = nOut;
  return LSM_OK;
}

static int compactLz4Uncompress(
  void *pCtx,
  char *aOut, int *pnOut,
  const char *aIn, int nIn
){
  int nOut;
  (void)pCtx;

  nOut = LZ4_decompress_safe(aIn, aOut, nIn, *pnOut);
  if( nOut<0 ) return LSM_ERROR;

  *pnOut = nOut;
  return LSM_OK;
}

static lsm_compress compactLz4Compression = {
  0,
  COMPACT_COMPRESSION_LZ4_ID,
  compactLz4Bound,
  compactLz4Compress,
  compactLz4Uncompress,
  0
};

static int configureCompression(lsm_db *pDb, const char *zCompression){
  lsm_compress *pCompression = 0;

  if( zCompression==0 || strcmp(zCompression, "none")==0 ){
    return LSM_OK;
  }else if( strcmp(zCompression, "zlib")==0 ){
    pCompression = &compactZlibCompression;
  }else if( strcmp(zCompression, "lz4")==0 ){
    pCompression = &compactLz4Compression;
  }else{
    fprintf(stderr, "unknown compression: %s\n", zCompression);
    return LSM_MISUSE;
  }

  return lsm_config(pDb, LSM_CONFIG_SET_COMPRESSION, pCompression);
}

static void printInfo(lsm_db *pDb, lsm_env *pEnv, int eInfo, const char *zLabel){
  char *zInfo = 0;
  int rc = lsm_info(pDb, eInfo, &zInfo);

  if( rc==LSM_OK ){
    fprintf(stderr, "%s: %s\n", zLabel, zInfo ? zInfo : "(null)");
  }else{
    fprintf(stderr, "%s: <lsm_info failed: %d>\n", zLabel, rc);
  }

  lsm_free(pEnv, zInfo);
}

static int runReclaimOnly(lsm_db *pDb){
  int rc = LSM_OK;
  int nWritten = 0;
  int nTotal = 0;
  int nIter = 0;

  fprintf(stderr, "Reclaiming free space only (no merge)\n");

  do {
    nWritten = 0;
    rc = lsm_reclaim(pDb, RECLAIM_STEP_KB, &nWritten);
    if( rc!=LSM_OK ){
      fprintf(stderr, "lsm_reclaim failed: %d\n", rc);
      return rc;
    }

    if( nWritten>0 ){
      nIter++;
      nTotal += nWritten;
      fprintf(stderr, "  iter %d: %d KB relocated (%d KB total)\n",
          nIter, nWritten, nTotal);

      if( nIter>=RECLAIM_MAX_ITER ){
        fprintf(stderr, "reclaim stopped: max iterations reached\n");
        return LSM_BUSY;
      }
    }
  } while( nWritten>0 );

  fprintf(stderr, "Reclaim done. Total: %d KB relocated.\n", nTotal);
  return LSM_OK;
}

int main(int argc, char **argv){
  lsm_db *pDb = 0;
  lsm_env *pEnv = lsm_default_env();
  const char *zDb;
  const char *zCompression = "none";
  int rc;

  if( argc<2 || argc>3 ){
    fprintf(stderr, "Usage: %s <database_file> [none|zlib|lz4]\n", argv[0]);
    return 1;
  }

  zDb = argv[1];
  if( argc>=3 ){
    zCompression = argv[2];
  }

  rc = lsm_new(pEnv, &pDb);
  if( rc!=LSM_OK ){
    fprintf(stderr, "lsm_new failed: %d\n", rc);
    return 1;
  }

  rc = configureCompression(pDb, zCompression);
  if( rc!=LSM_OK ){
    fprintf(stderr, "lsm_config compression failed: %d\n", rc);
    lsm_close(pDb);
    return 1;
  }

  rc = lsm_open(pDb, zDb);
  if( rc!=LSM_OK ){
    fprintf(stderr, "lsm_open failed: %d\n", rc);
    lsm_close(pDb);
    return 1;
  }

  fprintf(stderr, "DB: %s\n", zDb);
  fprintf(stderr, "Compression: %s\n", zCompression);
  printInfo(pDb, pEnv, LSM_INFO_DB_STRUCTURE, "Before");
  printInfo(pDb, pEnv, LSM_INFO_FREELIST, "In-file freelist before");

  rc = runReclaimOnly(pDb);

  printInfo(pDb, pEnv, LSM_INFO_DB_STRUCTURE, "Final");
  printInfo(pDb, pEnv, LSM_INFO_FREELIST, "In-file freelist final");

  if( lsm_close(pDb)!=LSM_OK && rc==LSM_OK ){
    rc = LSM_ERROR;
  }

  return rc==LSM_OK ? 0 : 1;
}
