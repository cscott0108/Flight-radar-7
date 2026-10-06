#pragma once

/* Multi-provider reconciliation (0.0.28).
 *
 * When more than one provider is enabled, each provider's last successfully
 * parsed list is kept as a snapshot (PSRAM, allocated on first use) and the
 * one aircraft list (gAircraft) is rebuilt from all fresh snapshots after
 * every successful poll, with ICAO24 as the only identity:
 *
 *   - an ICAO24 reported by several providers becomes ONE entry;
 *   - position / altitude / speed / heading / on-ground state come from the
 *     most recent report (parse time; the provider that just polled wins a tie);
 *   - every text/hint field (and, 0.0.30, a speed or vertical rate the newer
 *     report did not carry) keeps the newer report's value when it has one and
 *     otherwise takes the other provider's value, so a field one provider
 *     never sends (registration, owner/operator, type hint: adsb.lol only;
 *     origin country: OpenSky only) is never erased by the other provider;
 *   - airborne entries before on-ground entries (the existing priority), the
 *     provider that just polled first, at most `max` entries.
 *
 * A snapshot older than its max age is ignored, so a failing provider's
 * aircraft fade out while the other provider keeps the list current.
 * With a single provider nothing here is used. RadarTask only (not
 * thread-safe by design: the snapshots have one writer and one reader). */

#include <stdbool.h>
#include <stdint.h>

#include "aircraft_provider.h"

/* Copy a provider's freshly parsed list (before stale carry). False if the
 * snapshot memory could not be allocated (the caller then keeps the plain
 * single-provider list). */
bool ProviderMerge_Store(AircraftProviderType provider, const Aircraft *list, int count, uint32_t nowMs);

/* Drop a provider's snapshot (provider disabled). */
void ProviderMerge_Forget(AircraftProviderType provider);

/* True when the provider has a snapshot no older than maxAgeMs. */
bool ProviderMerge_IsFresh(AircraftProviderType provider, uint32_t nowMs, uint32_t maxAgeMs);

/* Build the merged list into out[0..max). maxAgeMs[p] == 0 excludes provider
 * p. `first` is the provider that just polled. Returns the entry count. */
int ProviderMerge_Build(Aircraft *out, int max, uint32_t nowMs,
                        const uint32_t maxAgeMs[AIRCRAFT_PROVIDER_COUNT], AircraftProviderType first);

/* Field rule used by Build, exposed for tests: `newer` keeps its kinematics;
 * empty text / missing hint fields are filled from `older`. */
void ProviderMerge_Fill(Aircraft *newer, const Aircraft *older);

/* Host tests: free everything. */
void ProviderMerge_ResetForTest(void);
