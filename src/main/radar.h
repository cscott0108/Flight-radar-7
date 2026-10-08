#pragma once

#include "opensky_client.h"
#include "ui.h"

extern int selectedAircraft;
extern char selectedIcao24[16];

Aircraft *Radar_GetSelectedAircraft(void);

void Radar_ReconcileSelection(void);

void Radar_SetAutoSelectClosest(bool enabled);
bool Radar_GetAutoSelectClosest(void);
/* A manual Prev/Next choice: auto-select holds it for a while (auto_select.h). */
void Radar_NoteManualSelection(void);

void Radar_Init(void);

void Radar_SetCenter(
    float lat,
    float lon,
    float radiusKm);

void Radar_Refresh(void);

void Radar_AttachToObject(
    lv_obj_t *obj);

void Radar_SweepTick(void);

/* Zero-traffic display idle (see radar.c): main.c reports whether idle dim is active; the radar
 * render is frozen only while that is true AND gAircraftCount == 0. */
void Radar_SetIdleDimActive(bool active);
bool Radar_IsDisplayIdle(void);
/* Ask for one radar frame even while frozen (safe from any task). */
void Radar_RequestRedraw(void);
/* 0.0.32: display rotation R in degrees (north_ref.h): display bearing =
 * true bearing - R; 0 when the radar is true-up. Used by the projection. */
float Radar_DisplayRotationDeg(void);

void Radar_PredictAircraft(void);
extern bool showAircraftLabels;

/* Offsets from radar center in pixels; shared by screen and WebUI preview. */
bool Radar_ProjectPosition(float lat, float lon, float centerLat, float centerLon,
                           float radiusKm, int radiusPixels, int *x, int *y);

/* Geographic bearing (compass convention: 0=N, 90=E, ...) from
 * (centerLat,centerLon) to (lat,lon), plus the great-circle-approximated
 * ground distance in km (same flat-earth approximation Radar_ProjectPosition
 * and the rest of this file already use - accurate enough at radar ranges).
 * Returns false only if any input is non-finite; never guesses a bearing for
 * bad data. This is the ONE authoritative bearing/range calculation - used
 * for the off-screen aircraft indicator so it never needs (and must never
 * use) aircraft heading. */
bool Radar_GeoBearingAndDistance(float lat, float lon, float centerLat, float centerLon,
                                 float *bearingDegOut, float *distanceKmOut);
