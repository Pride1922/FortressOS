Set-Location "C:\Sources\FortressOS"

$owner = "Pride1922"
$projNum = 2

# 1. Create the issue with the strict telemetry and measurement contract
$issueBody = @"
## Focus
Measurement-only per-CPU telemetry for kernel-stack allocation/free workload on `main`.

## Safeguards & Acceptance Criteria
- [ ] Dedicated per-CPU telemetry structure (zero changes to generic spinlock behavior or \`cpu_local_t\`).
- [ ] Verify IRQ/NMI reentrancy and CPU stability at update sites (IF=0 alone does not exclude NMIs).
- [ ] Use ordered timestamps as elapsed TSC ticks (no raw cycles/time confusion).
- [ ] Separate uncontended acquisition overhead from contention wait. Define scan-step units per allocation path.
- [ ] Snapshot telemetry after worker CPUs quiesce; no resets while updates are active.
- [ ] No nested or overlapping timing totals added together.
- [ ] Bounded kstack alloc/free workload verified on worker CPUs across 1, 4, and 8 CPU cases with matching telemetry-disabled controls.
- [ ] Allocator policy, ownership, synchronization, and shootdown behavior remain unchanged.
- [ ] QEMU-only diagnostic results; defer Dell physical testing until specific hardware question arises.
"@

$issueUrl = gh issue create `
    --title "26. Memory telemetry — kstack bounded measurement (1/4/8 CPUs)" `
    --body $issueBody

Write-Host "Created issue: $issueUrl" -ForegroundColor Green

# 2. Add the issue as an item in Project #2
$itemRaw = gh project item-add$projNum --owner $owner --url$issueUrl --format json
$itemId = ($itemRaw | ConvertFrom-Json).id

# 3. Resolve Project and Field IDs
$projData = gh project list --owner$owner --format json | ConvertFrom-Json
$projId = ($projData.projects | Where-Object { $_.number -eq$projNum }).id

$fields = (gh project field-list $projNum --owner$owner --format json | ConvertFrom-Json).fields
$tierF = $fields \vert{} Where-Object {$_.name -eq "Tier" }
$subF  = $fields \vert{} Where-Object {$_.name -eq "Subsystem" }
$statF = $fields \vert{} Where-Object {$_.name -eq "Status" }

$critId = ($tierF.options \vert{} Where-Object {$_.name -eq "Critical Path" }).id
$mmId   = ($subF.options  \vert{} Where-Object {$_.name -eq "mm" }).id
$inProgId = ($statF.options \vert{} Where-Object {$_.name -eq "In Progress" }).id

# 4. Set board fields
if ($critId) {
    gh project item-edit --id $itemId --field-id$tierF.id --project-id $projId --single-select-option-id$critId | Out-Null
}
if ($mmId) {
    gh project item-edit --id $itemId --field-id$subF.id --project-id $projId --single-select-option-id$mmId | Out-Null
}
if ($inProgId) {
    gh project item-edit --id $itemId --field-id$statF.id --project-id $projId --single-select-option-id$inProgId | Out-Null
}

Write-Host "Issue #26 added to Project #2 and marked [In Progress | Critical Path | mm]!" -ForegroundColor Cyan