# retrieval

Selection evaluation harness for the Guidelines feature. Reads `rag/sources` and `rag/gold`, writes `rag/candidates` and `rag/results`. Code only; no data here.

## Setup

```
py -3.12 -m venv rag\venv
rag\venv\Scripts\python -m pip install -r tools\retrieval\requirements.txt
```

`rag\venv`, `rag\candidates` and `rag\results` may be junctions to another drive. Checkpoints download to `rag\candidates\.src\<org>--<model>` without symlinks, so an exFAT drive works. IPv4 is forced for downloads; `RETRIEVAL_IPV6=1` disables that.

## Run order

```
python chunk.py nice                                   NICE JSON -> rag/results/chunks/nice-<fetchdate>.jsonl
python gold.py map-ucl                                 draft mapping; hand-check, save as mapping.jsonl
python gold.py build                                   -> rag/results/queries/queries-<date>.jsonl
python export.py --role embedder                       fp16 and int8 IRs -> rag/candidates/<id>-<precision>
python export.py --role reranker
python embed.py <id> --precision int8 --reference      -> rag/results/emb/<id>-int8-text
python evaluate.py <id> --precision int8 --rerankers gte-reranker-modernbert,minilm-l6 --hybrid off,on
python latency.py --embedders <ids> --rerankers <ids>
python pdf_compare.py                                  31 client PDFs, pypdfium2 against pdftotext
python vector_compare.py --emb rag/results/emb/<run>/docs.npy
```

Every run writes `config.json` beside its outputs under `rag/results/<stamp>-<name>/`.

## Files

```
shortlist.json      candidates: id, hf, role, licence, architecture, pooling, dims, max_length, instructions, export task
common.py           paths, jsonl io, run directories
chunk.py            recommendation chunks from NICE JSON; heading chunks from PDF text
gold.py             UCL mapping draft; unified query file from the four gold sets
export.py           optimum-cli export with provenance.json (revision, hashes)
embed.py            document embeddings on CPU; faithfulness against sentence-transformers
evaluate.py         recall@k, nDCG@10, MRR, P@1, entity match; threshold sweep on negatives; --v1-baseline
rerank_core.py      cross-encoder on ov.Core with explicit pairs; the default rerank backend
latency.py          per sentence, per note, scan, rerank 30 and 50 pairs; RSS
native_check.py     C++ proof against the Python pipelines and the reference model
pdf_compare.py      word agreement, order, recommendation patterns, image-only detection
vector_compare.py   numpy exact against hnswlib, usearch, sqlite-vec
native/             proof.cpp: same embed and rerank through the GenAI C++ pipelines, for parity
```

## Native proof

```
cmake -S tools\retrieval\native -B D:\ambient-rag\native-build -G "Visual Studio 18 2026" -A x64 ^
  -DOpenVINO_DIR=external\openvino\runtime\cmake -DOpenVINOGenAI_DIR=external\openvino\runtime\cmake
cmake --build D:\ambient-rag\native-build --config Release
proof embed  <model_dir> cls 512 texts.txt
proof rerank <model_dir> 512 "<query>" texts.txt
```

Compare the printed vectors with `embed.py` output; cosine 0.999 or better is the gate. Run from a shell with `external\openvino\setupvars.bat` applied so the DLLs resolve.

## Notes

- CPU only. The GPU stays free for the note model.
- The sentence splitter here is a regex for harness use; the engine uses ICU with clinical suppressions.
- Qwen3 rerankers get their instruct template from `evaluate.py`; the pipeline applies none.
- GenAI `TextRerankPipeline` (2026.3.0 and 2026.3.1) crashes on batched pairs with queries over about 30 words; `--rerank-backend genai` is for parity checks only.
- Gold files: `rag/gold/<set>/*.jsonl`, fields in `gold.py`.
