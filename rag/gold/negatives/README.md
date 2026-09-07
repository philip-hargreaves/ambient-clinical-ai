# negatives

Inputs that must retrieve nothing. Written for this project; no third-party text.

```
negatives.jsonl    one item per line: qid, kind, mode, text
```

Two kinds

- non-clinical text: a synthetic CV, meeting notes, a recipe, a software changelog, a weather report, a match report. The CV is the v1 failure case.
- clinical notes for conditions with no guideline in the corpus, each checked against every guideline title and recommendation text before inclusion: tennis elbow, ingrowing toenail, motion sickness, athlete's foot, seborrhoeic dermatitis, nosebleed, plantar fasciitis, chilblains, jet lag, ganglion, hiccups, stye.

Used for the false-positive rate and the threshold calibration. No expected ids.
