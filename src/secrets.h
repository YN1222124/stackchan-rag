#pragma once

// =============================================================================
// secrets.h.example
// このファイルを "secrets.h" にコピー（リネーム）し、実際の環境に合わせて書き換えてください。
// ※ secrets.h は機密情報（パスワード・APIキー）を含むため Git にコミットしないでください。
// =============================================================================

// Wi-Fi 接続設定 (ESP32-S3 のため 2.4GHz 帯の SSID を指定してください / 5GHz 非対応)
const char* const WIFI_SSID   = "YOUR_WIFI_SSID";
const char* const WIFI_PASS   = "YOUR_WIFI_PASSWORD";

// FastAPI サーバーのベースURL
// ※ ポート番号まで含め、末尾のスラッシュ '/' は含めないでください (例: "http://192.168.1.100:8001")
const char* const SERVER_BASE = "http://192.168.X.X:8001";

// FastAPI サーバーの共通認証 API キー
const char* const API_KEY     = "your_api_key_here";
