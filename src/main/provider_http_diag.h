#pragma once
/* 0.1.9: map an esp_http_client_perform() error to a ProviderTransportKind (diagnostics only; host-tested). */
#include "esp_err.h"
#include "esp_http_client.h"
#include "aircraft_provider.h"

static inline ProviderTransportKind ProviderHttp_TransportKind(esp_err_t err)
{
    switch (err) {
    case ESP_OK: return PXPORT_NONE;
    case ESP_ERR_HTTP_EAGAIN:
    case ESP_ERR_HTTP_READ_TIMEOUT:
    case ESP_ERR_HTTP_CONNECTING: return PXPORT_TIMEOUT;
    case ESP_ERR_HTTP_CONNECT: return PXPORT_CONNECT;
    case ESP_ERR_HTTP_INCOMPLETE_DATA: return PXPORT_INCOMPLETE;
    case ESP_ERR_HTTP_CONNECTION_CLOSED: return PXPORT_CLOSED;
    case ESP_ERR_HTTP_FETCH_HEADER: return PXPORT_HEADER;
    case ESP_ERR_HTTP_WRITE_DATA: return PXPORT_WRITE;
    default: return PXPORT_OTHER;
    }
}
