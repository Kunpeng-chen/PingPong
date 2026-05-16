# AGENTS.md

This file is the entry point for agents working on this repository.

Its purpose is to keep cross-session and cross-agent work aligned. Before creating plans, changing code, proposing PRs, or updating statuses, agents must use this file to locate the current rules and plans.

## 1. Required Reading Order

Before working on any task, read documents in this order:

1. `docs/rule/production-readiness-rules.md`
2. Relevant plan files under `docs/plan/`
3. `README.md`
4. Source files and tests related to the current task

Do not infer process rules from memory if the rule document exists.

## 2. Rule Documents

General execution rules are stored under:

```text
docs/rule/
```

Current rule entry:

```text
docs/rule/production-readiness-rules.md
```

Rule documents define:

- plan document naming
- phase splitting requirements
- plan priority evaluation rules
- status markers `[ ]` and `[√]`
- branch, PR, CI, and squash merge requirements
- README check requirements
- restrictions on mixing unrelated changes

Rule documents must stay generic. Do not write concrete feature plans or implementation roadmaps into `docs/rule/`.

## 3. Plan Documents

Concrete plans are stored under:

```text
docs/plan/
```

Plan documents must follow the naming rule defined in `docs/rule/production-readiness-rules.md`.

General format:

```text
<scope>-<topic>-plan.md
```

Example:

```text
docs/plan/runtime-state-dispatch-plan.md
```

Plans should be split into multiple independently verifiable phases according to complexity. A plan should not default to a single broad Phase.

Agents must not hardcode a single “current important plan” in this file. When choosing what to work on, agents should inspect `docs/plan/` and apply the priority rules from `docs/rule/production-readiness-rules.md`.

## 4. Agent Behavior Requirements

Agents must:

- read the rule document before drafting or modifying plans
- inspect relevant plans under `docs/plan/` before selecting or executing work
- place concrete plans under `docs/plan/`
- keep generic process rules under `docs/rule/`
- split complex plans into independently verifiable phases
- keep each implementation PR limited to one Phase or one clearly scoped task
- update existing tests when behavior changes
- check README after implementation PRs merge
- update plan status only after the required completion conditions are met

Agents must not:

- invent a new workflow without checking `docs/rule/`
- put concrete implementation plans in `docs/rule/`
- put generic workflow rules in `docs/plan/`
- hardcode a current priority plan in `AGENTS.md`
- mark `[√]` before PR merge, CI success, tests, README check, and status update
- mix multiple phases into one implementation PR
- change public API or protocol format unless the relevant plan explicitly allows it

## 5. When Rules and Plans Conflict

If a task conflicts with existing rules or plans:

1. Stop and identify the conflict.
2. Update the relevant rule or plan document first.
3. Then continue with the implementation or planning work.

Rules are authoritative for process. Plans are authoritative for the concrete work scope.

## 6. Repository-Specific Notes

PingPong is intended to remain a lightweight communication protocol module.

For internal refactors, prefer minimal changes that preserve:

- public API compatibility
- existing packet format, unless a plan explicitly changes it
- simple source layout, unless a plan explicitly requires splitting files
- testability and clear phase-level verification
