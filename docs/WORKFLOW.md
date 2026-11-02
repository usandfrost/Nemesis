# How changes go in

`main` is what works: the checks pass on it. Changes go in through a branch and a pull request. Everything is checked on this computer; there is no CI (no GitHub Actions).

1. **Branch** from an up-to-date `main`: `git switch main && git pull && git switch -c <kind>/<short-name>`. Kinds: `feature/`, `fix/`, `docs/`, `workflow/`.
2. **Commit** in steps that make sense on their own. The message says what changed and why; the first line is the summary.
3. **Check** before pushing: `tools/check_all.sh` (the host tests, the preset files, the DSP fork against upstream, the ARM build; about a minute and a half), or `tools/check_all.sh --quick` for the host tests and the preset files only. The firmware's flash headroom is about 13 KB: say so in the PR if a change moves it.
4. **Push** the branch and open a pull request against `main` (`gh pr create`). The template lists what to say and what was checked.
5. **Review**: a separate reviewer and a separate tester go over a feature before it merges (they report; the author fixes), as for every phase so far.
6. **Merge** when it passes and the review is done: squash merge (`gh pr merge --squash --delete-branch`), so `main` gets one commit per pull request.

What GitHub cannot enforce here: branch protection needs a paid plan for a private repository, so "no direct pushes to `main`" is a rule we keep, not a setting. Tests must build on Linux as well as macOS (include `<math.h>` and the like yourself); `tools/check_all.sh` does not catch that, a build on another system does.
