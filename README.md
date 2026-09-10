# ESP32-C3 Long Term RTC Clock Test

This repository accompanies the article "**How precise is the timing of an ESP32-C3 processor in deep sleep? A personal experiment answers this question.**" published here: https://medium.com/@androidcrypto/how-precise-is-the-timing-of-an-esp32-c3-processor-in-deep-sleep-12ccb7412fb2?sharedUserId=androidcrypto

## ESP32-C3 Long-Term Internal RTC Accuracy Tester

### Description:

This sketch evaluates the drift and accuracy of the internal Real-Time Clock (RTC) of an ESP32-C3 microcontroller over an extended period.

### Key Features:

- Initial One-Time Sync: Synchronizes internal RTC via NTP upon initial boot.
- Periodic Drift Measurement: Connects to WiFi at set intervals to query.
- NTP time and immediately samples local RTC time to calculate drift.
- Power Failure Detection: Re-initializes time sync on brownout/power loss and appends a "BOOT" marker to the log file.
- LittleFS Data Logging: Appends CSV time-stamp entries to local flash.
- Manual Reset Feature: Holding the BOOT button (GPIO 9) for 3 seconds during startup deletes the log file (/rtc_log.txt), resets all persistent counters, and provides visual confirmation via LED flashing (GPIO 8).
- Automated Email Reporting: Sends formatted email updates with total runtime, current drift, and attached log file via SMTP.
- Low-Power & Hardware Friendly: Reduces WiFi TX power (11dBm) to prevent ESP32-C3 Super Mini power supply brownouts. Supports optional Deep Sleep.

### Data Log Format (CSV):

 - [RTC_TIMESTAMP],[DRIFT_SECONDS],[ONLINE_STATUS/EVENT]
 - Example: 1725360000,2,Y    (Regular NTP check, RTC lagging by 2s)
 - Example: 1725363600,0,BOOT (System reset / Power failure recovered)

## Example mail
````plaintext
[Deep Sleep] -3033s - ESP32-C3 Long-Term RTC Accuracy Report

ESP32-C3 Long-Term RTC Clock Test Report
----------------------------------------
Data logging for 4688 minutes (78 hours)
Current RTC Time Difference: -3033 seconds.

Please find the original LittleFS log file attached to this email.
````

## Example attachment
````plaintext
1788600660,0,BOOT
1788604263,-32,Y
1788607869,-71,Y
1788611475,-108,Y
...
1788874757,-2957,Y
1788878364,-2995,Y
1788881970,-3033,Y
````

## Development Environment (Arduino)
````plaintext
Arduino IDE Version 2.3.10 (MacOS)
arduino-esp32 boards Version 3.3.11 (https://github.com/espressif/arduino-esp32) that is based on Espressif ESP32 Version 5.5.5
````
