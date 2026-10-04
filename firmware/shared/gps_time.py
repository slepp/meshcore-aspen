# SPDX-License-Identifier: Apache-2.0
"""Connect the pinned UART and I2C GPS providers to the trusted clock."""
from pathlib import Path
import shutil


def stage_gps_time(target):
    sensors = Path(target) / "src/helpers/sensors"
    path = sensors / "MicroNMEALocationProvider.h"
    text = path.read_text()
    old = "_clock->setCurrentTime(getTimestamp());"
    previous = ("const uint32_t utc = getTimestamp();\n"
                "                    if (!meshcore::publishGpsTime(utc)) return;\n"
                "                    _clock->setCurrentTime(utc);")
    new = "if (!meshcore::publishGpsTime(getTimestamp())) return;"
    if old in text:
        if text.count(old) != 1:
            raise ValueError("Pinned NMEA clock update changed")
        text = text.replace(old, new)
        text = text.replace('#include "LocationProvider.h"',
                            '#include "LocationProvider.h"\n#include "GpsTime.h"', 1)
        path.write_text(text)
    elif previous in text:
        text = text.replace(previous, new, 1)
        path.write_text(text)
    elif new not in text:
        raise ValueError("Pinned NMEA clock update changed")
    if "meshcore::gpsRtcClock() = _clock;" not in text:
        constructor = "_pin_en(pin_en) {\n"
        if text.count(constructor) != 1:
            raise ValueError("Pinned NMEA clock constructor changed")
        text = text.replace(constructor, constructor +
                            "        meshcore::gpsRtcClock() = _clock;\n", 1)
        path.write_text(text)
    if "last_utc_sentence" not in text:
        replacements = (
            ("    long time_valid = 0;",
             "    long time_valid = 0;\n"
             "    unsigned long last_utc_sentence = 0;\n"
             "    bool utc_sentence_seen = false;"),
            ("            nmea.process(c);",
             "            if (nmea.process(c) && MicroNMEA::testChecksum(nmea.getSentence()) &&\n"
             '                !strcmp(nmea.getMessageID(), "RMC") && nmea.isValid()) {\n'
             "                last_utc_sentence = millis();\n"
             "                utc_sentence_seen = true;\n"
             "            }"),
            ("        if (!isValid()) time_valid = 0;",
             "        const bool fresh_utc = utc_sentence_seen && uint32_t(millis() - last_utc_sentence) <= 3000u;\n"
             "        if (!isValid() || !fresh_utc) time_valid = 0;"),
            ("            if (isValid()) {\n                time_valid ++;",
             "            if (isValid() && fresh_utc) {\n                time_valid ++;"),
        )
        for old, _ in replacements:
            if text.count(old) != 1:
                raise ValueError("Pinned NMEA UTC freshness anchor changed: " + old.strip())
        for old, new in replacements:
            text = text.replace(old, new, 1)
        path.write_text(text)
    path = sensors / "EnvironmentSensorManager.cpp"
    text = path.read_text()
    original = "    if (ublox_GNSS.getGnssFixOk(8)) {"
    fresh = ("    const bool fresh = ublox_GNSS.getPVT(8);\n"
             "    if (fresh && ublox_GNSS.getGnssFixOk(2)) {")
    original_epoch = "    _epoch = ublox_GNSS.getUnixEpoch(2);"
    utc = """    _epoch = 0;
    if (fresh && ublox_GNSS.getDateValid(2) && ublox_GNSS.getTimeValid(2) &&
        ublox_GNSS.getTimeFullyResolved(2)) {
      const auto year = ublox_GNSS.getYear(2);
      const auto month = ublox_GNSS.getMonth(2);
      // SparkFun's epoch conversion indexes tables for 2020..2099 and months 1..12.
      if (year >= 2020 && year <= 2099 && month >= 1 && month <= 12) {
        _epoch = ublox_GNSS.getUnixEpoch(2);
        if (_fix) meshcore::publishGpsTime(uint32_t(_epoch));
      }
    }"""
    if original in text and original_epoch in text:
        if text.count(original) != 1 or text.count(original_epoch) != 1:
            raise ValueError("Pinned RAK12500 UTC update changed")
        text = text.replace(original, fresh, 1).replace(original_epoch, utc, 1)
        text = text.replace("#include <SparkFun_u-blox_GNSS_Arduino_Library.h>",
                            '#include <SparkFun_u-blox_GNSS_Arduino_Library.h>\n#include "GpsTime.h"', 1)
        path.write_text(text)
    elif fresh not in text or utc not in text:
        raise ValueError("Pinned RAK12500 UTC update changed")
    shutil.copyfile(Path(__file__).with_name("GpsTime.h"), sensors / "GpsTime.h")
