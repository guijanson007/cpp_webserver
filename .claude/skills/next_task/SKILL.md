---
name: next_task
description: Analyze the current state of this C++20 epoll HTTP server project against its goals in CLAUDE.md and hand the user the single most logical next task, written as a Jira-style ticket. Use when the user types /next_task or asks "what should I work on next".
---

# next_task

Produce ONE well-scoped, Jira-style ticket for the next piece of work. Do not
implement it. Respect the working agreement in `CLAUDE.md` (mentor mode for core
code: the user writes reactor, parser, router, thread pool; Claude may write tests,
CI, CMake, docs scaffolding).

## Step 1: Analyze current state (do this before answering, never skip)

Gather facts yourself; do not ask the user for anything you can look up.

1. Read `CLAUDE.md`: goals, milestones M1-M6, definition of done, known issues
   backlog, decisions that are settled.
2. `git log --oneline -20` and `git status`: what was done recently, uncommitted work.
3. `git ls-files` and read the relevant sources, including `tests/` and `docs/` if they exist.
4. Try to build and test when a build setup exists
   (`cmake -S . -B build && cmake --build build`, then `ctest --test-dir build`).
   Report failures as facts; a broken build or failing test outranks new work.
5. Check the M1-M6 checklist in `CLAUDE.md` against the code and mark each
   milestone as not started / in progress / done, with evidence (file or commit).
6. Re-verify every item in the "Known issues" list against the current code. Some
   may already be fixed; do not assign work that is already done.

## Step 2: Pick the task

Priority order:

1. Broken build, failing tests, crashes, sanitizer findings.
2. Open items in the current milestone, in dependency order (a task whose
   prerequisites are unmet is never the next task).
3. Known-issue backlog items that block the current milestone.
4. The first unstarted item of the next milestone, only if the current one is done.

Rules:
- Never pick M6 or roadmap items before M5 is complete.
- Size the task at about 1-3 hours (the user has 2-3 h/day). Split anything larger
  and ticket only the first slice, mentioning what follows.
- One concern per ticket, shippable and committable on its own.
- Prefer tasks that leave the project in a demonstrable state (a `curl` command that
  works, a test that passes).
- If a profiling decision from `CLAUDE.md` is now unlocked (e.g. the feature exists to
  measure), make a profiling ticket following baseline -> profile -> hypothesis ->
  change -> re-measure.

## Step 3: Output format

First, a short **State of the project** section (5-10 lines max): milestone status
table, build/test status, and what changed recently. Then the ticket:

```
## WEB-<n>: <imperative title>

**Type:** Task | Bug | Spike | Tech Debt | Test
**Milestone:** M<x> - <name>
**Priority:** Blocker | High | Medium | Low   (one-line reason)
**Estimate:** <hours>
**Mode:** Mentor (user writes the code) | Pair (Claude may write it)

### Description
Why this matters and the context, 2-5 sentences. Link to the goal in CLAUDE.md.

### Scope
- In: ...
- Out (explicitly): ...

### Acceptance criteria (Definition of Done)
- [ ] Verifiable, observable criteria (commands to run and expected output)
- [ ] Builds clean with no new warnings
- [ ] Tests added or updated (name them)
- [ ] No raw syscalls outside the wrapper layer; errors via Result<T, Error>
- [ ] Small, focused commit(s) with a descriptive message

### Technical notes / hints
Pointers to files and functions, relevant C++20 features, pitfalls. Hints, not a
finished solution, when in Mentor mode.

### How to verify
Exact commands (e.g. `curl -v localhost:8080/`, `ctest`, `wrk ...`).

### Dependencies
Blocked by / blocks, using ticket ids from earlier /next_task output if known.

### Interview talking point
One sentence the user should be able to defend after finishing.
```

Ticket numbering: derive `<n>` from the highest `WEB-<n>` found in `git log` messages
or `docs/` (start at WEB-1). Remind the user to put the ticket id in the commit message.

End with one line offering what Claude can do now (review once written, or write the
test/CI/CMake parts if the ticket is Pair mode). Do not start implementing.

## Don'ts

- Do not hand out several tasks. One ticket.
- Do not invent state: every claim about the code must come from files or commands
  run in Step 1.
- Do not modify project files, except when the user asks to save the ticket (then
  write it to `docs/tickets/WEB-<n>.md`).
