---
name: ship-ticket
description: Verify, document and PR the current WEB-N ticket
---
1. Identify the ticket from the branch name.
2. Clean rebuild, then run all tests and scripts/check_*.sh. Report failures and stop if any fail.
3. Create or update docs/tickets/WEB-N.md with scope, bugs found and verification output.
4. Update the README feature checklist and the roadmap.
5. Make small unsigned commits, push the branch and run `gh pr create --base main`.
6. Print a checklist showing each step as done or not done.
