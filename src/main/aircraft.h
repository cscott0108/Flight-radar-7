#pragma once

#define MAX_AIRCRAFT 200

typedef struct
{
    char icao24[12];
    char callsign[16];

    float longitude;
    float latitude;

    float altitude;
    float velocity;
    float heading;

    bool valid;
} Aircraft;

extern Aircraft gAircraft[MAX_AIRCRAFT];
extern int gAircraftCount;