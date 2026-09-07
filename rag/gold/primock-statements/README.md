# primock-statements

Sentences from the app's own clinical notes of PriMock57 consultations, each labelled with the NICE recommendations it should retrieve. A primary gold set: selection decisions are made on it.

```
statements.jsonl    one statement per line
```

Fields

```
qid             pm-NN
consult         PriMock57 recording the note came from
text            the sentence as the note wrote it, lightly trimmed
expected_ids    recommendation ids in the corpus; empty means nothing should be retrieved
expected_codes  guideline codes for the entity check
mode            sentence
rationale       why these recommendations
status          review date
```

Selection: conditions covered by the corpus; spread across referral, investigation, management and monitoring; plus negated findings, family history, and conditions the corpus does not cover, which carry no expected ids. Only clean rows: the sentence states a specific finding or action and the recommendation addresses exactly that for the same population.

Source notes: `bench/summarisation/notes/tier-default-standard/` in the research repo, banked for the note-tier baseline. Licence and attribution in `LICENCE` and `ATTRIBUTION`.
