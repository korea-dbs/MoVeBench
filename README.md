# MoVesBench

Overview of MoVesBench

### Datasets

how to download test datasets

### Build

On the root directory, do

```
make
```

If you want to build separately, do

```
make libsql
make lsmobivec
make compact
```

### Run

```
python3 benchmark.py
```

We provided benchmark with some helpful options. see below;


### Evaluate


--fixing--

## [DBS]

### 파일 및 디렉토리 설명
    
`dataset/` : 데이터 각각의 insert/query/groundtruth 포함

**데이터셋 용량문제로 인해 허깅페이스에 따로 업로드 했습니다.**

https://huggingface.co/datasets/jiwanyuk/recall_test_dataset/tree/main

다운로드 방법

1. 허깅페이스 라이브러리 설치
  
```
pip install huggingface-hub
```

2. ./dataset/ 디렉토리에 다운로드
```  
hf download jiwanyuk/recall_test_dataset --repo-type dataset --local-dir ./dataset
```
- 또는 특정 파일만 다운로드 (e.g. `insert100k_sift.sql`):
```
hf download jiwanyuk/recall_test_dataset insert100k_sift.sql --repo-type dataset --local-dir ./dataset
```

---

`benchmark.py` : glove, sift 데이터 각각 sqlite4, sqlite3 한번에 돌리는 코드

### 테스트 코드 돌리는 법
0. numpy 설치된 파이썬 환경 준비
1. 각 sqlite 실행코드 및 sqlite4의 compact_db 코드 컴파일
2. `recall_test.py`
    
    `LSM_CONFIG_AUTOWORK = 1` 로 빌드했을 때 

    ```
    python3 recall_test.py --auto-compact 1
    ```

    `LSM_CONFIG_AUTOWORK = 0` 으로 빌드했을 때 

    ```
    python3 recall_test.py --auto-compact 0
    ```

    recall_test.py 안에 추가로 사용할 수 있는 옵션들 있으니 확인

3. `incremental_test.py`

    `LSM_CONFIG_AUTOWORK = 1` 로 빌드했을 때 

    ```
    python3 incremental_test.py --auto-compact 1
    ```

    `LSM_CONFIG_AUTOWORK = 0` 으로 빌드했을 때 

    ```
    python3 incremental_test.py --auto-compact 0
    ```

    incremental_test.py 안에 추가로 사용할 수 있는 옵션들 있으니 확인
