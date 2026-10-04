#pragma once

#include <helpers/ESP32Board.h>
#include <helpers/AutoDiscoverRTCClock.h>
#include <helpers/ArduinoHelpers.h>
#include <helpers/sensors/EnvironmentSensorManager.h>
#include <XiaoS3Board.h>
#include <RemoteKissRadio.h>
#include <Esp32TcpKissLink.h>
#ifdef DISPLAY_CLASS
#include <helpers/ui/NullDisplayDriver.h>
#include <helpers/ui/MomentaryButton.h>
extern DISPLAY_CLASS display;
extern MomentaryButton user_btn;
#endif

extern XiaoS3Board board;
extern Esp32TcpKissLink kiss_link;
extern Esp32RemoteKissRadio radio_driver;
extern AutoDiscoverRTCClock rtc_clock;
extern EnvironmentSensorManager sensors;

bool radio_init();
mesh::LocalIdentity radio_new_identity();
