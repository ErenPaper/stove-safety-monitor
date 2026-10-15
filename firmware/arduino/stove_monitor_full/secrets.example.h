// Copy this file to secrets.h (same folder) and fill in your own values.
// secrets.h is listed in .gitignore and must never be committed.
#pragma once

const char* WIFI_SSID     = "your-wifi-name";
const char* WIFI_PASSWORD = "your-wifi-password";

// AWS IoT Core -> Settings -> Device data endpoint
const char* AWS_ENDPOINT  = "xxxxxxxxxxxxxx-ats.iot.<region>.amazonaws.com";

// The three PEM files from AWS IoT Core -> Manage -> Things -> Certificates:
// device certificate, its private key, and Amazon Root CA 1 (public).
const char* DEVICE_CERT = R"(-----BEGIN CERTIFICATE-----
paste the device certificate here
-----END CERTIFICATE-----)";

const char* PRIVATE_KEY = R"(-----BEGIN RSA PRIVATE KEY-----
paste the device private key here
-----END RSA PRIVATE KEY-----)";

const char* ROOT_CA = R"(-----BEGIN CERTIFICATE-----
paste Amazon Root CA 1 here
-----END CERTIFICATE-----)";
