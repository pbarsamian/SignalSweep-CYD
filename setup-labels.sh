#!/usr/bin/env bash
# Creates or updates the backlog labels. Safe to re-run (--force).
set -euo pipefail
gh label create "type:feature"   --color 1d76db --description "New feature or change"            --force
gh label create "type:bug"       --color d73a4a --description "Something is not working"          --force
gh label create "type:chore"     --color cfd3d7 --description "Maintenance, docs, tooling"        --force
gh label create "status:idea"    --color fbca04 --description "Captured, not yet refined"         --force
gh label create "status:refined" --color 0e8a16 --description "Acceptance criteria are ready"     --force
gh label create "claude"         --color 8957e5 --description "Queued for Claude Code (human adds this)" --force
gh label create "needs-human"    --color b60205 --description "Blocked: needs a human answer"     --force
