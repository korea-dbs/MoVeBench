# LSM-tree Architecture: Flush & Merge Flow

## Key Parameters

| Parameter | Default | Description |
|-----------|---------|-------------|
| `LSM_DFLT_AUTOFLUSH` | 1MB (8 pages in examples below) | In-memory tree size threshold before marking as "old" |
| `LSM_AUTOWORK_QUANT` | 32 pages (2 pages in examples below) | Disk maintenance work budget per unit of tree growth |
| `LSM_DFLT_AUTOMERGE` | 4 | Number of sorted runs at a level before merging to next level |

> Examples below use simplified numbers for clarity.

---

## Step 1: In-memory tree reaches `LSM_DFLT_AUTOFLUSH`

When the in-memory tree accumulates 8 pages of data, `lsmTreeMakeOld` marks it as "old" and starts a new empty tree.

```
old tree: [1 2 3 4 5 6 7 8]    (to be flushed)
new tree: []                     (new writes go here)
```

## Step 2: `LSM_AUTOWORK_QUANT` triggers disk maintenance during new writes

As data is written to the new tree, every 2 pages of growth triggers 2 pages worth of disk maintenance work (merge or old tree flush).

The old tree flush happens via `sortedNewToplevel` — it flushes the **entire old tree at once** as a single sorted run, not in chunks.

```
new tree: [9 10] written   → 2 pages of disk work
new tree: [11 12] written  → 2 pages of disk work
...                         → accumulated work flushes old tree

level 0: [1 2 3 4 5 6 7 8]   ← sorted run A (entire old tree as one run)
```

## Step 3: New tree fills up, same process repeats

```
old tree: [9 10 11 12 13 14 15 16]   (marked old)
new tree: []

→ subsequent writes' AUTOWORK_QUANT flushes old tree

level 0: [1...8] [9...16]   ← 2 sorted runs
```

## Step 4: `LSM_DFLT_AUTOMERGE` sorted runs trigger merge to next level

When 4 sorted runs accumulate at one level, they merge into a single run at the next level.

```
level 0: [1...8] [9...16] [17...24] [25...32]   ← 4 runs reached

                     merge ↓

level 0: (empty)
level 1: [1...32]   ← 4 runs merged into one
```

As inserts continue:

```
level 0: (empty)
level 1: [1...32] [33...64] [65...96] [97...128]   ← 4 runs reached

                     merge ↓

level 0: (empty)
level 1: (empty)
level 2: [1...128]
```

---

## Key Takeaway

`LSM_AUTOWORK_QUANT` does **not** split the old tree flush into chunks. It provides a **disk maintenance work budget** proportional to new tree growth. The old tree flush itself (`sortedNewToplevel`) is an atomic operation that writes the entire old tree as a single sorted run.

---

## The DiskANN Crash Bug (autocommit mode)

### Root Cause

When `lsmTreeMakeOld` fires, `lsmFinishWriteTrans` originally called:

```c
lsmSortedAutoWork(pDb, 1);   // only 1 page of work budget
```

Inside `doLsmSingleWork`, the work budget is shared between two tasks:

1. **Merge work** (if `sortedDbIsFull`) — merging existing sorted runs
2. **Old tree flush** (`sortedNewToplevel`) — flushing old tree to disk

```c
// Task 1: merge (runs first, consumes budget)
if( sortedDbIsFull(pDb) ){
    sortedWork(pDb, nRem, ...);
    nRem -= nPg;                  // budget consumed
}

// Task 2: flush (only if budget remains)
if( nRem > 0 ){                   // SKIPPED when budget is 0
    sortedNewToplevel(pDb, TREE_OLD, &nPg);
    lsmTreeDiscardOld(pDb);
}
```

For normal SQL (1 row ≈ 50 bytes per INSERT):
- `lsmTreeMakeOld` fires once per ~20,000 INSERTs
- Subsequent writes provide massive `AUTOWORK_QUANT` budget to flush old tree
- The `1` page in `lsmFinishWriteTrans` is just a small kick-start

For DiskANN (~30-40 BlobSpot writes ≈ 1MB per INSERT):
- `lsmTreeMakeOld` fires after **every INSERT**
- After thousands of inserts, `sortedDbIsFull` is true (many sorted runs)
- The 1-page budget is consumed by merge → old tree flush is **skipped**
- Old tree never flushed → `lsmTreeMakeOld` becomes a no-op → tree grows unbounded → crash at `treeShmalloc`

### Fix

```c
// Before: 1 page — consumed by merge, nothing left for flush
lsmSortedAutoWork(pDb, 1);

// After: nTreeLimit/pageSize pages (256 with defaults) — enough for both
int nFlushWork = pDb->nTreeLimit / lsmFsPageSize(pDb->pFS);
if( nFlushWork < 1 ) nFlushWork = 1;
lsmSortedAutoWork(pDb, nFlushWork);
```

This ensures the work budget is large enough to cover merge work **and** still have `nRem > 0` for the old tree flush.
