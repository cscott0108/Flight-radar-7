#pragma once

/* One-time ESP-IDF initialization warm-ups.
 *
 * Several legitimate ESP-IDF subsystems allocate small permanent objects the
 * first time they are used (crypto shared GDMA channels, the AES/SHA crypto
 * lock, the bignum (MPI) completion interrupt, and lwIP's per-task netconn
 * semaphore). Left alone, all of these happen during the first OpenSky TLS
 * request, when internal RAM is nearly exhausted and the HTTP client's
 * buffers already occupy part of the last large DMA-capable block - so the
 * permanent objects land mid-block and split it (~14 KB -> ~7.4 KB), and the
 * aircraft TLS handshake later fails with "esp-aes: Failed to allocate
 * memory". Triggering them deliberately, through public APIs only, at a
 * point where nothing transient sits in front of them, keeps that block
 * intact. See PROJECT_STATE_COMPACT.md.
 *
 * Both functions log and continue on failure; the firmware then behaves
 * exactly as it did before the warm-up existed.
 */

#include <stdbool.h>

typedef enum
{
    WARMUP_NOT_RUN = 0,
    WARMUP_OK,
    WARMUP_FAILED,
} WarmupStatus;

// Call from app_main right after nvs_flash_init(). Uses PSA AES-CTR over
// PSRAM buffers (shared GDMA + AES/SHA lock) and PSA secp256r1 key
// generation (bignum interrupt).
void BootWarmup_Crypto(void);

// Call from the task that will make network requests (RadarTask), once,
// after lwIP is running and before its first network call. Creates that
// task's lwIP per-thread semaphore via getaddrinfo("127.0.0.1") - no
// network traffic.
void BootWarmup_NetCurrentTask(void);

WarmupStatus BootWarmup_CryptoStatus(void);
WarmupStatus BootWarmup_NetStatus(void);
const char *BootWarmup_StatusText(WarmupStatus s);
