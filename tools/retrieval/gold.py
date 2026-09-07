"""Gold sets: map the UCL statements onto corpus recommendations, then build one query file.

  python gold.py map-ucl        -> rag/gold/ucl-triplets/mapping-draft.jsonl (hand-check, save as mapping.jsonl)
  python gold.py build          -> rag/results/queries/queries-<date>.jsonl

Gold inputs (each optional; missing sets are reported and skipped):
  rag/gold/st-georges-cases/cases.jsonl       {qid, text, expected_ids, expected_codes}
  rag/gold/ucl-triplets/triplets.csv + mapping.jsonl
  rag/gold/primock-statements/statements.jsonl {qid, text, expected_ids, expected_codes, mode}
  rag/gold/negatives/negatives.jsonl           {qid, text, kind}
"""

import argparse
import csv
import difflib
import re
import time

from common import GOLD, RESULTS, latest_chunks, log, read_jsonl, write_jsonl

UCL = GOLD / "ucl-triplets"
NON_NICE = {"anaphylaxis"}  # Resuscitation Council UK text, not in the corpus
GUIDELINE_CODES = [
    ("urinary", "ng109"), ("type1", "ng17"), ("type 1", "ng17"), ("type 2", "ng28"), ("thyroid", "ng145"),
    ("ovarian", "cg122"), ("hypertension", "ng136"), ("gastro", "cg184"), ("heart failure", "ng106"),
    ("kidney", "ng203"), ("anaphylaxis", "cg134"),
]


def code_for(name: str) -> str:
    name = name.lower()
    for key, code in GUIDELINE_CODES:
        if key in name:
            return code
    return ""


def norm_statement(s: str) -> str:
    s = re.sub(r"^\d+\.\s*", "", s.strip())
    s = re.sub(r"\s*[\(\[]\s*[\d\.]+(?:\s[^\)\]]*)?[\)\]]\s*$", "", s)
    return re.sub(r"\s+", " ", s).strip().lower()


def read_ucl():
    with open(UCL / "triplets.csv", encoding="utf-8-sig", newline="") as f:
        rows = list(csv.reader(f))[1:]
    out = []
    for r in rows:
        r = (r + [""] * 8)[:8]
        name = r[2].replace("_recommendation", "").strip()
        out.append({"guideline": name, "code": code_for(name), "statement": r[3].strip(),
                    "scenario": r[5].strip(), "question": r[6].strip()})
    return out


def map_ucl():
    chunks = read_jsonl(latest_chunks())
    by_code = {}
    for c in chunks:
        by_code.setdefault(c["code"], []).append(c)
    seen, drafts = set(), []
    for row in read_ucl():
        if row["statement"] in seen:
            continue
        seen.add(row["statement"])
        if any(k in row["guideline"].lower() for k in NON_NICE) or not by_code.get(row["code"]):
            drafts.append({**row, "best_id": "", "ratio": 0, "kind": "not-nice", "decision": "drop"})
            continue
        ns = norm_statement(row["statement"])
        best, ratio = None, 0.0
        for c in by_code[row["code"]]:
            r = difflib.SequenceMatcher(None, ns, c["text"].lower()).ratio()
            if r > ratio:
                best, ratio = c, r
        contained = len(ns) > 30 and ns[:60] in best["text"].lower()
        kind = "verbatim" if ratio >= 0.9 or contained else "near" if ratio >= 0.6 else "check"
        drafts.append({**row, "best_id": best["id"], "best_number": best["number"], "best_text": best["text"][:200],
                       "ratio": round(ratio, 3), "kind": kind,
                       "decision": "accept" if kind != "check" else "check"})
    out = UCL / "mapping-draft.jsonl"
    n = write_jsonl(out, drafts)
    kinds = {}
    for d in drafts:
        kinds[d["kind"]] = kinds.get(d["kind"], 0) + 1
    log(f"{n} statements -> {out}; {kinds}")


def split_sentences(text: str) -> list[str]:
    # Harness-only splitter; the engine uses ICU with clinical suppressions
    parts = re.split(r"(?<=[.!?])\s+(?=[A-Z0-9\"'(])|\n+", text)
    return [p.strip() for p in parts if len(p.split()) >= 3]


def build():
    queries = []

    cases = GOLD / "st-georges-cases" / "cases.jsonl"
    if cases.exists():
        for r in read_jsonl(cases):
            queries.append({"qid": r["qid"], "set": "st-georges", "text": r["text"], "mode": "note",
                            "expected_ids": r["expected_ids"], "expected_codes": r.get("expected_codes", []),
                            "negative": False})
    else:
        log("st-georges cases missing")

    mapping = UCL / "mapping.jsonl"
    if mapping.exists() and (UCL / "triplets.csv").exists():
        by_statement = {m["statement"]: m for m in read_jsonl(mapping)}
        kept = dropped = 0
        for i, row in enumerate(read_ucl()):
            m = by_statement.get(row["statement"])
            if not m or m["decision"] == "drop" or not m.get("expected_ids"):
                dropped += 1
                continue
            kept += 1
            queries.append({"qid": f"ucl-{i:03d}", "set": "ucl", "text": row["scenario"], "mode": "sentence",
                            "question": row["question"], "expected_ids": m["expected_ids"],
                            "expected_codes": [row["code"]], "negative": False})
        log(f"ucl: {kept} kept, {dropped} dropped")
    else:
        log("ucl mapping missing (run map-ucl, hand-check, save as mapping.jsonl)")

    primock = GOLD / "primock-statements" / "statements.jsonl"
    if primock.exists():
        for r in read_jsonl(primock):
            queries.append({"qid": r["qid"], "set": "primock", "text": r["text"], "mode": r.get("mode", "sentence"),
                            "expected_ids": r["expected_ids"], "expected_codes": r.get("expected_codes", []),
                            "negative": not r["expected_ids"]})
    else:
        log("primock statements missing")

    negatives = GOLD / "negatives" / "negatives.jsonl"
    if negatives.exists():
        for r in read_jsonl(negatives):
            queries.append({"qid": r["qid"], "set": "negatives", "text": r["text"], "mode": r.get("mode", "note"),
                            "kind": r.get("kind", ""), "expected_ids": [], "expected_codes": [], "negative": True})
    else:
        log("negatives missing")

    out = RESULTS / "queries" / f"queries-{time.strftime('%Y%m%d')}.jsonl"
    n = write_jsonl(out, queries)
    sets = {}
    for q in queries:
        sets[q["set"]] = sets.get(q["set"], 0) + 1
    log(f"{n} queries -> {out}; {sets}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["map-ucl", "build"])
    args = ap.parse_args()
    map_ucl() if args.cmd == "map-ucl" else build()


if __name__ == "__main__":
    main()
