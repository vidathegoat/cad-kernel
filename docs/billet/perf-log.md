# Performance log

One entry per optimization attempt, newest first. Rejected attempts are logged too, so they are not retried (SPEC.md section 7.1). Phase 2 adds the ranked hotspot list below.

## Entry format

```
### YYYY-MM-DD: <operation>: <short title>

- Hotspot: function, share of total time, call path
- Expected change and gain: written before coding
- Change: what changed and where
- Before -> after: median wall time per affected operation (single-threaded and all cores), peak memory
- Verification: fingerprints, BREP diff, test grid, speed thresholds (SPEC.md 7.1)
- Result: accepted (commit) or rejected (reason)
```

## Hotspots

None recorded yet (Phase 2).

## Entries

None yet.
