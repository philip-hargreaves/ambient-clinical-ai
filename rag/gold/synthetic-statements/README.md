# synthetic-statements

Consultation extracts written for this project, each labelled with the NICE recommendations it should retrieve. A primary gold set, reported in its own column. Weighted towards rheumatology, the specialty of the client GPs, with general and paediatric rows for spread.

```
statements.jsonl    one item per line
```

Fields

```
qid             syn-NN
area            rheumatology, general, paediatric, control
kind            statement, extract, negated, family-history, history
condition       what the item is about
scenario        the clinical situation the text was written from
text            note-style text, one sentence for a statement, several for an extract
mode            sentence, or note for an extract
expected_ids    recommendation ids in the corpus; empty means nothing should be retrieved
expected_codes  guideline codes for the entity check
rationale       why these recommendations
status          review date
```

How the rows were written: the scenario first, then the text in the voice of the app's clinical notes, then the label by reading the guideline section. `gold.py build` refuses any row that shares a four-word sequence with a recommendation it expects, and reports lexical overlap per set.

Paediatric rows expect the children's guideline; a hit on the adult guideline fails the entity check. Control rows carry negated findings, family history or past history and expect nothing.

No third-party text. Written by the project team with model assistance and reviewed.
