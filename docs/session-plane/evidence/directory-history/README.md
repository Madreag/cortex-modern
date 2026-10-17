The original signed near-2 MiB fixture is GREEN; its prior HTTP 503 failure is retained in [capacity-green.json](capacity-green.json). The fixture and its capacity property are unchanged.

The live record stores an authenticated manifest of separately encrypted decision rows. Both are committed atomically, all decisions remain retained, and the 2 MiB record/entry limit remains enforced. The regression checks migration, a history larger than the limit surviving restart, corruption, transaction rollback, expiry, and both size bounds. The 142 directory tests and the separate original capacity fixture pass locally.

This changes the persisted format. Older service code refuses the new record; a rollback must restore matching service code and state. No directory was deployed, and this does not resolve the network fixture’s authority-wait failures.
