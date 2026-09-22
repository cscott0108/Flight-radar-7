#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MAX_AIRPORTS 10
#define AIRPORT_NAME_LENGTH 24
#define AIRPORT_DEFAULT_DIAMETER 12
#define AIRPORT_DEFAULT_COLOR 0xFF0000 /* red, the original fixed airport color */

typedef struct {
    char name[AIRPORT_NAME_LENGTH + 1];
    float latitude;
    float longitude;
    uint8_t diameter;
    uint32_t color; /* 0xRRGGBB */
} AirportMarker;

/* Call after nvs_flash_init(), before the web server or radar starts. */
bool Airports_Init(void);
size_t Airports_Count(void);
bool Airports_Get(size_t index, AirportMarker *out);
bool Airports_Save(int index, const AirportMarker *marker); /* -1 appends */
bool Airports_Delete(size_t index);
