// hw_settings.h
//
// Loads per-board calibration values from data/hw_settings.ini, so they
// don't have to be hardcoded in source (they vary radio to radio: crystal
// filter tolerance, PA output flatness, TCXO trim).

#ifndef HW_SETTINGS_H
#define HW_SETTINGS_H

void hw_settings_load(void);

#endif /* HW_SETTINGS_H */
