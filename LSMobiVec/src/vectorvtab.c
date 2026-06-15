/*
** 2024-04-25
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
** libSQL vector search virtual table for sqlite4.
*/
#if !defined(SQLITE4_OMIT_VECTOR) && !defined(SQLITE4_OMIT_VIRTUALTABLE)
#include "sqliteInt.h"
#include "vdbeInt.h"
#include "vectorIndexInt.h"

typedef struct vectorVtab vectorVtab;
struct vectorVtab {
  sqlite4_vtab base;       /* Base class - must be first */
  sqlite4 *db;             /* Database connection */
};

typedef struct vectorVtab_cursor vectorVtab_cursor;
struct vectorVtab_cursor {
  sqlite4_vtab_cursor base;  /* Base class - must be first */
  int nReads;                /* Number of rows read */
  int nWrites;               /* Number of rows written */
  VectorOutRows rows;
  int iRow;
};

/* Column numbers */
#define VECTOR_COLUMN_IDX    0
#define VECTOR_COLUMN_VECTOR 1
#define VECTOR_COLUMN_K      2
#define VECTOR_COLUMN_OFFSET 3

static int vectorVtabConnect(
  sqlite4 *db,
  void *pAux,
  int argc, const char *const *argv,
  sqlite4_vtab **ppVtab,
  char **pzErr
){
  vectorVtab *pVtab = NULL;
  int rc;
  rc = sqlite4_declare_vtab(db, "CREATE TABLE x(idx hidden, vector hidden, k hidden, id);");
  if( rc != SQLITE4_OK ){
    return rc;
  }
  pVtab = sqlite4_malloc(sqlite4_env_default(), sizeof(vectorVtab));
  if( pVtab == NULL ){
    return SQLITE4_NOMEM;
  }
  memset(pVtab, 0, sizeof(*pVtab));
  pVtab->db = db;
  *ppVtab = (sqlite4_vtab*)pVtab;
  return SQLITE4_OK;
}

static int vectorVtabDisconnect(sqlite4_vtab *pVtab){
  sqlite4_free(sqlite4_env_default(), pVtab);
  return SQLITE4_OK;
}

static int vectorVtabOpen(sqlite4_vtab *p, sqlite4_vtab_cursor **ppCursor){
  vectorVtab_cursor *pCur;
  pCur = sqlite4_malloc(sqlite4_env_default(), sizeof(vectorVtab_cursor));
  if( pCur == NULL ){
    return SQLITE4_NOMEM;
  }
  memset(pCur, 0, sizeof(*pCur));
  *ppCursor = &pCur->base;
  return SQLITE4_OK;
}

static int vectorVtabClose(sqlite4_vtab_cursor *cur){
  vectorVtab_cursor *pCur = (vectorVtab_cursor*)cur;
  vectorVtab *pVTab = (vectorVtab *)cur->pVtab;
  vectorOutRowsFree(pVTab->db, &pCur->rows);
  sqlite4_free(sqlite4_env_default(), pCur);
  return SQLITE4_OK;
}

static int vectorVtabNext(sqlite4_vtab_cursor *cur){
  vectorVtab_cursor *pCur = (vectorVtab_cursor*)cur;
  pCur->iRow++;
  return SQLITE4_OK;
}

static int vectorVtabEof(sqlite4_vtab_cursor *cur){
  vectorVtab_cursor *pCur = (vectorVtab_cursor*)cur;
  return pCur->iRow >= pCur->rows.nRows;
}

static int vectorVtabColumn(
  sqlite4_vtab_cursor *cur,
  sqlite4_context *context,
  int iCol
){
  vectorVtab_cursor *pCur = (vectorVtab_cursor*)cur;
  vectorOutRowsGet(context, &pCur->rows, pCur->iRow, iCol - VECTOR_COLUMN_OFFSET);
  return SQLITE4_OK;
}

static int vectorVtabRowid(sqlite4_vtab_cursor *cur, sqlite4_int64 *pRowid){
  vectorVtab_cursor *pCur = (vectorVtab_cursor*)cur;
  if( pCur->rows.aIntValues != NULL ){
    *pRowid = pCur->rows.aIntValues[pCur->iRow];
  }else{
    *pRowid = pCur->iRow;
  }
  return SQLITE4_OK;
}

static int vectorVtabFilter(
  sqlite4_vtab_cursor *pVtabCursor,
  int idxNum, const char *idxStr,
  int argc, sqlite4_value **argv
){
  vectorVtab_cursor *pCur = (vectorVtab_cursor *)pVtabCursor;
  vectorVtab *pVTab = (vectorVtab *)pVtabCursor->pVtab;
  pCur->rows.aIntValues = NULL;
  pCur->rows.ppValues = NULL;

  if( vectorIndexSearch(pVTab->db, argc, argv, &pCur->rows, &pCur->nReads, &pCur->nWrites, &pVTab->base.zErrMsg) != 0 ){
    return SQLITE4_ERROR;
  }

  assert( pCur->rows.nRows >= 0 );
  assert( pCur->rows.nCols > 0 );
  pCur->iRow = 0;
  return SQLITE4_OK;
}

static int vectorVtabBestIndex(
  sqlite4_vtab *tab,
  sqlite4_index_info *pIdxInfo
){
  const struct sqlite4_index_constraint *pConstraint;
  int i;

  pIdxInfo->estimatedCost = (double)1;
  pIdxInfo->idxNum = 1;

  pConstraint = pIdxInfo->aConstraint;
  for(i=0; i<pIdxInfo->nConstraint; i++, pConstraint++){
    if( pConstraint->usable == 0 ) continue;
    if( pConstraint->op != SQLITE4_INDEX_CONSTRAINT_EQ ) continue;
    switch( pConstraint->iColumn ){
      case VECTOR_COLUMN_IDX:
        pIdxInfo->aConstraintUsage[i].argvIndex = 1;
        pIdxInfo->aConstraintUsage[i].omit = 1;
        break;
      case VECTOR_COLUMN_VECTOR:
        pIdxInfo->aConstraintUsage[i].argvIndex = 2;
        pIdxInfo->aConstraintUsage[i].omit = 1;
        break;
      case VECTOR_COLUMN_K:
        pIdxInfo->aConstraintUsage[i].argvIndex = 3;
        pIdxInfo->aConstraintUsage[i].omit = 1;
        break;
    }
  }
  return SQLITE4_OK;
}

static sqlite4_module vectorModule = {
  /* iVersion    */ 0,
  /* xCreate     */ 0,
  /* xConnect    */ vectorVtabConnect,
  /* xBestIndex  */ vectorVtabBestIndex,
  /* xDisconnect */ vectorVtabDisconnect,
  /* xDestroy    */ 0,
  /* xOpen       */ vectorVtabOpen,
  /* xClose      */ vectorVtabClose,
  /* xFilter     */ vectorVtabFilter,
  /* xNext       */ vectorVtabNext,
  /* xEof        */ vectorVtabEof,
  /* xColumn     */ vectorVtabColumn,
  /* xRowid      */ vectorVtabRowid,
  /* xUpdate     */ 0,
  /* xBegin      */ 0,
  /* xSync       */ 0,
  /* xCommit     */ 0,
  /* xRollback   */ 0,
  /* xFindMethod */ 0,
  /* xRename     */ 0,
  /* xSavepoint  */ 0,
  /* xRelease    */ 0,
  /* xRollbackTo */ 0,
};

int vectorVtabInit(sqlite4 *db){
  return sqlite4_create_module(db, VECTOR_INDEX_VTAB_NAME, &vectorModule, 0);
}
#else
/* Stub when vector or vtab is disabled */
int vectorVtabInit(void *db){ return 0; }
#endif /* !defined(SQLITE4_OMIT_VECTOR) && !defined(SQLITE4_OMIT_VIRTUALTABLE) */
