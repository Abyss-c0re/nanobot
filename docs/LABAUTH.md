# nanobot + labauth

**labauth** is a separate project (`~/Dev/labauth`). Browser OAuth session sealing
(`session.key` / `nbenc1:`) remains nanobot-local.

When `/mnt/data/master.key` exists (USB enrolled), future work may:

- Treat master as ultimate grant for peer WRITE + shell
- Mint peer tokens sealed under master
- Align `session.key` derivation with master (optional)

Do **not** rebrand labauth as nanobot.
