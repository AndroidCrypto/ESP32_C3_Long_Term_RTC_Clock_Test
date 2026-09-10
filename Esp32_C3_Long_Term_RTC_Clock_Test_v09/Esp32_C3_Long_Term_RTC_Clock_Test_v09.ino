/*
 * ============================================================================
 * ESP32-C3 Long-Term Internal RTC Accuracy Tester
 * ============================================================================
 *
 * Description:
 *   This sketch evaluates the drift and accuracy of the internal Real-Time
 *   Clock (RTC) of an ESP32-C3 microcontroller over an extended period.
 *
 * Key Features:
 *   - Initial One-Time Sync: Synchronizes internal RTC via NTP upon initial boot.
 *   - Periodic Drift Measurement: Connects to WiFi at set intervals to query
 *     NTP time and immediately samples local RTC time to calculate drift.
 *   - Power Failure Detection: Re-initializes time sync on brownout/power loss
 *     and appends a "BOOT" marker to the log file.
 *   - LittleFS Data Logging: Appends CSV time-stamp entries to local flash.
 *   - Manual Reset Feature: Holding the BOOT button (GPIO 9) for 3 seconds
 *     during startup deletes the log file (/rtc_log.txt), resets all persistent
 *     counters, and provides visual confirmation via LED flashing (GPIO 8).
 *   - Automated Email Reporting: Sends formatted email updates with total 
 *     runtime, current drift, and attached log file via SMTP.
 *   - Low-Power & Hardware Friendly: Reduces WiFi TX power (11dBm) to prevent
 *     ESP32-C3 Super Mini power supply brownouts. Supports optional Deep Sleep.
 *
 * Data Log Format (CSV):
 *   [RTC_TIMESTAMP],[DRIFT_SECONDS],[ONLINE_STATUS/EVENT]
 *   Example: 1725360000,2,Y    (Regular NTP check, RTC lagging by 2s)
 *   Example: 1725363600,0,BOOT (System reset / Power failure recovered)
 *
 * Language: English (Log outputs, emails, and CSV formatting)
 * ============================================================================
 */

// https://gemini.google.com/app/c0fba7c87cded451

#include <WiFi.h>
#include <WiFiUdp.h>
#include <time.h>
#include <LittleFS.h>
#include <ESP_Mail_Client.h>  // https://github.com/mobizt/ESP-Mail-Client version 3.4.24 DEPRECATED !!

#include <WiFi.h>
#include <time.h>
#include <LittleFS.h>
#include <ESP_Mail_Client.h>

// ============================================================================
// CONFIGURATION HEADER
// ============================================================================

#define WIFI_SSID "change to your router ssid"
#define WIFI_PASSWORD "change to your password"

// change these settings to your email provider data
#define SMTP_HOST "mail.gmx.net"
#define SMTP_PORT 587
#define AUTHOR_EMAIL "author_email@provider.net"
#define AUTHOR_PASSWORD "author_email_password"
#define RECIPIENT_EMAIL "recipient_email@provider.net"

// don't change this data
#define NTP_SERVER_PRIMARY "pool.ntp.org"
#define NTP_SERVER_BACKUP "time.google.com"
#define FILE_PATH "/rtc_log.txt"

// Intervals
#define NTP_CHECK_INTERVAL_MIN 60   // Time between NTP checks
#define EMAIL_SEND_INTERVAL_MIN 60  // Time between email reports

//#define NTP_CHECK_INTERVAL_MIN  1   // Time between NTP checks
//#define EMAIL_SEND_INTERVAL_MIN 2   // Time between email reports

// Sleep Mode Configuration (1 = Enabled, 0 = Disabled)
#define USE_DEEP_SLEEP 1

#define BUTTON_PIN 9  // Built-in BOOT button on ESP32-C3 Super Mini
#define LED_PIN 8     // Built-in LED on ESP32-C3 Super Mini

// ============================================================================
// RTC PERSISTENT VARIABLES (Preserved across Deep Sleep)
// ============================================================================
RTC_DATA_ATTR bool g_initial_sync_done = false;
RTC_DATA_ATTR time_t g_start_timestamp = 0;
RTC_DATA_ATTR time_t g_last_ntp_check = 0;
RTC_DATA_ATTR time_t g_last_email_send = 0;
RTC_DATA_ATTR long g_last_time_diff = 0;

// Mail Client Session Object
SMTPSession g_smtp;

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

void printLog(const String &msg) {
  Serial.println("[RTC-Test] " + msg);
}

void checkResetButton() {
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);  // Turn off LED (active LOW on C3)

  // Check if button is pressed (LOW) at boot time
  if (digitalRead(BUTTON_PIN) == LOW) {
    printLog("BOOT button pressed. Hold for 3 seconds to clear log file...");

    unsigned long startTime = millis();
    bool held = true;

    while (millis() - startTime < 3000) {
      if (digitalRead(BUTTON_PIN) == HIGH) {
        held = false;
        break;  // Released early
      }
      delay(50);
    }

    if (held) {
      printLog("Button held for 3s! Formatting/Deleting log file...");

      // Fast blinking LED signal for confirmation
      for (int i = 0; i < 10; i++) {
        digitalWrite(LED_PIN, !digitalRead(LED_PIN));
        delay(100);
      }
      digitalWrite(LED_PIN, HIGH);  // Turn off LED

      if (LittleFS.exists(FILE_PATH)) {
        LittleFS.remove(FILE_PATH);
        printLog("Log file /rtc_log.txt successfully deleted!");
      } else {
        printLog("Log file did not exist.");
      }

      // Reset internal persistent flags
      g_initial_sync_done = false;
      g_start_timestamp = 0;
      g_last_ntp_check = 0;
      g_last_email_send = 0;
      g_last_time_diff = 0;

      printLog("System reset complete. Proceeding with fresh boot...");
    } else {
      printLog("Button released before 3 seconds. Normal boot sequence.");
    }
  }
}

bool connectWiFi() {
  printLog("Connecting to WiFi...");
  WiFi.mode(WIFI_STA);

  // Reduce WiFi TX Power to fix ESP32-C3 Super Mini antenna / brownout stability issues
  WiFi.setTxPower(WIFI_POWER_11dBm);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    printLog("WiFi Connected! IP: " + WiFi.localIP().toString());
    delay(500);  // Give network stack time to stabilize
    return true;
  } else {
    printLog("WiFi Connection Failed!");
    return false;
  }
}

time_t getNtpTime() {
  WiFiUDP udp;
  // open local port for UDP-Kommunikation
  if (!udp.begin(2390)) {
    printLog("UDP initialisation failed");
    return 0;
  }

  // NTP-Paket-Structure (48 Bytes)
  byte ntpPacketBuffer[48];
  memset(ntpPacketBuffer, 0, 48);
  ntpPacketBuffer[0] = 0b11100011;  // LI, Version, Mode

  // NTP-Request to primary NTP server
  if (!udp.beginPacket(NTP_SERVER_PRIMARY, 123)) {
    printLog("NTP Packet build failed");
    return 0;
  }
  udp.write(ntpPacketBuffer, 48);
  udp.endPacket();

  // wait for an answer (Timeout 3 seconds)
  unsigned long startMs = millis();
  while (udp.parsePacket() < 48) {
    if (millis() - startMs > 3000) {
      printLog("NTP Reply Timeout!");
      return 0;
    }
    delay(10);
  }

  // Read data
  udp.read(ntpPacketBuffer, 48);

  // Find the seconds in bytes 40, 41, 42 and 43
  unsigned long highWord = word(ntpPacketBuffer[40], ntpPacketBuffer[41]);
  unsigned long lowWord = word(ntpPacketBuffer[42], ntpPacketBuffer[43]);
  unsigned long secsSince1900 = highWord << 16 | lowWord;

  // Get the timestamp from NTP-time (since 1900) to Unix-time (since 1970)
  const unsigned long seventyYears = 2208988800UL;
  time_t epochTime = secsSince1900 - seventyYears;

  return epochTime;
}

/*

// WARNING: DON'T use this method as it will destroy your measurements

time_t getNtpTime() {
  configTime(0, 0, NTP_SERVER_PRIMARY, NTP_SERVER_BACKUP); // Force UTC time
  struct tm timeinfo;
  // Use extended 10 second timeout for reliable DNS resolution and response
  if (!getLocalTime(&timeinfo, 10000)) { 
    return 0;
  }
  return mktime(&timeinfo);
}
*/

void appendToLogFile(time_t rtc_ts, long diff, bool is_online) {
  File file = LittleFS.open(FILE_PATH, FILE_APPEND);
  if (!file) {
    printLog("Failed to open log file for appending");
    return;
  }

  String entry = String(rtc_ts) + "," + String(diff) + "," + (is_online ? "Y" : "N") + "\n";
  file.print(entry);
  file.close();
  printLog("Appended to log: " + entry);
}

void performNtpCheck() {
  printLog("Starting NTP Check procedure...");

  bool wifi_ok = connectWiFi();
  time_t ntp_now = 0;
  time_t rtc_now = 0;

  if (wifi_ok) {
    ntp_now = getNtpTime();
    // CRITICAL: Fetch local RTC time IMMEDIATELY after getting NTP time
    rtc_now = time(NULL);
  }

  if (wifi_ok && ntp_now > 0) {
    g_last_time_diff = (long)ntp_now - (long)rtc_now;  // Positive if RTC lags
    printLog("NTP Fetch Success.");
    printLog("NTP Time: " + String(ntp_now) + " | Local RTC Time: " + String(rtc_now));
    printLog("Calculated Diff: " + String(g_last_time_diff) + "s");

    appendToLogFile(rtc_now, g_last_time_diff, true);
  } else {
    // If WiFi or NTP failed, sample local RTC time now
    rtc_now = time(NULL);
    printLog("NTP Fetch Failed or WiFi offline. Retaining last diff: " + String(g_last_time_diff) + "s");
    appendToLogFile(rtc_now, g_last_time_diff, false);
  }

  if (wifi_ok) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }
}

bool sendEmail() {
  if (!connectWiFi()) {
    printLog("Cannot send email: WiFi unavailable");
    return false;
  }

  printLog("Preparing Email...");

  if (!LittleFS.exists(FILE_PATH)) {
    printLog("Log file missing, skipping email delivery!");
    WiFi.disconnect(true);
    return false;
  }

  // Calculate overall runtime since initial start
  time_t current_rtc = time(NULL);
  long total_elapsed_sec = (long)(current_rtc - g_start_timestamp);
  long elapsed_hours = total_elapsed_sec / 3600;
  long elapsed_minutes = total_elapsed_sec / 60;

  // Setup ESP Mail Client Session
  Session_Config config;
  config.server.host_name = SMTP_HOST;
  config.server.port = SMTP_PORT;
  config.login.email = AUTHOR_EMAIL;
  config.login.password = AUTHOR_PASSWORD;

  SMTP_Message message;
  message.sender.name = "ESP32-C3 RTC Tester";
  message.sender.email = AUTHOR_EMAIL;

  // Subject definition: [Mode] Diff: Xs - Subject
  String subject = (USE_DEEP_SLEEP ? "[Deep Sleep] " : "[Always On] ");
  subject += String(g_last_time_diff) + "s - ";
  subject += "ESP32-C3 Long-Term RTC Accuracy Report";
  message.subject = subject;

  message.addRecipient("Admin", RECIPIENT_EMAIL);

  // Email Body text in English containing elapsed runtime line and time difference
  String body_content = "ESP32-C3 Long-Term RTC Clock Test Report\n";
  body_content += "----------------------------------------\n";
  body_content += "Data logging for " + String(elapsed_minutes) + " minutes (" + String(elapsed_hours) + " hours)\n";
  body_content += "Current RTC Time Difference: " + String(g_last_time_diff) + " seconds.\n\n";
  body_content += "Please find the original LittleFS log file attached to this email.";
  message.text.content = body_content.c_str();

  // Attach the LittleFS log file directly
  SMTP_Attachment att;
  att.descr.filename = "rtc_log.txt";
  att.file.path = FILE_PATH;
  att.file.storage_type = esp_mail_file_storage_type_flash;
  message.addAttachment(att);

  printLog("Connecting to SMTP server...");
  if (!g_smtp.connect(&config)) {
    printLog("SMTP Server connection failed!");
    WiFi.disconnect(true);
    return false;
  }

  printLog("Sending email with file attachment...");
  if (!MailClient.sendMail(&g_smtp, &message)) {
    printLog("Error sending Email: " + g_smtp.errorReason());
    WiFi.disconnect(true);
    return false;
  }

  printLog("Email sent successfully!");
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  return true;
}

// ============================================================================
// MAIN SETUP & LOOP
// ============================================================================

void setup() {
  Serial.begin(115200);
  delay(1000);

  printLog("System Booting...");

  if (!LittleFS.begin(true)) {
    printLog("LittleFS Mount Failed!");
    return;
  }

  // Check for 3-second button press to clear LittleFS log
  checkResetButton();

  // First time start execution
  if (!g_initial_sync_done) {
    printLog("First start detected. Synchronizing RTC with NTP...");
    if (connectWiFi()) {
      time_t initial_ntp = getNtpTime();
      if (initial_ntp > 0) {
        struct timeval tv = { .tv_sec = initial_ntp, .tv_usec = 0 };
        settimeofday(&tv, NULL);  // Synchronize internal RTC ONCE

        g_start_timestamp = initial_ntp;
        g_last_ntp_check = initial_ntp;
        g_last_email_send = initial_ntp;
        g_initial_sync_done = true;

        printLog("RTC successfully synchronized: " + String(initial_ntp));

        // EXPICIT POWER LOSS / REBOOT LOG ENTRY
        File file = LittleFS.open(FILE_PATH, FILE_APPEND);
        if (file) {
          file.println(String(initial_ntp) + ",0,BOOT");  // "BOOT" signalisiert Neustart
          file.close();
        }
      } else {
        printLog("Initial NTP Sync Failed! Retrying on next reboot.");
      }
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
    }
  }

  // Execution flow
  if (g_initial_sync_done) {
    time_t now = time(NULL);

    // Check if NTP check is due
    if (now - g_last_ntp_check >= (NTP_CHECK_INTERVAL_MIN * 60)) {
      performNtpCheck();
      g_last_ntp_check = now;
    }

    // Check if Email send is due
    if (now - g_last_email_send >= (EMAIL_SEND_INTERVAL_MIN * 60)) {
      if (sendEmail()) {
        g_last_email_send = now;
      }
    }

#if USE_DEEP_SLEEP
    uint64_t sleep_time_sec = NTP_CHECK_INTERVAL_MIN * 60;
    printLog("Entering Deep Sleep for " + String(sleep_time_sec) + " seconds...");
    Serial.flush();
    esp_sleep_enable_timer_wakeup(sleep_time_sec * 1000000ULL);
    esp_deep_sleep_start();
#endif
  }
}

void loop() {
#if !USE_DEEP_SLEEP
  if (g_initial_sync_done) {
    time_t now = time(NULL);

    if (now - g_last_ntp_check >= (NTP_CHECK_INTERVAL_MIN * 60)) {
      performNtpCheck();
      g_last_ntp_check = now;
    }

    if (now - g_last_email_send >= (EMAIL_SEND_INTERVAL_MIN * 60)) {
      if (sendEmail()) {
        g_last_email_send = now;
      }
    }
  }
  delay(1000);
#endif
}
