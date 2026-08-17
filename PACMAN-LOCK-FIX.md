# MSYS2 pacman lock recovery

This patch makes the Windows build recover from an interrupted/stale MSYS2 pacman database lock.

- It never deletes db.lck while pacman.exe is actually running.
- It waits up to 60 seconds for an active pacman process.
- If no pacman process owns the lock, it removes the stale lock.
- The MSYS2 package install retries up to three times if lock contention races with startup.
