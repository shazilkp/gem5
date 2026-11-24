# Third-party corpus — Monocypher

The prevalence study and the A4 end-to-end target use an upstream Monocypher
checkout. The full clone is intentionally **not** committed (see
`../.gitignore`), so a fresh clone of this repository cannot run those scripts
without fetching it first.

Pinned upstream:

```
repo    https://github.com/LoupVaillant/Monocypher.git
commit  f8bb8dbd224556d58f5b4fa176c3f18fbfe50ebe
date    2026-11-22
```

Fetch (from `experiment/egdiff_eval/prevalence/`):

```bash
git clone https://github.com/LoupVaillant/Monocypher.git third_party/Monocypher
git -C third_party/Monocypher checkout f8bb8dbd224556d58f5b4fa176c3f18fbfe50ebe
```

Monocypher is dual-licensed CC0-1.0 / BSD-2-Clause; see the upstream `LICENCE.md`
in the checkout. The pinned commit is the one used for every number in
`../results/prevalence.md` and `../../RESULTS_A4.md`; changing it can change the
generated code and therefore the measured distances.
