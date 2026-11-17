---
name: ship-ticket
description: Verify, document and PR the current WEB-N ticket
---
1. Identify the ticket from the branch name.
2. Clean rebuild, then run all tests and scripts/check_*.sh. Report failures and stop if any fail.
3. Create or update docs/tickets/WEB-N.md with scope, bugs found and verification output.
4. Update the README feature checklist and the roadmap. Every part of the README that mentions
   the ticket's work must change, not just the feature docs:
   - **Built section:** add a checked `- [x]` item for each deliverable of the ticket (feature,
     tests, scripts), under the right milestone heading. Planned items the ticket completes move
     from Planned to Built.
   - **Progress section:** recount from the checklist, never from the old numbers (the old
     numbers can be stale). Count `- [x]` items (done) and `- [x]` plus `- [ ]` items for M1-M5
     (total; M6 is excluded). Then update the percentage, the bar (20 chars, `round(pct/5)`
     hashes), the "N of M required items" text and the "M required items" sentence in "How it is
     computed".
   - **Milestone headings:** fix the `(done/total)` counts on every heading the ticket touched,
     and reconcile them with the actual list under it.
   - Mention any pre-existing count error you found when you report back.
   - Before committing, grep README.md for the ticket id and for the old percentage and counts
     to catch leftovers.
5. Make small unsigned commits, push the branch and run `gh pr create --base main`.
6. Re-read the final README diff against the list in step 4 before pushing.
7. Print a checklist showing each step as done or not done.
