# Guidance fixtures

Invented recommendations for engine tests, in the shape the indexer produces: one chunk per recommendation, id `<code>-<section>_<number>`, four made-up guidelines (`fx100` to `fx400`) with sibling recommendations that differ in one clause. `notes.jsonl` holds six notes with the ids each should surface, one with a negated finding that must not retrieve its guideline, one non-clinical text that must retrieve nothing. No guideline publisher's text appears here.

`embeddings.json` holds three of these texts with the vectors the evaluation harness produced for them under the staged embedder (`gte-large-int8`, mean pooling, normalised), so the engine's embedder can be checked against the harness.
