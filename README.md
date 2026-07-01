# MoVeBench : A Mobile Vector Benchmark

**MoVeBench** is a DiskANN benchmark tool for mobile and on-device environments, with a focus on the storage layer evaluation.

Although several approximate nearest neighbor(ANN) benchmark tools have been released, few provide performance metrics that isolate the impact of different storage layers.
By varying only the storage layer, MoVeBench enables a direct evaluation of the trade-offs between two widely used storage structures, B-Tree and LSM-Tree, in your on-device environment.

MoVeBench uses [libSQL](https://docs.turso.tech/libsql), which is a DiskANN implementation on top of SQLite3's B-Tree storage layer, and LSMoVe, which is our port of libSQL's DiskANN layer to [SQLite4](https://sqlite.org/src4/doc/trunk/www/index.wiki), to evaluate an LSM-Tree-based storage layer.

## Datasets

We provide four pre-calculated embedding datasets using different embedding algorithms for this benchmark: [SIFT](https://www.tensorflow.org/datasets/catalog/sift1m), [GloVe](https://nlp.stanford.edu/projects/glove/), [COCO](https://cocodataset.org/#download) and [Cohere](https://cohere.com/blog/embedding-archives-wikipedia).

| Dataset | Dimensions | # of Insert | # of Search | Distance |
|---|---:|---:|---:|---|
| SIFT | 128 | 100,000 | 10,000 | Euclidean | 
| GloVe | 200 | 100,000 | 10,000 | Cosine |
| COCO | 512 | 100,000 | 10,000 | Cosine |
| Cohere | 768 | 100,000 | 10,000 | Cosine |


You can download datasets on our [huggingface repository](https://huggingface.co/datasets/korea-dbs/MoVeBench/tree/main).

To download all datasets, run:

```bash
pip install huggingface-hub     #if needed
hf download korea-dbs/MoVeBench --repo-type dataset --local-dir ./dataset
```
Or you can download a specific dataset (e.g. SIFT)
```bash
hf download korea-dbs/MoVeBench '*_sift.*' --repo-type dataset --local-dir ./dataset
```
Note that the benchmark requires all three files : `insert100k_{dataset_name}.sql` , `query10k_{dataset_name}.sql` and `groundtruth_{dataset_name}.txt`.

## How To Run

Once you clone this repository, do

```bash
make -j
```

You can also build each engines with the code below.

```bash
make libsql     # libSQL
make lsmove     # LSMoVe
make compact    # additional compaction code for LSMoVe
```

### Run

```bash
python3 benchmark.py
```

We provided benchmark with some useful options.

| Options | Description | Default |
| --- | --- | --- |
| `dataset-dir` | Directory with SQL files | `./Dataset` |
| `datasets` | Dataset names separated by `,` | `sift,glove,coco,cohere` |
| `sqlite4-dir` | Directory containing the LSMoVe/sqlite4 shell and optional `compact_db` | `./LSMoVe` |
| `sqlite3-dir` | Directory containing sqlite3 | `./libSQL` |
| `db-dir` | Directory to store db | `.` |
| `page_sizes` | Setting SQLite's page size(`LSM_CONFIG_PAGE_SIZE`) (KB) | `4,16,32,64` |
| `lsm-compression` | LSM storage page compression algorithms | `zlib` , `lz4` , `none` (default) |
| `lsm-use-copmaction` | Use `compact_db` after insert for LSMoVe configs for an additional compaction `0` : skip , `1` : use | `0` (skip) |
| `lsm-autoflush` | Set LSM-Tree autoflush(`LSM_CONFIG_AUTOFLUSH`) threshold for LSMoVe configs (MB) | 256 MB |
| `lsm-automerge` | Set LSM automerge segment threshold(`LSM_CONFIG_AUTOMERGE`) for LSMoVe configs | 4 |
| `drop-cache` | Drop OS Cache before each benchmark phase (add `--drop-cache` to activate) | No
| `disk-device` | Block device name from /proc/diskstats, or 'auto' to detect from `db-dir` | `auto`
| `search-only` | Run search query and recall only using existing `bench_{dataset_name}.db` files (add `--search-only` to activate) | No
| `keep-db` | Keep generated `bench_{dataset_name}.db` files after all runs (add `--keep-db`) to activate | No (delete DB file whenever each run ends)

---

### example

1. _Run the benchmark on SIFT dataset, with 4KB SQLite page size, 128MB LSM autoflush threshold, drop OS cache before each phase starts, do additional compaction between insert and search phase and keep DB file after each run._
    ```bash
    python benchmark.py --datasets sift --page-sizes 4 --lsm-autoflush 128 --lsm-use-compaction 1 --drop-cache --keep-db
    ```

2. _Run the benchmark on SIFT, GloVe, COCO and Cohere dataset, with 4, 16, 32, 64 KB SQLite page size, 256MB LSM autoflush & 4 LSM automerge threshold, drop OS cache before each phase starts and delete DB file after each run._

    ```bash
    python benchmark.py
    ```

### Run on Android Device (pixel_benchmark.py)

`pixel_benchmark.py` runs the same benchmark on an Android device via `adb`. Binaries (`sqlite4`, `compact_db`, `sqlite3`) must be pre-built for the target device architecture (e.g., arm64) and pushed to the device beforehand.

```bash
python3 pixel_benchmark.py --adb-serial <device_serial>
```

| Option | Description | Default |
| --- | --- | --- |
| `dataset-dir` | Directory with SQL files (on host) | `./dataset` |
| `datasets` | Dataset names separated by `,` | `sift,glove,coco,cohere` |
| `sqlite4-dir` | Device path containing `sqlite4` and `compact_db` | `/data/local/sqlite4_lsm/MoVeBench` |
| `sqlite3-dir` | Device path containing `sqlite3` | `/data/local/sqlite3_libsql/MoVeBench` |
| `device-db-dir` | Device directory to store database files | `/data/local/tmp` |
| `device-tmp-dir` | Device directory for temporary SQL files | `/data/local/tmp` |
| `page-sizes` | Setting SQLite's page size (KB) | `4,16,32,64` |
| `lsm-compression` | LSM storage page compression algorithm | `none` |
| `lsm-use-compaction` | Use `compact_db` after insert for LSMoVe configs (`0`: skip, `1`: use) | `0` |
| `lsm-autoflush-mb` | Set LSM autoflush threshold for LSMoVe configs (MB) | 256 MB |
| `lsm-automerge` | Set LSM automerge segment threshold for LSMoVe configs | 4 |
| `drop-cache` | Drop OS page cache before each phase — requires root (add `--drop-cache` to activate) | No |
| `disk-device` | Block device name from `/proc/diskstats` | `sda` |
| `search-only` | Run search and recall only using existing `bench_*.db` files on device (add `--search-only` to activate) | No |
| `keep-db` | Keep DB files on device after all runs (add `--keep-db` to activate) | No |
| `adb-serial` | ADB device serial from `adb devices`; omit if only one device is connected | None |
| `shell-timeout` | Timeout in seconds for each `adb shell` command | `20000` |

#### example

```bash
python3 pixel_benchmark.py --adb-serial 2B261FDH200B0M --datasets sift --page-sizes 4 --disk-device sda
```

---

### LSM Configurations

The following table shows the adjustable LSM configurations of SQLite4.

| Configuration | Description | Default |
| --- | --- | --- |
| LSM_CONFIG_AUTOFLUSH | Threshold for flushing memtable to disk | 256MB |
|LSM_CONFIG_AUTOWORK | Enabling LSM-Tree related work (merge & compaction) (0 : deactivate 1 : activate) | 1 |
| LSM_CONFIG_AUTOCHECKPOINT | Threshold for checkpoint size | 2 MB |
| LSM_CONFIG_PAGE_SIZE | Database Page size | 4 KB |
| LSM_CONFIG_BLOCK_SIZE | LSM block (for separating disk layout) size | 1 MB |
| LSM_CONFIG_SAFETY | Set safety level (`0` : `OFF` , `1` : `NORMAL` , `2` : `FULL` ) | 1 |
| LSM_CONFIG_MMAP | Enabling mmap (`0` : off , `1` : on)| 1 |
| LSM_CONFIG_USE_LOG | Enabling log file (`0` : off , `1` : on) | 1 |
| LSM_CONFIG_AUTOMERGE | The number of accumulated LSM segment to together | 4 | 
| LSM_CONFIG_MAX_FREELIST | Maximum number of freelist(unused block list) stored in a checkpoint | 24 |
| LSM_CONFIG_MULTIPLE_PROCESSES | To allow multi-process access (`0` : off , `1` : on) | 1 |
| LSM_CONFIG_READONLY | Open the DB file as a read-only mode (`0` : read-write , `1` : read-only) | 0 |
| LSM_CONFIG_SET_COMPRESSION | Set the DB compression algorithm | `LSM_COMPRESSION_NONE` |
| LSM_CONFIG_SET_COMPRESSION_FACTORY | Register a compression algorithms using `compression_id` | `LSM_COMPRESSION_NONE` |
| LSM_CONFIG_GET_COMPRESSION | Get info of current compression algorithms | `LSM_COMPRESSION_NONE` |

## Results

The following results were obtained on a **NVIDIA Jetson AGX** and **Google Pixel 7** device using `benchmark.py` / `pixel_benchmark.py` each, with a 4KB SQLite page size and no compaction.

Detailed profiling outputs including per-phase timing and I/O statistics are available in [`./result_examples`](./result_examples/) directory.


### Insert Time (s)

<table>
  <tr>
    <th></th><th colspan="2">NVIDIA Jetson AGX</th><th colspan="2">Google Pixel 7</th>
  </tr>
  <tr>
    <th>Dataset</th><th>LSMoVe(4KB)</th><th>libSQL(4KB)</th><th>LSMoVe(4KB)</th><th>libSQL(4KB)</th>
  </tr>
  <tr>
    <td>SIFT</td> <td>455.1</td> <td>807.7</td> <td>184.8</td> <td>212.7</td>
  </tr>
  <tr>
    <td>GloVe</td> <td>612.2</td> <td>1072.3</td> <td>212.7</td> <td>653.7</td>
  </tr>
  <tr>
    <td>COCO</td> <td>1357.5</td> <td>2302.9</td> <td>593.6</td> <td>959.1</td>
  </tr>
    <td>Cohere</td> <td>1522.1</td> <td>2559.8</td> <td>733.0</td> <td>1084.2</td>
</table>


### Search Time (s) & Throughput (Q/s)


<table>
  <tr>
    <th></th><th colspan="2">NVIDIA Jetson AGX</th><th colspan="2">Google Pixel 7</th>
  </tr>
  <tr>
    <th>Dataset</th><th>LSMoVe(4KB)</th><th>libSQL(4KB)</th><th>LSMoVe(4KB)</th><th>libSQL(4KB)</th>
  </tr>
  <tr>
    <td>SIFT</td> <td>60.6 (165 Q/s)</td> <td>92.0 (109 Q/s)</td> <td>43.0 (233 Q/s)</td> <td>83.3 (120 Q/s)</td>
  </tr>
  <tr>
    <td>GloVe</td> <td>77.8 (128)</td> <td>105.8 (94)</td> <td>47.8 (209)</td> <td>84.0 (119)</td>
  </tr>
  <tr>
    <td>COCO</td> <td>125.9 (79)</td> <td>140.6 (71)</td> <td>71.2 (140)</td> <td>80.2 (125)</td>
  </tr>
    <td>Cohere</td> <td>176.0 (57)</td> <td>260.0 (38)</td> <td>104.2 (96)</td> <td>166.4 (60)</td>
</table>

### Recall@10 %

<table>
  <tr>
    <th></th><th colspan="2">NVIDIA Jetson AGX</th><th colspan="2">Google Pixel 7</th>
  </tr>
  <tr>
    <th>Dataset</th><th>libSQL(4KB)</th><th>LSMoVe(4KB)</th><th>LSMoVe(4KB)</th><th>libSQL(4KB)</th>
  </tr>
  <tr>
    <td>SIFT</td> <td>98.69%</td> <td>98.70%</td> <td>98.68%</td> <td>98.71%</td>
  </tr>
  <tr>
    <td>GloVe</td> <td>70.91%</td> <td>71.00%</td> <td>71.12%</td> <td>71.04%</td>
  </tr>
  <tr>
    <td>COCO</td> <td>95.01%</td> <td>95.00%</td> <td>95.02%</td> <td>94.98%</td>
  </tr>
    <td>Cohere</td> <td>91.65%</td> <td>91.71%</td> <td>91.69%</td> <td>91.73%</td>
</table>

### DB Size (MB)

<table>
  <tr>
    <th></th><th colspan="2">NVIDIA Jetson AGX</th><th colspan="2">Google Pixel 7</th>
  </tr>
  <tr>
    <th>Dataset</th><th>libSQL(4KB)</th><th>LSMoVe(4KB)</th><th>LSMoVe(4KB)</th><th>libSQL(4KB)</th>
  </tr>
  <tr>
    <td>SIFT</td> <td>1253.0</td> <td>448.8</td> <td>1253.0</td> <td>448.8</td>
  </tr>
  <tr>
    <td>GloVe</td> <td>1563.0</td> <td>600.7</td> <td>1559.0</td> <td>600.7</td>
  </tr>
  <tr>
    <td>COCO</td> <td>8099.0</td> <td>1620.7</td> <td>8092.0</td> <td>1620.7</td>
  </tr>
    <td>Cohere</td> <td>6969.0</td> <td>2346.9</td> <td>6967.0</td> <td>2346.9</td>
</table>