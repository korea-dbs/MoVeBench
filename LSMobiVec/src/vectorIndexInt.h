#ifndef _VECTOR_INDEX_H
#define _VECTOR_INDEX_H

#include "sqlite4.h"
#include "vectorInt.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DiskAnnIndex DiskAnnIndex;
typedef struct BlobSpot BlobSpot;

/*
 * Main type which holds all necessary index information and will be passed as a first argument in all index-related operations
*/
struct DiskAnnIndex {
  sqlite4 *db;         /* Database connection */
  char *zDbSName;      /* Database name */
  char *zName;         /* Index name */
  char *zShadow;       /* Shadow table name */
  int nFormatVersion;  /* DiskAnn format version */
  int nDistanceFunc;   /* Distance function */
  int nBlockSize;      /* Size of the block which stores all data for single node */
  int nVectorDims;     /* Vector dimensions */
  int nNodeVectorType; /* Vector type of each node */
  int nEdgeVectorType; /* Vector type of each edge */
  int nNodeVectorSize; /* Vector size of each node in bytes */
  int nEdgeVectorSize; /* Vector size of each edge in bytes */
  i64 nShadowRows;    /* Cached count of shadow table rows (for random start) */
  float pruningAlpha;  /* Alpha parameter for edge pruning during INSERT operation */
  int insertL;         /* Max size of candidate set (L) visited during INSERT operation */
  int searchL;         /* Max size of candidate set (L) visited during SEARCH operation */

  /* Direct KV access for blob operations (bypasses SQL/VDBE overhead) */
  int iShadowTnum;     /* tnum (root) of the shadow table in KV store */
  int nKeyPrefix;      /* length of the varint-encoded tnum prefix */
  u8 aKeyPrefix[9];    /* varint-encoded tnum prefix (max 9 bytes for varint64) */
  KVCursor *pReadCsr;  /* persistent read cursor (reused across blobSpotReload calls) */

  int nReads;
  int nWrites;
  double totalSearchMs;      /* greedy search */
  double totalShadowInsMs;   /* shadow row insert */
  double totalPass1Ms;       /* pass1: add edges to new node */
  double totalPass2Ms;       /* pass2: update neighbor edges + their flush */
  double totalNewFlushMs;    /* flush new node's blob */
  double totalBuildReadMs;   /* index build read I/O */
  double totalBuildWriteMs;  /* index build write I/O */
  double totalBuildDistMs;   /* index build distance compute */
  double totalBuildLsmMs;    /* LSM work during index build */
};

/*
 * BlobSpot replacement for sqlite4 (no sqlite4_blob API).
 * Uses direct KV store access for reads/writes, bypassing VDBE entirely.
*/
struct BlobSpot {
  u64 nRowid;       /* row id of the currently loaded node */
  u8 *pBuffer;      /* buffer for BLOB data */
  int nBufferSize;  /* buffer size */
  u8 isWritable;    /* blob open mode (readonly or read/write) */
  u8 isInitialized; /* was blob read after creation or not */
  u8 isAborted;     /* set to true if last operation with blob failed */
};

/* Special error code for blobSpotCreate/blobSpotReload functions */
#define DISKANN_ROW_NOT_FOUND 1001

#define DISKANN_BLOB_WRITABLE 1
#define DISKANN_BLOB_READONLY 0

/* BlobSpot operations */
int blobSpotCreate(const DiskAnnIndex *pIndex, BlobSpot **ppBlobSpot, u64 nRowid, int nBufferSize, int isWritable);
int blobSpotReload(DiskAnnIndex *pIndex, BlobSpot *pBlobSpot, u64 nRowid, int nBufferSize);
int blobSpotFlush(DiskAnnIndex *pIndex, BlobSpot *pBlobSpot);
void blobSpotFree(BlobSpot *pBlobSpot);

/*
 * Accessor for node binary format
 * - default format is the following:
 *   [u64 nRowid] [u16 nEdges] [6 byte padding] [node vector] [edge vector] * nEdges [trash vector] * (nMaxEdges - nEdges) ([u32 unused] [f32 distance] [u64 edgeId]) * nEdges
 *   Note, that 6 byte padding after nEdges required to align [node vector] by word boundary and avoid unaligned reads
 *   Note, that node vector and edge vector can have different representations (and edge vector can be smaller in size than node vector)
*/
int nodeEdgesMaxCount(const DiskAnnIndex *pIndex);
int nodeEdgesMetadataOffset(const DiskAnnIndex *pIndex);
void nodeBinInit(const DiskAnnIndex *pIndex, BlobSpot *pBlobSpot, u64 nRowid, Vector *pVector);
void nodeBinVector(const DiskAnnIndex *pIndex, const BlobSpot *pBlobSpot, Vector *pVector);
u16 nodeBinEdges(const DiskAnnIndex *pIndex, const BlobSpot *pBlobSpot);
void nodeBinEdge(const DiskAnnIndex *pIndex, const BlobSpot *pBlobSpot, int iEdge, u64 *pRowid, float *distance, Vector *pVector);
int nodeBinEdgeFindIdx(const DiskAnnIndex *pIndex, const BlobSpot *pBlobSpot, u64 nRowid);
void nodeBinPruneEdges(const DiskAnnIndex *pIndex, BlobSpot *pBlobSpot, int nPruned);
void nodeBinReplaceEdge(const DiskAnnIndex *pIndex, BlobSpot *pBlobSpot, int iReplace, u64 nRowid, float distance, Vector *pVector);
void nodeBinDeleteEdge(const DiskAnnIndex *pIndex, BlobSpot *pBlobSpot, int iDelete);
void nodeBinDebug(const DiskAnnIndex *pIndex, const BlobSpot *pBlobSpot);

/**************************************************************************
** Vector index utilities
****************************************************************************/

/* Vector index utility objects */
typedef struct VectorIdxKey VectorIdxKey;
typedef struct VectorIdxParams VectorIdxParams;
typedef struct VectorInRow VectorInRow;
typedef struct VectorOutRows VectorOutRows;

typedef u8 IndexType;
typedef u8 MetricType;

/*
 * All vector index parameters must be known to the vectorIndex module although it's interpretation are up to the specific implementation of the index
*/

/* format version */
#define VECTOR_FORMAT_PARAM_ID              1
#define VECTOR_FORMAT_V1                    1
#define VECTOR_FORMAT_V2                    2
#define VECTOR_FORMAT_DEFAULT               3

/* type of the vector index */
#define VECTOR_INDEX_TYPE_PARAM_ID          2
#define VECTOR_INDEX_TYPE_DISKANN           1

/* type of the underlying vector for the vector index */
#define VECTOR_TYPE_PARAM_ID                3
/* dimension of the underlying vector for the vector index */
#define VECTOR_DIM_PARAM_ID                 4

/* metric type used for comparing two vectors */
#define VECTOR_METRIC_TYPE_PARAM_ID         5
#define VECTOR_METRIC_TYPE_COS              1
#define VECTOR_METRIC_TYPE_L2               2

/* block size */
#define VECTOR_BLOCK_SIZE_PARAM_ID          6
#define VECTOR_BLOCK_SIZE_DEFAULT           128

#define VECTOR_PRUNING_ALPHA_PARAM_ID       7
#define VECTOR_PRUNING_ALPHA_DEFAULT        1.2

#define VECTOR_INSERT_L_PARAM_ID            8
#define VECTOR_INSERT_L_DEFAULT             70

#define VECTOR_SEARCH_L_PARAM_ID            9
#define VECTOR_SEARCH_L_DEFAULT             200

#define VECTOR_MAX_NEIGHBORS_PARAM_ID       10

#define VECTOR_COMPRESS_NEIGHBORS_PARAM_ID  11

/* total amount of vector index parameters */
#define VECTOR_PARAM_IDS_COUNT              11

/*
 * Vector index parameters are stored in simple binary format (1 byte tag + 8 byte u64 integer / f64 float)
*/
#define VECTOR_INDEX_PARAMS_BUF_SIZE 128
struct VectorIdxParams {
  u8 pBinBuf[VECTOR_INDEX_PARAMS_BUF_SIZE];
  int nBinSize;
};


/*
 * Structure which holds information about primary key of the base table for vector index
*/
#define VECTOR_INDEX_MAX_KEY_COLUMNS 16
struct VectorIdxKey {
  int nKeyColumns;
  char aKeyAffinity[VECTOR_INDEX_MAX_KEY_COLUMNS];
  /* collation is owned by the caller and structure is not responsible for reclamation of collation string resources */
  const char *azKeyCollation[VECTOR_INDEX_MAX_KEY_COLUMNS];
};

/*
 * Structure which holds information about input payload for vector index (for INSERT/DELETE operations)
 * pVector must be NULL for DELETE operation
 *
 * Resources must be reclaimed with vectorInRowFree(...) method
 *
 * In the sqlite4 port, only rowid-like (single INTEGER PK) tables are supported.
 * nRowid stores the base table's primary key value directly.
*/
struct VectorInRow {
  Vector *pVector;
  int nKeys;
  u64 nRowid;           /* Direct rowid for rowid-like PK tables */
  sqlite4_value *pKeyValues;  /* For non-rowid tables (not used in sqlite4 port) */
};

/*
 * Structure which holds information about result set of SEARCH operation
*/
#define VECTOR_OUT_ROWS_MAX_CELLS (1<<30)
struct VectorOutRows {
  int nRows;
  int nCols;
  i64 *aIntValues;
  sqlite4_value **ppValues;
};

/* limit to the sql part which we render in order to perform operations with shadow tables */
#define VECTOR_INDEX_SQL_RENDER_LIMIT 128

void vectorIdxParamsInit(VectorIdxParams *, u8 *, int);
u64 vectorIdxParamsGetU64(const VectorIdxParams *, char);
double vectorIdxParamsGetF64(const VectorIdxParams *, char);
int vectorIdxParamsPutU64(VectorIdxParams *, char, u64);
int vectorIdxParamsPutF64(VectorIdxParams *, char, double);

int vectorIdxKeyGet(const Index *, VectorIdxKey *, const char **);
int vectorIdxKeyRowidLike(const VectorIdxKey *);
int vectorIdxKeyDefsRender(const VectorIdxKey *, const char *, char *, int);
int vectorIdxKeyNamesRender(int, const char *, char *, int);

int vectorInRowAlloc(sqlite4 *, i64, sqlite4_value *, VectorInRow *, char **);
sqlite4_value* vectorInRowKey(const VectorInRow *, int);
int vectorInRowTryGetRowid(const VectorInRow *, u64 *);
i64 vectorInRowLegacyId(const VectorInRow *);
int vectorInRowPlaceholderRender(const VectorInRow *, char *, int);
void vectorInRowFree(sqlite4 *, VectorInRow *);

int vectorOutRowsAlloc(sqlite4 *, VectorOutRows *, int, int, int);
int vectorOutRowsPut(VectorOutRows *, int, int, const u64 *, sqlite4_value *);
void vectorOutRowsGet(sqlite4_context *, const VectorOutRows *, int, int);
void vectorOutRowsFree(sqlite4 *, VectorOutRows *);

int diskAnnCreateIndex(sqlite4 *, const char *, const char *, const VectorIdxKey *, VectorIdxParams *, const char **);
int diskAnnClearIndex(sqlite4 *, const char *, const char *);
int diskAnnDropIndex(sqlite4 *, const char *, const char *);
int diskAnnOpenIndex(sqlite4 *, const char *, const char *, const VectorIdxParams *, DiskAnnIndex **);
void diskAnnCloseIndex(DiskAnnIndex *);
int diskAnnInsert(DiskAnnIndex *, const VectorInRow *, char **);
int diskAnnDelete(DiskAnnIndex *, const VectorInRow *, char **);
int diskAnnSearch(DiskAnnIndex *, const Vector *, int, const VectorIdxKey *, VectorOutRows *, char **);

typedef struct VectorIdxCursor VectorIdxCursor;

#define VECTOR_INDEX_VTAB_NAME         "vector_top_k"
#define VECTOR_INDEX_GLOBAL_META_TABLE "libsql_vector_meta_shadow"
#define VECTOR_INDEX_MARKER_FUNCTION   "libsql_vector_idx"

int vectorIdxParseColumnType(const char *, int *, int *, const char **);

int vectorIndexCreate(Parse*, const Index*, const char *, const IdList*, ExprList*);
int vectorIndexClear(sqlite4 *, const char *, const char *);
int vectorIndexDrop(sqlite4 *, const char *, const char *);
int vectorIndexSearch(sqlite4 *, int, sqlite4_value **, VectorOutRows *, int *, int *, char **);
int vectorIndexCursorInit(sqlite4 *, const char *, const char *, VectorIdxCursor **);
void vectorIndexCursorClose(sqlite4 *, VectorIdxCursor *, int *, int *);
int vectorIndexInsert(VectorIdxCursor *, i64, sqlite4_value *, char **);
int vectorIndexDelete(VectorIdxCursor *, i64, char **);

#ifdef __cplusplus
}  /* end of the 'extern "C"' block */
#endif

#endif /* _VECTOR_INDEX_H */
