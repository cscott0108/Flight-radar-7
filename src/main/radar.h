#pragma once

#include "opensky_client.h"
#include "ui.h"

extern int selectedAircraft;
extern char selectedIcao24[16];

Aircraft *Radar_GetSelectedAircraft(void);

void Radar_ReconcileSelection(void);

void Radar_Init(void);

void Radar_SetCenter(
    float lat,
    float lon,
    float radiusKm);

void Radar_Refresh(void);

void Radar_AttachToObject(
    lv_obj_t *obj);

void Radar_SweepTick(void);

void Radar_PredictAircraft(void);
extern bool showAircraftLabels;