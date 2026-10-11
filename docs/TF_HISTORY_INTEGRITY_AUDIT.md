# Aircraft-record lifecycle and storage-integrity audit

Read-only audit of firmware 0.1.4 (2026-10-08), followed by the targeted History Manager eviction fix in 0.1.5.
Source code is the authority; line numbers refer to 0.1.4 unless noted.

## Verdict

Leaving the 3,000-entry **Hot Seen** list does **not** lose an aircraft's history, because nothing is handed from
Seen to the TF card in the first place: **Seen and TF history are two parallel pipelines fed by the same poll**
(`main.c` `OnSuccessfulPoll`). The TF card receives its own record of each aircraft at the first History flush
after its first sighting (at most about 10 minutes), long before Seen's least-recently-seen eviction reaches it.
With a healthy card the evicted aircraft is already safe on the card. Seen eviction deliberately discards only
Seen's own copy (RAM, then its SPIFFS slot at the next Seen flush), including the provider-supplied operator name,
which TF does not store.

Real loss paths existed elsewhere; the most important one (History Manager slot eviction) is fixed in 0.1.5.

## Architecture (as implemented)

```
OpenSky / adsb.lol poll -> gAircraft[] (merged, <= 200) -> OnSuccessfulPoll()            main.c
   |
   +-> SeenAircraft_ObservePoll()   [only when Seen Logging is ON]
   |      RAM table, 3,000 records; when full the victim's slot is overwritten in place
   |      every 600 s: changed slots -> SPIFFS seen_aircraft.dat (fixed 84-byte slots)
   |                                   (the reused slot overwrites the victim on SPIFFS too)
   |
   +-> HistoryManager_Observe()     [TF_HISTORY_ENABLED; skips invalid/stale entries]
          RAM shadow table, 200 slots (PSRAM)
          every 600 s: dirty slots -> TfHistory_UpsertRecord()
                                        +- bucket file <fingerprint>.dat (append, or in-place update)
                                        +- indexv2.dat (one slot per ICAO24 -> current record)
          table full: slot reuse (0.1.5: see below)

Maintenance (idle_maint.c): once per local day at zero traffic -> the same two flushes, early.
Reboot (reboot_flush.c, all four reboot paths): the same two flushes, History bounded at 10 s.
```

There is no journal, pending queue, unsorted dump, compaction or archive stage. The only intermediate state is a
single in-RAM "pending orphan" (`s_pending` in `tf_history.c`) used to retry a failed index-slot write without
appending a duplicate. TF never deletes a record; it appends or updates the current record in place.
The "HOT -> WARM -> COLD" wording in older documentation suggested a flow from Seen into TF; there is none.

## Storage layers

| Layer | Medium | Limit | Key | Written | Removed / superseded | Role | After reboot |
|---|---|---|---|---|---|---|---|
| `gAircraft[]` | RAM | 200 | ICAO24 | every poll | next poll | live | empty |
| Seen table | PSRAM | 3,000 | ICAO24 | every poll | eviction (in place), `/seen/clear` | Seen copy | loaded from SPIFFS |
| `seen_aircraft.dat` | SPIFFS | 3,000 x 84 B | slot index | changed slots, 600 s after the first change | slot reused by a new aircraft | durable Seen copy | loaded at boot |
| History shadow slots | PSRAM | 200 | ICAO24 | every poll | reuse when full (0.1.5 rules) | write cache for TF | restored from TF on the next sighting |
| TF buckets `<fp>.dat` | TF card | card size | fingerprint + offset, per-record CRC | append (new aircraft, group or call-sign change) or in-place update, every 600 s | never deleted; older records stay as Superseded | **authoritative history** | read on demand |
| TF `indexv2.dat` | TF card | 32,768 slots, hard cap 90 % | ICAO24 -> (fp, offset), slot CRC | one slot per create or move | rewritten on move | index only, rebuildable | loaded; rebuilt only if invalid |

The 3,000 limit applies only to Seen. The TF index refuses NEW aircraft above 29,491 slots before anything is
written; no limit deletes or overwrites stored TF records.

## The 2,999 -> 3,000 -> next-aircraft scenario

1. Aircraft A becomes Seen record 3,000 (`AllocateSlot`, appended, dirty). Independently, `HistoryManager_Observe`
   gives it a shadow slot and the next 600-s History flush creates its TF record and index slot.
2. Aircraft B arrives with Seen full. `PickEvictionVictim` picks V (LRU within the class evicted first, or the
   oldest first-seen under FIFO). V's RAM record is overwritten by B immediately; nothing about V is written.
3. The next Seen flush writes the reused slot, so V's SPIFFS copy is overwritten too.
4. V's history depends only on the TF pipeline, which wrote it at the first History flush after V's first
   sighting - normally days earlier. Not preserved only if TF was absent, failing, full or at its index cap then.
5. The maintenance window does not matter for V; it only flushes what is dirty.

## Timing (from the code)

* Seen update: every successful poll. Seen to SPIFFS: `SeenAircraft_FlushIfDue`, 600 s after the first unsaved
  change; a failed write retries after another 600 s.
* TF writes and index changes: `HistoryManager_FlushIfDue`, every 600 s (first pass right after boot); failed
  slots stay dirty and are retried on every later pass.
* Synchronous TF write: only when a full History shadow table needs a slot (0.1.5: only if every slot is dirty).
* Maintenance: once per local day, >= 300 s after boot, after two consecutive zero-aircraft polls; flush only.

## Failure matrix (0.1.4 findings; 0.1.5 changes marked)

| Event | Seen | TF | Class |
|---|---|---|---|
| Deliberate reboot | flushed | flushed (bounded 10 s) | confirmed safe if the writes succeed |
| Power loss | <= 10 min of changes lost | <= 10 min of changes lost | conditional (documented) |
| Power loss during an in-place TF update | - | torn record fails CRC -> treated as absent -> overwritten with fresh statistics | **potential loss** (that aircraft's accumulated stats) - recommendation A |
| Power loss between append and index write | - | record durable but not indexed; the next sighting appends a duplicate with fresh stats; no automatic rediscovery | persisted, not indexed - recommendation B |
| TF write error / card full during a flush | - | slot stays dirty, retried every 600 s | conditional |
| TF failing while a History slot is reused | - | 0.1.4: written best-effort, slot cleared anyway, silently. **0.1.5: never cleared unless written; the new aircraft waits** | **fixed in 0.1.5** |
| Slot created while TF not ready, reused before any flush | - | 0.1.4: write not even attempted. **0.1.5: attempted like any other** | **fixed in 0.1.5** |
| TF absent / History disabled | Seen is the only copy | nothing written | Seen eviction = permanent loss by design |
| TF index at hard cap | Seen only | new aircraft refused before writing | documented policy, shown on /diag |
| Interrupted index build, bad bucket header | - | rebuilt next boot / repaired from file size, never truncated | confirmed safe (host-tested) |
| Truncated or bad-header `seen_aircraft.dat` | partial load, legacy CSV merge, full rewrite; or start empty and overwrite | - | potential Seen-only loss (not changed) |
| Seen `fclose` fails after slots were written | dirty flags already cleared | - | minor Seen-only loss (not changed) |
| Torn Seen slot | no per-record CRC | - | unknown impact (not changed) |

Unknown and only measurable on hardware: whether the SD card and FATFS write a 52-byte record atomically
(there is no fsync; durability relies on fclose).

## 0.1.5 fix: History Manager slot reuse never drops unsaved history

`history_manager.c` `AllocSlot` / `FlushSlotNow`:

1. A free slot is used if there is one.
2. Otherwise the least-recently-seen **clean** slot is reused: its latest state is already on the card and a later
   sighting restores it by lookup. No write is needed.
3. Only if every slot holds unsaved changes is the least-recently-seen one written synchronously, and the slot is
   reused only if that write returned success.
4. If the write fails, or TF is paused/unavailable, nothing is reused or cleared: the new aircraft is not admitted
   to History this time (it is still in Hot Seen and is offered again on its next poll). After a failed attempt,
   no further synchronous write is tried until the next flush pass, which retries every dirty slot anyway.

Counters on /diag (TF history section): "History Manager slot reuse" (clean / after a successful write) and
"History pending protection" (failed writes / new aircraft not admitted, ATTENTION when non-zero).
Formats, index layout, Seen policy and every other storage path are unchanged. RAM: +16 B internal (.bss).

Tests: `host_tests/hm_evict_test.c` (real History Manager, TF mock): clean-slot reuse with no write, clean slots
preferred over older dirty ones, all-dirty write-then-reuse with the victim's latest data, write failure (slots
byte-identical, one attempt, new aircraft refused, no retry storm), failed flush pass, recovery writing the pending
data, TF unavailable and paused (no attempt, nothing cleared), and an end-to-end invariant that every aircraft that
left the table was written. A mutant restoring the old behavior fails the test. `/diag` rows: `diag_harness.c`.

## Recommendations not implemented (separate decisions)

**A. Torn in-place record update (power loss during the ~50 ms write).** Detection is automatic already (record
CRC). Automatic *recovery of the lost statistics* is not possible without a second copy. Options:
1. Smallest mitigation: when the current record fails its CRC, append a new record instead of overwriting the
   damaged one (keeps the evidence; statistics restart). Small change, no format change.
2. Full protection: copy-on-write updates (append every update and move the index slot). No format change, but
   bucket growth of one 52-byte record per changed aircraft per flush (for example 100 aircraft x 6 flushes/h x
   24 h is about 14,400 records or 750 KB per day), longer scans and rebuilds.
3. Two alternating copies per aircraft: a format change. Not recommended.
Recommendation: (1) now; (2) only with an agreed card-growth budget.

**B. Orphan record after power loss between the append and the index-slot write.** Automatic recovery is
feasible with existing, tested code: keep an "unclean shutdown" marker file on the card (created at TF init,
removed by the clean unmount / reboot path); if it is present at boot, run the existing index rebuild
(`BuildIndexV2`, reads bucket files only, picks the newest record per aircraft by last seen). That re-indexes
orphans before the aircraft is seen again, so no duplicate is appended, and it also repairs torn index slots.
Cost: a rebuild (seconds at today's sizes, more as history grows) after every power loss, and one tiny file.
Alternative: a one-record write-ahead marker (ICAO24, fingerprint, offset) written before each append and
completed at boot - cheaper at boot, more code on the write path.

**Seen-only items (low severity, unchanged):** rename a bad or short `seen_aircraft.dat` aside instead of falling
back and overwriting it; clear Seen dirty flags only after `fclose` succeeds; per-record CRC for Seen slots
(format change); whether TF should also keep the provider-supplied operator name (format change).
