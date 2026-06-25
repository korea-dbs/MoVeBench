/*
** 2001 September 15
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** Main file for the SQLite library.  The routines in this file
** implement the programmer interface to the library.  Routines in
** other files are for internal use by SQLite and should not be
** accessed by users of the library.
*/

#include <sys/time.h>
#include "sqliteInt.h"

static double shell_exec_now_ms(void){
  struct timeval tv;
  gettimeofday(&tv, 0);
  return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
}

static double gShellExecMs = 0.0;
static double gShellPrepareMs = 0.0;
static double gShellStepMs = 0.0;
static double gShellFinalizeMs = 0.0;
static int gShellStmtCount = 0;

extern void sqlite4_step_timing_report(void);

void sqlite4_shell_timing_report(void){
  double shellOtherMs = gShellExecMs - gShellPrepareMs - gShellStepMs - gShellFinalizeMs;
  if( shellOtherMs<0.0 ) shellOtherMs = 0.0;
  fprintf(stderr, "shell statements: %d\n", gShellStmtCount);
  fprintf(stderr, "shell prepare: %.1f ms\n", gShellPrepareMs);
  fprintf(stderr, "shell step: %.1f ms\n", gShellStepMs);
  fprintf(stderr, "shell finalize: %.1f ms\n", gShellFinalizeMs);
  fprintf(stderr, "shell other: %.1f ms\n", shellOtherMs);
  sqlite4_step_timing_report();
}

/*
** Execute SQL code.  Return one of the SQLITE4_ success/failure
** codes.  Also write an error message into memory obtained from
** malloc() and make *pzErrMsg point to that message.
**
** If the SQL is a query, then for each row in the query result
** the xCall() function is called.  pArg becomes the first
** argument to xCall().  If xCall=NULL then no callback
** is invoked, even for queries.
*/
int sqlite4_exec(
  sqlite4 *db,                /* The database on which the SQL executes */
  const char *zSql,           /* The SQL to be executed */
  int (*xCall)(void*,int,sqlite4_value**,const char**),  /* Callback function */
  void *pArg                  /* First argument to xCall() */
){
  int rc = SQLITE4_OK;        /* Return code */
  int nRetry = 0;             /* Number of retry attempts */
  int bAbort = 0;             /* Set to true if callback returns non-zero */
  double shellExecStartMs = shell_exec_now_ms();

  if( !sqlite4SafetyCheckOk(db) ) return SQLITE4_MISUSE_BKPT;
  if( zSql==0 ) zSql = "";

  sqlite4_mutex_enter(db->mutex);
  sqlite4Error(db, SQLITE4_OK, 0);

  /* Loop until we run out of SQL to execute, or the callback function
  ** returns non-zero, or an error occurs.  */
  while( zSql[0] && bAbort==0
     && (rc==SQLITE4_OK || (rc==SQLITE4_SCHEMA && (++nRetry)<2))
  ){
    int nUsed;                    /* Length of first SQL statement in zSql */
    int nCol;                     /* Number of returned columns */
    sqlite4_stmt *pStmt = 0;      /* The current SQL statement */
    const char **azCol = 0;       /* Names of result columns */
    sqlite4_value **apVal = 0;    /* Row of value objects */
    double t0;

    pStmt = 0;
    t0 = shell_exec_now_ms();
    rc = sqlite4_prepare(db, zSql, -1, &pStmt, &nUsed);
    gShellPrepareMs += shell_exec_now_ms() - t0;
    assert( rc==SQLITE4_OK || pStmt==0 );
    if( rc!=SQLITE4_OK ){
      continue;
    }
    if( !pStmt ){
      /* this happens for a comment or white-space */
      zSql += nUsed;
      continue;
    }
    gShellStmtCount++;

    nCol = sqlite4_column_count(pStmt);
    do {

      /* Step the statement. Then invoke the callback function if required */
      t0 = shell_exec_now_ms();
      rc = sqlite4_step(pStmt);
      gShellStepMs += shell_exec_now_ms() - t0;
      if( xCall && SQLITE4_ROW==rc ){
        if( azCol==0 ){
          int nAlloc;             /* Bytes of space to allocate */

          nAlloc = (sizeof(char *) + sizeof(sqlite4_value *)) * nCol;
          azCol = (const char **)sqlite4DbMallocZero(db, nAlloc);
          if( azCol ){
            int i;                /* Used to iterate through result columns */
            apVal = (sqlite4_value **)&azCol[nCol];
            for(i=0; i<nCol; i++){
              azCol[i] = sqlite4_column_name(pStmt, i);
              /* sqlite4VdbeSetColName() installs column names as UTF8
              ** strings so there is no way for column_name() to fail. */
              assert( azCol[i]!=0 );
            }
          }
        }

        if( azCol ){
          int i;                  /* Used to iterate through result columns */
          for(i=0; i<nCol; i++){
            apVal[i] = sqlite4ColumnValue(pStmt, i);
            assert( apVal[i]!=0 );
          }
          bAbort = xCall(pArg, nCol, apVal, azCol);
        }
      }

      if( bAbort || rc!=SQLITE4_ROW ){
        t0 = shell_exec_now_ms();
        rc = sqlite4VdbeFinalize((Vdbe *)pStmt);
        gShellFinalizeMs += shell_exec_now_ms() - t0;
        pStmt = 0;
        if( rc!=SQLITE4_SCHEMA ){
          nRetry = 0;
          zSql += nUsed;
          while( sqlite4Isspace(zSql[0]) ) zSql++;
        }
        assert( rc!=SQLITE4_ROW );
      }
    }while( rc==SQLITE4_ROW );

    sqlite4DbFree(db, azCol);
  }

  if( bAbort ) rc = SQLITE4_ABORT;
  rc = sqlite4ApiExit(db, rc);

  sqlite4_mutex_leave(db->mutex);
  gShellExecMs += shell_exec_now_ms() - shellExecStartMs;
  return rc;
}
