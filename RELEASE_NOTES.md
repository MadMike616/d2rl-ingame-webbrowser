## Changes

- Successful first-run file setup now records a completion token in the plugin TOML and is skipped on later launches.
- Existing installations and user settings are preserved; unsuccessful setup can be retried.
- The browser panel layout is created once and preserved. A missing layout after completion is reported instead of silently recreated.
