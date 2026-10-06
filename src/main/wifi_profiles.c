#include "wifi_profiles.h"

#include <stdio.h>
#include <string.h>

#include "nvs.h"

#define NS "wifi"

/* RAM mirror of "which slots hold an SSID": makes the failover decision RAM-only. A single byte,
 * so concurrent readers (event-loop task) and the writer (httpd / LVGL task) never see a torn value. */
static volatile uint8_t s_usedMask;
static volatile int8_t s_lastGood = -1;
static volatile int8_t s_persistedLastGood = -1;
static volatile int8_t s_trySlot = -1;
static volatile uint8_t s_failures;

const char *WifiProfiles_ResultText(WifiProfileResult r)
{
    switch (r) {
    case WP_OK: return "Saved";
    case WP_BAD_SLOT: return "Invalid slot";
    case WP_BAD_SSID: return "Network name must be 1-32 characters";
    case WP_BAD_PASSWORD: return "Password must be empty (open network), 8-63 characters, or 64 hex digits";
    case WP_DUPLICATE: return "That network name is already saved in another slot";
    case WP_FULL: return "All 5 slots are in use";
    case WP_NOT_FOUND: return "That slot is empty";
    default: return "Could not write the setting";
    }
}

static void Keys(int slot, char *ssidKey, char *passKey)
{
    if (slot == 0) {
        strcpy(ssidKey, "ssid");
        strcpy(passKey, "pass");
    } else {
        snprintf(ssidKey, 12, "ssid%d", slot);
        snprintf(passKey, 12, "pass%d", slot);
    }
}

static bool SlotOk(int slot) { return slot >= 0 && slot < WIFI_PROFILE_SLOTS; }

bool WifiProfiles_ValidSsid(const char *s)
{
    if (!s)
        return false;
    size_t n = strlen(s);
    if (n < 1 || n > WIFI_SSID_MAX)
        return false;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)s[i] < 0x20 || s[i] == 0x7f)
            return false;
    return true;
}

bool WifiProfiles_ValidPassword(const char *p)
{
    if (!p)
        return false;
    size_t n = strlen(p);
    if (n == 0)
        return true; /* open network */
    if (n >= 8 && n <= 63) {
        for (size_t i = 0; i < n; i++)
            if ((unsigned char)p[i] < 0x20 || p[i] == 0x7f)
                return false;
        return true;
    }
    if (n == 64) {
        for (size_t i = 0; i < n; i++) {
            char c = p[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
                return false;
        }
        return true;
    }
    return false;
}

static bool ReadSsid(nvs_handle_t h, int slot, char *out, size_t cap)
{
    char sk[12], pk[12];
    Keys(slot, sk, pk);
    size_t n = cap;
    if (nvs_get_str(h, sk, out, &n) != ESP_OK || out[0] == '\0')
        return false;
    return true;
}

void WifiProfiles_Init(void)
{
    uint8_t mask = 0;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        char ssid[WIFI_SSID_MAX + 1];
        for (int i = 0; i < WIFI_PROFILE_SLOTS; i++)
            if (ReadSsid(h, i, ssid, sizeof(ssid)))
                mask |= (uint8_t)(1u << i);
        uint8_t lg = 0xFF;
        if (nvs_get_u8(h, "lastgood", &lg) != ESP_OK || lg >= WIFI_PROFILE_SLOTS || !(mask & (1u << lg)))
            lg = 0xFF;
        s_lastGood = (lg == 0xFF) ? -1 : (int8_t)lg;
        s_persistedLastGood = s_lastGood;
        nvs_close(h);
    } else {
        s_lastGood = s_persistedLastGood = -1;
    }
    s_usedMask = mask;
    s_failures = 0;
    s_trySlot = -1;
}

bool WifiProfiles_SlotUsed(int slot) { return SlotOk(slot) && (s_usedMask & (1u << slot)); }

int WifiProfiles_Count(void)
{
    int n = 0;
    for (int i = 0; i < WIFI_PROFILE_SLOTS; i++)
        n += WifiProfiles_SlotUsed(i);
    return n;
}

bool WifiProfiles_Info(int slot, WifiProfileInfo *out)
{
    if (!out || !SlotOk(slot))
        return false;
    memset(out, 0, sizeof(*out));
    if (!WifiProfiles_SlotUsed(slot))
        return true;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK)
        return false;
    bool ok = ReadSsid(h, slot, out->ssid, sizeof(out->ssid));
    if (ok) {
        char sk[12], pk[12];
        Keys(slot, sk, pk);
        size_t n = 0;
        out->used = true;
        out->hasPassword = nvs_get_str(h, pk, NULL, &n) == ESP_OK && n > 1;
    }
    nvs_close(h);
    return ok;
}

bool WifiProfiles_Get(int slot, char *ssid, size_t ssidCap, char *pass, size_t passCap)
{
    if (!SlotOk(slot) || !ssid || !pass || ssidCap < WIFI_SSID_MAX + 1 || passCap < WIFI_PASS_MAX + 1)
        return false;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK)
        return false;
    bool ok = ReadSsid(h, slot, ssid, ssidCap);
    pass[0] = '\0';
    if (ok) {
        char sk[12], pk[12];
        Keys(slot, sk, pk);
        size_t n = passCap;
        if (nvs_get_str(h, pk, pass, &n) != ESP_OK)
            pass[0] = '\0'; /* no password key = open network */
    }
    nvs_close(h);
    return ok;
}

/* Another slot (not `except`) already holds this exact SSID? */
static int FindSsid(nvs_handle_t h, const char *ssid, int except)
{
    char cur[WIFI_SSID_MAX + 1];
    for (int i = 0; i < WIFI_PROFILE_SLOTS; i++)
        if (i != except && WifiProfiles_SlotUsed(i) && ReadSsid(h, i, cur, sizeof(cur)) && strcmp(cur, ssid) == 0)
            return i;
    return -1;
}

/* Password first, SSID second: a write interrupted in between leaves a slot with no SSID, i.e. an
 * unused slot, never a half-valid profile. */
static WifiProfileResult WriteSlot(nvs_handle_t h, int slot, const char *ssid, const char *password)
{
    char sk[12], pk[12];
    Keys(slot, sk, pk);
    if (password) {
        if (nvs_set_str(h, pk, password) != ESP_OK)
            return WP_STORAGE;
    }
    if (nvs_set_str(h, sk, ssid) != ESP_OK)
        return WP_STORAGE;
    if (nvs_commit(h) != ESP_OK)
        return WP_STORAGE;
    s_usedMask |= (uint8_t)(1u << slot);
    return WP_OK;
}

WifiProfileResult WifiProfiles_Update(int slot, const char *ssid, const char *password)
{
    if (!SlotOk(slot))
        return WP_BAD_SLOT;
    if (!WifiProfiles_ValidSsid(ssid))
        return WP_BAD_SSID;
    if (password && !WifiProfiles_ValidPassword(password))
        return WP_BAD_PASSWORD;
    if (!WifiProfiles_SlotUsed(slot))
        return WP_NOT_FOUND;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK)
        return WP_STORAGE;
    WifiProfileResult r = (FindSsid(h, ssid, slot) >= 0) ? WP_DUPLICATE : WriteSlot(h, slot, ssid, password);
    nvs_close(h);
    return r;
}

WifiProfileResult WifiProfiles_Add(const char *ssid, const char *password, int *slotOut)
{
    if (!WifiProfiles_ValidSsid(ssid))
        return WP_BAD_SSID;
    if (!password || !WifiProfiles_ValidPassword(password))
        return WP_BAD_PASSWORD;
    int free = -1;
    for (int i = 0; i < WIFI_PROFILE_SLOTS; i++)
        if (!WifiProfiles_SlotUsed(i)) { free = i; break; }
    if (free < 0)
        return WP_FULL;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK)
        return WP_STORAGE;
    WifiProfileResult r;
    if (FindSsid(h, ssid, -1) >= 0) {
        r = WP_DUPLICATE;
    } else {
        r = WriteSlot(h, free, ssid, password);
        if (r == WP_OK && slotOut)
            *slotOut = free;
    }
    nvs_close(h);
    return r;
}

WifiProfileResult WifiProfiles_Upsert(const char *ssid, const char *password, int *slotOut)
{
    if (!WifiProfiles_ValidSsid(ssid))
        return WP_BAD_SSID;
    if (!password || !WifiProfiles_ValidPassword(password))
        return WP_BAD_PASSWORD;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK)
        return WP_STORAGE;
    int existing = FindSsid(h, ssid, -1);
    nvs_close(h);
    if (existing >= 0) {
        WifiProfileResult r = WifiProfiles_Update(existing, ssid, password);
        if (r == WP_OK && slotOut)
            *slotOut = existing;
        return r;
    }
    return WifiProfiles_Add(ssid, password, slotOut);
}

WifiProfileResult WifiProfiles_Delete(int slot)
{
    if (!SlotOk(slot))
        return WP_BAD_SLOT;
    if (!WifiProfiles_SlotUsed(slot))
        return WP_NOT_FOUND;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK)
        return WP_STORAGE;
    char sk[12], pk[12];
    Keys(slot, sk, pk);
    /* SSID first: the slot becomes unused immediately; the password is then removed. */
    esp_err_t e = nvs_erase_key(h, sk);
    if (e != ESP_OK && e != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(h);
        return WP_STORAGE;
    }
    s_usedMask &= (uint8_t)~(1u << slot);
    (void)nvs_erase_key(h, pk);
    WifiProfileResult r = nvs_commit(h) == ESP_OK ? WP_OK : WP_STORAGE;
    if (r == WP_OK && s_persistedLastGood == slot) {
        (void)nvs_erase_key(h, "lastgood");
        (void)nvs_commit(h);
        s_persistedLastGood = -1;
    }
    nvs_close(h);
    if (s_lastGood == slot)
        s_lastGood = -1;
    return r;
}

/* ---- selection / failover ---- */

static int NextUsedAfter(int slot)
{
    for (int step = 1; step <= WIFI_PROFILE_SLOTS; step++) {
        int i = (slot + step) % WIFI_PROFILE_SLOTS;
        if (WifiProfiles_SlotUsed(i))
            return i;
    }
    return -1;
}

int WifiProfiles_FirstSlotToTry(void)
{
    if (s_lastGood >= 0 && WifiProfiles_SlotUsed(s_lastGood))
        return s_lastGood;
    for (int i = 0; i < WIFI_PROFILE_SLOTS; i++)
        if (WifiProfiles_SlotUsed(i))
            return i;
    return -1;
}

void WifiProfiles_SetTrySlot(int slot)
{
    s_trySlot = (int8_t)(SlotOk(slot) ? slot : -1);
    s_failures = 0;
}

int WifiProfiles_TrySlot(void) { return s_trySlot; }

WifiRetryAction WifiProfiles_OnDisconnectAction(int *nextOut)
{
    if (nextOut)
        *nextOut = -1;
    if (++s_failures < WIFI_FAILS_BEFORE_SWITCH)
        return WIFI_RETRY_NOW;
    s_failures = 0;
    if (s_trySlot >= 0) {
        int next = NextUsedAfter(s_trySlot);
        if (next >= 0 && next != s_trySlot) {
            s_trySlot = (int8_t)next;
            if (nextOut)
                *nextOut = next;
            return WIFI_RETRY_SWITCH;
        }
    }
    return WIFI_RETRY_BACKOFF; /* no other usable profile: retrying at once would only keep the radio busy */
}

int WifiProfiles_OnDisconnect(void)
{
    int next = -1;
    return WifiProfiles_OnDisconnectAction(&next) == WIFI_RETRY_SWITCH ? next : -1;
}

void WifiProfiles_OnBackoffRetry(void)
{
    /* One attempt after a backoff: its failure reaches the threshold at once (switch if another
     * profile exists, otherwise back off again). */
    s_failures = WIFI_FAILS_BEFORE_SWITCH - 1;
}

void WifiProfiles_ResetFailures(void) { s_failures = 0; }

void WifiProfiles_OnConnected(void)
{
    s_failures = 0;
    int slot = s_trySlot;
    if (slot < 0 || !WifiProfiles_SlotUsed(slot))
        return;
    s_lastGood = (int8_t)slot;
    if (s_persistedLastGood == slot)
        return;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK)
        return;
    if (nvs_set_u8(h, "lastgood", (uint8_t)slot) == ESP_OK && nvs_commit(h) == ESP_OK)
        s_persistedLastGood = (int8_t)slot;
    nvs_close(h);
}
