// ───────────────────────────────────────────────────────────────
//  WiFi credentials template (committed).
// ───────────────────────────────────────────────────────────────
//  The real credentials live in `wifi_secrets.h` (gitignored — never
//  committed). Either:
//
//    (a) Copy this file to wifi_secrets.h and fill in your networks:
//          cp wifi_secrets.example.h wifi_secrets.h
//
//    (b) Or generate wifi_secrets.h from env vars:
//          WIFI_SSID_1='your-ssid'   WIFI_PASS_1='your-password' \
//          WIFI_SSID_2='your-ssid-2' WIFI_PASS_2='your-password-2' \
//          ../../scripts/gen_wifi_secrets.sh
//
//  Every entry below is added to wifiMulti on boot (in addition to any
//  networks saved via the web UI / NVS). WiFiMulti auto-selects the
//  strongest signal, so order doesn't matter.
#pragma once

struct WifiCred {
  const char* ssid;
  const char* pass;
};

const WifiCred WIFI_GROUP[] = {
  { "YOUR_SSID_1", "YOUR_PASSWORD_1" },
  { "YOUR_SSID_2", "YOUR_PASSWORD_2" },
  // add more pairs as needed (no hard limit beyond flash size)
};

const int WIFI_GROUP_COUNT = sizeof(WIFI_GROUP) / sizeof(WIFI_GROUP[0]);
