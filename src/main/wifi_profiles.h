#pragma once
/* Saved Wi-Fi networks ("profiles"), up to WIFI_PROFILE_SLOTS, with one authoritative store: NVS
 * namespace "wifi". Slot 0 IS the legacy single-network entry (keys "ssid"/"pass"), so a device
 * that already has a saved network keeps it with no migration; slots 1..4 use "ssid1".."ssid4" /
 * "pass1".."pass4". "lastgood" (u8) remembers the slot that last connected.
 *
 * Passwords never leave this module except through WifiProfiles_Get (used only to connect); the
 * WebUI is given WifiProfileInfo, which carries no password. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WIFI_PROFILE_SLOTS 5
#define WIFI_SSID_MAX 32
#define WIFI_PASS_MAX 64 /* 8..63 printable characters, or exactly 64 hex digits */
#define WIFI_FAILS_BEFORE_SWITCH 3

typedef enum {
    WP_OK = 0,
    WP_BAD_SLOT,
    WP_BAD_SSID,
    WP_BAD_PASSWORD,
    WP_DUPLICATE, /* another slot already has this SSID */
    WP_FULL,
    WP_NOT_FOUND, /* slot is empty */
    WP_STORAGE    /* NVS failure */
} WifiProfileResult;

typedef struct {
    bool used;
    bool hasPassword; /* false = open network */
    char ssid[WIFI_SSID_MAX + 1];
} WifiProfileInfo;

const char *WifiProfiles_ResultText(WifiProfileResult r);

/* Reads which slots are in use (NVS) into a small RAM mask and loads "lastgood". */
void WifiProfiles_Init(void);

int WifiProfiles_Count(void);
bool WifiProfiles_SlotUsed(int slot);
bool WifiProfiles_Info(int slot, WifiProfileInfo *out);
/* Credentials for connecting. ssid/pass buffers must hold WIFI_SSID_MAX+1 / WIFI_PASS_MAX+1. */
bool WifiProfiles_Get(int slot, char *ssid, size_t ssidCap, char *pass, size_t passCap);

/* password == NULL on Update keeps the saved password. */
WifiProfileResult WifiProfiles_Add(const char *ssid, const char *password, int *slotOut);
WifiProfileResult WifiProfiles_Update(int slot, const char *ssid, const char *password);
WifiProfileResult WifiProfiles_Delete(int slot);
/* Same SSID already saved -> its password is replaced; otherwise added to the first free slot. */
WifiProfileResult WifiProfiles_Upsert(const char *ssid, const char *password, int *slotOut);

bool WifiProfiles_ValidSsid(const char *ssid);
bool WifiProfiles_ValidPassword(const char *password);

/* ---- connection selection / failover (RAM only; safe to call from the event-loop task) ---- */
int WifiProfiles_FirstSlotToTry(void); /* last good, else lowest used; -1 = none saved */
void WifiProfiles_SetTrySlot(int slot); /* -1 = manual connection: never rotates */
int WifiProfiles_TrySlot(void);
/* Call on every STA disconnect. Returns the slot to switch to after WIFI_FAILS_BEFORE_SWITCH
 * consecutive failures (next used slot, wrapping), or -1 = retry the same network. */
int WifiProfiles_OnDisconnect(void);

/* Same bookkeeping, but also says what to do when the threshold is reached with no other usable
 * profile: WIFI_RETRY_NOW (below threshold), WIFI_RETRY_SWITCH (*nextOut = slot), or
 * WIFI_RETRY_BACKOFF (defer the retry so the setup AP gets uninterrupted radio time). */
typedef enum { WIFI_RETRY_NOW = 0, WIFI_RETRY_SWITCH, WIFI_RETRY_BACKOFF } WifiRetryAction;
WifiRetryAction WifiProfiles_OnDisconnectAction(int *nextOut);
/* The deferred retry is about to run: its failure counts as the threshold-reaching one. */
void WifiProfiles_OnBackoffRetry(void);
void WifiProfiles_ResetFailures(void); /* on STA connected (RAM only) */
/* Persists "lastgood" (only when it changed, to spare flash). Call off the event-loop task. */
void WifiProfiles_OnConnected(void);
