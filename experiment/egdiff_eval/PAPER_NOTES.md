# EgDiff paper notes — quotes supporting the model

Source: `Revisiting Global Value Prediction: A Resurgent Complement to Local
Predictors`, Yang et al., ISCA 2026 (DOI 10.1109/ISCA66397.2026.00053).
Text extracted to `/tmp/opencode/egdiff_paper.txt`; the PDF is in
`docs/papers/Revisiting_Global_Value_Prediction_A_Resurgent_Complement_to_Local_Predictors.pdf`.

These quotes are the fidelity anchor for the attack: they establish that the
global value queue is populated for every load, and that the base value is
selected by distance with no base tag.

## Queue allocation is unconditional

Section III-B (Prediction):

> “Regardless of whether the prediction is valid or not, a new entry is
> allocated in the speculative queue for all candidates.”

This is the key sentence. A load with no valid prediction, or with a
misprediction, still gets a `SpecGVQ` entry. The victim's key load therefore
occupies a slot even if the predictor abstains or is trained on another PC.

Table I (“Speculative Queue Entry Example”) shows entries with `VP = 0` and
`Misp = 0/1` that are nevertheless present in the queue, with `Value` and
`Distance`/`Diff` fields. That is the shape the attack needs.

## Base value is selected by distance, with no base tag

Section III-B (Prediction):

> “The speculative queue is then accessed using the distance info to retrieve
> the base value. The predicted value is computed as the sum of the base value
> and the stride value.”

The queue entry fields are `Value`, `Distance`, `Diff`, `VP`, `Misp`,
`Avail.`, `Wake` (Table I). There is no base PC, base tag, or producer
identity in the entry. The link between the consumer and the producer is the
dynamic distance alone. That is the untagged-queue property the attack uses.

## EVES does not gate allocation; it only populates values earlier

Section V-A:

> “The value queue is primarily updated by results produced during speculative
> execution. In addition, at dispatch time, if the 32KB EVES predictor provides
> a valid prediction, this predicted value is also pushed into the queue so that
> subsequent instructions can use it as a base value before the true producer
> result becomes available.”

> “It should be noted that EVES predictions are only used to populate the value
> queue, they are not written into the PRF and therefore have no direct impact
> on performance.”

Section V-A also reports the ablation:

> “we disable the EVES-assisted mechanism and re-measure the performance… IPC
> speedup decreasing slightly from 3.24% to 3.11%. Coverage is reduced from
> 26.15% to 22.45%.”

So the no-EVES baseline is a valid EgDiff configuration, and the queue is
still populated from execution results. Our model implements that baseline.
The optional EVES-assisted population is not implemented; it would push
*predicted* values into the queue earlier, which can only make a base value
available sooner and cannot remove the attack. (It may add attacker-visible
bases from the local predictor, but that is a separate question.)

## Speculative entries are squashed; committed values are not

Section III (architecture):

> “The speculative segment is indexed identically to the LQ, as both are
> circular buffers allocated and squashed in lockstep.”

> “On a pipeline squash, all speculative entries younger than the squash point
> are invalidated.”

Section III-B (Update):

> “The actual values of committed instructions are inserted into the
> non-speculative part and used to update the global value predictor.”

Our model matches: `squashFrom` drops speculative entries younger than the
squash point; the NVQ is not squashed. Mode 2 relies on the NVQ; Mode 1 uses a
surviving speculative entry or the NVQ.

## Confidence, allocation, and suppression

Section III-A:

- 3-bit FPC with probability vector `{1, 1/4, 1/4, 1/8, 1/8, 1/8, 1/8}`.
- Tagged table entry; allocation only if `u == 0`, with probability `1/16`.
- 10-bit TICK decays `u`.
- `lastMisp` suppresses all predictions for 1024 instructions after a
  misprediction.

Our model implements these. The `lastMisp` window is the paper's Section
III-A mechanism, not an implementation invention.

## What the paper does not discuss

A search for `security`, `attack`, `Spectre`, `side channel`, `confidential`,
and `transient` in the extracted text finds no security analysis. The paper
does not claim isolation between loads from different security domains, and it
does not tag the queue base. That is the gap the attack explores.

## Consequence for the review

R1’s load-bearing objection — “if the paper’s EVES selector gates queue
population, the attack has no carrier” — is **resolved by the paper**: EVES
does not gate population, and allocation is unconditional for all candidates.
The remaining fidelity deviation is the optional EVES-assisted *early value
population*, which is not required for the attack and can only make bases
available sooner.
