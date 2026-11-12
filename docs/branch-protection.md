# Branch protection for main

`main` has no required status check today: a per-commit job that runs past its
two-minute `timeout-minutes` concludes `cancelled`, not `failure`, and nothing
stops a merge on a cancelled, missing or failed check. Two things close that:

- `tools/land.py` (see [RELEASING.md, "Landing on main"](RELEASING.md#landing-on-main)),
  which merges only an up-to-date head whose per-commit jobs are all `success`;
- the ruleset in [.github/rulesets/main.json](../.github/rulesets/main.json),
  which makes GitHub itself refuse the merge.

## The ruleset

The file is the exact ruleset to apply. It requires, on the default branch:

- every per-commit job, by exact job name, reported by GitHub Actions
  (`integration_id` 15368, so no other app or commit status can satisfy it);
- a head that is up to date with `main` before it merges
  (`strict_required_status_checks_policy`);
- no bypass (`bypass_actors` is empty).

The required names are exactly the jobs a pull request starts in every
workflow it starts without a path filter, matrix rows expanded, the nightly
tier's `if: inputs.full` jobs left out. roadmap.yml is path-filtered, so it is
not a required check (it would block every pull request it does not run on);
`tools/land.py` judges it when it ran. `tools/check_branch_protection.py`, a
lint step, refuses the file when its names differ from the names derived from
the workflows, so a renamed, added or removed per-commit job is red until the
file follows; then the ruleset on GitHub must be updated with the command
below.

## Applying it

The repository owner applies it, once, with an account that administers the
repository:

```
gh api -X POST repos/mas-bandwidth/rocketnet/rulesets --input .github/rulesets/main.json
```

After a change to the file, update the same ruleset in place (its id is in
`gh api repos/mas-bandwidth/rocketnet/rulesets`):

```
gh api -X PUT repos/mas-bandwidth/rocketnet/rulesets/<id> --input .github/rulesets/main.json
```

## Decisions for the owner

- Bypass: the file has none. roadmap.yml's `regenerate` job pushes ROADMAP.md
  to `main` with the workflow token; under this ruleset that push is refused.
  Either add a bypass actor for it, or have that job open a pull request.
- A job that times out at the cap stays a refusal under the ruleset; GitHub
  offers no automatic re-run. `tools/land.py` does the one re-run.
