#include "boot_warmup.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "psa/crypto.h"

#include "adv_diag.h"

static const char *TAG = "WARMUP";

static WarmupStatus s_cryptoStatus = WARMUP_NOT_RUN;
static WarmupStatus s_netStatus = WARMUP_NOT_RUN;

WarmupStatus BootWarmup_CryptoStatus(void) { return s_cryptoStatus; }
WarmupStatus BootWarmup_NetStatus(void) { return s_netStatus; }

const char *BootWarmup_StatusText(WarmupStatus s)
{
    switch (s)
    {
    case WARMUP_OK: return "OK";
    case WARMUP_FAILED: return "failed (falls back to first-use initialization)";
    default: return "not run yet";
    }
}

// AES-CTR over buffers in PSRAM, longer than the 128-byte DMA threshold:
// PSA ESP AES driver -> esp_aes_crypt_ctr -> esp_aes_process_dma_ext_ram,
// the same path the TLS records take. First use creates the crypto shared
// GDMA channels and the AES/SHA crypto lock (both permanent by design).
static bool WarmupAesDma(void)
{
    const size_t len = 1024;
    const size_t outCap = PSA_CIPHER_ENCRYPT_OUTPUT_SIZE(PSA_KEY_TYPE_AES, PSA_ALG_CTR, len);
    uint8_t *in = heap_caps_calloc(1, len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *out = heap_caps_malloc(outCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bool ok = false;
    psa_key_id_t key = 0;

    if (!in || !out)
    {
        ESP_LOGW(TAG, "AES warm-up: no PSRAM for buffers");
        goto done;
    }

    static const uint8_t keyBytes[16] = {0}; // throwaway key; output discarded
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, 128);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT);
    psa_set_key_algorithm(&attr, PSA_ALG_CTR);

    psa_status_t st = psa_import_key(&attr, keyBytes, sizeof(keyBytes), &key);
    psa_reset_key_attributes(&attr);
    if (st != PSA_SUCCESS)
    {
        ESP_LOGW(TAG, "AES warm-up: psa_import_key failed (%d)", (int)st);
        goto done;
    }

    size_t outLen = 0;
    st = psa_cipher_encrypt(key, PSA_ALG_CTR, in, len, out, outCap, &outLen);
    if (st != PSA_SUCCESS)
    {
        ESP_LOGW(TAG, "AES warm-up: psa_cipher_encrypt failed (%d)", (int)st);
        goto done;
    }
    ok = true;

done:
    if (key)
        psa_destroy_key(key);
    heap_caps_free(in);
    heap_caps_free(out);
    return ok;
}

// secp256r1 key generation + public-key export: computing Q = d*G needs a
// modular inverse -> mbedtls_mpi_gcd_modinv_odd -> hardware exp-mod, whose
// first use installs the bignum completion interrupt (permanent by design).
static bool WarmupMpi(void)
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attr, 256);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_EXPORT);

    psa_key_id_t key = 0;
    psa_status_t st = psa_generate_key(&attr, &key);
    psa_reset_key_attributes(&attr);
    if (st != PSA_SUCCESS)
    {
        ESP_LOGW(TAG, "MPI warm-up: psa_generate_key failed (%d)", (int)st);
        return false;
    }

    uint8_t pub[PSA_EXPORT_PUBLIC_KEY_OUTPUT_SIZE(PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1), 256)];
    size_t pubLen = 0;
    st = psa_export_public_key(key, pub, sizeof(pub), &pubLen);
    psa_destroy_key(key);
    if (st != PSA_SUCCESS)
    {
        ESP_LOGW(TAG, "MPI warm-up: psa_export_public_key failed (%d)", (int)st);
        return false;
    }
    return true;
}

void BootWarmup_Crypto(void)
{
    if (s_cryptoStatus != WARMUP_NOT_RUN)
        return;

    AdvDiag_HeapCheckpoint("Warm-up: before crypto");
    AdvDiag_AllocTraceBegin("Crypto warm-up");

    int64_t t0 = esp_timer_get_time();
    bool aesOk = WarmupAesDma();
    bool mpiOk = WarmupMpi();
    int64_t t1 = esp_timer_get_time();

    AdvDiag_AllocTraceEnd("Crypto warm-up");
    AdvDiag_HeapCheckpoint("Warm-up: after crypto");

    s_cryptoStatus = (aesOk && mpiOk) ? WARMUP_OK : WARMUP_FAILED;
    ESP_LOGI(TAG, "Crypto warm-up %s (AES-DMA %s, MPI %s, %lld ms)",
             s_cryptoStatus == WARMUP_OK ? "OK" : "incomplete",
             aesOk ? "ok" : "failed", mpiOk ? "ok" : "failed",
             (long long)((t1 - t0) / 1000));
}

void BootWarmup_NetCurrentTask(void)
{
    if (s_netStatus != WARMUP_NOT_RUN)
        return;

    AdvDiag_HeapCheckpoint("Warm-up: before net (task-local)");
    AdvDiag_AllocTraceBegin("Net warm-up");

    // Numeric host, no AI_NUMERICHOST: lwip_getaddrinfo ->
    // netconn_gethostbyname_addrtype_n -> this task's per-thread semaphore
    // (sys_thread_sem_init + pthread TLS), the same chain as the first real
    // DNS lookup. dns_gethostbyname parses the literal; nothing is sent.
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    struct addrinfo *res = NULL;
    int rc = getaddrinfo("127.0.0.1", NULL, &hints, &res);
    if (res)
        freeaddrinfo(res);

    AdvDiag_AllocTraceEnd("Net warm-up");
    AdvDiag_HeapCheckpoint("Warm-up: after net (task-local)");

    s_netStatus = rc == 0 ? WARMUP_OK : WARMUP_FAILED;
    ESP_LOGI(TAG, "Net warm-up %s in task '%s' (getaddrinfo rc=%d)",
             rc == 0 ? "OK" : "failed", pcTaskGetName(NULL), rc);
}
