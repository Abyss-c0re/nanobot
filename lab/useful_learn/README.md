# Useful-learn loop (algorithm development)

Goal: CubeChain must **develop usefully** — not inflate `self_teaches` while docked on static sensors.

## Measure

```bash
cd AI/nanobot
./lab/useful_learn/measure_robot.sh 12 5   # N samples, sleep sec
```

Watch columns: `useful` should rise on **clean / bump / state change**; `skipped` should rise when **docked static**.

## Algorithm (plugin continuous loop)

- **Skip teach** when docked + features unchanged (`last_teach_src=skip`).
- **Teach** on: bump, error, free_ok while cleaning/undocked clear, low battery undocked, state *transitions* only.
- Always **tick** for prediction/viz; only **feedback** when useful.
- Metrics: `useful_teaches`, `skipped_teaches` in `live_snap.json`.

## Next iterations (keep looping)

1. When clean starts (state→5), boost free_ok exclusivity / path odom features.
2. Optional: sample rockctl path deltas as extra digits when cleaning.
3. Meta conflict reduction: exclusive fire already in tick; measure agree/conflict ratio over clean cycles.
4. UI: show useful/skipped so Brain tab does not look “empty.”
