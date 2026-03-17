#pragma once
#include <Arduino.h>

// Раскомментируй эту строку для отладки на столе через кабель.
// ЗАКОММЕНТИРУЙ перед установкой в машину, чтобы отключить помехи от UART.
#define USE_HW_SERIAL 

// Уровни логирования
enum class LogLevel : uint8_t {
    INFO,    // Всегда выводится
    DEBUG,   // Выводится только если g_debugMode == true
    VERBOSE  // Для спама, не сохраняется в историю
};

extern bool g_debugMode;

typedef void (*WebUICallback)(const String& cmd);

void webUI_setATCallback(WebUICallback cb);
void webUI_log(const String &line, LogLevel level = LogLevel::INFO);
void webUI_broadcastCdcRaw(const String &line);

void webUI_init();
void webUI_toggleWiFi(); // Вкл/выкл WiFi (Точка доступа и Web-сервер)
bool webUI_isWiFiActive();