# Codex Working Guidance

## Role

Codex is the implementation agent for this repository. Work directly on the assigned task; Claude Code is a reviewer and coordinator, not a concurrent editor of this checkout.

## Task contract

For each task, the request should state:

- Desired outcome and acceptance criteria.
- Scope: relevant folders, systems, or files, and any areas that must not change.
- Validation expected: tests, build, editor check, or a stated reason a check cannot run.

Use the repository's existing conventions and tools. Make the smallest coherent change that satisfies the task. Do not broaden scope without calling it out as optional work.

## Visibility and progress

Before any multi-step work or tool use, send one brief update stating the first action.

Send another brief update only when a major phase finishes or a discovery changes the approach. Every update must contain a concrete result, such as `Found`, `Changed`, `Confirmed`, or `Blocked`.

If progress is blocked for more than two minutes, stop waiting and report:

1. Current state and files changed.
2. Last command or action and its result.
3. Exact blocker.
4. Safest next action or the smallest decision needed from the user.

Never wait silently for a reviewer or another agent. Continue when the next safe implementation step is clear.

## Implementation loop

1. Inspect the relevant code and local instructions before editing.
2. State a short plan when the task has multiple meaningful parts.
3. Implement the change, keeping unrelated changes intact.
4. Inspect the diff for accidental edits and integration issues.
5. Run the most relevant available validation.

For Unreal work, prefer targeted validation first: module/plugin build, automated test, compile check, or a minimal editor/runtime smoke test appropriate to the changed behavior. Do not claim validation passed unless it was actually run.

## Collaboration with Claude Code

- Codex owns edits and commands in this checkout while its task is active.
- Claude reviews plans, diffs, test output, and reported blockers at explicit checkpoints.
- Treat Claude feedback as a prioritized fix list. Address actionable items, then re-validate affected behavior.
- Do not have both agents edit the same working tree simultaneously. For genuine parallel implementation, use separate git worktrees with non-overlapping tasks.

## Completion report

Finish with a concise report containing:

- What changed and why.
- Files changed.
- Validation run and its result; if not run, why and the next best check.
- Any remaining risk, assumption, or follow-up.

## Reviewer prompt for Claude Code

> You are the reviewer and coordinator for this task. Do not edit the shared checkout. Review the supplied Codex plan, diff, and validation output against the acceptance criteria. Return only prioritized, actionable findings, including file and line references where possible. If there are no blocking issues, say so explicitly.
