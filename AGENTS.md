# Project operating rules

- Classify each work stream by urgency and dependency. Keep critical-path and safety-gate work running first. When streams compete for slots or resources, pause or interrupt lower-priority dashboard, broad-telemetry, polish, and documentation work; reuse freed capacity on the next ready critical task, then resume paused work after the blocker clears.
- Tell the user immediately about blockers or required physical actions. Do not wait for a future unblock or invent low-value work to fill slots.
- Give each live system or file-ownership area one writer. Run read-only preflight and backups before remote writes, and serialize conflicting writers.
