/*
** 2024-03-23
**
** Copyright 2024 the libSQL authors
**
** Permission is hereby granted, free of charge, to any person obtaining a copy of
** this software and associated documentation files (the "Software"), to deal in
** the Software without restriction, including without limitation the rights to
** use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
** the Software, and to permit persons to whom the Software is furnished to do so,
** subject to the following conditions:
**
** The above copyright notice and this permission notice shall be included in all
** copies or substantial portions of the Software.
**
** THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
** IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
** FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
** COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
** IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
** CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
**
******************************************************************************
**
** DiskANN for SQLite4/libSQL.
** Ported from veclite (SQLite3-based) to SQLite4 API.
**
** Key differences from the SQLite3 version:
**  - No sqlite4_blob API: blob I/O is done via prepared SELECT/UPDATE statements
**  - No rowid concept: shadow table uses index_key (= base table INTEGER PK)
**  - Only rowid-like (single INTEGER PK) tables are supported
**  - All API uses sqlite4_* prefix and SQLITE4_* constants
*/
#ifndef SQLITE4_OMIT_VECTOR

#include "math.h"
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include "sqliteInt.h"
#include "vectorIndexInt.h"

/* Forward declarations for per-operation KV timing (accumulated in blobSpot functions) */
static double g_totalKvReadMs;
static double g_totalKvWriteMs;
static long long g_searchEdgesTotal;
static int g_searchVisitedTotal;

/* Search-specific stats (accumulated across all diskAnnSearch calls) */
static int g_queryCount = 0;
static double g_queryTotalMs = 0;       /* total wall-clock time */
static double g_queryGraphMs = 0;       /* graph traversal (diskAnnSearchInternal) */
static double g_queryResultMs = 0;      /* result collection */
static double g_queryKvReadMs = 0;      /* KV read I/O during search only */
static int g_queryKvReads = 0;          /* KV read count during search only */
static int g_queryNodesVisited = 0;     /* total nodes visited across all queries */
static long long g_queryEdgesExamined = 0; /* total edges examined */
static double g_queryDistanceMs = 0;    /* distance computation time */
static double g_buildDistanceMs = 0;    /* distance computation during build */

/* Auto-compaction timing globals from lsm_sorted.c */
extern double g_autoworkTotalMs;
static int g_ioTimingEnabled = -1;

static int diskAnnIoTimingEnabled(void){
  if( g_ioTimingEnabled < 0 ){
    const char *zEnv = getenv("DISKANN_IO_TIMING");
    g_ioTimingEnabled = (zEnv && zEnv[0] && zEnv[0] != '0') ? 1 : 0;
  }
  return g_ioTimingEnabled;
}

// #define SQLITE4_VECTOR_TRACE
#if defined(SQLITE4_DEBUG) && defined(SQLITE4_VECTOR_TRACE)
#define DiskAnnTrace(X) sqlite4DebugPrintf X;
#else
#define DiskAnnTrace(X)
#endif

/* limit to the sql part which we render in order to perform operations with shadow table */
#define DISKANN_SQL_RENDER_LIMIT 128

/* limit to the maximum size of DiskANN block (128 MB) */
#define DISKANN_MAX_BLOCK_SZ 134217728

/*
 * Due to historical reasons parameter for index block size were stored as u16 value and divided by 512 (2^9)
 * So, we will make inverse transform before initializing index from stored parameters
*/
#define DISKANN_BLOCK_SIZE_SHIFT 9


typedef struct VectorPair VectorPair;
typedef struct DiskAnnSearchCtx DiskAnnSearchCtx;
typedef struct DiskAnnNode DiskAnnNode;

/* VectorPair represents single vector where pNode is an exact representation and pEdge - compressed representation */
struct VectorPair {
  int nodeType;
  int edgeType;
  Vector *pNode;
  Vector *pEdge;
};

/* DiskAnnNode represents single node in the DiskAnn graph */
struct DiskAnnNode {
  u64 nRowid;           /* node id */
  int visited;          /* is this node visited? */
  DiskAnnNode *pNext;   /* next node in the visited list */
  BlobSpot *pBlobSpot;  /* reference to the blob with node data */
};

/*
 * DiskAnnSearchCtx stores information required for search operation to succeed
*/
struct DiskAnnSearchCtx {
  VectorPair query;             /* initial query vector */
  DiskAnnNode **aCandidates;    /* array of unvisited candidates ordered by distance */
  float *aDistances;            /* array of distances to the query vector */
  unsigned int nCandidates;     /* current size of aCandidates/aDistances arrays */
  unsigned int maxCandidates;   /* max size of aCandidates/aDistances arrays */
  DiskAnnNode **aTopCandidates; /* top candidates with exact distance calculated */
  float *aTopDistances;         /* top candidates exact distances */
  int nTopCandidates;           /* current size of aTopCandidates/aTopDistances arrays */
  int maxTopCandidates;         /* max size of aTopCandidates/aTopDistances arrays */
  DiskAnnNode *visitedList;     /* list of all visited candidates */
  unsigned int nUnvisited;      /* amount of unvisited candidates in the aCandidates array */
  int blobMode;                 /* DISKANN_BLOB_READONLY or DISKANN_BLOB_WRITABLE */
};

/**************************************************************************
** Serialization utilities
**************************************************************************/

static inline u16 readLE16(const unsigned char *p){
  return (u16)p[0] | (u16)p[1] << 8;
}

static inline u32 readLE32(const unsigned char *p){
  return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

static inline u64 readLE64(const unsigned char *p){
  return (u64)p[0]
       | (u64)p[1] << 8
       | (u64)p[2] << 16
       | (u64)p[3] << 24
       | (u64)p[4] << 32
       | (u64)p[5] << 40
       | (u64)p[6] << 48
       | (u64)p[7] << 56;
}

static inline void writeLE16(unsigned char *p, u16 v){
  p[0] = v;
  p[1] = v >> 8;
}

static inline void writeLE32(unsigned char *p, u32 v){
  p[0] = v;
  p[1] = v >> 8;
  p[2] = v >> 16;
  p[3] = v >> 24;
}

static inline void writeLE64(unsigned char *p, u64 v){
  p[0] = v;
  p[1] = v >> 8;
  p[2] = v >> 16;
  p[3] = v >> 24;
  p[4] = v >> 32;
  p[5] = v >> 40;
  p[6] = v >> 48;
  p[7] = v >> 56;
}

/**************************************************************************
** BlobSpot utilities — Direct KV store access
**
** In sqlite4, there is no incremental blob API (sqlite4_blob_*).
** BlobSpot uses direct KV store access to bypass VDBE overhead:
**  - Read:  fresh KV cursor → seek → data extraction → close
**  - Write: KV store replace with encoded data record
**
** KV key format:  varint(shadow_tnum) + encoded_int_key(rowid)
** KV value format: sqlite4 data record (2-column: integer + blob)
**   = varint(hdr_size) + byte(int_type) + varint(blob_type)
**     + int_bytes(big-endian) + raw_blob_bytes
**
** CRITICAL: sqlite4KVCursorSeek with dir=0 returns SQLITE4_INEXACT when
** the exact key is not found (cursor lands on a nearby key). This MUST
** be treated as NOTFOUND — reading the wrong row corrupts the graph.
**************************************************************************/

/* Forward declarations — defined in vdbecodec.c, declared in vdbeInt.h */
int sqlite4VdbeEncodeIntKey(u8 *aBuf, sqlite4_int64 v);
int sqlite4VdbeDecodeNumericKey(const KVByteArray*, KVSize, sqlite4_num*);

/*
** Build a KV key for a shadow table row:
**   key = pIndex->aKeyPrefix (varint-encoded tnum) + encoded_int_key(rowid)
*/
static int blobSpotBuildKey(const DiskAnnIndex *pIndex, i64 nRowid, u8 *aKey){
  int n = pIndex->nKeyPrefix;
  memcpy(aKey, pIndex->aKeyPrefix, n);
  n += sqlite4VdbeEncodeIntKey(aKey + n, nRowid);
  return n;
}

/*
** Decode a sqlite4 data record to extract the BLOB column (column 1).
** The shadow table has 2 columns: (index_key INTEGER PK, data BLOB).
** The VDBE encodes ALL columns in the data record:
**   varint(hdr_size) + type_code_col0 + type_code_col1 + payload_col0 + payload_col1
** Returns pointer to blob data within pData, and sets *pnBlob to its size.
*/
static const u8 *blobSpotDecodeRecord(const u8 *pData, int nData, int *pnBlob){
  u64 hdrSize, typeCode;
  int nHdr, n, nBlob;
  int payloadOff;
  int col0Size;

  nHdr = sqlite4GetVarint64(pData, nData, &hdrSize);
  if( nHdr <= 0 || nHdr + (int)hdrSize > nData ) return 0;

  payloadOff = nHdr + (int)hdrSize;

  /* Column 0 type code (integer) */
  n = sqlite4GetVarint64(pData + nHdr, nData - nHdr, &typeCode);
  if( n <= 0 ) return 0;

  if( typeCode == 0 ){
    col0Size = 0;
  }else if( typeCode >= 2 && typeCode <= 10 ){
    col0Size = (int)(typeCode - 2);
  }else{
    return 0;
  }

  /* Column 1 type code (blob) */
  {
    int n2;
    u64 blobTypeCode;
    n2 = sqlite4GetVarint64(pData + nHdr + n, nData - nHdr - n, &blobTypeCode);
    if( n2 <= 0 ) return 0;
    if( blobTypeCode < 23 || (blobTypeCode & 3) != 3 ) return 0;

    nBlob = (int)((blobTypeCode - 23) / 4);
    *pnBlob = nBlob;
    return pData + payloadOff + col0Size;
  }
}

/*
** Encode a 2-column sqlite4 data record (INTEGER + BLOB).
** Format matches what sqlite4VdbeEncodeData produces.
** Caller must provide aOut with at least (30 + nBlob) bytes.
** Returns total encoded size.
*/
static int blobSpotEncodeRecord(i64 nRowid, const u8 *pBlob, int nBlob, u8 *aOut){
  u8 aHdrBuf[18];
  int nHdrContent;
  int nHdrSizeVarint;
  int nn;
  int nIntBytes;

  /* significantBytes — matches sqlite4VdbeEncodeData */
  {
    i64 v = nRowid;
    i64 x;
    nIntBytes = 1;
    if( v < 0 ){
      x = -128;
      while( v < x && nIntBytes < 8 ){ nIntBytes++; x *= 256; }
    }else{
      x = 127;
      while( v > x && nIntBytes < 8 ){ nIntBytes++; x *= 256; }
    }
  }

  /* Header: int type code + blob type code */
  {
    int p = 0;
    aHdrBuf[p++] = (u8)(nIntBytes + 2);
    p += sqlite4PutVarint64(aHdrBuf + p, (u64)(23 + 4*(i64)nBlob));
    nHdrContent = p;
  }

  nHdrSizeVarint = sqlite4PutVarint64(aOut, (u64)nHdrContent);
  nn = nHdrSizeVarint;
  memcpy(aOut + nn, aHdrBuf, nHdrContent);
  nn += nHdrContent;

  /* Integer payload (big-endian) */
  {
    int k = nIntBytes;
    i64 v = nRowid;
    aOut[nn + (--k)] = v & 0xff;
    while( k ){
      v >>= 8;
      aOut[nn + (--k)] = v & 0xff;
    }
    nn += nIntBytes;
  }

  memcpy(aOut + nn, pBlob, nBlob);
  nn += nBlob;
  return nn;
}

int blobSpotCreate(const DiskAnnIndex *pIndex, BlobSpot **ppBlobSpot,
                   u64 nRowid, int nBufferSize, int isWritable) {
  BlobSpot *pBlobSpot;
  u8 *pBuffer;
  sqlite4_env *pEnv = pIndex->db->pEnv;

  DiskAnnTrace(("blob spot created: rowid=%lld, isWritable=%d\n", nRowid, isWritable));
  assert( nBufferSize > 0 );

  pBlobSpot = sqlite4_malloc(pEnv, sizeof(BlobSpot));
  if( pBlobSpot == NULL ){
    return SQLITE4_NOMEM;
  }

  pBuffer = sqlite4_malloc(pEnv, nBufferSize);
  if( pBuffer == NULL ){
    sqlite4_free(pEnv, pBlobSpot);
    return SQLITE4_NOMEM;
  }

  pBlobSpot->nRowid = nRowid;
  pBlobSpot->pBuffer = pBuffer;
  pBlobSpot->nBufferSize = nBufferSize;
  pBlobSpot->isWritable = isWritable;
  pBlobSpot->isInitialized = 0;
  pBlobSpot->isAborted = 0;

  *ppBlobSpot = pBlobSpot;
  return SQLITE4_OK;
}

int blobSpotReload(DiskAnnIndex *pIndex, BlobSpot *pBlobSpot,
                   u64 nRowid, int nBufferSize) {
  int rc;
  u8 aKey[32];
  int nKey;
  const KVByteArray *pData;
  KVSize nData;

  DiskAnnTrace(("blob spot reload: rowid=%lld\n", nRowid));
  assert( pBlobSpot != NULL );
  assert( pBlobSpot->nBufferSize == nBufferSize );

  if( pBlobSpot->nRowid == nRowid && pBlobSpot->isInitialized ){
    return SQLITE4_OK;
  }

  if( pBlobSpot->isAborted ){
    pBlobSpot->isAborted = 0;
    pBlobSpot->isInitialized = 0;
  }

  pBlobSpot->nRowid = nRowid;
  pBlobSpot->isInitialized = 0;

  nKey = blobSpotBuildKey(pIndex, (i64)nRowid, aKey);

  {
    struct timespec _kvr0, _kvr1;
    int doTiming = diskAnnIoTimingEnabled();
    if( doTiming ) clock_gettime(CLOCK_MONOTONIC, &_kvr0);

    /* Lazily open persistent read cursor on first use */
    if( pIndex->pReadCsr == NULL ){
      rc = sqlite4KVStoreOpenCursor(pIndex->db->aDb[0].pKV, &pIndex->pReadCsr);
      if( rc != SQLITE4_OK ) goto abort;
    }

    rc = sqlite4KVCursorSeek(pIndex->pReadCsr, aKey, nKey, 0);
    if( rc == SQLITE4_NOTFOUND || rc == SQLITE4_INEXACT ){
      return DISKANN_ROW_NOT_FOUND;
    }
    if( rc != SQLITE4_OK ) goto abort;

    rc = sqlite4KVCursorData(pIndex->pReadCsr, 0, -1, &pData, &nData);
    if( rc != SQLITE4_OK ) goto abort;

    {
      int nBlob = 0;
      const u8 *pBlob = blobSpotDecodeRecord((const u8*)pData, (int)nData, &nBlob);
      if( pBlob == NULL || nBlob < nBufferSize ){
        rc = SQLITE4_ERROR;
        goto abort;
      }
      memcpy(pBlobSpot->pBuffer, pBlob, nBufferSize);
    }

    if( doTiming ){
      clock_gettime(CLOCK_MONOTONIC, &_kvr1);
      g_totalKvReadMs += (_kvr1.tv_sec - _kvr0.tv_sec)*1000.0
                       + (_kvr1.tv_nsec - _kvr0.tv_nsec)/1e6;
    }
  }

  pIndex->nReads++;
  pBlobSpot->isInitialized = 1;
  return SQLITE4_OK;

abort:
  pBlobSpot->isAborted = 1;
  pBlobSpot->isInitialized = 0;
  return (rc == SQLITE4_OK) ? SQLITE4_ERROR : rc;
}

int blobSpotFlush(DiskAnnIndex *pIndex, BlobSpot *pBlobSpot) {
  int rc;
  u8 aKey[32];
  int nKey;
  u8 *aRec;
  int nRec;
  sqlite4_env *pEnv = pIndex->db->pEnv;
  struct timespec _kvw0, _kvw1;

  nKey = blobSpotBuildKey(pIndex, (i64)pBlobSpot->nRowid, aKey);

  aRec = sqlite4_malloc(pEnv, pBlobSpot->nBufferSize + 40);
  if( aRec == NULL ) return SQLITE4_NOMEM;

  nRec = blobSpotEncodeRecord((i64)pBlobSpot->nRowid, pBlobSpot->pBuffer,
                              pBlobSpot->nBufferSize, aRec);

  if( diskAnnIoTimingEnabled() ) clock_gettime(CLOCK_MONOTONIC, &_kvw0);
  rc = sqlite4KVStoreReplace(pIndex->db->aDb[0].pKV, aKey, nKey, aRec, nRec);
  if( diskAnnIoTimingEnabled() ){
    clock_gettime(CLOCK_MONOTONIC, &_kvw1);
    g_totalKvWriteMs += (_kvw1.tv_sec - _kvw0.tv_sec)*1000.0
                      + (_kvw1.tv_nsec - _kvw0.tv_nsec)/1e6;
  }

  sqlite4_free(pEnv, aRec);

  if( rc != SQLITE4_OK ) return rc;
  pIndex->nWrites++;
  return SQLITE4_OK;
}

void blobSpotFree(BlobSpot *pBlobSpot) {
  sqlite4_env *pEnv = sqlite4_env_default();
  if( pBlobSpot == NULL ) return;
  if( pBlobSpot->pBuffer != NULL ){
    sqlite4_free(pEnv, pBlobSpot->pBuffer);
  }
  sqlite4_free(pEnv, pBlobSpot);
}

/**************************************************************************
** Layout specific utilities
**************************************************************************/

int nodeMetadataSize(int nFormatVersion){
  if( nFormatVersion <= VECTOR_FORMAT_V2 ){
    return (sizeof(u64) + sizeof(u16));
  }else{
    return (sizeof(u64) + sizeof(u64));
  }
}

int edgeMetadataSize(int nFormatVersion){
  return (sizeof(u64) + sizeof(u64));
}

int nodeEdgeOverhead(int nFormatVersion, int nEdgeVectorSize){
  return nEdgeVectorSize + edgeMetadataSize(nFormatVersion);
}

int nodeOverhead(int nFormatVersion, int nNodeVectorSize){
  return nNodeVectorSize + nodeMetadataSize(nFormatVersion);
}

int nodeEdgesMaxCount(const DiskAnnIndex *pIndex){
  unsigned int nMaxEdges = (pIndex->nBlockSize - nodeOverhead(pIndex->nFormatVersion, pIndex->nNodeVectorSize)) / nodeEdgeOverhead(pIndex->nFormatVersion, pIndex->nEdgeVectorSize);
  assert( nMaxEdges > 0);
  return nMaxEdges;
}

int nodeEdgesMetadataOffset(const DiskAnnIndex *pIndex){
  unsigned int offset;
  unsigned int nMaxEdges = nodeEdgesMaxCount(pIndex);
  offset = nodeMetadataSize(pIndex->nFormatVersion) + pIndex->nNodeVectorSize + nMaxEdges * pIndex->nEdgeVectorSize;
  assert( offset <= (unsigned int)pIndex->nBlockSize );
  return offset;
}

void nodeBinInit(const DiskAnnIndex *pIndex, BlobSpot *pBlobSpot, u64 nRowid, Vector *pVector){
  assert( nodeMetadataSize(pIndex->nFormatVersion) + pIndex->nNodeVectorSize <= pBlobSpot->nBufferSize );

  memset(pBlobSpot->pBuffer, 0, pBlobSpot->nBufferSize);
  writeLE64(pBlobSpot->pBuffer, nRowid);
  /* neighbours count already zero after memset */

  vectorSerializeToBlob(pVector, pBlobSpot->pBuffer + nodeMetadataSize(pIndex->nFormatVersion), pIndex->nNodeVectorSize);
}

void nodeBinVector(const DiskAnnIndex *pIndex, const BlobSpot *pBlobSpot, Vector *pVector) {
  assert( nodeMetadataSize(pIndex->nFormatVersion) + pIndex->nNodeVectorSize <= pBlobSpot->nBufferSize );

  vectorInitStatic(pVector, pIndex->nNodeVectorType, pIndex->nVectorDims, pBlobSpot->pBuffer + nodeMetadataSize(pIndex->nFormatVersion));
}

u16 nodeBinEdges(const DiskAnnIndex *pIndex, const BlobSpot *pBlobSpot) {
  assert( nodeMetadataSize(pIndex->nFormatVersion) <= pBlobSpot->nBufferSize );

  return readLE16(pBlobSpot->pBuffer + sizeof(u64));
}

void nodeBinEdge(const DiskAnnIndex *pIndex, const BlobSpot *pBlobSpot, int iEdge, u64 *pRowid, float *pDistance, Vector *pVector) {
  u32 distance;
  int offset = nodeEdgesMetadataOffset(pIndex);

  if( pRowid != NULL ){
    assert( offset + (iEdge + 1) * edgeMetadataSize(pIndex->nFormatVersion) <= pBlobSpot->nBufferSize );
    *pRowid = readLE64(pBlobSpot->pBuffer + offset + iEdge * edgeMetadataSize(pIndex->nFormatVersion) + sizeof(u64));
  }
  if( pIndex->nFormatVersion != VECTOR_FORMAT_V1 && pDistance != NULL ){
    distance = readLE32(pBlobSpot->pBuffer + offset + iEdge * edgeMetadataSize(pIndex->nFormatVersion) + sizeof(u32));
    *pDistance = *((float*)&distance);
  }
  if( pVector != NULL ){
    assert( nodeMetadataSize(pIndex->nFormatVersion) + pIndex->nNodeVectorSize + iEdge * pIndex->nEdgeVectorSize < offset );
    vectorInitStatic(
      pVector,
      pIndex->nEdgeVectorType,
      pIndex->nVectorDims,
      pBlobSpot->pBuffer + nodeMetadataSize(pIndex->nFormatVersion) + pIndex->nNodeVectorSize + iEdge * pIndex->nEdgeVectorSize
    );
  }
}

int nodeBinEdgeFindIdx(const DiskAnnIndex *pIndex, const BlobSpot *pBlobSpot, u64 nRowid) {
  int i, nEdges = nodeBinEdges(pIndex, pBlobSpot);
  for(i = 0; i < nEdges; i++){
    u64 edgeId;
    nodeBinEdge(pIndex, pBlobSpot, i, &edgeId, NULL, NULL);
    if( edgeId == nRowid ){
      return i;
    }
  }
  return -1;
}

void nodeBinPruneEdges(const DiskAnnIndex *pIndex, BlobSpot *pBlobSpot, int nPruned) {
  assert( 0 <= nPruned && nPruned <= nodeBinEdges(pIndex, pBlobSpot) );

  writeLE16(pBlobSpot->pBuffer + sizeof(u64), nPruned);
}

/* replace edge at position iReplace or add new one if iReplace == nEdges */
void nodeBinReplaceEdge(const DiskAnnIndex *pIndex, BlobSpot *pBlobSpot, int iReplace, u64 nRowid, float distance, Vector *pVector) {
  int nMaxEdges = nodeEdgesMaxCount(pIndex);
  int nEdges = nodeBinEdges(pIndex, pBlobSpot);
  int edgeVectorOffset, edgeMetaOffset;

  assert( 0 <= iReplace && iReplace < nMaxEdges );
  assert( 0 <= iReplace && iReplace <= nEdges );

  if( iReplace == nEdges ){
    nEdges++;
  }

  edgeVectorOffset = nodeMetadataSize(pIndex->nFormatVersion) + pIndex->nNodeVectorSize + iReplace * pIndex->nEdgeVectorSize;
  edgeMetaOffset = nodeEdgesMetadataOffset(pIndex) + iReplace * edgeMetadataSize(pIndex->nFormatVersion);

  assert( edgeVectorOffset + pIndex->nEdgeVectorSize <= pBlobSpot->nBufferSize );
  assert( edgeMetaOffset + edgeMetadataSize(pIndex->nFormatVersion) <= pBlobSpot->nBufferSize );

  vectorSerializeToBlob(pVector, pBlobSpot->pBuffer + edgeVectorOffset, pIndex->nEdgeVectorSize);
  writeLE32(pBlobSpot->pBuffer + edgeMetaOffset + sizeof(u32), *((u32*)&distance));
  writeLE64(pBlobSpot->pBuffer + edgeMetaOffset + sizeof(u64), nRowid);

  writeLE16(pBlobSpot->pBuffer + sizeof(u64), nEdges);
}

/* delete edge at position iDelete by swapping it with the last edge */
void nodeBinDeleteEdge(const DiskAnnIndex *pIndex, BlobSpot *pBlobSpot, int iDelete) {
  int nEdges = nodeBinEdges(pIndex, pBlobSpot);
  int edgeVectorOffset, edgeMetaOffset, lastVectorOffset, lastMetaOffset;

  assert( 0 <= iDelete && iDelete < nEdges );

  edgeVectorOffset = nodeMetadataSize(pIndex->nFormatVersion) + pIndex->nNodeVectorSize + iDelete * pIndex->nEdgeVectorSize;
  lastVectorOffset = nodeMetadataSize(pIndex->nFormatVersion) + pIndex->nNodeVectorSize + (nEdges - 1) * pIndex->nEdgeVectorSize;
  edgeMetaOffset = nodeEdgesMetadataOffset(pIndex) + iDelete * edgeMetadataSize(pIndex->nFormatVersion);
  lastMetaOffset = nodeEdgesMetadataOffset(pIndex) + (nEdges - 1) * edgeMetadataSize(pIndex->nFormatVersion);

  assert( edgeVectorOffset + pIndex->nEdgeVectorSize <= pBlobSpot->nBufferSize );
  assert( lastVectorOffset + pIndex->nEdgeVectorSize <= pBlobSpot->nBufferSize );
  assert( edgeMetaOffset + edgeMetadataSize(pIndex->nFormatVersion) <= pBlobSpot->nBufferSize );
  assert( lastMetaOffset + edgeMetadataSize(pIndex->nFormatVersion) <= pBlobSpot->nBufferSize );

  if( edgeVectorOffset < lastVectorOffset ){
    memmove(pBlobSpot->pBuffer + edgeVectorOffset, pBlobSpot->pBuffer + lastVectorOffset, pIndex->nEdgeVectorSize);
    memmove(pBlobSpot->pBuffer + edgeMetaOffset, pBlobSpot->pBuffer + lastMetaOffset, edgeMetadataSize(pIndex->nFormatVersion));
  }

  writeLE16(pBlobSpot->pBuffer + sizeof(u64), nEdges - 1);
}

void nodeBinDebug(const DiskAnnIndex *pIndex, const BlobSpot *pBlobSpot) {
#if defined(SQLITE4_DEBUG) && defined(SQLITE4_VECTOR_TRACE)
  int nEdges, nMaxEdges, i;
  u64 nRowid;
  float distance = 0;
  Vector vector;

  nEdges = nodeBinEdges(pIndex, pBlobSpot);
  nMaxEdges = nodeEdgesMaxCount(pIndex);
  nodeBinVector(pIndex, pBlobSpot, &vector);

  DiskAnnTrace(("debug blob content for root=%lld (buffer size=%d)\n", pBlobSpot->nRowid, pBlobSpot->nBufferSize));
  DiskAnnTrace(("  nEdges=%d, nMaxEdges=%d, vector=", nEdges, nMaxEdges));
  vectorDump(&vector);
  for(i = 0; i < nEdges; i++){
    nodeBinEdge(pIndex, pBlobSpot, i, &nRowid, &distance, &vector);
    DiskAnnTrace(("  to=%lld, distance=%f, vector=", nRowid, distance));
    vectorDump(&vector);
  }
#endif
}

/*******************************************************************************
** DiskANN shadow index operations
********************************************************************************/

int diskAnnCreateIndex(
  sqlite4 *db,
  const char *zDbSName,
  const char *zIdxName,
  const VectorIdxKey *pKey,
  VectorIdxParams *pParams,
  const char **pzErrMsg
){
  int rc;
  int type, dims, metric, neighbours;
  u64 maxNeighborsParam, blockSizeBytes;
  char *zSql;
  /* Render column defs and names - for rowid-like case only "index_key INTEGER" */
  char columnSqlDefs[VECTOR_INDEX_SQL_RENDER_LIMIT];
  char columnSqlNames[VECTOR_INDEX_SQL_RENDER_LIMIT];

  if( vectorIdxKeyDefsRender(pKey, "index_key", columnSqlDefs, sizeof(columnSqlDefs)) != 0 ){
    return SQLITE4_ERROR;
  }
  if( vectorIdxKeyNamesRender(pKey->nKeyColumns, "index_key", columnSqlNames, sizeof(columnSqlNames)) != 0 ){
    return SQLITE4_ERROR;
  }
  if( vectorIdxParamsPutU64(pParams, VECTOR_INDEX_TYPE_PARAM_ID, VECTOR_INDEX_TYPE_DISKANN) != 0 ){
    return SQLITE4_ERROR;
  }
  type = vectorIdxParamsGetU64(pParams, VECTOR_TYPE_PARAM_ID);
  if( type == 0 ){
    return SQLITE4_ERROR;
  }
  dims = vectorIdxParamsGetU64(pParams, VECTOR_DIM_PARAM_ID);
  if( dims == 0 ){
    return SQLITE4_ERROR;
  }
  assert( 0 < dims && dims <= MAX_VECTOR_SZ );

  metric = vectorIdxParamsGetU64(pParams, VECTOR_METRIC_TYPE_PARAM_ID);
  if( metric == 0 ){
    metric = VECTOR_METRIC_TYPE_COS;
    if( vectorIdxParamsPutU64(pParams, VECTOR_METRIC_TYPE_PARAM_ID, metric) != 0 ){
      return SQLITE4_ERROR;
    }
  }
  neighbours = vectorIdxParamsGetU64(pParams, VECTOR_COMPRESS_NEIGHBORS_PARAM_ID);
  if( neighbours == VECTOR_TYPE_FLOAT1BIT && metric != VECTOR_METRIC_TYPE_COS ){
    *pzErrMsg = "1-bit compression available only for cosine metric";
    return SQLITE4_ERROR;
  }
  if( neighbours == 0 ){
    neighbours = type;
  }

  maxNeighborsParam = vectorIdxParamsGetU64(pParams, VECTOR_MAX_NEIGHBORS_PARAM_ID);
  if( maxNeighborsParam == 0 ){
    maxNeighborsParam = MIN(3 * ((int)(sqrt(dims)) + 1), (50 * nodeOverhead(VECTOR_FORMAT_DEFAULT, vectorDataSize(type, dims))) / nodeEdgeOverhead(VECTOR_FORMAT_DEFAULT, vectorDataSize(neighbours, dims)) + 1);
  }
  blockSizeBytes = nodeOverhead(VECTOR_FORMAT_DEFAULT, vectorDataSize(type, dims)) + maxNeighborsParam * (u64)nodeEdgeOverhead(VECTOR_FORMAT_DEFAULT, vectorDataSize(neighbours, dims));
  if( blockSizeBytes > DISKANN_MAX_BLOCK_SZ ){
    return SQLITE4_ERROR;
  }
  if( vectorIdxParamsPutU64(pParams, VECTOR_BLOCK_SIZE_PARAM_ID, MAX(256, blockSizeBytes)) != 0 ){
    return SQLITE4_ERROR;
  }

  if( vectorIdxParamsGetF64(pParams, VECTOR_PRUNING_ALPHA_PARAM_ID) == 0 ){
    if( vectorIdxParamsPutF64(pParams, VECTOR_PRUNING_ALPHA_PARAM_ID, VECTOR_PRUNING_ALPHA_DEFAULT) != 0 ){
      return SQLITE4_ERROR;
    }
  }
  if( vectorIdxParamsGetU64(pParams, VECTOR_INSERT_L_PARAM_ID) == 0 ){
    if( vectorIdxParamsPutU64(pParams, VECTOR_INSERT_L_PARAM_ID, VECTOR_INSERT_L_DEFAULT) != 0 ){
      return SQLITE4_ERROR;
    }
  }
  if( vectorIdxParamsGetU64(pParams, VECTOR_SEARCH_L_PARAM_ID) == 0 ){
    if( vectorIdxParamsPutU64(pParams, VECTOR_SEARCH_L_PARAM_ID, VECTOR_SEARCH_L_DEFAULT) != 0 ){
      return SQLITE4_ERROR;
    }
  }

  /* For rowid-like key: shadow table uses index_key as primary key */
  if( vectorIdxKeyRowidLike(pKey) ){
    zSql = sqlite4MPrintf(
        db,
        "CREATE TABLE IF NOT EXISTS \"%w\".%s_shadow (%s, data BLOB, PRIMARY KEY (%s))",
        zDbSName,
        zIdxName,
        columnSqlDefs,
        columnSqlNames
        );
  }else{
    /* Non-rowid-like keys: not officially supported in sqlite4 port, but create structure anyway */
    zSql = sqlite4MPrintf(
        db,
        "CREATE TABLE IF NOT EXISTS \"%w\".%s_shadow (rowid INTEGER PRIMARY KEY, %s, data BLOB, UNIQUE (%s))",
        zDbSName,
        zIdxName,
        columnSqlDefs,
        columnSqlNames
        );
  }
  if( zSql == NULL ){
    return SQLITE4_NOMEM;
  }
  rc = sqlite4_exec(db, zSql, 0, 0);
  sqlite4DbFree(db, zSql);
  if( rc != SQLITE4_OK ){
    return rc;
  }

  /* Create an index on index_key for efficient random row selection */
  zSql = sqlite4MPrintf(
      db,
      "CREATE INDEX IF NOT EXISTS \"%w\".%s_shadow_idx ON %s_shadow (%s)",
      zDbSName,
      zIdxName,
      zIdxName,
      columnSqlNames
  );
  if( zSql == NULL ){
    return SQLITE4_NOMEM;
  }
  rc = sqlite4_exec(db, zSql, 0, 0);
  sqlite4DbFree(db, zSql);
  return rc;
}

int diskAnnClearIndex(sqlite4 *db, const char *zDbSName, const char *zIdxName) {
  char *zSql = sqlite4MPrintf(db, "DELETE FROM \"%w\".%s_shadow", zDbSName, zIdxName);
  int rc;
  if( zSql == NULL ) return SQLITE4_NOMEM;
  rc = sqlite4_exec(db, zSql, 0, 0);
  sqlite4DbFree(db, zSql);
  return rc;
}

int diskAnnDropIndex(sqlite4 *db, const char *zDbSName, const char *zIdxName){
  char *zSql = sqlite4MPrintf(db, "DROP TABLE IF EXISTS \"%w\".%s_shadow", zDbSName, zIdxName);
  int rc;
  if( zSql == NULL ) return SQLITE4_NOMEM;
  rc = sqlite4_exec(db, zSql, 0, 0);
  sqlite4DbFree(db, zSql);
  return rc;
}

/*
 * Select random row from the shadow table using direct KV access.
 * Seeks to the shadow table's key prefix and picks the first row found.
 * Returns SQLITE4_DONE if no row found (table is empty).
*/
static int diskAnnSelectRandomShadowRow(DiskAnnIndex *pIndex, u64 *pRowid){
  int rc;
  KVCursor *pCsr = NULL;
  const KVByteArray *pKey;
  KVSize nKey;

  if( pIndex->nShadowRows <= 0 ){
    /* No rows known yet — try seeking to first row to check if table is empty */
    rc = sqlite4KVStoreOpenCursor(pIndex->db->aDb[0].pKV, &pCsr);
    if( rc != SQLITE4_OK ) return rc;

    rc = sqlite4KVCursorSeek(pCsr, pIndex->aKeyPrefix, pIndex->nKeyPrefix, 1);
    if( rc == SQLITE4_NOTFOUND ){
      sqlite4KVCursorClose(pCsr);
      return SQLITE4_DONE;
    }
    if( rc != SQLITE4_OK && rc != SQLITE4_INEXACT ){
      sqlite4KVCursorClose(pCsr);
      return rc;
    }

    rc = sqlite4KVCursorKey(pCsr, &pKey, &nKey);
    if( rc != SQLITE4_OK ){
      sqlite4KVCursorClose(pCsr);
      return rc;
    }
    if( nKey <= pIndex->nKeyPrefix ||
        memcmp(pKey, pIndex->aKeyPrefix, pIndex->nKeyPrefix) != 0 ){
      sqlite4KVCursorClose(pCsr);
      return SQLITE4_DONE;
    }

    /* Decode the first rowid as fallback */
    {
      sqlite4_num num;
      int decRc = sqlite4VdbeDecodeNumericKey(
          pKey + pIndex->nKeyPrefix,
          nKey - pIndex->nKeyPrefix,
          &num);
      if( decRc <= 0 ){
        sqlite4KVCursorClose(pCsr);
        return SQLITE4_ERROR;
      }
      *pRowid = (u64)sqlite4_num_to_int64(num, 0);
    }
    sqlite4KVCursorClose(pCsr);
    return SQLITE4_OK;
  }

  /* Pick a random rowid in [1, nShadowRows] and seek to nearest existing row */
  {
    u64 randVal;
    u8 aKey[32];
    int nKey2;
    i64 targetRowid;

    sqlite4_randomness(pIndex->db->pEnv, sizeof(randVal), &randVal);
    targetRowid = (i64)(randVal % (u64)pIndex->nShadowRows) + 1;

    nKey2 = blobSpotBuildKey(pIndex, targetRowid, aKey);

    rc = sqlite4KVStoreOpenCursor(pIndex->db->aDb[0].pKV, &pCsr);
    if( rc != SQLITE4_OK ) return rc;

    /* Seek to nearest key >= target (dir=1) */
    rc = sqlite4KVCursorSeek(pCsr, aKey, nKey2, 1);
    if( rc == SQLITE4_NOTFOUND ){
      /* Overshot past end — wrap to first row */
      rc = sqlite4KVCursorSeek(pCsr, pIndex->aKeyPrefix, pIndex->nKeyPrefix, 1);
    }
    if( rc != SQLITE4_OK && rc != SQLITE4_INEXACT ){
      sqlite4KVCursorClose(pCsr);
      return rc;
    }

    rc = sqlite4KVCursorKey(pCsr, &pKey, &nKey);
    if( rc != SQLITE4_OK ){
      sqlite4KVCursorClose(pCsr);
      return rc;
    }

    /* Verify it belongs to this shadow table */
    if( nKey <= pIndex->nKeyPrefix ||
        memcmp(pKey, pIndex->aKeyPrefix, pIndex->nKeyPrefix) != 0 ){
      /* Wrapped past shadow table — seek back to first row */
      rc = sqlite4KVCursorSeek(pCsr, pIndex->aKeyPrefix, pIndex->nKeyPrefix, 1);
      if( rc == SQLITE4_NOTFOUND ){
        sqlite4KVCursorClose(pCsr);
        return SQLITE4_DONE;
      }
      if( rc != SQLITE4_OK && rc != SQLITE4_INEXACT ){
        sqlite4KVCursorClose(pCsr);
        return rc;
      }
      rc = sqlite4KVCursorKey(pCsr, &pKey, &nKey);
      if( rc != SQLITE4_OK ){
        sqlite4KVCursorClose(pCsr);
        return rc;
      }
    }

    /* Decode the rowid */
    {
      sqlite4_num num;
      int decRc = sqlite4VdbeDecodeNumericKey(
          pKey + pIndex->nKeyPrefix,
          nKey - pIndex->nKeyPrefix,
          &num);
      if( decRc <= 0 ){
        sqlite4KVCursorClose(pCsr);
        return SQLITE4_ERROR;
      }
      *pRowid = (u64)sqlite4_num_to_int64(num, 0);
    }

    sqlite4KVCursorClose(pCsr);
    return SQLITE4_OK;
  }
}

/*
 * Find row by keys from pInRow and set its rowid to pRowid
 * In sqlite4 port, only rowid-like keys are supported, so this uses index_key directly
*/
static int diskAnnGetShadowRowid(const DiskAnnIndex *pIndex, const VectorInRow *pInRow, u64 *pRowid) {
  /* For rowid-like keys, vectorInRowTryGetRowid always succeeds */
  return vectorInRowTryGetRowid(pInRow, pRowid);
}

/*
 * Find row keys by rowid and put them in pRows structure
 * Only needed for non-rowid-like key case (not used in sqlite4 rowid-only port)
*/
static int diskAnnGetShadowRowKeys(const DiskAnnIndex *pIndex, u64 nRowid, const VectorIdxKey *pKey, VectorOutRows *pRows, int iRow) {
  /* For rowid-like case, pRows->aIntValues is non-NULL and this function should not be called */
  assert( 0 );
  return SQLITE4_ERROR;
}

/*
 * Insert new empty row to the shadow table
 * For rowid-like keys: INSERT INTO shadow(index_key, data) VALUES (nRowid, zeroblob)
*/
static int diskAnnInsertShadowRow(const DiskAnnIndex *pIndex, const VectorInRow *pVectorInRow, u64 *pRowid){
  static int s_shadowInsertCount = 0;
  int rc;
  sqlite4_stmt *pStmt = NULL;
  char *zSql = NULL;
  u8 *pZero = NULL;
  sqlite4_env *pEnv = pIndex->db->pEnv;
  s_shadowInsertCount++;
  *pRowid = pVectorInRow->nRowid;

  zSql = sqlite4MPrintf(
      pIndex->db,
      "INSERT INTO \"%w\".%s(index_key, data) VALUES (?, ?)",
      pIndex->zDbSName, pIndex->zShadow
  );
  if( zSql == NULL ){
    rc = SQLITE4_NOMEM;
    goto out;
  }
  rc = sqlite4_prepare(pIndex->db, zSql, -1, &pStmt, NULL);
  sqlite4DbFree(pIndex->db, zSql);
  zSql = NULL;
  if( rc != SQLITE4_OK ){
    goto out;
  }

  rc = sqlite4_bind_int64(pStmt, 1, (i64)pVectorInRow->nRowid);
  if( rc != SQLITE4_OK ){
    goto out;
  }

  pZero = sqlite4_malloc(pEnv, pIndex->nBlockSize);
  if( pZero == NULL ){
    rc = SQLITE4_NOMEM;
    goto out;
  }
  memset(pZero, 0, pIndex->nBlockSize);
  rc = sqlite4_bind_blob(pStmt, 2, pZero, pIndex->nBlockSize, SQLITE4_TRANSIENT, 0);
  sqlite4_free(pEnv, pZero);
  pZero = NULL;
  if( rc != SQLITE4_OK ){
    goto out;
  }

  rc = sqlite4_step(pStmt);
  if( rc != SQLITE4_DONE ){
    rc = SQLITE4_ERROR;
    goto out;
  }
  rc = SQLITE4_OK;
out:
  if( pStmt != NULL ){
    sqlite4_finalize(pStmt);
  }
  return rc;
}

/*
 * Delete row from the shadow table
*/
static int diskAnnDeleteShadowRow(const DiskAnnIndex *pIndex, i64 nRowid){
  int rc;
  sqlite4_stmt *pStmt = NULL;
  char *zSql = sqlite4MPrintf(
      pIndex->db,
      "DELETE FROM \"%w\".%s WHERE index_key = ?",
      pIndex->zDbSName, pIndex->zShadow
  );
  if( zSql == NULL ){
    rc = SQLITE4_NOMEM;
    goto out;
  }
  rc = sqlite4_prepare(pIndex->db, zSql, -1, &pStmt, NULL);
  sqlite4DbFree(pIndex->db, zSql);
  zSql = NULL;
  if( rc != SQLITE4_OK ){
    goto out;
  }
  rc = sqlite4_bind_int64(pStmt, 1, nRowid);
  if( rc != SQLITE4_OK ){
    goto out;
  }
  rc = sqlite4_step(pStmt);
  if( rc != SQLITE4_DONE ){
    rc = SQLITE4_ERROR;
    goto out;
  }
  rc = SQLITE4_OK;
out:
  if( pStmt != NULL ){
    sqlite4_finalize(pStmt);
  }
  return rc;
}

/**************************************************************************
** Generic utilities
**************************************************************************/

int initVectorPair(int nodeType, int edgeType, int dims, VectorPair *pPair){
  pPair->nodeType = nodeType;
  pPair->edgeType = edgeType;
  pPair->pNode = NULL;
  pPair->pEdge = NULL;
  if( pPair->nodeType == pPair->edgeType ){
    return 0;
  }
  pPair->pEdge = vectorAlloc(edgeType, dims);
  if( pPair->pEdge == NULL ){
    return SQLITE4_NOMEM;
  }
  return 0;
}

void loadVectorPair(VectorPair *pPair, const Vector *pVector){
  pPair->pNode = (Vector*)pVector;
  if( pPair->edgeType != pPair->nodeType ){
    vectorConvert(pPair->pNode, pPair->pEdge);
  }else{
    pPair->pEdge = pPair->pNode;
  }
}

void deinitVectorPair(VectorPair *pPair) {
  if( pPair->pEdge != NULL && pPair->pNode != pPair->pEdge ){
    vectorFree(pPair->pEdge);
  }
}

int distanceBufferInsertIdx(const float *aDistances, int nSize, int nMaxSize, float distance){
  int i;
#ifdef SQLITE4_DEBUG
  for(i = 0; i < nSize - 1; i++){
    assert(aDistances[i] <= aDistances[i + 1]);
  }
#endif
  for(i = 0; i < nSize; i++){
    if( distance < aDistances[i] ){
      return i;
    }
  }
  return nSize < nMaxSize ? nSize : -1;
}

void bufferInsert(u8 *aBuffer, int nSize, int nMaxSize, int iInsert, int nItemSize, const u8 *pItem, u8 *pLast) {
  int itemsToMove;

  assert( nMaxSize > 0 && nItemSize > 0 );
  assert( nSize <= nMaxSize );
  assert( 0 <= iInsert && iInsert <= nSize && iInsert < nMaxSize );

  if( nSize == nMaxSize ){
    if( pLast != NULL ){
      memcpy(pLast, aBuffer + (nSize - 1) * nItemSize, nItemSize);
    }
    nSize--;
  }
  itemsToMove = nSize - iInsert;
  memmove(aBuffer + (iInsert + 1) * nItemSize, aBuffer + iInsert * nItemSize, itemsToMove * nItemSize);
  memcpy(aBuffer + iInsert * nItemSize, pItem, nItemSize);
}

void bufferDelete(u8 *aBuffer, int nSize, int iDelete, int nItemSize) {
  int itemsToMove;

  assert( nItemSize > 0 );
  assert( 0 <= iDelete && iDelete < nSize );

  itemsToMove = nSize - iDelete - 1;
  memmove(aBuffer + iDelete * nItemSize, aBuffer + (iDelete + 1) * nItemSize, itemsToMove * nItemSize);
}

/**************************************************************************
** DiskANN internals
**************************************************************************/

static int g_distTimingMode = 0;  /* 0=off, 1=query, 2=build */

static float diskAnnVectorDistance(const DiskAnnIndex *pIndex, const Vector *pVec1, const Vector *pVec2){
  float result;
  struct timespec _d0, _d1;
  if( g_distTimingMode ){
    clock_gettime(CLOCK_MONOTONIC, &_d0);
  }
  switch( pIndex->nDistanceFunc ){
    case VECTOR_METRIC_TYPE_COS:
      result = vectorDistanceCos(pVec1, pVec2);
      break;
    case VECTOR_METRIC_TYPE_L2:
      result = vectorDistanceL2(pVec1, pVec2);
      break;
    default:
      assert(0);
      result = 0.0;
      break;
  }
  if( g_distTimingMode ){
    double ms;
    clock_gettime(CLOCK_MONOTONIC, &_d1);
    ms = (_d1.tv_sec - _d0.tv_sec)*1000.0
       + (_d1.tv_nsec - _d0.tv_nsec)/1e6;
    if( g_distTimingMode==1 ){
      g_queryDistanceMs += ms;
    }else if( g_distTimingMode==2 ){
      g_buildDistanceMs += ms;
    }
  }
  return result;
}

static DiskAnnNode *diskAnnNodeAlloc(const DiskAnnIndex *pIndex, u64 nRowid){
  sqlite4_env *pEnv = pIndex->db->pEnv;
  DiskAnnNode *pNode = sqlite4_malloc(pEnv, sizeof(DiskAnnNode));
  if( pNode == NULL ){
    return NULL;
  }
  pNode->nRowid = nRowid;
  pNode->visited = 0;
  pNode->pNext = NULL;
  pNode->pBlobSpot = NULL;
  return pNode;
}

static void diskAnnNodeFree(DiskAnnNode *pNode){
  if( pNode->pBlobSpot != NULL ){
    blobSpotFree(pNode->pBlobSpot);
  }
  /* Use env_default since we don't have a db reference here */
  sqlite4_free(sqlite4_env_default(), pNode);
}

static int diskAnnSearchCtxInit(const DiskAnnIndex *pIndex, DiskAnnSearchCtx *pCtx, const Vector* pQuery, int maxCandidates, int topCandidates, int blobMode){
  sqlite4_env *pEnv = pIndex->db->pEnv;

  if( initVectorPair(pIndex->nNodeVectorType, pIndex->nEdgeVectorType, pIndex->nVectorDims, &pCtx->query) != 0 ){
    return SQLITE4_NOMEM;
  }
  loadVectorPair(&pCtx->query, pQuery);

  pCtx->aDistances = sqlite4_malloc(pEnv, maxCandidates * sizeof(float));
  pCtx->aCandidates = sqlite4_malloc(pEnv, maxCandidates * sizeof(DiskAnnNode*));
  pCtx->nCandidates = 0;
  pCtx->maxCandidates = maxCandidates;
  pCtx->aTopDistances = sqlite4_malloc(pEnv, topCandidates * sizeof(float));
  pCtx->aTopCandidates = sqlite4_malloc(pEnv, topCandidates * sizeof(DiskAnnNode*));
  pCtx->nTopCandidates = 0;
  pCtx->maxTopCandidates = topCandidates;
  pCtx->visitedList = NULL;
  pCtx->nUnvisited = 0;
  pCtx->blobMode = blobMode;

  if( pCtx->aDistances != NULL && pCtx->aCandidates != NULL && pCtx->aTopDistances != NULL && pCtx->aTopCandidates != NULL ){
    return SQLITE4_OK;
  }
  if( pCtx->aDistances != NULL ) sqlite4_free(pEnv, pCtx->aDistances);
  if( pCtx->aCandidates != NULL ) sqlite4_free(pEnv, pCtx->aCandidates);
  if( pCtx->aTopDistances != NULL ) sqlite4_free(pEnv, pCtx->aTopDistances);
  if( pCtx->aTopCandidates != NULL ) sqlite4_free(pEnv, pCtx->aTopCandidates);
  deinitVectorPair(&pCtx->query);
  return SQLITE4_NOMEM;
}

static void diskAnnSearchCtxDeinit(DiskAnnSearchCtx *pCtx){
  int i;
  DiskAnnNode *pNode, *pNext;
  sqlite4_env *pEnv = sqlite4_env_default();

  for(i = 0; i < (int)pCtx->nCandidates; i++){
    if( !pCtx->aCandidates[i]->visited ){
      diskAnnNodeFree(pCtx->aCandidates[i]);
    }
  }

  pNode = pCtx->visitedList;
  while( pNode != NULL ){
    pNext = pNode->pNext;
    diskAnnNodeFree(pNode);
    pNode = pNext;
  }
  sqlite4_free(pEnv, pCtx->aCandidates);
  sqlite4_free(pEnv, pCtx->aDistances);
  sqlite4_free(pEnv, pCtx->aTopCandidates);
  sqlite4_free(pEnv, pCtx->aTopDistances);
  deinitVectorPair(&pCtx->query);
}

static int diskAnnSearchCtxIsVisited(const DiskAnnSearchCtx *pCtx, u64 nRowid){
  DiskAnnNode *pNode;
  for(pNode = pCtx->visitedList; pNode != NULL; pNode = pNode->pNext){
    if( pNode->nRowid == nRowid ){
      return 1;
    }
  }
  return 0;
}

static int diskAnnSearchCtxHasCandidate(const DiskAnnSearchCtx *pCtx, u64 nRowid){
  int i;
  for(i = 0; i < (int)pCtx->nCandidates; i++){
    if( pCtx->aCandidates[i]->nRowid == nRowid ){
      return 1;
    }
  }
  return 0;
}

static int diskAnnSearchCtxShouldAddCandidate(const DiskAnnIndex *pIndex, const DiskAnnSearchCtx *pCtx, float candidateDist){
  int i;
  for(i = 0; i < (int)pCtx->nCandidates; i++){
    float distCandidate = pCtx->aDistances[i];
    if( candidateDist < distCandidate ){
      return i;
    }
  }
  return pCtx->nCandidates < pCtx->maxCandidates ? (int)pCtx->nCandidates : -1;
}

static void diskAnnSearchCtxMarkVisited(DiskAnnSearchCtx *pCtx, DiskAnnNode *pNode, float distance){
  int iInsert;

  assert( pCtx->nUnvisited > 0 );
  assert( pNode->visited == 0 );

  pNode->visited = 1;
  pCtx->nUnvisited--;

  pNode->pNext = pCtx->visitedList;
  pCtx->visitedList = pNode;

  iInsert = distanceBufferInsertIdx(pCtx->aTopDistances, pCtx->nTopCandidates, pCtx->maxTopCandidates, distance);
  if( iInsert < 0 ){
    return;
  }
  bufferInsert((u8*)pCtx->aTopCandidates, pCtx->nTopCandidates, pCtx->maxTopCandidates, iInsert, sizeof(DiskAnnNode*), (u8*)&pNode, NULL);
  bufferInsert((u8*)pCtx->aTopDistances, pCtx->nTopCandidates, pCtx->maxTopCandidates, iInsert, sizeof(float), (u8*)&distance, NULL);
  pCtx->nTopCandidates = MIN(pCtx->nTopCandidates + 1, pCtx->maxTopCandidates);
}

static int diskAnnSearchCtxHasUnvisited(const DiskAnnSearchCtx *pCtx){
  return pCtx->nUnvisited > 0;
}

static void diskAnnSearchCtxGetCandidate(DiskAnnSearchCtx *pCtx, int i, DiskAnnNode **ppNode, float *pDistance){
  assert( 0 <= i && i < (int)pCtx->nCandidates );
  *ppNode = pCtx->aCandidates[i];
  *pDistance = pCtx->aDistances[i];
}

static void diskAnnSearchCtxDeleteCandidate(DiskAnnSearchCtx *pCtx, int iDelete){
  int i;
  assert( pCtx->nUnvisited > 0 );
  assert( !pCtx->aCandidates[iDelete]->visited );
  assert( pCtx->aCandidates[iDelete]->pBlobSpot == NULL );

  diskAnnNodeFree(pCtx->aCandidates[iDelete]);
  bufferDelete((u8*)pCtx->aCandidates, pCtx->nCandidates, iDelete, sizeof(DiskAnnNode*));
  bufferDelete((u8*)pCtx->aDistances, pCtx->nCandidates, iDelete, sizeof(float));

  pCtx->nCandidates--;
  pCtx->nUnvisited--;
}

static void diskAnnSearchCtxInsertCandidate(DiskAnnSearchCtx *pCtx, int iInsert, DiskAnnNode* pCandidate, float distance){
  DiskAnnNode *pLast = NULL;
  bufferInsert((u8*)pCtx->aCandidates, pCtx->nCandidates, pCtx->maxCandidates, iInsert, sizeof(DiskAnnNode*), (u8*)&pCandidate, (u8*)&pLast);
  bufferInsert((u8*)pCtx->aDistances, pCtx->nCandidates, pCtx->maxCandidates, iInsert, sizeof(float), (u8*)&distance, NULL);
  pCtx->nCandidates = MIN(pCtx->nCandidates + 1, pCtx->maxCandidates);
  if( pLast != NULL && !pLast->visited ){
    assert( pLast->pBlobSpot == NULL );
    pCtx->nUnvisited--;
    diskAnnNodeFree(pLast);
  }
  pCtx->nUnvisited++;
}

static int diskAnnSearchCtxFindClosestCandidateIdx(const DiskAnnSearchCtx *pCtx){
  int i;
#ifdef SQLITE4_DEBUG
  for(i = 0; i < (int)pCtx->nCandidates - 1; i++){
    assert(pCtx->aDistances[i] <= pCtx->aDistances[i + 1]);
  }
#endif
  for(i = 0; i < (int)pCtx->nCandidates; i++){
    DiskAnnNode *pCandidate = pCtx->aCandidates[i];
    if( pCandidate->visited ){
      continue;
    }
    return i;
  }
  return -1;
}

/**************************************************************************
** DiskANN core
**************************************************************************/

static int diskAnnReplaceEdgeIdx(
  const DiskAnnIndex *pIndex,
  BlobSpot *pNodeBlob,
  u64 newRowid,
  VectorPair *pNewVector,
  VectorPair *pPlaceholder,
  float *pNodeToNew
) {
  int i, nEdges, nMaxEdges, iReplace = -1;
  Vector nodeVector, edgeVector;
  float nodeToNew, nodeToReplace = 0;

  nEdges = nodeBinEdges(pIndex, pNodeBlob);
  nMaxEdges = nodeEdgesMaxCount(pIndex);
  nodeBinVector(pIndex, pNodeBlob, &nodeVector);
  loadVectorPair(pPlaceholder, &nodeVector);

  nodeToNew = diskAnnVectorDistance(pIndex, pPlaceholder->pEdge, pNewVector->pEdge);
  *pNodeToNew = nodeToNew;

  for(i = nEdges - 1; i >= 0; i--){
    u64 edgeRowid;
    float edgeToNew, nodeToEdge;

    nodeBinEdge(pIndex, pNodeBlob, i, &edgeRowid, &nodeToEdge, &edgeVector);
    if( edgeRowid == newRowid ){
      return i;
    }

    if( pIndex->nFormatVersion == VECTOR_FORMAT_V1 ){
      nodeToEdge = diskAnnVectorDistance(pIndex, pPlaceholder->pEdge, &edgeVector);
    }

    edgeToNew = diskAnnVectorDistance(pIndex, &edgeVector, pNewVector->pEdge);
    if( nodeToNew > pIndex->pruningAlpha * edgeToNew ){
      return -1;
    }
    if( nodeToNew < nodeToEdge && (iReplace == -1 || nodeToReplace < nodeToEdge) ){
      nodeToReplace = nodeToEdge;
      iReplace = i;
    }
  }
  if( nEdges < nMaxEdges ){
    return nEdges;
  }
  return iReplace;
}

static void diskAnnPruneEdges(const DiskAnnIndex *pIndex, BlobSpot *pNodeBlob, int iInserted, VectorPair *pPlaceholder) {
  int i, nEdges;
  Vector nodeVector, hintEdgeVector;
  u64 hintRowid;

  nodeBinVector(pIndex, pNodeBlob, &nodeVector);
  loadVectorPair(pPlaceholder, &nodeVector);

  nEdges = nodeBinEdges(pIndex, pNodeBlob);

  assert( 0 <= iInserted && iInserted < nEdges );

  nodeBinEdge(pIndex, pNodeBlob, iInserted, &hintRowid, NULL, &hintEdgeVector);

  i = 0;
  while( i < nEdges ){
    Vector edgeVector;
    float nodeToEdge, hintToEdge;
    u64 edgeRowid;
    nodeBinEdge(pIndex, pNodeBlob, i, &edgeRowid, &nodeToEdge, &edgeVector);

    if( hintRowid == edgeRowid ){
      i++;
      continue;
    }
    if( pIndex->nFormatVersion == VECTOR_FORMAT_V1 ){
      nodeToEdge = diskAnnVectorDistance(pIndex, pPlaceholder->pEdge, &edgeVector);
    }

    hintToEdge = diskAnnVectorDistance(pIndex, &hintEdgeVector, &edgeVector);
    if( nodeToEdge > pIndex->pruningAlpha * hintToEdge ){
      nodeBinDeleteEdge(pIndex, pNodeBlob, i);
      nEdges--;
    }else{
      i++;
    }
  }

  assert( nEdges > 0 );
}

/* main search routine - called from both SEARCH and INSERT operation */
static int diskAnnSearchInternal(DiskAnnIndex *pIndex, DiskAnnSearchCtx *pCtx, u64 nStartRowid, char **pzErrMsg){
  DiskAnnTrace(("diskAnnSearchInternal: ready to search: rootId=%lld\n", nStartRowid));
  DiskAnnNode *start = NULL;
  BlobSpot *pReusableBlobSpot = NULL;
  Vector startVector;
  float startDistance;
  int rc, i, nVisited = 0;

  start = diskAnnNodeAlloc(pIndex, nStartRowid);
  if( start == NULL ){
    *pzErrMsg = sqlite4_mprintf(pIndex->db->pEnv, "vector index(search): failed to allocate new node");
    rc = SQLITE4_NOMEM;
    goto out;
  }

  rc = blobSpotCreate(pIndex, &start->pBlobSpot, nStartRowid, pIndex->nBlockSize, pCtx->blobMode);
  if( rc != SQLITE4_OK ){
    *pzErrMsg = sqlite4_mprintf(pIndex->db->pEnv, "vector index(search): failed to create new blob");
    goto out;
  }

  rc = blobSpotReload(pIndex, start->pBlobSpot, nStartRowid, pIndex->nBlockSize);
  if( rc != SQLITE4_OK ){
    *pzErrMsg = sqlite4_mprintf(pIndex->db->pEnv, "vector index(search): failed to load new blob");
    goto out;
  }

  nodeBinVector(pIndex, start->pBlobSpot, &startVector);
  startDistance = diskAnnVectorDistance(pIndex, pCtx->query.pNode, &startVector);

  if( pCtx->blobMode == DISKANN_BLOB_READONLY ){
    assert( start->pBlobSpot != NULL );
    pReusableBlobSpot = start->pBlobSpot;
    start->pBlobSpot = NULL;
  }
  diskAnnSearchCtxInsertCandidate(pCtx, 0, start, startDistance);
  start = NULL;

  while( diskAnnSearchCtxHasUnvisited(pCtx) ){
    int nEdges;
    Vector vCandidate;
    DiskAnnNode *pCandidate;
    BlobSpot *pCandidateBlob;
    float distance;
    int iCandidate = diskAnnSearchCtxFindClosestCandidateIdx(pCtx);
    diskAnnSearchCtxGetCandidate(pCtx, iCandidate, &pCandidate, &distance);

    rc = SQLITE4_OK;
    if( pReusableBlobSpot != NULL ){
      rc = blobSpotReload(pIndex, pReusableBlobSpot, pCandidate->nRowid, pIndex->nBlockSize);
      pCandidateBlob = pReusableBlobSpot;
    }else{
      if( pCandidate->pBlobSpot == NULL ){
        rc = blobSpotCreate(pIndex, &pCandidate->pBlobSpot, pCandidate->nRowid, pIndex->nBlockSize, pCtx->blobMode);
      }
      if( rc == SQLITE4_OK ){
        rc = blobSpotReload(pIndex, pCandidate->pBlobSpot, pCandidate->nRowid, pIndex->nBlockSize);
      }
      pCandidateBlob = pCandidate->pBlobSpot;
    }

    if( rc == DISKANN_ROW_NOT_FOUND ){
      diskAnnSearchCtxDeleteCandidate(pCtx, iCandidate);
      continue;
    }else if( rc != SQLITE4_OK ){
      *pzErrMsg = sqlite4_mprintf(pIndex->db->pEnv, "vector index(search): failed to create new blob for candidate");
      goto out;
    }

    nVisited += 1;
    g_searchVisitedTotal++;
    DiskAnnTrace(("visiting candidate(%d): id=%lld\n", nVisited, pCandidate->nRowid));
    nodeBinVector(pIndex, pCandidateBlob, &vCandidate);
    nEdges = nodeBinEdges(pIndex, pCandidateBlob);
    g_searchEdgesTotal += nEdges;

    if( pCtx->query.pNode != pCtx->query.pEdge ){
      distance = diskAnnVectorDistance(pIndex, &vCandidate, pCtx->query.pNode);
    }

    diskAnnSearchCtxMarkVisited(pCtx, pCandidate, distance);

    for(i = 0; i < nEdges; i++){
      u64 edgeRowid;
      Vector edgeVector;
      float edgeDistance;
      int iInsert;
      DiskAnnNode *pNewCandidate;
      nodeBinEdge(pIndex, pCandidateBlob, i, &edgeRowid, NULL, &edgeVector);
      if( diskAnnSearchCtxIsVisited(pCtx, edgeRowid) || diskAnnSearchCtxHasCandidate(pCtx, edgeRowid) ){
        continue;
      }

      edgeDistance = diskAnnVectorDistance(pIndex, pCtx->query.pEdge, &edgeVector);
      iInsert = diskAnnSearchCtxShouldAddCandidate(pIndex, pCtx, edgeDistance);
      if( iInsert < 0 ){
        continue;
      }
      pNewCandidate = diskAnnNodeAlloc(pIndex, edgeRowid);
      if( pNewCandidate == NULL ){
        continue;
      }
      DiskAnnTrace(("want to insert new candidate %lld at position %d with distance %f\n", edgeRowid, iInsert, edgeDistance));
      diskAnnSearchCtxInsertCandidate(pCtx, iInsert, pNewCandidate, edgeDistance);
    }
  }
  rc = SQLITE4_OK;
out:
  if( start != NULL ){
    diskAnnNodeFree(start);
  }
  if( pReusableBlobSpot != NULL ){
    blobSpotFree(pReusableBlobSpot);
  }
  return rc;
}

/**************************************************************************
** DiskANN main internal API
**************************************************************************/

int diskAnnSearch(
  DiskAnnIndex *pIndex,
  const Vector *pVector,
  int k,
  const VectorIdxKey *pKey,
  VectorOutRows *pRows,
  char **pzErrMsg
){
  int rc = SQLITE4_OK;
  DiskAnnSearchCtx ctx;
  u64 nStartRowid;
  int nOutRows;
  int i;
  struct timespec _q0, _q1, _qg0, _qg1, _qr0, _qr1;
  double kvReadBefore, kvReadAfter;
  int kvReadCountBefore, kvReadCountAfter;
  int visitedBefore, visitedAfter;
  long long edgesBefore, edgesAfter;

  DiskAnnTrace(("diskAnnSearch started\n"));

  if( k < 0 ){
    *pzErrMsg = sqlite4_mprintf(pIndex->db->pEnv, "vector index(search): k must be a non-negative integer");
    return SQLITE4_ERROR;
  }
  if( pVector->dims != pIndex->nVectorDims ){
    *pzErrMsg = sqlite4_mprintf(pIndex->db->pEnv, "vector index(search): dimensions are different: %d != %d", pVector->dims, pIndex->nVectorDims);
    return SQLITE4_ERROR;
  }
  if( pVector->type != pIndex->nNodeVectorType ){
    *pzErrMsg = sqlite4_mprintf(pIndex->db->pEnv, "vector index(search): vector type differs from column type: %d != %d", pVector->type, pIndex->nNodeVectorType);
    return SQLITE4_ERROR;
  }

  clock_gettime(CLOCK_MONOTONIC, &_q0);

  /* Snapshot counters before search to isolate search-only I/O */
  kvReadBefore = g_totalKvReadMs;
  kvReadCountBefore = pIndex->nReads;
  visitedBefore = g_searchVisitedTotal;
  edgesBefore = g_searchEdgesTotal;

  rc = diskAnnSelectRandomShadowRow(pIndex, &nStartRowid);
  if( rc == SQLITE4_DONE ){
    pRows->nRows = 0;
    pRows->nCols = pKey->nKeyColumns;
    return SQLITE4_OK;
  }else if( rc != SQLITE4_OK ){
    *pzErrMsg = sqlite4_mprintf(pIndex->db->pEnv, "vector index(search): failed to select start node for search");
    return rc;
  }
  rc = diskAnnSearchCtxInit(pIndex, &ctx, pVector, pIndex->searchL, k, DISKANN_BLOB_READONLY);
  if( rc != SQLITE4_OK ){
    *pzErrMsg = sqlite4_mprintf(pIndex->db->pEnv, "vector index(search): failed to initialize search context");
    goto out;
  }

  /* Graph traversal (timed) */
  clock_gettime(CLOCK_MONOTONIC, &_qg0);
  g_distTimingMode = 1;
  rc = diskAnnSearchInternal(pIndex, &ctx, nStartRowid, pzErrMsg);
  g_distTimingMode = 0;
  clock_gettime(CLOCK_MONOTONIC, &_qg1);
  if( rc != SQLITE4_OK ){
    goto out;
  }

  /* Result collection (timed) */
  clock_gettime(CLOCK_MONOTONIC, &_qr0);
  nOutRows = MIN(k, ctx.nTopCandidates);
  rc = vectorOutRowsAlloc(pIndex->db, pRows, nOutRows, pKey->nKeyColumns, vectorIdxKeyRowidLike(pKey));
  if( rc != SQLITE4_OK ){
    *pzErrMsg = sqlite4_mprintf(pIndex->db->pEnv, "vector index(search): failed to allocate output rows");
    goto out;
  }
  for(i = 0; i < nOutRows; i++){
    if( pRows->aIntValues != NULL ){
      rc = vectorOutRowsPut(pRows, i, 0, &ctx.aTopCandidates[i]->nRowid, NULL);
    }else{
      rc = diskAnnGetShadowRowKeys(pIndex, ctx.aTopCandidates[i]->nRowid, pKey, pRows, i);
    }
    if( rc != SQLITE4_OK ){
      *pzErrMsg = sqlite4_mprintf(pIndex->db->pEnv, "vector index(search): failed to put result in the output row");
      goto out;
    }
  }
  clock_gettime(CLOCK_MONOTONIC, &_qr1);

  /* Accumulate search stats */
  kvReadAfter = g_totalKvReadMs;
  kvReadCountAfter = pIndex->nReads;
  visitedAfter = g_searchVisitedTotal;
  edgesAfter = g_searchEdgesTotal;

  g_queryCount++;
  {
    double totalMs = (_qr1.tv_sec - _q0.tv_sec)*1000.0 + (_qr1.tv_nsec - _q0.tv_nsec)/1e6;
    double graphMs = (_qg1.tv_sec - _qg0.tv_sec)*1000.0 + (_qg1.tv_nsec - _qg0.tv_nsec)/1e6;
    double resultMs = (_qr1.tv_sec - _qr0.tv_sec)*1000.0 + (_qr1.tv_nsec - _qr0.tv_nsec)/1e6;
    g_queryTotalMs += totalMs;
    g_queryGraphMs += graphMs;
    g_queryResultMs += resultMs;
    g_queryKvReadMs += (kvReadAfter - kvReadBefore);
    g_queryKvReads += (kvReadCountAfter - kvReadCountBefore);
    g_queryNodesVisited += (visitedAfter - visitedBefore);
    g_queryEdgesExamined += (edgesAfter - edgesBefore);
  }

  rc = SQLITE4_OK;
out:
  clock_gettime(CLOCK_MONOTONIC, &_q1);
  diskAnnSearchCtxDeinit(&ctx);
  return rc;
}

int diskAnnInsert(
  DiskAnnIndex *pIndex,
  const VectorInRow *pVectorInRow,
  char **pzErrMsg
){
  int rc, first = 0;
  u64 nStartRowid, nNewRowid;
  BlobSpot *pBlobSpot = NULL;
  DiskAnnNode *pVisited;
  DiskAnnSearchCtx ctx;
  VectorPair vInsert, vCandidate;
  double buildReadStart = 0.0, buildWriteStart = 0.0, buildDistStart = 0.0;
  double insertLsmStart = g_autoworkTotalMs;
  double buildReadMs = 0.0, buildWriteMs = 0.0, buildDistMs = 0.0;
  vInsert.pNode = NULL; vInsert.pEdge = NULL;
  vCandidate.pNode = NULL; vCandidate.pEdge = NULL;

  if( pVectorInRow->pVector->dims != pIndex->nVectorDims ){
    *pzErrMsg = sqlite4_mprintf(pIndex->db->pEnv, "vector index(insert): dimensions are different: %d != %d", pVectorInRow->pVector->dims, pIndex->nVectorDims);
    return SQLITE4_ERROR;
  }
  if( pVectorInRow->pVector->type != pIndex->nNodeVectorType ){
    *pzErrMsg = sqlite4_mprintf(pIndex->db->pEnv, "vector index(insert): vector type differs from column type: %d != %d", pVectorInRow->pVector->type, pIndex->nNodeVectorType);
    return SQLITE4_ERROR;
  }

  DiskAnnTrace(("diskAnnInsert started\n"));

  rc = diskAnnSearchCtxInit(pIndex, &ctx, pVectorInRow->pVector, pIndex->insertL, 1, DISKANN_BLOB_WRITABLE);
  if( rc != SQLITE4_OK ){
    *pzErrMsg = sqlite4_mprintf(pIndex->db->pEnv, "vector index(insert): failed to initialize search context");
    return rc;
  }

  if( initVectorPair(pIndex->nNodeVectorType, pIndex->nEdgeVectorType, pIndex->nVectorDims, &vInsert) != 0 ){
    *pzErrMsg = sqlite4_mprintf(pIndex->db->pEnv, "vector index(insert): unable to allocate mem for node VectorPair");
    rc = SQLITE4_NOMEM;
    goto out;
  }

  if( initVectorPair(pIndex->nNodeVectorType, pIndex->nEdgeVectorType, pIndex->nVectorDims, &vCandidate) != 0 ){
    *pzErrMsg = sqlite4_mprintf(pIndex->db->pEnv, "vector index(insert): unable to allocate mem for candidate VectorPair");
    rc = SQLITE4_NOMEM;
    goto out;
  }

  /* select random row before inserting new row */
  rc = diskAnnSelectRandomShadowRow(pIndex, &nStartRowid);
  if( rc == SQLITE4_DONE ){
    first = 1;
  }else if( rc != SQLITE4_OK ){
    *pzErrMsg = sqlite4_mprintf(pIndex->db->pEnv, "vector index(insert): failed to select start node for search");
    rc = SQLITE4_ERROR;
    goto out;
  }
  if( !first ){
    struct timespec _ts0, _ts1;
    buildReadStart = g_totalKvReadMs;
    buildWriteStart = g_totalKvWriteMs;
    buildDistStart = g_buildDistanceMs;
    clock_gettime(CLOCK_MONOTONIC, &_ts0);
    g_distTimingMode = 2;
    rc = diskAnnSearchInternal(pIndex, &ctx, nStartRowid, pzErrMsg);
    g_distTimingMode = 0;
    clock_gettime(CLOCK_MONOTONIC, &_ts1);
    pIndex->totalSearchMs += (_ts1.tv_sec - _ts0.tv_sec)*1000.0
                           + (_ts1.tv_nsec - _ts0.tv_nsec)/1e6;
    buildReadMs += g_totalKvReadMs - buildReadStart;
    buildWriteMs += g_totalKvWriteMs - buildWriteStart;
    buildDistMs += g_buildDistanceMs - buildDistStart;
    if( rc != SQLITE4_OK ){
      goto out;
    }
  }

  {
    struct timespec _si0, _si1;
    clock_gettime(CLOCK_MONOTONIC, &_si0);
    rc = diskAnnInsertShadowRow(pIndex, pVectorInRow, &nNewRowid);
    clock_gettime(CLOCK_MONOTONIC, &_si1);
    pIndex->totalShadowInsMs += (_si1.tv_sec - _si0.tv_sec)*1000.0
                              + (_si1.tv_nsec - _si0.tv_nsec)/1e6;
  }
  if( rc == SQLITE4_OK ){
    pIndex->nShadowRows++;
  }
  if( rc != SQLITE4_OK ){
    *pzErrMsg = sqlite4_mprintf(pIndex->db->pEnv, "vector index(insert): failed to insert shadow row");
    goto out;
  }

  rc = blobSpotCreate(pIndex, &pBlobSpot, nNewRowid, pIndex->nBlockSize, 1);
  if( rc != SQLITE4_OK ){
    *pzErrMsg = sqlite4_mprintf(pIndex->db->pEnv, "vector index(insert): failed to create blob for shadow row");
    goto out;
  }
  nodeBinInit(pIndex, pBlobSpot, nNewRowid, pVectorInRow->pVector);

  if( first ){
    DiskAnnTrace(("inserted first row\n"));
    rc = SQLITE4_OK;
    goto out;
  }

  /* first pass - add all visited nodes as potential neighbours of new node */
  buildReadStart = g_totalKvReadMs;
  buildWriteStart = g_totalKvWriteMs;
  buildDistStart = g_buildDistanceMs;
  g_distTimingMode = 2;
  {
    struct timespec _p1a, _p1b;
    clock_gettime(CLOCK_MONOTONIC, &_p1a);
    for(pVisited = ctx.visitedList; pVisited != NULL; pVisited = pVisited->pNext){
      Vector nodeVector;
      int iReplace;
      float nodeToNew;
      nodeBinVector(pIndex, pVisited->pBlobSpot, &nodeVector);
      loadVectorPair(&vCandidate, &nodeVector);
      iReplace = diskAnnReplaceEdgeIdx(pIndex, pBlobSpot, pVisited->nRowid, &vCandidate, &vInsert, &nodeToNew);
      if( iReplace == -1 ){
        continue;
      }
      nodeBinReplaceEdge(pIndex, pBlobSpot, iReplace, pVisited->nRowid, nodeToNew, vCandidate.pEdge);
      diskAnnPruneEdges(pIndex, pBlobSpot, iReplace, &vInsert);
    }
    clock_gettime(CLOCK_MONOTONIC, &_p1b);
    pIndex->totalPass1Ms += (_p1b.tv_sec - _p1a.tv_sec)*1000.0
                          + (_p1b.tv_nsec - _p1a.tv_nsec)/1e6;
  }

  /* second pass - add new node as potential neighbour of all visited nodes */
  {
    struct timespec _p2a, _p2b;
    clock_gettime(CLOCK_MONOTONIC, &_p2a);
    loadVectorPair(&vInsert, pVectorInRow->pVector);
    for(pVisited = ctx.visitedList; pVisited != NULL; pVisited = pVisited->pNext){
      int iReplace;
      float nodeToNew;

      iReplace = diskAnnReplaceEdgeIdx(pIndex, pVisited->pBlobSpot, nNewRowid, &vInsert, &vCandidate, &nodeToNew);
      if( iReplace == -1 ){
        continue;
      }
      nodeBinReplaceEdge(pIndex, pVisited->pBlobSpot, iReplace, nNewRowid, nodeToNew, vInsert.pEdge);
      diskAnnPruneEdges(pIndex, pVisited->pBlobSpot, iReplace, &vCandidate);

      rc = blobSpotFlush(pIndex, pVisited->pBlobSpot);
      if( rc != SQLITE4_OK ){
        *pzErrMsg = sqlite4_mprintf(pIndex->db->pEnv, "vector index(insert): failed to flush blob");
        goto out;
      }
    }
    clock_gettime(CLOCK_MONOTONIC, &_p2b);
    pIndex->totalPass2Ms += (_p2b.tv_sec - _p2a.tv_sec)*1000.0
                          + (_p2b.tv_nsec - _p2a.tv_nsec)/1e6;
  }
  g_distTimingMode = 0;

  rc = SQLITE4_OK;
out:
  g_distTimingMode = 0;
  deinitVectorPair(&vInsert);
  deinitVectorPair(&vCandidate);
  if( rc == SQLITE4_OK ){
    struct timespec _fl0, _fl1;
    clock_gettime(CLOCK_MONOTONIC, &_fl0);
    rc = blobSpotFlush(pIndex, pBlobSpot);
    clock_gettime(CLOCK_MONOTONIC, &_fl1);
    pIndex->totalNewFlushMs += (_fl1.tv_sec - _fl0.tv_sec)*1000.0
                             + (_fl1.tv_nsec - _fl0.tv_nsec)/1e6;
    if( rc != SQLITE4_OK ){
      *pzErrMsg = sqlite4_mprintf(pIndex->db->pEnv, "vector index(insert): failed to flush blob");
    }else{
      buildReadMs += g_totalKvReadMs - buildReadStart;
      buildWriteMs += g_totalKvWriteMs - buildWriteStart;
      buildDistMs += g_buildDistanceMs - buildDistStart;
      pIndex->totalBuildReadMs += buildReadMs;
      pIndex->totalBuildWriteMs += buildWriteMs;
      pIndex->totalBuildDistMs += buildDistMs;
      pIndex->totalBuildLsmMs += g_autoworkTotalMs - insertLsmStart;
    }
  }
  if( pBlobSpot != NULL ){
    blobSpotFree(pBlobSpot);
  }
  diskAnnSearchCtxDeinit(&ctx);

  return rc;
}

int diskAnnDelete(
  DiskAnnIndex *pIndex,
  const VectorInRow *pInRow,
  char **pzErrMsg
){
  int rc;
  BlobSpot *pNodeBlob = NULL, *pEdgeBlob = NULL;
  u64 nodeRowid;
  int iDelete, nNeighbours, i;

  if( vectorInRowTryGetRowid(pInRow, &nodeRowid) != 0 ){
    rc = diskAnnGetShadowRowid(pIndex, pInRow, &nodeRowid);
    if( rc != SQLITE4_OK ){
      *pzErrMsg = sqlite4_mprintf(pIndex->db->pEnv, "vector index(delete): failed to determined node id for deletion");
      goto out;
    }
  }

  DiskAnnTrace(("diskAnnDelete started: rowid=%lld\n", nodeRowid));

  rc = blobSpotCreate(pIndex, &pNodeBlob, nodeRowid, pIndex->nBlockSize, DISKANN_BLOB_WRITABLE);
  if( rc == DISKANN_ROW_NOT_FOUND ){
    rc = SQLITE4_OK;
    goto out;
  }else if( rc != SQLITE4_OK ){
    *pzErrMsg = sqlite4_mprintf(pIndex->db->pEnv, "vector index(delete): failed to create blob for node row");
    goto out;
  }
  rc = blobSpotReload(pIndex, pNodeBlob, nodeRowid, pIndex->nBlockSize);
  if( rc == DISKANN_ROW_NOT_FOUND ){
    rc = SQLITE4_OK;
    goto out;
  }else if( rc != SQLITE4_OK ){
    *pzErrMsg = sqlite4_mprintf(pIndex->db->pEnv, "vector index(delete): failed to reload blob for node row");
    goto out;
  }
  rc = blobSpotCreate(pIndex, &pEdgeBlob, nodeRowid, pIndex->nBlockSize, DISKANN_BLOB_WRITABLE);
  if( rc != SQLITE4_OK ){
    *pzErrMsg = sqlite4_mprintf(pIndex->db->pEnv, "vector index(delete): failed to create blob for edge rows");
    goto out;
  }
  nNeighbours = nodeBinEdges(pIndex, pNodeBlob);
  for(i = 0; i < nNeighbours; i++){
    u64 edgeRowid;
    nodeBinEdge(pIndex, pNodeBlob, i, &edgeRowid, NULL, NULL);
    rc = blobSpotReload(pIndex, pEdgeBlob, edgeRowid, pIndex->nBlockSize);
    if( rc == DISKANN_ROW_NOT_FOUND ){
      continue;
    }else if( rc != SQLITE4_OK ){
      *pzErrMsg = sqlite4_mprintf(pIndex->db->pEnv, "vector index(delete): failed to reload blob for edge row: %d", rc);
      goto out;
    }
    iDelete = nodeBinEdgeFindIdx(pIndex, pEdgeBlob, nodeRowid);
    if( iDelete == -1 ){
      continue;
    }
    nodeBinDeleteEdge(pIndex, pEdgeBlob, iDelete);
    rc = blobSpotFlush(pIndex, pEdgeBlob);
    if( rc != SQLITE4_OK ){
      *pzErrMsg = sqlite4_mprintf(pIndex->db->pEnv, "vector index(delete): failed to flush blob for edge row");
      goto out;
    }
  }

  rc = diskAnnDeleteShadowRow(pIndex, (i64)nodeRowid);
  if( rc != SQLITE4_OK ){
    *pzErrMsg = sqlite4_mprintf(pIndex->db->pEnv, "vector index(delete): failed to remove shadow row");
    goto out;
  }
  if( pIndex->nShadowRows > 0 ) pIndex->nShadowRows--;

  rc = SQLITE4_OK;
out:
  if( pNodeBlob != NULL ){
    blobSpotFree(pNodeBlob);
  }
  if( pEdgeBlob != NULL ){
    blobSpotFree(pEdgeBlob);
  }
  return rc;
}

int diskAnnOpenIndex(
  sqlite4 *db,
  const char *zDbSName,
  const char *zIdxName,
  const VectorIdxParams *pParams,
  DiskAnnIndex **ppIndex
){
  DiskAnnIndex *pIndex;
  u64 nBlockSize;
  int compressNeighbours;

  pIndex = sqlite4DbMallocRaw(db, sizeof(DiskAnnIndex));
  if( pIndex == NULL ){
    return SQLITE4_NOMEM;
  }
  memset(pIndex, 0, sizeof(DiskAnnIndex));
  pIndex->db = db;
  pIndex->zDbSName = sqlite4DbStrDup(db, zDbSName);
  pIndex->zName = sqlite4DbStrDup(db, zIdxName);
  pIndex->zShadow = sqlite4MPrintf(db, "%s_shadow", zIdxName);
  if( pIndex->zShadow == NULL ){
    diskAnnCloseIndex(pIndex);
    return SQLITE4_NOMEM;
  }
  nBlockSize = vectorIdxParamsGetU64(pParams, VECTOR_BLOCK_SIZE_PARAM_ID);
  if( nBlockSize <= 128 ){
    nBlockSize <<= DISKANN_BLOCK_SIZE_SHIFT;
  }

  pIndex->nFormatVersion = vectorIdxParamsGetU64(pParams, VECTOR_FORMAT_PARAM_ID);
  pIndex->nDistanceFunc = vectorIdxParamsGetU64(pParams, VECTOR_METRIC_TYPE_PARAM_ID);
  pIndex->nBlockSize = nBlockSize;
  pIndex->nNodeVectorType = vectorIdxParamsGetU64(pParams, VECTOR_TYPE_PARAM_ID);
  pIndex->nVectorDims = vectorIdxParamsGetU64(pParams, VECTOR_DIM_PARAM_ID);
  pIndex->pruningAlpha = vectorIdxParamsGetF64(pParams, VECTOR_PRUNING_ALPHA_PARAM_ID);
  pIndex->insertL = vectorIdxParamsGetU64(pParams, VECTOR_INSERT_L_PARAM_ID);
  pIndex->searchL = vectorIdxParamsGetU64(pParams, VECTOR_SEARCH_L_PARAM_ID);
  pIndex->nReads = 0;
  pIndex->nWrites = 0;

  if( pIndex->nDistanceFunc == 0 ||
      pIndex->nBlockSize == 0 ||
      pIndex->nNodeVectorType == 0 ||
      pIndex->nVectorDims == 0
    ){
    diskAnnCloseIndex(pIndex);
    return SQLITE4_ERROR;
  }
  if( pIndex->pruningAlpha == 0 ){
    pIndex->pruningAlpha = VECTOR_PRUNING_ALPHA_DEFAULT;
  }
  if( pIndex->insertL == 0 ){
    pIndex->insertL = VECTOR_INSERT_L_DEFAULT;
  }
  if( pIndex->searchL == 0 ){
    pIndex->searchL = VECTOR_SEARCH_L_DEFAULT;
  }
  pIndex->nNodeVectorSize = vectorDataSize(pIndex->nNodeVectorType, pIndex->nVectorDims);

  compressNeighbours = vectorIdxParamsGetU64(pParams, VECTOR_COMPRESS_NEIGHBORS_PARAM_ID);
  if( compressNeighbours == 0 ){
    pIndex->nEdgeVectorType = pIndex->nNodeVectorType;
    pIndex->nEdgeVectorSize = pIndex->nNodeVectorSize;
  }else{
    pIndex->nEdgeVectorType = compressNeighbours;
    pIndex->nEdgeVectorSize = vectorDataSize(compressNeighbours, pIndex->nVectorDims);
  }

  /* Look up shadow table PK tnum for direct KV access */
  {
    Table *pShadowTab = sqlite4FindTable(db, pIndex->zShadow, zDbSName);
    Index *pPk;
    if( pShadowTab == NULL || pShadowTab->pIndex == NULL ){
      diskAnnCloseIndex(pIndex);
      return SQLITE4_ERROR;
    }
    /* Must use the PRIMARY KEY index tnum, not the secondary index.
    ** pShadowTab->pIndex is the list head (last-created = secondary index).
    ** Walk the list to find the PK index (eIndexType==SQLITE4_INDEX_PRIMARYKEY). */
    pPk = sqlite4FindPrimaryKey(pShadowTab, 0);
    if( pPk == NULL ){
      diskAnnCloseIndex(pIndex);
      return SQLITE4_ERROR;
    }
    pIndex->iShadowTnum = pPk->tnum;
    pIndex->nKeyPrefix = sqlite4PutVarint64(pIndex->aKeyPrefix, pIndex->iShadowTnum);
  }

  /* Count existing shadow rows by seeking to last row in shadow table */
  {
    KVCursor *pCsr = NULL;
    u8 aEndKey[10];
    int nEndKey;
    /* Build a key just past the shadow table range (tnum+1) */
    nEndKey = sqlite4PutVarint64(aEndKey, pIndex->iShadowTnum + 1);
    if( sqlite4KVStoreOpenCursor(db->aDb[0].pKV, &pCsr) == SQLITE4_OK ){
      int seekRc = sqlite4KVCursorSeek(pCsr, aEndKey, nEndKey, -1);
      if( seekRc == SQLITE4_OK || seekRc == SQLITE4_INEXACT ){
        const KVByteArray *pLastKey;
        KVSize nLastKey;
        /* Move back one to last row in our shadow table */
        if( seekRc == SQLITE4_OK ){
          sqlite4KVCursorPrev(pCsr);
        }
        if( sqlite4KVCursorKey(pCsr, &pLastKey, &nLastKey) == SQLITE4_OK &&
            nLastKey > pIndex->nKeyPrefix &&
            memcmp(pLastKey, pIndex->aKeyPrefix, pIndex->nKeyPrefix) == 0 ){
          sqlite4_num num;
          if( sqlite4VdbeDecodeNumericKey(
                pLastKey + pIndex->nKeyPrefix,
                nLastKey - pIndex->nKeyPrefix,
                &num) > 0 ){
            pIndex->nShadowRows = (i64)sqlite4_num_to_int64(num, 0);
          }
        }
      }
      sqlite4KVCursorClose(pCsr);
    }
  }

  *ppIndex = pIndex;

  // fprintf(stderr, "diskAnnOpenIndex: %s  blockSize=%d  maxEdges=%d  insertL=%d  searchL=%d  formatVersion=%d\n",
  //         zIdxName, pIndex->nBlockSize, nodeEdgesMaxCount(pIndex),
  //         pIndex->insertL, pIndex->searchL, pIndex->nFormatVersion);
  DiskAnnTrace(("opened index %s: max edges %d\n", zIdxName, nodeEdgesMaxCount(pIndex)));
  return SQLITE4_OK;
}

static double g_totalSearchMs = 0;
static double g_totalShadowInsMs = 0;
static double g_totalPass1Ms = 0;
static double g_totalPass2Ms = 0;
static double g_totalNewFlushMs = 0;
static double g_totalBuildReadMs = 0;
static double g_totalBuildWriteMs = 0;
static double g_totalBuildDistMs = 0;
static double g_totalBuildLsmMs = 0;
static int g_totalInsertCount = 0;
static int g_atexitRegistered = 0;

static void diskAnnPrintSearchStats(void){
  if( g_queryCount > 0 ){
    double avgTotal = g_queryTotalMs / g_queryCount;
    double avgGraph = g_queryGraphMs / g_queryCount;
    double avgResult = g_queryResultMs / g_queryCount;
    double avgKvRead = g_queryKvReadMs / g_queryCount;
    double avgDist = g_queryDistanceMs / g_queryCount;
    double qps = g_queryTotalMs > 0 ? g_queryCount / (g_queryTotalMs / 1000.0) : 0;
    fprintf(stderr, "\n=== diskAnn search breakdown (%d queries) ===\n", g_queryCount);
    fprintf(stderr, "  total:          %8.1f ms  (avg %.3f ms/q, %.0f q/s)\n",
            g_queryTotalMs, avgTotal, qps);
    fprintf(stderr, "  graph traversal:%8.1f ms  (avg %.3f ms/q, %5.1f%%)\n",
            g_queryGraphMs, avgGraph, g_queryGraphMs/g_queryTotalMs*100);
    fprintf(stderr, "    query read I/O:%7.1f ms  (avg %.3f ms/q, %5.1f%% of graph)\n",
            g_queryKvReadMs, avgKvRead,
            g_queryGraphMs > 0 ? g_queryKvReadMs/g_queryGraphMs*100 : 0);
    fprintf(stderr, "    query distance:%6.1f ms  (avg %.3f ms/q, %5.1f%% of graph)\n",
            g_queryDistanceMs, avgDist,
            g_queryGraphMs > 0 ? g_queryDistanceMs/g_queryGraphMs*100 : 0);
    fprintf(stderr, "  result collect: %8.1f ms  (avg %.3f ms/q, %5.1f%%)\n",
            g_queryResultMs, avgResult, g_queryResultMs/g_queryTotalMs*100);
    fprintf(stderr, "================================================\n");
  }
}

static void diskAnnPrintInsertStats(void){
  if( g_totalInsertCount > 0 ){
    double buildTotal = g_totalSearchMs + g_totalPass1Ms + g_totalPass2Ms + g_totalNewFlushMs;
    fprintf(stderr, "\n=== diskAnn insert breakdown (%d inserts) ===\n", g_totalInsertCount);
    fprintf(stderr, "  table insert:   %8.1f ms\n", g_totalShadowInsMs);
    fprintf(stderr, "  index build:    %8.1f ms\n", buildTotal);
    fprintf(stderr, "    build read I/O:%7.1f ms\n", g_totalBuildReadMs);
    fprintf(stderr, "    build write I/O:%6.1f ms\n", g_totalBuildWriteMs);
    fprintf(stderr, "    build distance:%7.1f ms\n", g_totalBuildDistMs);
    fprintf(stderr, "    LSM work during build: %.1f ms\n", g_totalBuildLsmMs);
    fprintf(stderr, "================================================\n");
  }
}

void diskAnnCloseIndex(DiskAnnIndex *pIndex){
  if( pIndex->totalSearchMs > 0 || pIndex->totalShadowInsMs > 0
   || pIndex->totalPass1Ms > 0 || pIndex->totalPass2Ms > 0
   || pIndex->totalNewFlushMs > 0 ){
    g_totalSearchMs += pIndex->totalSearchMs;
    g_totalShadowInsMs += pIndex->totalShadowInsMs;
    g_totalPass1Ms += pIndex->totalPass1Ms;
    g_totalPass2Ms += pIndex->totalPass2Ms;
    g_totalNewFlushMs += pIndex->totalNewFlushMs;
    g_totalBuildReadMs += pIndex->totalBuildReadMs;
    g_totalBuildWriteMs += pIndex->totalBuildWriteMs;
    g_totalBuildDistMs += pIndex->totalBuildDistMs;
    g_totalBuildLsmMs += pIndex->totalBuildLsmMs;
    g_totalInsertCount++;
  }
  if( !g_atexitRegistered ){
    atexit(diskAnnPrintInsertStats);
    atexit(diskAnnPrintSearchStats);
    g_atexitRegistered = 1;
  }
  if( pIndex->zDbSName ){
    sqlite4DbFree(pIndex->db, pIndex->zDbSName);
  }
  if( pIndex->zName ){
    sqlite4DbFree(pIndex->db, pIndex->zName);
  }
  if( pIndex->zShadow ){
    sqlite4DbFree(pIndex->db, pIndex->zShadow);
  }
  if( pIndex->pReadCsr ){
    sqlite4KVCursorClose(pIndex->pReadCsr);
  }
  sqlite4DbFree(pIndex->db, pIndex);
}

#endif /* !defined(SQLITE4_OMIT_VECTOR) */
