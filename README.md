# MoVeBench : Mobile Vector Benchmark

**MoVeBench** is a DiskANN benchmark tool for on-device environments.

Although several approximate nearest neighbor(ANN) benchmark tools have been released, few provide performance metrics that isolate the impact of different storage layers.

By varying only the storage layer, MoVeBench enables a direct evaluation of the trade-offs between two widely used storage structures, B-Tree and LSM-Tree, in your on-device environment.

MoVeBench uses LibSQL as the B-tree-based baseline built on SQLite3, and LSMoVe, our port of LibSQL's DiskANN layer to SQLite4, to evaluate an LSM-tree-based storage layer.

## Datasets

We provide four pre-trained datasets based on each different embedding algorithms which can be used in MoVeBench; SIFT , GloVe , COCO and Cohere.

| Dataset | Dimensions | # of Insert | # of Search | Distance |
|---|---:|---:|---:|---|
| SIFT | 128 | 100,000 | 10,000 | Euclidean | 
| GloVe | 200 | 100,000 | 10,000 | Cosine |
| COCO | 512 | 100,000 | 10,000 | Cosine |
| Cohere | 768 | 100,000 | 10,000 | Cosine |


## How To Run

Once you clone this repository, do

```
make -j
```

You can also build each engines with the code below.

```
make libsql     # LibSQL
make lsmove     # LSMoVe
make compact    # additional compaction code for LSMoVe
```

## Run

```
python3 benchmark.py
```

We provided benchmark with some helpful options.


## Results 
