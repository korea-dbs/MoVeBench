/*
** 2024-03-18
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
** libSQL vector search - ported to SQLite4.
** Glue between SQLite4 internals and the DiskANN implementation.
*/
#ifndef SQLITE4_OMIT_VECTOR
#include "sqliteInt.h"
#include "vdbeInt.h"
#include "vectorIndexInt.h"

/**************************************************************************
** VectorIdxParams utilities
****************************************************************************/

void vectorIdxParamsInit(VectorIdxParams *pParams, u8 *pBinBuf, int nBinSize) {
  assert( nBinSize <= VECTOR_INDEX_PARAMS_BUF_SIZE );

  pParams->nBinSize = nBinSize;
  if( pBinBuf != NULL ){
    memcpy(pParams->pBinBuf, pBinBuf, nBinSize);
  }
}

u64 vectorIdxParamsGetU64(const VectorIdxParams *pParams, char tag) {
  int i, offset;
  u64 value = 0;
  for (i = 0; i + 9 <= pParams->nBinSize; i += 9){
    if( pParams->pBinBuf[i] != tag ){
      continue;
    }
    value = 0;
    for(offset = 0; offset < 8; offset++){
      value |= ((u64)(pParams->pBinBuf[i + 1 + offset]) << (u64)(8 * offset));
    }
  }
  return value;
}

int vectorIdxParamsPutU64(VectorIdxParams *pParams, char tag, u64 value) {
  int i;
  if( pParams->nBinSize + 9 > VECTOR_INDEX_PARAMS_BUF_SIZE ){
    return -1;
  }
  pParams->pBinBuf[pParams->nBinSize++] = tag;
  for(i = 0; i < 8; i++){
    pParams->pBinBuf[pParams->nBinSize++] = value & 0xff;
    value >>= 8;
  }
  return 0;
}

double vectorIdxParamsGetF64(const VectorIdxParams *pParams, char tag) {
  u64 value = vectorIdxParamsGetU64(pParams, tag);
  return *((double*)&value);
}

int vectorIdxParamsPutF64(VectorIdxParams *pParams, char tag, double value) {
  return vectorIdxParamsPutU64(pParams, tag, *((u64*)&value));
}

/**************************************************************************
** VectorIdxKey utilities
****************************************************************************/

/*
** Get the primary key structure for a vector index.
**
** In sqlite4, every table has an explicit primary key.
** We use sqlite4FindPrimaryKey() to get the PK index, then extract
** column affinity and collation from it.
**
** Only single-column INTEGER PK (rowid-like) is supported.
*/
int vectorIdxKeyGet(const Index *pIndex, VectorIdxKey *pKey, const char **pzErrMsg) {
  Table *pTable;
  Index *pPkIndex;
  int i;

  pTable = pIndex->pTable;
  pPkIndex = sqlite4FindPrimaryKey(pTable, 0);
  if( pPkIndex == NULL ){
    *pzErrMsg = "failed to find primary key for table";
    return -1;
  }

  if( pPkIndex->nColumn > VECTOR_INDEX_MAX_KEY_COLUMNS ){
    *pzErrMsg = "exceeded limit for composite columns in primary key index";
    return -1;
  }

  pKey->nKeyColumns = pPkIndex->nColumn;
  for(i = 0; i < pPkIndex->nColumn; i++){
    int iCol = pPkIndex->aiColumn[i];
    if( iCol >= 0 && iCol < pTable->nCol ){
      pKey->aKeyAffinity[i] = pTable->aCol[iCol].affinity;
    }else{
      pKey->aKeyAffinity[i] = SQLITE4_AFF_INTEGER;
    }
    pKey->azKeyCollation[i] = pPkIndex->azColl[i];
  }
  return 0;
}

int vectorIdxKeyRowidLike(const VectorIdxKey *pKey){
  return pKey->nKeyColumns == 1
      && pKey->aKeyAffinity[0] == SQLITE4_AFF_INTEGER
      && sqlite4_strnicmp(pKey->azKeyCollation[0], "BINARY", 6) == 0;
}

int vectorIdxKeyDefsRender(const VectorIdxKey *pKey, const char *prefix, char *pBuf, int nBufSize) {
  int i, size;
  for(i = 0; i < pKey->nKeyColumns && nBufSize > 0; i++){
    const char *zTypeName;
    const char *collation = pKey->azKeyCollation[i];
    char aff = pKey->aKeyAffinity[i];

    /* Map sqlite4 affinity to SQL type name */
    if( aff == SQLITE4_AFF_INTEGER )      zTypeName = " INTEGER";
    else if( aff == SQLITE4_AFF_REAL )    zTypeName = " REAL";
    else if( aff == SQLITE4_AFF_TEXT )    zTypeName = " TEXT";
    else if( aff == SQLITE4_AFF_NUMERIC ) zTypeName = " NUMERIC";
    else                                   zTypeName = " BLOB";

    if( sqlite4_strnicmp(collation, "BINARY", 6) == 0 ){
      collation = "";
    }
    if( i == 0 ){
      size = snprintf(pBuf, nBufSize, "%s%s %s", prefix, zTypeName, collation);
    }else{
      size = snprintf(pBuf, nBufSize, ",%s%d%s %s", prefix, i, zTypeName, collation);
    }
    if( size < 0 ){
      return -1;
    }
    pBuf += size;
    nBufSize -= size;
  }
  if( nBufSize <= 0 ){
    return -1;
  }
  return 0;
}

int vectorIdxKeyNamesRender(int nKeyColumns, const char *prefix, char *pBuf, int nBufSize) {
  int i, size;
  for(i = 0; i < nKeyColumns && nBufSize > 0; i++){
    if( i == 0 ){
      size = snprintf(pBuf, nBufSize, "%s", prefix);
    }else{
      size = snprintf(pBuf, nBufSize, ",%s%d", prefix, i);
    }
    if( size < 0 ){
      return -1;
    }
    pBuf += size;
    nBufSize -= size;
  }
  if( nBufSize <= 0 ){
    return -1;
  }
  return 0;
}

/**************************************************************************
** VectorInRow utilities
****************************************************************************/

sqlite4_value* vectorInRowKey(const VectorInRow *pVectorInRow, int iKey) {
  assert( 0 <= iKey && iKey < pVectorInRow->nKeys );
  return pVectorInRow->pKeyValues + iKey;
}

i64 vectorInRowLegacyId(const VectorInRow *pVectorInRow) {
  return (i64)pVectorInRow->nRowid;
}

int vectorInRowTryGetRowid(const VectorInRow *pVectorInRow, u64 *nRowid) {
  /* In sqlite4 port, always rowid-like single integer key */
  *nRowid = pVectorInRow->nRowid;
  return 0;
}

int vectorInRowPlaceholderRender(const VectorInRow *pVectorInRow, char *pBuf, int nBufSize) {
  int i;
  assert( pVectorInRow->nKeys > 0 );
  if( nBufSize < 2 * pVectorInRow->nKeys ){
    return -1;
  }
  for(i = 0; i < pVectorInRow->nKeys; i++){
    *(pBuf++) = '?';
    *(pBuf++) = ',';
  }
  *(pBuf - 1) = '\0';
  return 0;
}

/*
** Allocate and initialize a VectorInRow from a rowid + vector value.
** In the sqlite4 port, the primary key is always a single integer rowid.
*/
int vectorInRowAlloc(sqlite4 *db, i64 rowid, sqlite4_value *pVector, VectorInRow *pVectorInRow, char **pzErrMsg) {
  int rc = SQLITE4_OK;
  int type, dims;

  pVectorInRow->nRowid = (u64)rowid;
  pVectorInRow->nKeys = 1;
  pVectorInRow->pKeyValues = NULL;
  pVectorInRow->pVector = NULL;

  if( pVector == NULL || sqlite4_value_type(pVector) == SQLITE4_NULL ){
    /* NULL vector - skip insertion */
    rc = SQLITE4_OK;
    goto out;
  }

  if( detectVectorParameters(pVector, VECTOR_TYPE_FLOAT32, &type, &dims, pzErrMsg) != 0 ){
    rc = SQLITE4_ERROR;
    goto out;
  }

  pVectorInRow->pVector = vectorAlloc(type, dims);
  if( pVectorInRow->pVector == NULL ){
    rc = SQLITE4_NOMEM;
    goto out;
  }

  if( sqlite4_value_type(pVector) == SQLITE4_BLOB ){
    int nByte = 0;
    const void *pBlob = sqlite4_value_blob(pVector, &nByte);
    vectorInitFromBlob(pVectorInRow->pVector, pBlob, nByte);
  } else if( sqlite4_value_type(pVector) == SQLITE4_TEXT ){
    if( vectorParseWithType(pVector, pVectorInRow->pVector, pzErrMsg) != 0 ){
      rc = SQLITE4_ERROR;
      goto out;
    }
  }
  rc = SQLITE4_OK;
out:
  if( rc != SQLITE4_OK && pVectorInRow->pVector != NULL ){
    vectorFree(pVectorInRow->pVector);
    pVectorInRow->pVector = NULL;
  }
  return rc;
}

void vectorInRowFree(sqlite4 *db, VectorInRow *pVectorInRow) {
  vectorFree(pVectorInRow->pVector);
  pVectorInRow->pVector = NULL;
}

/**************************************************************************
** VectorOutRows utilities
****************************************************************************/

int vectorOutRowsAlloc(sqlite4 *db, VectorOutRows *pRows, int nRows, int nCols, int rowidLike){
  assert( nCols > 0 && nRows >= 0 );
  pRows->nRows = nRows;
  pRows->nCols = nCols;
  pRows->aIntValues = NULL;
  pRows->ppValues = NULL;

  if( (u64)nRows * (u64)nCols > VECTOR_OUT_ROWS_MAX_CELLS ){
    return SQLITE4_NOMEM;
  }

  if( rowidLike ){
    assert( nCols == 1 );
    pRows->aIntValues = sqlite4DbMallocRaw(db, nRows * sizeof(i64));
    if( pRows->aIntValues == NULL ){
      return SQLITE4_NOMEM;
    }
  }else{
    pRows->ppValues = sqlite4DbMallocZero(db, nRows * nCols * sizeof(sqlite4_value*));
    if( pRows->ppValues == NULL ){
      return SQLITE4_NOMEM;
    }
  }
  return SQLITE4_OK;
}

int vectorOutRowsPut(VectorOutRows *pRows, int iRow, int iCol, const u64 *pInt, sqlite4_value *pValue) {
  assert( 0 <= iRow && iRow < pRows->nRows );
  assert( 0 <= iCol && iCol < pRows->nCols );
  assert( pRows->aIntValues != NULL || pRows->ppValues != NULL );
  assert( pInt == NULL || pRows->aIntValues != NULL );
  assert( pInt != NULL || pValue != NULL );

  if( pRows->aIntValues != NULL && pInt != NULL ){
    assert( pRows->nCols == 1 );
    pRows->aIntValues[iRow] = (i64)*pInt;
  }else if( pRows->aIntValues != NULL ){
    assert( pRows->nCols == 1 );
    assert( sqlite4_value_type(pValue) == SQLITE4_INTEGER );
    pRows->aIntValues[iRow] = sqlite4_value_int64(pValue);
  }else{
    /* ppValues path: not needed for rowid-like case (sqlite4 port limitation) */
    /* Store a NULL pointer as placeholder; caller must not use this path */
    pRows->ppValues[iRow * pRows->nCols + iCol] = NULL;
  }
  return SQLITE4_OK;
}

void vectorOutRowsGet(sqlite4_context *context, const VectorOutRows *pRows, int iRow, int iCol) {
  assert( 0 <= iRow && iRow < pRows->nRows );
  assert( 0 <= iCol && iCol < pRows->nCols );
  assert( pRows->aIntValues != NULL || pRows->ppValues != NULL );
  if( pRows->aIntValues != NULL ){
    assert( pRows->nCols == 1 );
    sqlite4_result_int64(context, pRows->aIntValues[iRow]);
  }else{
    sqlite4_result_value(context, pRows->ppValues[iRow * pRows->nCols + iCol]);
  }
}

void vectorOutRowsFree(sqlite4 *db, VectorOutRows *pRows) {
  if( pRows->aIntValues != NULL ){
    sqlite4DbFree(db, pRows->aIntValues);
    pRows->aIntValues = NULL;
  }else if( pRows->ppValues != NULL ){
    /* In sqlite4 port, ppValues path is not used (rowid-only).
    ** Values stored there are not individually freed (they are NULL). */
    sqlite4DbFree(db, pRows->ppValues);
    pRows->ppValues = NULL;
  }
}

/**************************************************************************
** Internal type definitions for column type parsing
****************************************************************************/

struct VectorColumnType {
  const char *zName;
  int type;
};

static struct VectorColumnType VECTOR_COLUMN_TYPES[] = {
  { "FLOAT32",    VECTOR_TYPE_FLOAT32 },
  { "F32_BLOB",   VECTOR_TYPE_FLOAT32 },
  { "FLOAT64",    VECTOR_TYPE_FLOAT64 },
  { "F64_BLOB",   VECTOR_TYPE_FLOAT64 },
  { "FLOAT1BIT",  VECTOR_TYPE_FLOAT1BIT },
  { "F1BIT_BLOB", VECTOR_TYPE_FLOAT1BIT },
  { "FLOAT8",     VECTOR_TYPE_FLOAT8 },
  { "F8_BLOB",    VECTOR_TYPE_FLOAT8 },
  { "FLOAT16",    VECTOR_TYPE_FLOAT16 },
  { "F16_BLOB",   VECTOR_TYPE_FLOAT16 },
  { "FLOATB16",   VECTOR_TYPE_FLOATB16 },
  { "FB16_BLOB",  VECTOR_TYPE_FLOATB16 },
};

struct VectorParamName {
  const char *zName;
  int tag;
  int type; /* 0 - string enum, 1 - integer, 2 - float */
  const char *zValueStr;
  u64 value;
};

static struct VectorParamName VECTOR_PARAM_NAMES[] = {
  { "type",               VECTOR_INDEX_TYPE_PARAM_ID,         0, "diskann",   VECTOR_INDEX_TYPE_DISKANN },
  { "metric",             VECTOR_METRIC_TYPE_PARAM_ID,        0, "cosine",    VECTOR_METRIC_TYPE_COS },
  { "metric",             VECTOR_METRIC_TYPE_PARAM_ID,        0, "l2",        VECTOR_METRIC_TYPE_L2 },
  { "compress_neighbors", VECTOR_COMPRESS_NEIGHBORS_PARAM_ID, 0, "float1bit", VECTOR_TYPE_FLOAT1BIT },
  { "compress_neighbors", VECTOR_COMPRESS_NEIGHBORS_PARAM_ID, 0, "float8",    VECTOR_TYPE_FLOAT8 },
  { "compress_neighbors", VECTOR_COMPRESS_NEIGHBORS_PARAM_ID, 0, "float16",   VECTOR_TYPE_FLOAT16 },
  { "compress_neighbors", VECTOR_COMPRESS_NEIGHBORS_PARAM_ID, 0, "floatb16",  VECTOR_TYPE_FLOATB16 },
  { "compress_neighbors", VECTOR_COMPRESS_NEIGHBORS_PARAM_ID, 0, "float32",   VECTOR_TYPE_FLOAT32 },
  { "alpha",              VECTOR_PRUNING_ALPHA_PARAM_ID, 2, 0, 0 },
  { "search_l",           VECTOR_SEARCH_L_PARAM_ID,      1, 0, 0 },
  { "insert_l",           VECTOR_INSERT_L_PARAM_ID,      1, 0, 0 },
  { "max_neighbors",      VECTOR_MAX_NEIGHBORS_PARAM_ID, 1, 0, 0 },
};

static int parseVectorIdxParam(const char *zParam, VectorIdxParams *pParams, const char **pErrMsg) {
  int i, iDelimiter = 0, nValueLen = 0;
  const char *zValue;
  while( zParam[iDelimiter] && zParam[iDelimiter] != '=' ){
    iDelimiter++;
  }
  if( zParam[iDelimiter] != '=' ){
    *pErrMsg = "unexpected parameter format";
    return -1;
  }
  zValue = zParam + iDelimiter + 1;
  nValueLen = sqlite4Strlen30(zValue);
  for(i = 0; i < ArraySize(VECTOR_PARAM_NAMES); i++){
    if( iDelimiter != (int)strlen(VECTOR_PARAM_NAMES[i].zName) || sqlite4_strnicmp(VECTOR_PARAM_NAMES[i].zName, zParam, iDelimiter) != 0 ){
      continue;
    }
    if( VECTOR_PARAM_NAMES[i].type == 1 ){
      int value = sqlite4Atoi(zValue);
      if( value == 0 ){
        *pErrMsg = "invalid representation of integer vector index parameter";
        return -1;
      }
      if( value < 0 ){
        *pErrMsg = "integer vector index parameter must be positive";
        return -1;
      }
      if( vectorIdxParamsPutU64(pParams, VECTOR_PARAM_NAMES[i].tag, value) != 0 ){
        *pErrMsg = "unable to serialize integer vector index parameter";
        return -1;
      }
      return 0;
    }else if( VECTOR_PARAM_NAMES[i].type == 2 ){
      double value;
      char *endptr;
      value = strtod(zValue, &endptr);
      if( endptr == zValue ){
        *pErrMsg = "invalid representation of floating point vector index parameter";
        return -1;
      }
      if( vectorIdxParamsPutF64(pParams, VECTOR_PARAM_NAMES[i].tag, value) != 0 ){
        *pErrMsg = "unable to serialize floating point vector index parameter";
        return -1;
      }
      return 0;
    }else if( VECTOR_PARAM_NAMES[i].type == 0 ){
      if( sqlite4_strnicmp(VECTOR_PARAM_NAMES[i].zValueStr, zValue, nValueLen) == 0 ){
        if( vectorIdxParamsPutU64(pParams, VECTOR_PARAM_NAMES[i].tag, VECTOR_PARAM_NAMES[i].value) != 0 ){
          *pErrMsg = "unable to serialize vector index parameter";
          return -1;
        }
        return 0;
      }
    }else{
      *pErrMsg = "unexpected parameter type";
      return -1;
    }
  }
  *pErrMsg = "invalid parameter";
  return -1;
}

static int parseVectorIdxParams(Parse *pParse, VectorIdxParams *pParams, int type, int dims, ExprListItem *pArgList, int nArgs) {
  int i;
  const char *pErrMsg;
  if( vectorIdxParamsPutU64(pParams, VECTOR_FORMAT_PARAM_ID, VECTOR_FORMAT_DEFAULT) != 0 ){
    sqlite4ErrorMsg(pParse, "vector index: unable to serialize vector index parameter: format");
    return SQLITE4_ERROR;
  }
  if( vectorIdxParamsPutU64(pParams, VECTOR_TYPE_PARAM_ID, type) != 0 ){
    sqlite4ErrorMsg(pParse, "vector index: unable to serialize vector index parameter: type");
    return SQLITE4_ERROR;
  }
  if( vectorIdxParamsPutU64(pParams, VECTOR_DIM_PARAM_ID, dims) != 0 ){
    sqlite4ErrorMsg(pParse, "vector index: unable to serialize vector index parameter: dim");
    return SQLITE4_ERROR;
  }
  for(i = 0; i < nArgs; i++){
    Expr *pArgExpr = pArgList[i].pExpr;
    if( pArgExpr->op != TK_STRING ){
      sqlite4ErrorMsg(pParse, "vector index: all arguments after first must be strings");
      return SQLITE4_ERROR;
    }
    if( parseVectorIdxParam(pArgExpr->u.zToken, pParams, &pErrMsg) != 0 ){
      sqlite4ErrorMsg(pParse, "vector index: invalid vector index parameter '%s': %s", pArgExpr->u.zToken, pErrMsg);
      return SQLITE4_ERROR;
    }
  }
  return SQLITE4_OK;
}

/**************************************************************************
** Vector index cursor implementation
****************************************************************************/

struct VectorIdxCursor {
  sqlite4 *db;            /* Database connection */
  DiskAnnIndex *pIndex;   /* DiskANN index */
};


static void skipSpaces(const char **pzStr) {
  while( **pzStr != '\0' && sqlite4Isspace(**pzStr) ){
    (*pzStr)++;
  }
}

int vectorIdxParseColumnType(const char *zType, int *pType, int *pDims, const char **pErrMsg){
  assert( zType != NULL );

  int dimensions = 0;
  int i;
  skipSpaces(&zType);
  for(i = 0; i < ArraySize(VECTOR_COLUMN_TYPES); i++){
    const char* name = VECTOR_COLUMN_TYPES[i].zName;
    const char* zTypePtr = zType + strlen(name);
    if( sqlite4_strnicmp(zType, name, strlen(name)) != 0 ){
      continue;
    }
    skipSpaces(&zTypePtr);
    if( *zTypePtr != '(' ) {
      break;
    }
    zTypePtr++;
    skipSpaces(&zTypePtr);

    while( *zTypePtr != '\0' && *zTypePtr != ')' && !sqlite4Isspace(*zTypePtr) ){
      if( !sqlite4Isdigit(*zTypePtr) ){
        *pErrMsg = "non digit symbol in vector column parameter";
        return -1;
      }
      dimensions = dimensions*10 + (*zTypePtr - '0');
      if( dimensions > MAX_VECTOR_SZ ) {
        *pErrMsg = "max vector dimension exceeded";
        return -1;
      }
      zTypePtr++;
    }
    skipSpaces(&zTypePtr);
    if( *zTypePtr != ')' ){
      *pErrMsg = "missed closing brace for vector column type";
      return -1;
    }
    zTypePtr++;
    skipSpaces(&zTypePtr);

    if( *zTypePtr != '\0' ) {
      *pErrMsg = "extra data after dimension parameter for vector column type";
      return -1;
    }

    if( dimensions <= 0 ){
      *pErrMsg = "vector column must have non-zero dimension for index";
      return -1;
    }

    *pDims = dimensions;
    *pType = VECTOR_COLUMN_TYPES[i].type;
    return 0;
  }
  *pErrMsg = "unexpected vector column type";
  return -1;
}

static int initVectorIndexMetaTable(sqlite4 *db, const char *zDbSName) {
  int rc;
  static const char *zSqlTemplate = "CREATE TABLE IF NOT EXISTS \"%w\"." VECTOR_INDEX_GLOBAL_META_TABLE " ( name TEXT PRIMARY KEY, metadata BLOB );";
  char *zSql;

  assert( zDbSName != NULL );

  zSql = sqlite4_mprintf(db->pEnv, zSqlTemplate, zDbSName);
  if( zSql == NULL ){
    return SQLITE4_NOMEM;
  }
  rc = sqlite4_exec(db, zSql, 0, 0);
  sqlite4_free(db->pEnv, zSql);
  return rc;
}

static int insertIndexParameters(sqlite4 *db, const char *zDbSName, const char *zName, const VectorIdxParams *pParameters) {
  int rc = SQLITE4_ERROR;
  static const char *zSqlTemplate = "INSERT INTO \"%w\"." VECTOR_INDEX_GLOBAL_META_TABLE " VALUES (?, ?)";
  sqlite4_stmt *pStatement = NULL;
  char *zSql;

  assert( zDbSName != NULL );

  zSql = sqlite4_mprintf(db->pEnv, zSqlTemplate, zDbSName);
  if( zSql == NULL ){
    return SQLITE4_NOMEM;
  }

  rc = sqlite4_prepare(db, zSql, -1, &pStatement, NULL);
  sqlite4_free(db->pEnv, zSql);
  if( rc != SQLITE4_OK ){
    goto clear_and_exit;
  }
  rc = sqlite4_bind_text(pStatement, 1, zName, -1, SQLITE4_STATIC, 0);
  if( rc != SQLITE4_OK ){
    goto clear_and_exit;
  }
  rc = sqlite4_bind_blob(pStatement, 2, pParameters->pBinBuf, pParameters->nBinSize, SQLITE4_STATIC, 0);
  if( rc != SQLITE4_OK ){
    goto clear_and_exit;
  }
  rc = sqlite4_step(pStatement);
  if( (rc&0xff) == SQLITE4_CONSTRAINT ){
    rc = SQLITE4_CONSTRAINT;
  }else if( rc != SQLITE4_DONE ){
    rc = SQLITE4_ERROR;
  }else{
    rc = SQLITE4_OK;
  }
clear_and_exit:
  if( pStatement != NULL ){
    sqlite4_finalize(pStatement);
  }
  return rc;
}

static int removeIndexParameters(sqlite4 *db, const char *zName) {
  static const char *zSql = "DELETE FROM " VECTOR_INDEX_GLOBAL_META_TABLE " WHERE name = ?";
  sqlite4_stmt *pStatement = NULL;
  int rc = SQLITE4_ERROR;

  rc = sqlite4_prepare(db, zSql, -1, &pStatement, NULL);
  if( rc != SQLITE4_OK ){
    goto clear_and_exit;
  }
  rc = sqlite4_bind_text(pStatement, 1, zName, -1, SQLITE4_STATIC, 0);
  if( rc != SQLITE4_OK ){
    goto clear_and_exit;
  }
  rc = sqlite4_step(pStatement);
  if( rc != SQLITE4_DONE ){
    rc = SQLITE4_ERROR;
  } else {
    rc = SQLITE4_OK;
  }
clear_and_exit:
  if( pStatement != NULL ){
    sqlite4_finalize(pStatement);
  }
  return rc;
}

static int vectorIndexTryGetParametersFromBinFormat(sqlite4 *db, const char *zSql, const char *zIdxName, VectorIdxParams *pParams) {
  int rc = SQLITE4_OK;
  sqlite4_stmt *pStmt = NULL;
  int nBinSize;

  vectorIdxParamsInit(pParams, NULL, 0);

  rc = sqlite4_prepare(db, zSql, -1, &pStmt, NULL);
  if( rc != SQLITE4_OK ){
    goto out;
  }
  rc = sqlite4_bind_text(pStmt, 1, zIdxName, -1, SQLITE4_STATIC, 0);
  if( rc != SQLITE4_OK ){
    goto out;
  }
  if( sqlite4_step(pStmt) != SQLITE4_ROW ){
    rc = SQLITE4_ERROR;
    goto out;
  }
  assert( sqlite4_column_type(pStmt, 0) == SQLITE4_BLOB );
  {
    const void *pBlob = sqlite4_column_blob(pStmt, 0, &nBinSize);
    if( nBinSize > VECTOR_INDEX_PARAMS_BUF_SIZE ){
      rc = SQLITE4_ERROR;
      goto out;
    }
    vectorIdxParamsInit(pParams, (u8*)pBlob, nBinSize);
  }
  assert( sqlite4_step(pStmt) == SQLITE4_DONE );
  rc = SQLITE4_OK;
out:
  if( pStmt != NULL ){
    sqlite4_finalize(pStmt);
  }
  return rc;
}

static int vectorIndexGetParameters(
  sqlite4 *db,
  const char *zDbSName,
  const char *zIdxName,
  VectorIdxParams *pParams
) {
  int rc = SQLITE4_OK;
  char *zSelectSql;

  assert( zDbSName != NULL );

  static const char *zSelectSqlTemplate = "SELECT metadata FROM \"%w\"." VECTOR_INDEX_GLOBAL_META_TABLE " WHERE name = ?";
  zSelectSql = sqlite4_mprintf(db->pEnv, zSelectSqlTemplate, zDbSName);
  if( zSelectSql == NULL ){
    return SQLITE4_NOMEM;
  }
  rc = vectorIndexTryGetParametersFromBinFormat(db, zSelectSql, zIdxName, pParams);
  sqlite4_free(db->pEnv, zSelectSql);
  return rc;
}

int vectorIndexDrop(sqlite4 *db, const char *zDbSName, const char *zIdxName) {
  int rcIdx, rcParams;

  assert( zDbSName != NULL );

  rcIdx = diskAnnDropIndex(db, zDbSName, zIdxName);
  rcParams = removeIndexParameters(db, zIdxName);
  return rcIdx != SQLITE4_OK ? rcIdx : rcParams;
}

int vectorIndexClear(sqlite4 *db, const char *zDbSName, const char *zIdxName) {
  assert( zDbSName != NULL );
  return diskAnnClearIndex(db, zDbSName, zIdxName);
}

/*
** vectorIndexCreate analyzes any index creation expression and creates a
** vector index if the expression contains the VECTOR_INDEX_MARKER_FUNCTION.
**
** Returns:
**  -1 on error (pParse->zErrMsg set)
**   0 if this is not a vector index (ignore)
**   1 if vector index created but refill should be skipped
**   2 if vector index created and refill is needed
*/
int vectorIndexCreate(Parse *pParse, const Index *pIdx, const char *zDbSName, const IdList *pUsing, ExprList *pColExpr) {
  static const int CREATE_FAIL = -1;
  static const int CREATE_IGNORE = 0;
  static const int CREATE_OK_SKIP_REFILL = 1;
  static const int CREATE_OK = 2;

  int i, rc = SQLITE4_OK;
  int dims, type;
  int hasLibsqlVectorIdxFn = 0, hasCollation = 0;
  const char *pzErrMsg = NULL;

  assert( zDbSName != NULL );

  sqlite4 *db = pParse->db;
  Table *pTable = pIdx->pTable;
  ExprListItem *pListItem;
  ExprList *pArgsList;
  int iEmbeddingColumn;
  char *zEmbeddingColumnTypeName;
  VectorIdxKey idxKey;
  VectorIdxParams idxParams;
  vectorIdxParamsInit(&idxParams, NULL, 0);

  if( IN_DECLARE_VTAB ){
    return CREATE_IGNORE;
  }

  /* Deprecated USING syntax: reject for new indices */
  if( pParse->db->init.busy == 0 && pUsing != NULL ){
    if( pIdx->zName != NULL && pTable->zName != NULL ){
      sqlite4ErrorMsg(pParse, "vector index: USING syntax is deprecated, use CREATE INDEX with " VECTOR_INDEX_MARKER_FUNCTION "()");
    } else {
      sqlite4ErrorMsg(pParse, "vector index: USING syntax is deprecated");
    }
    return CREATE_FAIL;
  }
  if( db->init.busy == 1 && pUsing != NULL ){
    return CREATE_OK;
  }

  /* Vector index must have expressions over column */
  if( pColExpr == NULL ) {
    return CREATE_IGNORE;
  }

  pListItem = pColExpr->a;
  for(i=0; i<pColExpr->nExpr; i++, pListItem++){
    Expr *pExpr = pListItem->pExpr;
    if( pExpr==0 ) continue;
    while( pExpr->op == TK_COLLATE ){
      pExpr = pExpr->pLeft;
      hasCollation = 1;
    }
    if( pExpr->op == TK_FUNCTION && sqlite4_stricmp(pExpr->u.zToken, VECTOR_INDEX_MARKER_FUNCTION) == 0 ) {
      hasLibsqlVectorIdxFn = 1;
    }
  }
  if( !hasLibsqlVectorIdxFn ) {
    return CREATE_IGNORE;
  }
  if( hasCollation ){
    sqlite4ErrorMsg(pParse, "vector index: collation in expression is forbidden");
    return CREATE_FAIL;
  }
  if( pColExpr->nExpr != 1 ) {
    sqlite4ErrorMsg(pParse, "vector index: must contain exactly one column wrapped into the " VECTOR_INDEX_MARKER_FUNCTION " function");
    return CREATE_FAIL;
  }

  pArgsList = pColExpr->a[0].pExpr->x.pList;
  pListItem = pArgsList->a;

  if( pArgsList->nExpr < 1 ){
    sqlite4ErrorMsg(pParse, "vector index: " VECTOR_INDEX_MARKER_FUNCTION " must contain at least one argument");
    return CREATE_FAIL;
  }
  if( pListItem[0].pExpr->op == TK_COLUMN ) {
    iEmbeddingColumn = pListItem[0].pExpr->iColumn;
  }else if( pListItem[0].pExpr->op == TK_ID ) {
    /* In sqlite4, expression index args are parsed as TK_ID (not resolved
    ** to TK_COLUMN). Look up the column name in the table manually. */
    const char *zColName = pListItem[0].pExpr->u.zToken;
    int k;
    iEmbeddingColumn = -1;
    for(k=0; k<pTable->nCol; k++){
      if( sqlite4_stricmp(pTable->aCol[k].zName, zColName)==0 ){
        iEmbeddingColumn = k;
        break;
      }
    }
  }else{
    sqlite4ErrorMsg(pParse, "vector index: " VECTOR_INDEX_MARKER_FUNCTION " first argument must be a column");
    return CREATE_FAIL;
  }
  if( iEmbeddingColumn < 0 ){
    sqlite4ErrorMsg(pParse, "vector index: " VECTOR_INDEX_MARKER_FUNCTION " first argument must be column with vector type");
    return CREATE_FAIL;
  }
  assert( iEmbeddingColumn >= 0 && iEmbeddingColumn < pTable->nCol );

  /* In sqlite4, column type is stored as pTable->aCol[i].zType */
  zEmbeddingColumnTypeName = pTable->aCol[iEmbeddingColumn].zType;
  if( zEmbeddingColumnTypeName == NULL ) zEmbeddingColumnTypeName = "";
  if( vectorIdxParseColumnType(zEmbeddingColumnTypeName, &type, &dims, &pzErrMsg) != 0 ){
    sqlite4ErrorMsg(pParse, "vector index: %s: %s", pzErrMsg, zEmbeddingColumnTypeName);
    return CREATE_FAIL;
  }

  /* During db initialization, schema is locked so just mark index as valid */
  if( db->init.busy == 1 ){
    return CREATE_OK;
  }

  rc = initVectorIndexMetaTable(db, zDbSName);
  if( rc != SQLITE4_OK ){
    sqlite4ErrorMsg(pParse, "vector index: failed to init meta table: %s", sqlite4_errmsg(db));
    return CREATE_FAIL;
  }
  rc = parseVectorIdxParams(pParse, &idxParams, type, dims, pListItem + 1, pArgsList->nExpr - 1);
  if( rc != SQLITE4_OK ){
    return CREATE_FAIL;
  }
  if( vectorIdxKeyGet(pIdx, &idxKey, &pzErrMsg) != 0 ){
    sqlite4ErrorMsg(pParse, "vector index: failed to detect underlying table key: %s", pzErrMsg);
    return CREATE_FAIL;
  }
  if( idxKey.nKeyColumns != 1 ){
    sqlite4ErrorMsg(pParse, "vector index: unsupported for tables without a single-column INTEGER primary key");
    return CREATE_FAIL;
  }
  if( !vectorIdxKeyRowidLike(&idxKey) ){
    sqlite4ErrorMsg(pParse, "vector index: only tables with single INTEGER PRIMARY KEY are supported");
    return CREATE_FAIL;
  }
  rc = diskAnnCreateIndex(db, zDbSName, pIdx->zName, &idxKey, &idxParams, &pzErrMsg);
  if( rc != SQLITE4_OK ){
    if( pzErrMsg != NULL ){
      sqlite4ErrorMsg(pParse, "vector index: unable to initialize diskann: %s", pzErrMsg);
    }else{
      sqlite4ErrorMsg(pParse, "vector index: unable to initialize diskann");
    }
    return CREATE_FAIL;
  }
  rc = insertIndexParameters(db, zDbSName, pIdx->zName, &idxParams);

  if( (rc&0xff) == SQLITE4_CONSTRAINT ){
    /* Parameters already exist (e.g. loading a dump) - skip refill */
    return CREATE_OK_SKIP_REFILL;
  }
  if( rc != SQLITE4_OK ){
    sqlite4ErrorMsg(pParse, "vector index: unable to update global metadata table");
    return CREATE_FAIL;
  }
  return CREATE_OK;
}

/*
** vectorIndexSearch: search k nearest neighbors for the query vector.
** argv[0] = index name (TEXT)
** argv[1] = query vector (BLOB or TEXT)
** argv[2] = k (INTEGER)
*/
int vectorIndexSearch(
  sqlite4 *db,
  int argc,
  sqlite4_value **argv,
  VectorOutRows *pRows,
  int *nReads,
  int *nWrites,
  char **pzErrMsg
) {
  int type, dims, k, rc;
  double kDouble;
  const char *zIdxName;
  const char *zIdxDbSName = "main";
  const char *zErrMsg;
  Vector *pVector = NULL;
  DiskAnnIndex *pDiskAnn = NULL;
  Index *pIndex;
  VectorIdxKey pKey;
  VectorIdxParams idxParams;
  sqlite4_env *pEnv = db->pEnv;
  vectorIdxParamsInit(&idxParams, NULL, 0);

  if( argc != 3 ){
    *pzErrMsg = sqlite4_mprintf(pEnv, "vector index(search): got %d parameters, expected 3", argc);
    rc = SQLITE4_ERROR;
    goto out;
  }
  if( detectVectorParameters(argv[1], VECTOR_TYPE_FLOAT32, &type, &dims, pzErrMsg) != 0 ){
    rc = SQLITE4_ERROR;
    goto out;
  }

  pVector = vectorAlloc(type, dims);
  if( pVector == NULL ){
    rc = SQLITE4_NOMEM;
    goto out;
  }
  if( vectorParseWithType(argv[1], pVector, pzErrMsg) != 0 ){
    rc = SQLITE4_ERROR;
    goto out;
  }
  if( sqlite4_value_type(argv[2]) == SQLITE4_INTEGER ){
    k = sqlite4_value_int(argv[2]);
    if( k < 0 ){
      *pzErrMsg = sqlite4_mprintf(pEnv, "vector index(search): k must be a non-negative integer");
      rc = SQLITE4_ERROR;
      goto out;
    }
  }else if( sqlite4_value_type(argv[2]) == SQLITE4_FLOAT ) {
    kDouble = sqlite4_value_double(argv[2]);
    k = (int)kDouble;
    if( (double)k != kDouble ){
      *pzErrMsg = sqlite4_mprintf(pEnv, "vector index(search): k must be an integer, but float provided");
      rc = SQLITE4_ERROR;
      goto out;
    }
    if( k < 0 ){
      *pzErrMsg = sqlite4_mprintf(pEnv, "vector index(search): k must be a non-negative integer");
      rc = SQLITE4_ERROR;
      goto out;
    }
  }else{
    *pzErrMsg = sqlite4_mprintf(pEnv, "vector index(search): k must be an integer");
    rc = SQLITE4_ERROR;
    goto out;
  }

  if( sqlite4_value_type(argv[0]) != SQLITE4_TEXT ){
    *pzErrMsg = sqlite4_mprintf(pEnv, "vector index(search): first parameter (index) must be a string");
    rc = SQLITE4_ERROR;
    goto out;
  }
  zIdxName = (const char*)sqlite4_value_text(argv[0], 0);

  if( vectorIndexGetParameters(db, zIdxDbSName, zIdxName, &idxParams) != SQLITE4_OK ){
    *pzErrMsg = sqlite4_mprintf(pEnv, "vector index(search): failed to parse vector index parameters");
    rc = SQLITE4_ERROR;
    goto out;
  }
  pIndex = sqlite4FindIndex(db, zIdxName, zIdxDbSName);
  if( pIndex == NULL ){
    *pzErrMsg = sqlite4_mprintf(pEnv, "vector index(search): index not found");
    rc = SQLITE4_ERROR;
    goto out;
  }
  rc = diskAnnOpenIndex(db, zIdxDbSName, zIdxName, &idxParams, &pDiskAnn);
  if( rc != SQLITE4_OK ){
    *pzErrMsg = sqlite4_mprintf(pEnv, "vector index(search): failed to open diskann index");
    goto out;
  }
  if( vectorIdxKeyGet(pIndex, &pKey, &zErrMsg) != 0 ){
    *pzErrMsg = sqlite4_mprintf(pEnv, "vector index(search): failed to extract table key: %s", zErrMsg);
    rc = SQLITE4_ERROR;
    goto out;
  }
  rc = diskAnnSearch(pDiskAnn, pVector, k, &pKey, pRows, pzErrMsg);
out:
  if( pDiskAnn != NULL ){
    *nReads += pDiskAnn->nReads;
    *nWrites += pDiskAnn->nWrites;
    diskAnnCloseIndex(pDiskAnn);
  }
  if( pVector != NULL ){
    vectorFree(pVector);
  }
  return rc;
}

int vectorIndexInsert(
  VectorIdxCursor *pCur,
  i64 rowid,
  sqlite4_value *pVector,
  char **pzErrMsg
){
  int rc;
  VectorInRow vectorInRow;

  rc = vectorInRowAlloc(pCur->db, rowid, pVector, &vectorInRow, pzErrMsg);
  if( rc != SQLITE4_OK ){
    return rc;
  }
  if( vectorInRow.pVector == NULL ){
    /* NULL vector - skip insertion */
    return SQLITE4_OK;
  }
  rc = diskAnnInsert(pCur->pIndex, &vectorInRow, pzErrMsg);
  vectorInRowFree(pCur->db, &vectorInRow);
  return rc;
}

int vectorIndexDelete(
  VectorIdxCursor *pCur,
  i64 rowid,
  char **pzErrMsg
){
  VectorInRow payload;

  memset(&payload, 0, sizeof(payload));
  payload.pVector = NULL;
  payload.nKeys = 1;
  payload.nRowid = (u64)rowid;
  return diskAnnDelete(pCur->pIndex, &payload, pzErrMsg);
}

int vectorIndexCursorInit(
  sqlite4 *db,
  const char *zDbSName,
  const char *zIndexName,
  VectorIdxCursor **ppCursor
){
  int rc;
  VectorIdxCursor *pCursor;
  VectorIdxParams params;
  vectorIdxParamsInit(&params, NULL, 0);

  assert( zDbSName != NULL );

  if( vectorIndexGetParameters(db, zDbSName, zIndexName, &params) != SQLITE4_OK ){
    return SQLITE4_ERROR;
  }
  pCursor = sqlite4DbMallocZero(db, sizeof(VectorIdxCursor));
  if( pCursor == 0 ){
    return SQLITE4_NOMEM;
  }
  rc = diskAnnOpenIndex(db, zDbSName, zIndexName, &params, &pCursor->pIndex);
  if( rc != SQLITE4_OK ){
    sqlite4DbFree(db, pCursor);
    return rc;
  }
  pCursor->db = db;
  *ppCursor = pCursor;
  return SQLITE4_OK;
}

void vectorIndexCursorClose(sqlite4 *db, VectorIdxCursor *pCursor, int *nReads, int *nWrites){
  *nReads = pCursor->pIndex->nReads;
  *nWrites = pCursor->pIndex->nWrites;

  diskAnnCloseIndex(pCursor->pIndex);
  sqlite4DbFree(db, pCursor);
}

#endif /* !defined(SQLITE4_OMIT_VECTOR) */
