/*
** 2006 June 10
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file contains code used to help implement virtual tables.
** Ported from sqlite3 (veclite/src/vtab.c) to sqlite4 API.
*/
#ifndef SQLITE4_OMIT_VIRTUALTABLE
#include "sqliteInt.h"

/* Forward declarations for functions used before their definition */
static void addModuleArgument(Parse *pParse, Table *pTable, char *zArg);

/*
** Before a virtual table xCreate() or xConnect() method is invoked, the
** sqlite4.pVtabCtx member variable is set to point to an instance of
** this struct allocated on the stack. It is used by the implementation of
** the sqlite4_declare_vtab() and sqlite4_vtab_config() APIs, both of which
** are invoked only from within xCreate and xConnect methods.
*/
struct VtabCtx {
  VTable *pVTable;    /* The virtual table being constructed */
  Table *pTab;        /* The Table object to which the virtual table belongs */
  VtabCtx *pPrior;    /* Parent context (if any) */
  int bDeclared;      /* True after sqlite4_declare_vtab() is called */
};

/*
** Construct and install a Module object for a virtual table.  When this
** routine is called, it is guaranteed that all appropriate locks are held
** and the module is not already part of the connection.
**
** If there already exists a module with zName, replace it with the new one.
** If pModule==0, then delete the module zName if it exists.
*/
Module *sqlite4VtabCreateModule(
  sqlite4 *db,                        /* Database in which module is registered */
  const char *zName,                  /* Name assigned to this module */
  const sqlite4_module *pModule,      /* The definition of the module */
  void *pAux,                         /* Context pointer for xCreate/xConnect */
  void (*xDestroy)(void *)            /* Module destructor function */
){
  Module *pMod;
  Module *pDel;
  char *zCopy;
  if( pModule==0 ){
    zCopy = (char*)zName;
    pMod = 0;
  }else{
    int nName = sqlite4Strlen30(zName);
    pMod = (Module *)sqlite4Malloc(db->pEnv, sizeof(Module) + nName + 1);
    if( pMod==0 ){
      db->mallocFailed = 1;
      return 0;
    }
    memset(pMod, 0, sizeof(*pMod));
    zCopy = (char *)(&pMod[1]);
    memcpy(zCopy, zName, nName+1);
    pMod->zName = zCopy;
    pMod->pModule = pModule;
    pMod->pAux = pAux;
    pMod->xDestroy = xDestroy;
    pMod->pEpoTab = 0;
    pMod->nRefModule = 1;
  }
  pDel = (Module *)sqlite4HashInsert(&db->aModule, zCopy,
                                     sqlite4Strlen30(zCopy), (void*)pMod);
  if( pDel ){
    if( pDel==pMod ){
      db->mallocFailed = 1;
      sqlite4DbFree(db, pDel);
      pMod = 0;
    }else{
      sqlite4VtabEponymousTableClear(db, pDel);
      sqlite4VtabModuleUnref(db, pDel);
    }
  }
  return pMod;
}

/*
** The actual function that does the work of creating a new module.
** This function implements the sqlite4_create_module() and
** sqlite4_create_module_v2() interfaces.
*/
static int createModule(
  sqlite4 *db,                        /* Database in which module is registered */
  const char *zName,                  /* Name assigned to this module */
  const sqlite4_module *pModule,      /* The definition of the module */
  void *pAux,                         /* Context pointer for xCreate/xConnect */
  void (*xDestroy)(void *)            /* Module destructor function */
){
  int rc = SQLITE4_OK;

  sqlite4_mutex_enter(db->mutex);
  (void)sqlite4VtabCreateModule(db, zName, pModule, pAux, xDestroy);
  rc = sqlite4ApiExit(db, rc);
  if( rc!=SQLITE4_OK && xDestroy ) xDestroy(pAux);
  sqlite4_mutex_leave(db->mutex);
  return rc;
}


/*
** External API function used to create a new virtual-table module.
*/
int sqlite4_create_module(
  sqlite4 *db,                    /* Database in which module is registered */
  const char *zName,              /* Name assigned to this module */
  const sqlite4_module *pModule,  /* The definition of the module */
  void *pAux                      /* Context pointer for xCreate/xConnect */
){
  return createModule(db, zName, pModule, pAux, 0);
}

/*
** External API function used to create a new virtual-table module.
*/
int sqlite4_create_module_v2(
  sqlite4 *db,                    /* Database in which module is registered */
  const char *zName,              /* Name assigned to this module */
  const sqlite4_module *pModule,  /* The definition of the module */
  void *pAux,                     /* Context pointer for xCreate/xConnect */
  void (*xDestroy)(void *)        /* Module destructor function */
){
  return createModule(db, zName, pModule, pAux, xDestroy);
}

/*
** Decrement the reference count on a Module object.  Destroy the
** module when the reference count reaches zero.
*/
void sqlite4VtabModuleUnref(sqlite4 *db, Module *pMod){
  assert( pMod->nRefModule>0 );
  pMod->nRefModule--;
  if( pMod->nRefModule==0 ){
    if( pMod->xDestroy ){
      pMod->xDestroy(pMod->pAux);
    }
    assert( pMod->pEpoTab==0 );
    sqlite4DbFree(db, pMod);
  }
}

/*
** Lock the virtual table so that it cannot be disconnected.
** Locks nest.  Every lock should have a corresponding unlock.
** If an unlock is omitted, resources leaks will occur.
**
** If a disconnect is attempted while a virtual table is locked,
** the disconnect is deferred until all locks have been removed.
*/
void sqlite4VtabLock(VTable *pVTab){
  pVTab->nRef++;
}


/*
** pTab is a pointer to a Table structure representing a virtual-table.
** Return a pointer to the VTable object used by connection db to access
** this virtual-table, if one has been created, or NULL otherwise.
*/
VTable *sqlite4GetVTable(sqlite4 *db, Table *pTab){
  VTable *pVtab;
  assert( IsVirtual(pTab) );
  for(pVtab=pTab->pVTable; pVtab && pVtab->db!=db; pVtab=pVtab->pNext);
  return pVtab;
}

/*
** Decrement the ref-count on a virtual table object. When the ref-count
** reaches zero, call the xDisconnect() method to delete the object.
*/
void sqlite4VtabUnlock(VTable *pVTab){
  sqlite4 *db = pVTab->db;

  assert( db );
  assert( pVTab->nRef>0 );
  assert( sqlite4SafetyCheckOk(db) );

  pVTab->nRef--;
  if( pVTab->nRef==0 ){
    sqlite4_vtab *p = pVTab->pVtab;
    if( p ){
      p->pModule->xDisconnect(p);
    }
    sqlite4VtabModuleUnref(pVTab->db, pVTab->pMod);
    sqlite4DbFree(db, pVTab);
  }
}

/*
** Table p is a virtual table. This function moves all elements in the
** p->pVTable list to the sqlite4.pDisconnect lists of their associated
** database connections to be disconnected at the next opportunity.
** Except, if argument db is not NULL, then the entry associated with
** connection db is left in the p->pVTable list.
*/
static VTable *vtabDisconnectAll(sqlite4 *db, Table *p){
  VTable *pRet = 0;
  VTable *pVTable;

  assert( IsVirtual(p) );
  pVTable = p->pVTable;
  p->pVTable = 0;

  while( pVTable ){
    sqlite4 *db2 = pVTable->db;
    VTable *pNext = pVTable->pNext;
    assert( db2 );
    if( db2==db ){
      pRet = pVTable;
      p->pVTable = pRet;
      pRet->pNext = 0;
    }else{
      pVTable->pNext = db2->pDisconnect;
      db2->pDisconnect = pVTable;
    }
    pVTable = pNext;
  }

  assert( !db || pRet );
  return pRet;
}

/*
** Table *p is a virtual table. This function removes the VTable object
** for table *p associated with database connection db from the linked
** list in p->pVTable. It also decrements the VTable ref count.
*/
void sqlite4VtabDisconnect(sqlite4 *db, Table *p){
  VTable **ppVTab;

  assert( IsVirtual(p) );

  for(ppVTab=&p->pVTable; *ppVTab; ppVTab=&(*ppVTab)->pNext){
    if( (*ppVTab)->db==db  ){
      VTable *pVTab = *ppVTab;
      *ppVTab = pVTab->pNext;
      sqlite4VtabUnlock(pVTab);
      break;
    }
  }
}


/*
** Disconnect all the virtual table objects in the sqlite4.pDisconnect list.
*/
void sqlite4VtabUnlockList(sqlite4 *db){
  VTable *p = db->pDisconnect;

  if( p ){
    db->pDisconnect = 0;
    do {
      VTable *pNext = p->pNext;
      sqlite4VtabUnlock(p);
      p = pNext;
    }while( p );
  }
}

/*
** Clear any and all virtual-table information from the Table record.
** This routine is called, for example, just before deleting the Table
** record.
*/
void sqlite4VtabClear(sqlite4 *db, Table *p){
  if( !IsVirtual(p) ) return;
  if( db && db->pnBytesFreed==0 ) vtabDisconnectAll(0, p);
  if( p->azModuleArg ){
    int i;
    for(i=0; i<p->nModuleArg; i++){
      if( i!=1 ) sqlite4DbFree(db, p->azModuleArg[i]);
    }
    sqlite4DbFree(db, p->azModuleArg);
  }
}

/*
** Add a new module argument to pTable->azModuleArg[].
** The string is not copied - the pointer is stored.  The
** string will be freed automatically when the table is
** deleted.
*/
static void addModuleArgument(Parse *pParse, Table *pTable, char *zArg){
  int nBytes;
  char **azModuleArg;
  sqlite4 *db = pParse->db;

  assert( IsVirtual(pTable) );
  nBytes = sizeof(char *)*(2+pTable->nModuleArg);
  if( pTable->nModuleArg+3>=db->aLimit[SQLITE4_LIMIT_COLUMN] ){
    sqlite4ErrorMsg(pParse, "too many columns on %s", pTable->zName);
  }
  azModuleArg = sqlite4DbRealloc(db, pTable->azModuleArg, nBytes);
  if( azModuleArg==0 ){
    sqlite4DbFree(db, zArg);
  }else{
    int i = pTable->nModuleArg++;
    azModuleArg[i] = zArg;
    azModuleArg[i+1] = 0;
    pTable->azModuleArg = azModuleArg;
  }
}

/*
** The parser calls this routine when it first sees a CREATE VIRTUAL TABLE
** statement.  The module name has been parsed, but the optional list
** of parameters that follow the module name are still pending.
*/
void sqlite4VtabBeginParse(
  Parse *pParse,        /* Parsing context */
  Token *pName1,        /* Name of new table, or database name */
  Token *pName2,        /* Name of new table or NULL */
  Token *pModuleName    /* Name of the module for the virtual table */
){
  Table *pTable;        /* The new virtual table */
  sqlite4 *db;          /* Database connection */

  sqlite4StartTable(pParse, pName1, pName2, 0, 0, 1, 0);
  pTable = pParse->pNewTable;
  if( pTable==0 ) return;
  assert( 0==pTable->pIndex );
  pTable->tabFlags |= TF_Virtual;

  db = pParse->db;

  assert( pTable->nModuleArg==0 );
  addModuleArgument(pParse, pTable, sqlite4NameFromToken(db, pModuleName));
  addModuleArgument(pParse, pTable, 0);
  addModuleArgument(pParse, pTable, sqlite4DbStrDup(db, pTable->zName));
  assert( (pParse->sNameToken.z==pName2->z && pName2->z!=0)
       || (pParse->sNameToken.z==pName1->z && pName2->z==0)
  );
  pParse->sNameToken.n = (int)(
      &pModuleName->z[pModuleName->n] - pParse->sNameToken.z
  );

#ifndef SQLITE4_OMIT_AUTHORIZATION
  /* Creating a virtual table invokes the authorization callback twice.
  ** The first invocation, to obtain permission to INSERT a row into the
  ** sqlite_master table, has already been made by sqlite4StartTable().
  ** The second call, to obtain permission to create the table, is made now.
  */
  if( pTable->azModuleArg ){
    int iDb = sqlite4SchemaToIndex(db, pTable->pSchema);
    assert( iDb>=0 ); /* The database the table is being created in */
    sqlite4AuthCheck(pParse, SQLITE4_CREATE_VTABLE, pTable->zName,
            pTable->azModuleArg[0], pParse->db->aDb[iDb].zName);
  }
#endif
}

/*
** This routine takes the module argument that has been accumulating
** in pParse->zArg[] and appends it to the list of arguments on the
** virtual table currently under construction in pParse->pTable.
*/
static void addArgumentToVtab(Parse *pParse){
  if( pParse->sArg.z && pParse->pNewTable ){
    const char *z = (const char*)pParse->sArg.z;
    int n = pParse->sArg.n;
    sqlite4 *db = pParse->db;
    addModuleArgument(pParse, pParse->pNewTable, sqlite4DbStrNDup(db, z, n));
  }
}

/*
** The parser calls this routine after the CREATE VIRTUAL TABLE statement
** has been completely parsed.
*/
void sqlite4VtabFinishParse(Parse *pParse, Token *pEnd){
  Table *pTab = pParse->pNewTable;  /* The table being constructed */
  sqlite4 *db = pParse->db;         /* The database connection */

  if( pTab==0 ) return;
  assert( IsVirtual(pTab) );
  addArgumentToVtab(pParse);
  pParse->sArg.z = 0;
  if( pTab->nModuleArg<1 ) return;

  /* If the CREATE VIRTUAL TABLE statement is being entered for the
  ** first time (in other words if the virtual table is actually being
  ** created now instead of just being read out of sqlite_master) then
  ** do additional initialization work and store the statement text
  ** in the sqlite_master table.
  */
  if( !db->init.busy ){
    char *zStmt;
    char *zWhere;
    int iDb;
    Vdbe *v;

    sqlite4MayAbort(pParse);

    /* Compute the complete text of the CREATE VIRTUAL TABLE statement */
    if( pEnd ){
      pParse->sNameToken.n = (int)(pEnd->z - pParse->sNameToken.z) + pEnd->n;
    }
    zStmt = sqlite4MPrintf(db, "CREATE VIRTUAL TABLE %T", &pParse->sNameToken);

    /* A slot for the record has already been allocated in the
    ** schema table.  We just need to update that slot with all
    ** the information we've collected.
    **
    ** The VM register number pParse->regRowid holds the rowid of an
    ** entry in the sqlite_master table that was created for this vtab
    ** by sqlite4StartTable().
    */
    iDb = sqlite4SchemaToIndex(db, pTab->pSchema);
    sqlite4NestedParse(pParse,
      "UPDATE %Q.%s "
         "SET type='table', name=%Q, tbl_name=%Q, rootpage=0, sql=%Q "
       "WHERE rowid=#%d",
      db->aDb[iDb].zName,
      SCHEMA_TABLE(iDb),
      pTab->zName,
      pTab->zName,
      zStmt,
      pParse->regRowid
    );
    v = sqlite4GetVdbe(pParse);
    sqlite4ChangeCookie(pParse, iDb);

    sqlite4VdbeAddOp0(v, OP_Expire);
    zWhere = sqlite4MPrintf(db, "name=%Q AND sql=%Q", pTab->zName, zStmt);
    sqlite4VdbeAddParseSchemaOp(v, iDb, zWhere);
    sqlite4DbFree(db, zStmt);

    sqlite4VdbeAddOp4(v, OP_VCreate, iDb, 0, 0,
                      sqlite4DbStrDup(db, pTab->zName), P4_DYNAMIC);
  }else{
    /* If we are rereading the sqlite_master table create the in-memory
    ** record of the table. */
    Table *pOld;
    Schema *pSchema = pTab->pSchema;
    const char *zName = pTab->zName;
    int nName = sqlite4Strlen30(zName);
    assert( zName!=0 );
    pOld = sqlite4HashInsert(&pSchema->tblHash, zName, nName, pTab);
    if( pOld ){
      db->mallocFailed = 1;
      assert( pTab==pOld );  /* Malloc must have failed inside HashInsert() */
      return;
    }
    pParse->pNewTable = 0;
  }
}

/*
** The parser calls this routine when it sees the first token
** of an argument to the module name in a CREATE VIRTUAL TABLE statement.
*/
void sqlite4VtabArgInit(Parse *pParse){
  addArgumentToVtab(pParse);
  pParse->sArg.z = 0;
  pParse->sArg.n = 0;
}

/*
** The parser calls this routine for each token after the first token
** in an argument to the module name in a CREATE VIRTUAL TABLE statement.
*/
void sqlite4VtabArgExtend(Parse *pParse, Token *p){
  Token *pArg = &pParse->sArg;
  if( pArg->z==0 ){
    pArg->z = p->z;
    pArg->n = p->n;
  }else{
    assert(pArg->z <= p->z);
    pArg->n = (int)(&p->z[p->n] - pArg->z);
  }
}

/*
** Invoke a virtual table constructor (either xCreate or xConnect). The
** pointer to the function to invoke is passed as the fourth parameter
** to this procedure.
*/
static int vtabCallConstructor(
  sqlite4 *db,
  Table *pTab,
  Module *pMod,
  int (*xConstruct)(sqlite4*,void*,int,const char*const*,sqlite4_vtab**,char**),
  char **pzErr
){
  VtabCtx sCtx;
  VTable *pVTable;
  int rc;
  const char *const*azArg;
  int nArg = pTab->nModuleArg;
  char *zErr = 0;
  char *zModuleName;
  int iDb;
  VtabCtx *pCtx;

  assert( IsVirtual(pTab) );
  azArg = (const char *const*)pTab->azModuleArg;

  /* Check that the virtual-table is not already being initialized */
  for(pCtx=db->pVtabCtx; pCtx; pCtx=pCtx->pPrior){
    if( pCtx->pTab==pTab ){
      *pzErr = sqlite4MPrintf(db,
          "vtable constructor called recursively: %s", pTab->zName
      );
      return SQLITE4_LOCKED;
    }
  }

  zModuleName = sqlite4DbStrDup(db, pTab->zName);
  if( !zModuleName ){
    return SQLITE4_NOMEM;
  }

  pVTable = sqlite4DbMallocZero(db, sizeof(VTable));
  if( !pVTable ){
    db->mallocFailed = 1;
    sqlite4DbFree(db, zModuleName);
    return SQLITE4_NOMEM;
  }
  pVTable->db = db;
  pVTable->pMod = pMod;

  iDb = sqlite4SchemaToIndex(db, pTab->pSchema);
  pTab->azModuleArg[1] = db->aDb[iDb].zName;

  /* Invoke the virtual table constructor */
  assert( &db->pVtabCtx );
  assert( xConstruct );
  sCtx.pTab = pTab;
  sCtx.pVTable = pVTable;
  sCtx.pPrior = db->pVtabCtx;
  sCtx.bDeclared = 0;
  db->pVtabCtx = &sCtx;
  pTab->nRef++;
  rc = xConstruct(db, pMod->pAux, nArg, azArg, &pVTable->pVtab, &zErr);
  sqlite4DeleteTable(db, pTab);
  db->pVtabCtx = sCtx.pPrior;
  if( rc==SQLITE4_NOMEM ) db->mallocFailed = 1;
  assert( sCtx.pTab==pTab );

  if( SQLITE4_OK!=rc ){
    if( zErr==0 ){
      *pzErr = sqlite4MPrintf(db, "vtable constructor failed: %s", zModuleName);
    }else {
      *pzErr = sqlite4MPrintf(db, "%s", zErr);
      sqlite4_free(db->pEnv, zErr);
    }
    sqlite4DbFree(db, pVTable);
  }else if( pVTable->pVtab ){
    /* A correct vtab constructor must allocate the sqlite4_vtab object
    ** if successful. */
    memset(pVTable->pVtab, 0, sizeof(pVTable->pVtab[0]));
    pVTable->pVtab->pModule = pMod->pModule;
    pMod->nRefModule++;
    pVTable->nRef = 1;
    if( sCtx.bDeclared==0 ){
      const char *zFormat = "vtable constructor did not declare schema: %s";
      *pzErr = sqlite4MPrintf(db, zFormat, pTab->zName);
      sqlite4VtabUnlock(pVTable);
      rc = SQLITE4_ERROR;
    }else{
      int iCol;
      /* If everything went according to plan, link the new VTable structure
      ** into the linked list headed by pTab->pVTable. Then loop through the
      ** columns of the table to see if any of them contain the token "hidden".
      ** If so, set the Column.isHidden flag and remove the token from
      ** the type string.  */
      pVTable->pNext = pTab->pVTable;
      pTab->pVTable = pVTable;

      for(iCol=0; iCol<pTab->nCol; iCol++){
        char *zType = pTab->aCol[iCol].zType;
        int nType;
        int i = 0;
        if( zType==0 ) continue;
        nType = sqlite4Strlen30(zType);
        for(i=0; i<nType; i++){
          if( 0==sqlite4_strnicmp("hidden", &zType[i], 6)
           && (i==0 || zType[i-1]==' ')
           && (zType[i+6]=='\0' || zType[i+6]==' ')
          ){
            break;
          }
        }
        if( i<nType ){
          int j;
          int nDel = 6 + (zType[i+6] ? 1 : 0);
          for(j=i; (j+nDel)<=nType; j++){
            zType[j] = zType[j+nDel];
          }
          if( zType[i]=='\0' && i>0 ){
            assert(zType[i-1]==' ');
            zType[i-1] = '\0';
          }
          pTab->aCol[iCol].isHidden = 1;
        }
      }
    }
  }

  sqlite4DbFree(db, zModuleName);
  return rc;
}

/*
** This function is invoked by the parser to call the xConnect() method
** of the virtual table pTab. If an error occurs, an error code is returned
** and an error left in pParse.
**
** This call is a no-op if table pTab is not a virtual table.
*/
int sqlite4VtabCallConnect(Parse *pParse, Table *pTab){
  sqlite4 *db = pParse->db;
  const char *zMod;
  Module *pMod;
  int rc;

  assert( pTab );
  if( !IsVirtual(pTab) ){
    return SQLITE4_OK;
  }
  if( sqlite4GetVTable(db, pTab) ){
    return SQLITE4_OK;
  }

  /* Locate the required virtual table module */
  zMod = pTab->azModuleArg[0];
  pMod = (Module*)sqlite4HashFind(&db->aModule, zMod,
                                  sqlite4Strlen30(zMod));

  if( !pMod ){
    const char *zModule = pTab->azModuleArg[0];
    sqlite4ErrorMsg(pParse, "no such module: %s", zModule);
    rc = SQLITE4_ERROR;
  }else{
    char *zErr = 0;
    rc = vtabCallConstructor(db, pTab, pMod, pMod->pModule->xConnect, &zErr);
    if( rc!=SQLITE4_OK ){
      sqlite4ErrorMsg(pParse, "%s", zErr);
      pParse->rc = rc;
    }
    sqlite4DbFree(db, zErr);
  }

  return rc;
}

/*
** Grow the db->aVTrans[] array so that there is room for at least one
** more v-table. Return SQLITE4_NOMEM if a malloc fails, or SQLITE4_OK
** otherwise.
*/
static int growVTrans(sqlite4 *db){
  const int ARRAY_INCR = 5;

  /* Grow the sqlite4.aVTrans array if required */
  if( (db->nVTrans%ARRAY_INCR)==0 ){
    VTable **aVTrans;
    int nBytes = sizeof(sqlite4_vtab*) * (db->nVTrans + ARRAY_INCR);
    aVTrans = sqlite4DbRealloc(db, (void *)db->aVTrans, nBytes);
    if( !aVTrans ){
      return SQLITE4_NOMEM;
    }
    memset(&aVTrans[db->nVTrans], 0, sizeof(sqlite4_vtab *)*ARRAY_INCR);
    db->aVTrans = aVTrans;
  }

  return SQLITE4_OK;
}

/*
** Add the virtual table pVTab to the array sqlite4.aVTrans[]. Space should
** have already been reserved using growVTrans().
*/
static void addToVTrans(sqlite4 *db, VTable *pVTab){
  /* Add pVtab to the end of sqlite4.aVTrans */
  db->aVTrans[db->nVTrans++] = pVTab;
  sqlite4VtabLock(pVTab);
}

/*
** This function is invoked by the vdbe to call the xCreate method
** of the virtual table named zTab in database iDb.
**
** If an error occurs, *pzErr is set to point to an English language
** description of the error and an SQLITE4_XXX error code is returned.
** In this case the caller must call sqlite4DbFree(db, ) on *pzErr.
*/
int sqlite4VtabCallCreate(sqlite4 *db, int iDb, const char *zTab, char **pzErr){
  int rc = SQLITE4_OK;
  Table *pTab;
  Module *pMod;
  const char *zMod;

  pTab = sqlite4FindTable(db, zTab, db->aDb[iDb].zName);
  assert( pTab && IsVirtual(pTab) && !pTab->pVTable );

  /* Locate the required virtual table module */
  zMod = pTab->azModuleArg[0];
  pMod = (Module*)sqlite4HashFind(&db->aModule, zMod,
                                  sqlite4Strlen30(zMod));

  /* If the module has been registered and includes a Create method,
  ** invoke it now. If the module has not been registered, return an
  ** error. Otherwise, do nothing.
  */
  if( pMod==0 || pMod->pModule->xCreate==0 || pMod->pModule->xDestroy==0 ){
    *pzErr = sqlite4MPrintf(db, "no such module: %s", zMod);
    rc = SQLITE4_ERROR;
  }else{
    rc = vtabCallConstructor(db, pTab, pMod, pMod->pModule->xCreate, pzErr);
  }

  /* Justification of ALWAYS():  The xConstructor method is required to
  ** create a valid sqlite4_vtab if it returns SQLITE4_OK. */
  if( rc==SQLITE4_OK && sqlite4GetVTable(db, pTab) ){
    rc = growVTrans(db);
    if( rc==SQLITE4_OK ){
      addToVTrans(db, sqlite4GetVTable(db, pTab));
    }
  }

  return rc;
}

/*
** This function is used to set the schema of a virtual table.  It is only
** valid to call this function from within the xCreate() or xConnect() of a
** virtual table module.
*/
int sqlite4_declare_vtab(sqlite4 *db, const char *zCreateTable){
  VtabCtx *pCtx;
  int rc = SQLITE4_OK;
  Table *pTab;
  char *zErrMsg = 0;
  Parse sParse;
  int initBusy;

  sqlite4_mutex_enter(db->mutex);
  pCtx = db->pVtabCtx;
  if( !pCtx || pCtx->bDeclared ){
    sqlite4Error(db, SQLITE4_MISUSE, 0);
    sqlite4_mutex_leave(db->mutex);
    return SQLITE4_MISUSE;
  }
  pTab = pCtx->pTab;
  assert( IsVirtual(pTab) );

  memset(&sParse, 0, sizeof(sParse));
  sParse.db = db;
  sParse.declareVtab = 1;

  /* We should never be able to reach this point while loading the
  ** schema.  Nevertheless, defend against that (turn off db->init.busy)
  ** in case a bug arises. */
  assert( db->init.busy==0 );
  initBusy = db->init.busy;
  db->init.busy = 0;
  if( SQLITE4_OK==sqlite4RunParser(&sParse, zCreateTable, &zErrMsg)
   && sParse.pNewTable!=0
   && !db->mallocFailed
  ){
    if( !pTab->aCol ){
      Table *pNew = sParse.pNewTable;
      Index *pIdx;
      pTab->aCol = pNew->aCol;
      pTab->nCol = pNew->nCol;
      pNew->nCol = 0;
      pNew->aCol = 0;
      assert( pTab->pIndex==0 );
      pIdx = pNew->pIndex;
      if( pIdx ){
        assert( pIdx->pNext==0 );
        pTab->pIndex = pIdx;
        pNew->pIndex = 0;
        pIdx->pTable = pTab;
      }
    }
    pCtx->bDeclared = 1;
  }else{
    sqlite4Error(db, SQLITE4_ERROR, (zErrMsg ? "%s" : 0), zErrMsg);
    sqlite4DbFree(db, zErrMsg);
    rc = SQLITE4_ERROR;
  }
  sParse.declareVtab = 0;

  if( sParse.pVdbe ){
    sqlite4VdbeFinalize(sParse.pVdbe);
  }
  sqlite4DeleteTable(db, sParse.pNewTable);
  db->init.busy = initBusy;

  assert( (rc&0xff)==rc );
  rc = sqlite4ApiExit(db, rc);
  sqlite4_mutex_leave(db->mutex);
  return rc;
}

/*
** This function is invoked by the vdbe to call the xDestroy method
** of the virtual table named zTab in database iDb. This occurs
** when a DROP TABLE is mentioned.
**
** This call is a no-op if zTab is not a virtual table.
*/
int sqlite4VtabCallDestroy(sqlite4 *db, int iDb, const char *zTab){
  int rc = SQLITE4_OK;
  Table *pTab;

  pTab = sqlite4FindTable(db, zTab, db->aDb[iDb].zName);
  if( pTab!=0
   && IsVirtual(pTab)
   && pTab->pVTable!=0
  ){
    VTable *p;
    int (*xDestroy)(sqlite4_vtab *);
    for(p=pTab->pVTable; p; p=p->pNext){
      assert( p->pVtab );
      if( p->pVtab->nRef>0 ){
        return SQLITE4_LOCKED;
      }
    }
    p = vtabDisconnectAll(db, pTab);
    xDestroy = p->pMod->pModule->xDestroy;
    if( xDestroy==0 ) xDestroy = p->pMod->pModule->xDisconnect;
    assert( xDestroy!=0 );
    pTab->nRef++;
    rc = xDestroy(p->pVtab);
    /* Remove the sqlite4_vtab* from the aVTrans[] array, if applicable */
    if( rc==SQLITE4_OK ){
      assert( pTab->pVTable==p && p->pNext==0 );
      p->pVtab = 0;
      pTab->pVTable = 0;
      sqlite4VtabUnlock(p);
    }
    sqlite4DeleteTable(db, pTab);
  }

  return rc;
}

/*
** This function invokes either the xRollback or xCommit method
** of each of the virtual tables in the sqlite4.aVTrans array. The method
** called is identified by the second argument, "offset", which is
** the offset of the method to call in the sqlite4_module structure.
**
** The array is cleared after invoking the callbacks.
*/
static void callFinaliser(sqlite4 *db, int offset){
  int i;
  if( db->aVTrans ){
    VTable **aVTrans = db->aVTrans;
    db->aVTrans = 0;
    for(i=0; i<db->nVTrans; i++){
      VTable *pVTab = aVTrans[i];
      sqlite4_vtab *p = pVTab->pVtab;
      if( p ){
        int (*x)(sqlite4_vtab *);
        x = *(int (**)(sqlite4_vtab *))((char *)p->pModule + offset);
        if( x ) x(p);
      }
      pVTab->iSavepoint = 0;
      sqlite4VtabUnlock(pVTab);
    }
    sqlite4DbFree(db, aVTrans);
    db->nVTrans = 0;
  }
}

/*
** Invoke the xSync method of all virtual tables in the sqlite4.aVTrans
** array. Return the error code for the first error that occurs, or
** SQLITE4_OK if all xSync operations are successful.
**
** If an error message is available, leave it in *pzErrmsg.
*/
int sqlite4VtabSync(sqlite4 *db, char **pzErrmsg){
  int i;
  int rc = SQLITE4_OK;
  VTable **aVTrans = db->aVTrans;

  db->aVTrans = 0;
  for(i=0; rc==SQLITE4_OK && i<db->nVTrans; i++){
    int (*x)(sqlite4_vtab *);
    sqlite4_vtab *pVtab = aVTrans[i]->pVtab;
    if( pVtab && (x = pVtab->pModule->xSync)!=0 ){
      rc = x(pVtab);
      if( pVtab->zErrMsg && pzErrmsg ){
        *pzErrmsg = sqlite4DbStrDup(db, pVtab->zErrMsg);
      }
    }
  }
  db->aVTrans = aVTrans;
  return rc;
}

/*
** Invoke the xRollback method of all virtual tables in the
** sqlite4.aVTrans array. Then clear the array itself.
*/
int sqlite4VtabRollback(sqlite4 *db){
  callFinaliser(db, offsetof(sqlite4_module,xRollback));
  return SQLITE4_OK;
}

/*
** Invoke the xCommit method of all virtual tables in the
** sqlite4.aVTrans array. Then clear the array itself.
*/
int sqlite4VtabCommit(sqlite4 *db){
  callFinaliser(db, offsetof(sqlite4_module,xCommit));
  return SQLITE4_OK;
}

/*
** If the virtual table pVtab supports the transaction interface
** (xBegin/xRollback/xCommit and optionally xSync) and a transaction is
** not currently open, invoke the xBegin method now.
**
** If the xBegin call is successful, place the sqlite4_vtab pointer
** in the sqlite4.aVTrans array.
*/
int sqlite4VtabBegin(sqlite4 *db, VTable *pVTab){
  int rc = SQLITE4_OK;
  const sqlite4_module *pModule;

  /* Special case: If db->aVTrans is NULL and db->nVTrans is greater
  ** than zero, then this function is being called from within a
  ** virtual module xSync() callback. It is illegal to write to
  ** virtual module tables in this case, so return SQLITE4_LOCKED.
  */
  if( sqlite4VtabInSync(db) ){
    return SQLITE4_LOCKED;
  }
  if( !pVTab ){
    return SQLITE4_OK;
  }
  pModule = pVTab->pVtab->pModule;

  if( pModule->xBegin ){
    int i;

    /* If pVtab is already in the aVTrans array, return early */
    for(i=0; i<db->nVTrans; i++){
      if( db->aVTrans[i]==pVTab ){
        return SQLITE4_OK;
      }
    }

    /* Invoke the xBegin method. If successful, add the vtab to the
    ** sqlite4.aVTrans[] array. */
    rc = growVTrans(db);
    if( rc==SQLITE4_OK ){
      rc = pModule->xBegin(pVTab->pVtab);
      if( rc==SQLITE4_OK ){
        int iSvpt = db->nStatement + db->nSavepoint;
        addToVTrans(db, pVTab);
        if( iSvpt && pModule->xSavepoint ){
          pVTab->iSavepoint = iSvpt;
          rc = pModule->xSavepoint(pVTab->pVtab, iSvpt-1);
        }
      }
    }
  }
  return rc;
}

/*
** Invoke either the xSavepoint, xRollbackTo or xRelease method of all
** virtual tables that currently have an open transaction. Pass iSavepoint
** as the second argument to the virtual table method invoked.
**
** If op is SAVEPOINT_BEGIN, the xSavepoint method is invoked. If it is
** SAVEPOINT_ROLLBACK, the xRollbackTo method. Otherwise, if op is
** SAVEPOINT_RELEASE, then the xRelease method of each virtual table with
** an open transaction is invoked.
**
** If any virtual table method returns an error code other than SQLITE4_OK,
** processing is abandoned and the error returned to the caller of this
** function immediately. If all calls to virtual table methods are successful,
** SQLITE4_OK is returned.
*/
int sqlite4VtabSavepoint(sqlite4 *db, int op, int iSavepoint){
  int rc = SQLITE4_OK;

  assert( op==SAVEPOINT_RELEASE||op==SAVEPOINT_ROLLBACK||op==SAVEPOINT_BEGIN );
  assert( iSavepoint>=-1 );
  if( db->aVTrans ){
    int i;
    for(i=0; rc==SQLITE4_OK && i<db->nVTrans; i++){
      VTable *pVTab = db->aVTrans[i];
      const sqlite4_module *pMod = pVTab->pMod->pModule;
      if( pVTab->pVtab && pMod->iVersion>=2 ){
        int (*xMethod)(sqlite4_vtab *, int);
        sqlite4VtabLock(pVTab);
        switch( op ){
          case SAVEPOINT_BEGIN:
            xMethod = pMod->xSavepoint;
            pVTab->iSavepoint = iSavepoint+1;
            break;
          case SAVEPOINT_ROLLBACK:
            xMethod = pMod->xRollbackTo;
            break;
          default:
            xMethod = pMod->xRelease;
            break;
        }
        if( xMethod && pVTab->iSavepoint>iSavepoint ){
          rc = xMethod(pVTab->pVtab, iSavepoint);
        }
        sqlite4VtabUnlock(pVTab);
      }
    }
  }
  return rc;
}

/*
** The first parameter (pDef) is a function implementation.  The
** second parameter (pExpr) is the first argument to this function.
** If pExpr is a column in a virtual table, then let the virtual
** table implementation have an opportunity to overload the function.
**
** This routine is used to allow virtual table implementations to
** overload MATCH, LIKE, GLOB, and REGEXP operators.
**
** Return either the pDef argument (indicating no change) or a
** new FuncDef structure that is marked as ephemeral using the
** SQLITE4_FUNC_EPHEM flag.
*/
FuncDef *sqlite4VtabOverloadFunction(
  sqlite4 *db,    /* Database connection for reporting malloc problems */
  FuncDef *pDef,  /* Function to possibly overload */
  int nArg,       /* Number of arguments to the function */
  Expr *pExpr     /* First argument to the function */
){
  Table *pTab;
  sqlite4_vtab *pVtab;
  sqlite4_module *pMod;
  void (*xSFunc)(sqlite4_context*,int,sqlite4_value**) = 0;
  void *pArg = 0;
  FuncDef *pNew;
  int rc = 0;

  /* Check to see the left operand is a column in a virtual table */
  if( pExpr==0 ) return pDef;
  if( pExpr->op!=TK_COLUMN ) return pDef;
  pTab = pExpr->pTab;
  if( pTab==0 ) return pDef;
  if( !IsVirtual(pTab) ) return pDef;
  pVtab = sqlite4GetVTable(db, pTab)->pVtab;
  assert( pVtab!=0 );
  assert( pVtab->pModule!=0 );
  pMod = (sqlite4_module *)pVtab->pModule;
  if( pMod->xFindFunction==0 ) return pDef;

  /* Call the xFindFunction method on the virtual table implementation
  ** to see if the implementation wants to overload this function.
  */
  rc = pMod->xFindFunction(pVtab, nArg, pDef->zName, &xSFunc, &pArg);
  if( rc==0 ){
    return pDef;
  }

  /* Create a new ephemeral function definition for the overloaded
  ** function */
  pNew = sqlite4DbMallocZero(db, sizeof(*pNew)
                             + sqlite4Strlen30(pDef->zName) + 1);
  if( pNew==0 ){
    return pDef;
  }
  *pNew = *pDef;
  pNew->zName = (char*)&pNew[1];
  memcpy((char*)&pNew[1], pDef->zName, sqlite4Strlen30(pDef->zName)+1);
  pNew->xFunc = xSFunc;
  pNew->pUserData = pArg;
  pNew->flags |= SQLITE4_FUNC_EPHEM;
  return pNew;
}

/*
** Make sure virtual table pTab is contained in the pParse->apVirtualLock[]
** array so that an OP_VBegin will get generated for it.  Add pTab to the
** array if it is missing.  If pTab is already in the array, this routine
** is a no-op.
*/
void sqlite4VtabMakeWritable(Parse *pParse, Table *pTab){
  Parse *pToplevel = sqlite4ParseToplevel(pParse);
  int i, n;
  Table **apVtabLock;

  assert( IsVirtual(pTab) );
  for(i=0; i<pToplevel->nVtabLock; i++){
    if( pTab==pToplevel->apVtabLock[i] ) return;
  }
  n = (pToplevel->nVtabLock+1)*sizeof(pToplevel->apVtabLock[0]);
  apVtabLock = sqlite4Realloc(pToplevel->db->pEnv, pToplevel->apVtabLock, n);
  if( apVtabLock ){
    pToplevel->apVtabLock = apVtabLock;
    pToplevel->apVtabLock[pToplevel->nVtabLock++] = pTab;
  }else{
    pToplevel->db->mallocFailed = 1;
  }
}

/*
** Check to see if virtual table module pMod can have an eponymous
** virtual table instance.  If it can, create one if one does not already
** exist. Return non-zero if either the eponymous virtual table instance
** exists when this routine returns or if an attempt to create it failed
** and an error message was left in pParse.
**
** An eponymous virtual table instance is one that is named after its
** module, and more importantly, does not require a CREATE VIRTUAL TABLE
** statement in order to come into existence.  Eponymous virtual table
** instances always exist.  They cannot be DROP-ed.
**
** Any virtual table module for which xConnect and xCreate are the same
** method can have an eponymous virtual table instance.
*/
int sqlite4VtabEponymousTableInit(Parse *pParse, Module *pMod){
  const sqlite4_module *pModule = pMod->pModule;
  Table *pTab;
  char *zErr = 0;
  int rc;
  sqlite4 *db = pParse->db;
  if( pMod->pEpoTab ) return 1;
  if( pModule->xCreate!=0 && pModule->xCreate!=pModule->xConnect ) return 0;
  pTab = sqlite4DbMallocZero(db, sizeof(Table));
  if( pTab==0 ) return 0;
  pTab->zName = sqlite4DbStrDup(db, pMod->zName);
  if( pTab->zName==0 ){
    sqlite4DbFree(db, pTab);
    return 0;
  }
  pMod->pEpoTab = pTab;
  pTab->nRef = 1;
  pTab->tabFlags |= TF_Virtual;
  pTab->pSchema = db->aDb[0].pSchema;
  assert( pTab->nModuleArg==0 );
  addModuleArgument(pParse, pTab, sqlite4DbStrDup(db, pTab->zName));
  addModuleArgument(pParse, pTab, 0);
  addModuleArgument(pParse, pTab, sqlite4DbStrDup(db, pTab->zName));
  rc = vtabCallConstructor(db, pTab, pMod, pModule->xConnect, &zErr);
  if( rc ){
    sqlite4ErrorMsg(pParse, "%s", zErr);
    sqlite4DbFree(db, zErr);
    sqlite4VtabEponymousTableClear(db, pMod);
  }
  return 1;
}

/*
** Erase the eponymous virtual table instance associated with
** virtual table module pMod, if it exists.
*/
void sqlite4VtabEponymousTableClear(sqlite4 *db, Module *pMod){
  Table *pTab = pMod->pEpoTab;
  if( pTab!=0 ){
    /* Mark the table as Ephemeral prior to deleting it, so that the
    ** sqlite4DeleteTable() routine will know that it is not stored in
    ** the schema. */
    pTab->tabFlags |= TF_Ephemeral;
    sqlite4DeleteTable(db, pTab);
    pMod->pEpoTab = 0;
  }
}

/*
** Return the ON CONFLICT resolution mode in effect for the virtual
** table update operation currently in progress.
**
** The results of this routine are undefined unless it is called from
** within an xUpdate method.
*/
int sqlite4_vtab_on_conflict(sqlite4 *db){
  static const unsigned char aMap[] = {
    SQLITE4_ROLLBACK, SQLITE4_ABORT, SQLITE4_FAIL,
    SQLITE4_IGNORE, SQLITE4_REPLACE
  };
  assert( OE_Rollback==1 && OE_Abort==2 && OE_Fail==3 );
  assert( OE_Ignore==4 && OE_Replace==5 );
  assert( db->vtabOnConflict>=1 && db->vtabOnConflict<=5 );
  return (int)aMap[db->vtabOnConflict-1];
}

/*
** Call from within the xCreate() or xConnect() methods to provide
** the SQLite core with additional information about the behavior
** of the virtual table being implemented.
*/
int sqlite4_vtab_config(sqlite4 *db, int op, ...){
  va_list ap;
  int rc = SQLITE4_OK;
  VtabCtx *p;

  sqlite4_mutex_enter(db->mutex);
  p = db->pVtabCtx;
  if( !p ){
    rc = SQLITE4_MISUSE;
  }else{
    assert( p->pTab==0 || IsVirtual(p->pTab) );
    va_start(ap, op);
    switch( op ){
      case SQLITE4_VTAB_CONSTRAINT_SUPPORT: {
        p->pVTable->bConstraint = (u8)va_arg(ap, int);
        break;
      }
      default: {
        rc = SQLITE4_MISUSE;
        break;
      }
    }
    va_end(ap);
  }

  if( rc!=SQLITE4_OK ) sqlite4Error(db, rc, 0);
  sqlite4_mutex_leave(db->mutex);
  return rc;
}

#endif /* !defined(SQLITE4_OMIT_VIRTUALTABLE) */
