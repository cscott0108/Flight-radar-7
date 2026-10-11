// 0.1.9: per-provider poll diagnostics (see aircraft_provider.h "per-provider poll diagnostics").
// Recorded for every request whatever the Provider diagnostics level; logged by level. Host-tested
// (host_tests/provider_poll_diag_test.c). Writers: RadarTask (the provider clients). Reader: /diag (httpd).
#include "aircraft_provider.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "diag_telemetry.h"

static const char *TAG = "AircraftProvider"; /* same tag as the other provider diagnostics */

static ProviderPollDiag *s_poll; /* PSRAM, AIRCRAFT_PROVIDER_COUNT entries; NULL = not recorded */
#define SAMPLE_LINE_MAX 224
static char *s_sampleLine;      /* PSRAM, Raw samples (RadarTask only); NULL = no samples */
static portMUX_TYPE s_pollMux = portMUX_INITIALIZER_UNLOCKED;

void ProviderPollDiag_Init(void)
{
    if (!s_poll) /* one-time, under 1 KB of PSRAM; no internal RAM */
        s_poll = heap_caps_calloc(AIRCRAFT_PROVIDER_COUNT, sizeof(ProviderPollDiag), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_sampleLine)
        s_sampleLine = heap_caps_calloc(1, SAMPLE_LINE_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}


const char *ProviderPollOutcome_Name(ProviderPollOutcome o)
{
    switch (o) {
    case PPOLL_NONE: return "no request yet";
    case PPOLL_OK: return "ok";
    case PPOLL_OK_EMPTY: return "ok, no aircraft";
    case PPOLL_OK_PARTIAL: return "ok, partial (aircraft list full)";
    case PPOLL_FETCHED: return "response received";
    case PPOLL_JSON_INVALID: return "invalid JSON";
    case PPOLL_UNEXPECTED_SHAPE: return "unexpected JSON document";
    case PPOLL_HTTP_STATUS: return "HTTP error status";
    case PPOLL_RATE_LIMITED: return "rate limited";
    case PPOLL_TRANSPORT: return "transport failure";
    case PPOLL_TRUNCATED: return "response truncated (buffer full)";
    case PPOLL_CLIENT_INIT: return "HTTP client not created";
    case PPOLL_IN_PROGRESS: return "request in progress";
    default: return "?";
    }
}

const char *ProviderTransportKind_Name(ProviderTransportKind k)
{
    switch (k) {
    case PXPORT_NONE: return "none";
    case PXPORT_TIMEOUT: return "timeout";
    case PXPORT_CONNECT: return "connect/TLS failed";
    case PXPORT_INCOMPLETE: return "incomplete response";
    case PXPORT_CLOSED: return "connection closed";
    case PXPORT_HEADER: return "no valid response header";
    case PXPORT_WRITE: return "request send failed";
    default: return "other";
    }
}

static uint32_t UptimeS(void) { return DiagTelemetry_NowMs() / 1000u; }

static ProviderPollDiag *PollSlot(AircraftProviderType t)
{
    return (s_poll && t < AIRCRAFT_PROVIDER_COUNT) ? &s_poll[t] : NULL;
}

uint32_t ProviderDiag_FetchStart(ProviderFetch *f, AircraftProviderType type, const char *url)
{
    memset(f, 0, sizeof(*f));
    f->type = type;
    f->t0Ms = DiagTelemetry_NowMs();
    f->contentLength = -1;
    ProviderPollDiag *d = PollSlot(type);
    if (!d)
        return 0;
    char u[sizeof(d->url)]; /* formatted outside the critical section */
    snprintf(u, sizeof(u), "%s", url ? url : "");
    taskENTER_CRITICAL(&s_pollMux);
    d->polled = true;
    d->seq++;
    const uint32_t seq = d->seq;
    d->startUptimeS = f->t0Ms / 1000u;
    memcpy(d->url, u, sizeof(u));
    /* this request's fields start empty, so /diag never shows the previous request's result under its number */
    d->durationMs = 0;
    d->httpStatus = 0;
    d->bytes = d->received = 0;
    d->contentLength = -1;
    d->complete = d->truncated = false;
    d->transport = PXPORT_NONE;
    d->espErr = d->tlsErr = d->tlsFlags = 0;
    d->fetchOutcome = d->outcome = PPOLL_IN_PROGRESS;
    d->haveParse = false;
    d->jsonErrorOffset = -1;
    taskEXIT_CRITICAL(&s_pollMux);
    return seq;
}

void ProviderDiag_FetchEnd(const ProviderFetch *f, ProviderPollOutcome outcome)
{
    const char *name = AircraftProviderType_Name(f->type);
    const uint32_t ms = DiagTelemetry_NowMs() - f->t0Ms;
    ProviderPollDiag *d = PollSlot(f->type);
    const uint32_t seq = d ? d->seq : 0; /* written only by this task (RadarTask) */
    char err[sizeof(((ProviderPollDiag *)0)->lastError)];
    if (outcome == PPOLL_TRANSPORT)
        snprintf(err, sizeof(err), "request #%u: %s (esp_err 0x%x, TLS 0x%x)", (unsigned)seq,
                 ProviderTransportKind_Name(f->transport), (unsigned)f->espErr, (unsigned)f->tlsErr);
    else
        snprintf(err, sizeof(err), "request #%u: %s (HTTP %d, %u bytes)", (unsigned)seq, ProviderPollOutcome_Name(outcome),
                 f->httpStatus, (unsigned)f->bytes);
    const uint32_t nowS = UptimeS();
    if (d) {
        taskENTER_CRITICAL(&s_pollMux);
        d->durationMs = ms;
        d->httpStatus = f->httpStatus;
        d->bytes = f->bytes;
        d->received = f->received;
        d->contentLength = f->contentLength;
        d->complete = f->complete;
        d->truncated = f->truncated;
        d->transport = f->transport;
        d->espErr = f->espErr;
        d->tlsErr = f->tlsErr;
        d->tlsFlags = f->tlsFlags;
        d->fetchOutcome = outcome;
        d->outcome = outcome;
        d->haveParse = false;
        d->jsonErrorOffset = -1;
        if (outcome != PPOLL_FETCHED) {
            d->haveError = true;
            d->lastErrorUptimeS = nowS;
            memcpy(d->lastError, err, sizeof(err));
        }
        taskEXIT_CRITICAL(&s_pollMux);
    }
    if (outcome != PPOLL_FETCHED && AircraftProvider_GetDebugLevel() >= PROVIDER_DEBUG_NORMAL) {
        if (outcome == PPOLL_TRANSPORT)
            ESP_LOGW(TAG, "[%s] request #%u failed after %u ms: %s (esp_err 0x%x, esp-tls error 0x%x flags 0x%x)", name,
                     (unsigned)seq, (unsigned)ms, ProviderTransportKind_Name(f->transport), (unsigned)f->espErr,
                     (unsigned)f->tlsErr, (unsigned)f->tlsFlags);
        else
            ESP_LOGW(TAG, "[%s] request #%u: %s after %u ms (HTTP %d, %u of %u bytes kept, Content-Length %lld)", name,
                     (unsigned)seq, ProviderPollOutcome_Name(outcome), (unsigned)ms, f->httpStatus, (unsigned)f->bytes,
                     (unsigned)f->received, (long long)f->contentLength);
    }
    if (AircraftProvider_GetDebugLevel() >= PROVIDER_DEBUG_VERBOSE)
        ESP_LOGI(TAG, "[%s] request #%u detail: elapsed=%u ms status=%d received=%u stored=%u content-length=%lld complete=%s truncated=%s",
                 name, (unsigned)seq, (unsigned)ms, f->httpStatus, (unsigned)f->received, (unsigned)f->bytes,
                 (long long)f->contentLength, f->complete ? "yes" : "no", f->truncated ? "yes" : "no");
}

void ProviderDiag_ParseStats(AircraftProviderType type, ProviderPollOutcome outcome, const ProviderParseStats *s,
                             int jsonErrorOffset)
{
    ProviderPollDiag *d = PollSlot(type);
    const bool ok = outcome == PPOLL_OK || outcome == PPOLL_OK_EMPTY || outcome == PPOLL_OK_PARTIAL;
    if (d && !ok && d->truncated) /* d->truncated is written only by this task */
        outcome = PPOLL_TRUNCATED; /* the parse failed because the body was cut off */
    char err[sizeof(((ProviderPollDiag *)0)->lastError)];
    snprintf(err, sizeof(err), "request #%u: %s", (unsigned)(d ? d->seq : 0), ProviderPollOutcome_Name(outcome));
    const uint32_t nowS = UptimeS();
    if (d) {
        taskENTER_CRITICAL(&s_pollMux);
        d->outcome = outcome;
        d->haveParse = s != NULL;
        if (s)
            d->parse = *s;
        d->jsonErrorOffset = jsonErrorOffset;
        if (ok) {
            d->haveSuccess = true;
            d->lastSuccessUptimeS = nowS;
        } else {
            d->haveError = true;
            d->lastErrorUptimeS = nowS;
            memcpy(d->lastError, err, sizeof(err));
        }
        taskEXIT_CRITICAL(&s_pollMux);
    }
    if (AircraftProvider_GetDebugLevel() >= PROVIDER_DEBUG_VERBOSE && s)
        ESP_LOGI(TAG, "[%s] parse stages: %s; entries=%d malformed=%d missing_id=%d missing_position=%d invalid_position=%d "
                      "on_ground=%d (ground_shown=%d) not_examined_list_full=%d admitted=%d",
                 AircraftProviderType_Name(type), ProviderPollOutcome_Name(outcome), s->entries, s->malformed, s->missingId,
                 s->missingPosition, s->invalidPosition, s->onGround, s->groundShown, s->notExamined, s->admitted);
}

void ProviderDiag_HeapBeforeTls(const char *provider)
{
    if (AircraftProvider_GetDebugLevel() < PROVIDER_DEBUG_NORMAL) return;
    ESP_LOGI(TAG, "[%s] Before aircraft TLS: internal heap free=%u largest=%u, DMA largest=%u, PSRAM free=%u", provider,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

void ProviderDiag_Sample(const char *provider, const char *fmt, ...)
{
    if (AircraftProvider_GetDebugLevel() < PROVIDER_DEBUG_RAW) return;
    char *line = s_sampleLine; /* PSRAM; RadarTask only (the parsers' single caller): no stack or internal buffer */
    if (!line)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, SAMPLE_LINE_MAX, fmt, ap);
    va_end(ap);
    ESP_LOGI(TAG, "[%s] sample: %s", provider, line);
}

bool ProviderDiag_GetPoll(AircraftProviderType type, ProviderPollDiag *out)
{
    memset(out, 0, sizeof(*out));
    ProviderPollDiag *d = PollSlot(type);
    if (!d)
        return false;
    taskENTER_CRITICAL(&s_pollMux);
    *out = *d;
    taskEXIT_CRITICAL(&s_pollMux);
    return true;
}
