#include <Arduino.h>
#include <WiFi.h>
#include "BT1026D_driver.h"
#include "cdc.h"
#include "webUI.h" 

// Раскомментируй для работы с аппаратным Serial! 
// Должно совпадать с тем, что ты написал в webUI.h, если уже написал.
//#define USE_HW_SERIAL 

// ---------------- ПИНЫ ----------------
const int BT_RX_PIN = 17;
const int BT_TX_PIN = 16;
const int BT_SYS_CTRL_PIN = 19;

const int CDC_NEC_PIN = 4;
const int CDC_SCK_PIN = 18;
const int CDC_MOSI_PIN = 23;
const int CDC_MISO_PIN = 32;

const int STATUS_LED_PIN = 2; // Синий светодиод на плате

// ---------------- ОБЪЕКТЫ И МЬЮТЕКСЫ ----------------
BT1026D btModule(Serial1); 
CDC cdcModule;
SemaphoreHandle_t g_cdcMutex = NULL;
SemaphoreHandle_t g_btMutex = NULL;

// ---------------- ПОДКЛЮЧЕНИЕ ФИЧ ----------------
// Треки для статусов на дисплее магнитолы (как в V1)
struct DisplayTracks {
    static const uint8_t WAITING_FOR_BT = 88; // Waiting for connection
    static const uint8_t CONNECTED      = 10; // Just connected but paused
    static const uint8_t WIFI_OFF       = 60; // WiFi Disabled
    static const uint8_t MUTE_ON        = 33; // Mute Active
    static const uint8_t PAIRING        = 44; // Pairing Mode
    static const uint8_t DISCONNECTED   = 55; // Manually Disconnected
    static const uint8_t CLEARED_PAIRED = 54; // Cleared Paired List
    static const uint8_t STOPPED        = 22; // Stopped
};

static uint8_t g_currentTrack = DisplayTracks::WAITING_FOR_BT;
static bool g_hfpMuted = false;
static uint8_t g_prevTrackBeforeMute = 1;

// Для умного автоплея
static BTConnState g_lastBtState = BTConnState::DISCONNECTED;
static bool g_waitingForAutoplay = false;
static uint32_t g_connectedTimeMs = 0;

// Безопасная установка трека с мьютексом
void safeSetCdcTrack(uint8_t track, uint8_t min = 0, uint8_t sec = 0) {
    if (xSemaphoreTake(g_cdcMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        cdcModule.setTrack(track);
        cdcModule.setPlayTime(min, sec);
        xSemaphoreGive(g_cdcMutex);
    }
}

// Безопасная отправка команды BT (хотя очередь внутри BT1026D_driver и так безопасна, 
// мьютекс обеспечит строгую последовательность между потоками)
void safeEnqueueBtCmd(BTCmdType cmd, int param = 0) {
    if (xSemaphoreTake(g_btMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        btModule.enqueueCommand(cmd, param);
        xSemaphoreGive(g_btMutex);
    }
}

// ---------------- КОЛЛБЭКИ ИЗ ДРАЙВЕРА BLUETOOTH ----------------

// Прилетел лог от драйвера BT
void onBtLog(const char* msg) {
    webUI_log(String(msg), LogLevel::INFO);
}

// Изменилось состояние (нажали паузу на телефоне или заиграл трек)
void onBtStateChange(BTConnState newState, BTConnState oldState) {
    if (newState == BTConnState::PLAYING) {
        webUI_log("[MAIN] Music Playing", LogLevel::INFO);
        // Магнитола должна показывать 1 трек, когда просто играет музло
        g_currentTrack = 1;
        safeSetCdcTrack(g_currentTrack); 
        // Скажем CDC обновить время для передачи на дисплей, если оно застыло
    } else if (newState == BTConnState::PAUSED) {
        webUI_log("[MAIN] Music Paused", LogLevel::INFO);
    } else if (newState == BTConnState::DISCONNECTED) {
        webUI_log("[MAIN] BT Disconnected", LogLevel::INFO);
        g_currentTrack = DisplayTracks::WAITING_FOR_BT;
        safeSetCdcTrack(g_currentTrack); // 88 = Ожидание связи
    } else if (newState == BTConnState::A2DP_CONNECTED || newState == BTConnState::HFP_CONNECTED) {
        if (oldState == BTConnState::DISCONNECTED) {
            webUI_log("[MAIN] BT Device Connected. Waiting to autoplay...", LogLevel::INFO);
            g_currentTrack = DisplayTracks::CONNECTED;
            safeSetCdcTrack(g_currentTrack);
            
            // Взводим триггер умного автоплея
            g_waitingForAutoplay = true;
            g_connectedTimeMs = millis();
        }
    }
    
    g_lastBtState = newState;
}

// Прилетели метаданные (Время)
void onBtMetadata(const char* type, const char* data) {
    if (strcmp(type, "TRACKINFO") == 0) {
        // Передаем сырую инфу (название трека и т.д.) в WebUI
        webUI_broadcastState("TRACKINFO", String(data));
    } else if (strcmp(type, "TRACKSTAT") == 0) {
        // Формат данных: status,current_ms,total_ms (или что-то подобное)
        // Распарсим первое число как время, для передачи на панель приборов CDC
        webUI_broadcastState("TRACKSTAT", String(data));
        
        // Попробуем вытащить время: статус(отбросим) и миллисекунды
        int status;
        long timeMs;
        if (sscanf(data, "%d,%ld", &status, &timeMs) == 2) {
            uint8_t mins = (timeMs / 1000) / 60;
            uint8_t secs = (timeMs / 1000) % 60;

            // Передаем время в CDC безопасно!
            if (xSemaphoreTake(g_cdcMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                cdcModule.setPlayTime(mins, secs);
                xSemaphoreGive(g_cdcMutex);
            }
        }
    } else if (strcmp(type, "AVRCP_READY") == 0) {
        // Телефон сообщил по Bluetooth, что он готов принимать команды плеера
        if (g_waitingForAutoplay) {
            webUI_log("[MAIN] AVRCP Profile is Ready. Sending PLAY immediately!", LogLevel::INFO);
            safeEnqueueBtCmd(BTCmdType::PLAYPAUSE); 
            g_waitingForAutoplay = false;
        }
    }
}

// ---------------- КОЛЛБЭК ИЗ ЭМУЛЯТОРА МАГНИТОЛЫ (CDC) ----------------

// Пользователь нажал кнопку на физической панели магнитолы
void onRadioBtnPress(Button btn) {
    // Статические переменные для защиты от дребезга 
    // (Они живут на протяжении всей работы программы, но видны только внутри этой функции)
    static Button lastButton = Button::NONE;
    static uint32_t lastButtonTime = 0;
    
    uint32_t now = millis();

    // Защита от автоповтора (обычный дребезг контактов магнитолы около 300мс)
    if (btn == lastButton && (now - lastButtonTime < 300)) {
        return; 
    }
    lastButton = btn;
    lastButtonTime = now;

    switch (btn) {
        case Button::NEXT:
            webUI_log("[MAIN] Btn: NEXT -> Forward", LogLevel::INFO);
            safeEnqueueBtCmd(BTCmdType::FORWARD);
            break;

        case Button::PREV:
            webUI_log("[MAIN] Btn: PREV -> Backward", LogLevel::INFO);
            safeEnqueueBtCmd(BTCmdType::BACKWARD);
            break;

        case Button::CD1:
            webUI_log("[MAIN] Btn: CD1 -> PLAY/PAUSE", LogLevel::INFO);
            safeEnqueueBtCmd(BTCmdType::PLAYPAUSE);
            // Если включили паузу, время на экране замрет само (когда BT 
            // пришлет StateChange с PAUSED)
            break;

        case Button::CD2:
            webUI_log("[MAIN] Btn: CD2 -> STOP", LogLevel::INFO);
            safeEnqueueBtCmd(BTCmdType::STOP);
            // Покажем на экране 22 как статус Стопа
            g_currentTrack = DisplayTracks::STOPPED;
            safeSetCdcTrack(g_currentTrack); 
            break;

        case Button::CD3: {
            // Mute микрофона (HFP)
            g_hfpMuted = !g_hfpMuted;
            webUI_log(String("[MAIN] Btn: CD3 -> Mic Mute ") + (g_hfpMuted ? "ON" : "OFF"), LogLevel::INFO);
            safeEnqueueBtCmd(BTCmdType::MICMUTE, g_hfpMuted ? 1 : 0);
            
            if (g_hfpMuted) {
                g_prevTrackBeforeMute = g_currentTrack;
                g_currentTrack = DisplayTracks::MUTE_ON;
            } else {
                g_currentTrack = g_prevTrackBeforeMute;
            }
            safeSetCdcTrack(g_currentTrack);
            break;
        }

        case Button::CD4:
            // Режим сопряжения (Pairing)
            webUI_log("[MAIN] Btn: CD4 -> Enter Pairing Mode", LogLevel::INFO);
            safeEnqueueBtCmd(BTCmdType::A2DPDISC);
            safeEnqueueBtCmd(BTCmdType::HFPDISC);
            safeEnqueueBtCmd(BTCmdType::SCAN, 1);
            g_currentTrack = DisplayTracks::PAIRING;
            safeSetCdcTrack(g_currentTrack); // TRACK 44 = Пайринг
            break;

        case Button::CD5: {
            // Двойное нажатие (менее 1 сек): очистить список
            static uint32_t lastCd5Press = 0;
            if (now - lastCd5Press < 1000) { 
                webUI_log("[MAIN] Btn: CD5 (Double) -> Clear Paired List", LogLevel::INFO);
                safeEnqueueBtCmd(BTCmdType::PLIST, 0); // Очистить 
                g_currentTrack = DisplayTracks::CLEARED_PAIRED;
                safeSetCdcTrack(g_currentTrack); // TRACK 54 = Очистка 
                lastCd5Press = 0; 
            } else {
                webUI_log("[MAIN] Btn: CD5 (Single) -> Disconnect device", LogLevel::INFO);
                safeEnqueueBtCmd(BTCmdType::A2DPDISC);
                safeEnqueueBtCmd(BTCmdType::HFPDISC);
                g_currentTrack = DisplayTracks::DISCONNECTED;
                safeSetCdcTrack(g_currentTrack); // TRACK 55 = Разобрал связь
                lastCd5Press = now;
            }
            break;
        }

        case Button::CD6:
            // Вкл/Выкл WiFi
            webUI_log("[MAIN] Btn: CD6 -> Toggle WiFi", LogLevel::INFO);
            webUI_toggleWiFi();
            g_currentTrack = DisplayTracks::WIFI_OFF; // Если не будет экрана, чтобы не сбивало
            safeSetCdcTrack(g_currentTrack);
            break;

        case Button::SCAN:
            // Сбросить звонок
            webUI_log("[MAIN] Btn: SCAN -> Hangup / Reject Call", LogLevel::INFO);
            safeEnqueueBtCmd(BTCmdType::HFPCHUP);
            break;

        case Button::ASMIX:
            // Поднять трубку
            webUI_log("[MAIN] Btn: ASMIX(RANDOM) -> Answer Call", LogLevel::INFO);
            safeEnqueueBtCmd(BTCmdType::HFPANSW);
            break;

        default:
            webUI_log("[MAIN] Unknown Button", LogLevel::DEBUG);
            break;
    }
}

// ---------------- ARDUINO SETUP & LOOP ----------------

void updateStatusLed() {
    static uint32_t lastToggleMs = 0;
    static bool ledOn = false;

    if (!webUI_isWiFiActive()) {
        ledOn = false;
        digitalWrite(STATUS_LED_PIN, LOW);
        return;
    }

    if (WiFi.status() == WL_CONNECTED) {
        // WiFi включен И подключен к домашней сети
        ledOn = true;
        digitalWrite(STATUS_LED_PIN, HIGH);
        return;
    }

    // WiFi включен (точка доступа), но к домашней сети не подключен - медленно мигает
    uint32_t now = millis();
    if (now - lastToggleMs >= 500) {
        lastToggleMs = now;
        ledOn = !ledOn;
        digitalWrite(STATUS_LED_PIN, ledOn ? HIGH : LOW);
    }
}

void setup() {
#ifdef USE_HW_SERIAL
    Serial.begin(115200);
    delay(100); 
#endif

    pinMode(STATUS_LED_PIN, OUTPUT);
    digitalWrite(STATUS_LED_PIN, LOW);

    g_cdcMutex = xSemaphoreCreateMutex();
    g_btMutex = xSemaphoreCreateMutex();

    // 1. БЫСТРАЯ ИНИЦИАЛИЗАЦИЯ CDC-ЧЕЙНДЖЕРА!
    // Делаем это ДО запуска WebUI и WiFi! Магнитоле нужен немедленный отклик (SPI пинг) 
    // во время поворота ключа зажигания, иначе она решит что CDC "NOT CONNECTED" и сбросится на FM.
    cdcModule.begin(CDC_NEC_PIN, CDC_SCK_PIN, CDC_MOSI_PIN, CDC_MISO_PIN);
    cdcModule.setButtonCallback(onRadioBtnPress);
    cdcModule.setDiagCallback([](const char* line){
        webUI_broadcastCdcRaw(String(line));
    });
    safeSetCdcTrack(DisplayTracks::WAITING_FOR_BT);

    // КРИТИЧНО: Дать эмулятору CDC 300мс форы для общения с магнитолой
    // ДО старта тяжелых модулей WiFi и Bluetooth, иначе магнитола отбросит CDC!
    delay(100);

    // 2. Запускаем WiFi и WebUI Сервер (это может занять время, но CDC таск уже запущен параллельно)
    webUI_init();
    delay(100); // Даем потокам запуститься

    // Привязываем коллбэк для AT-команд из браузера к нашему блютуз-модулю
    webUI_setATCallback([](const String& cmd){
        if (xSemaphoreTake(g_btMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            btModule.sendRawCommand(cmd.c_str());
            xSemaphoreGive(g_btMutex);
        }
    });

    webUI_log("[SYS] Starting VW Bluetooth Setup...", LogLevel::INFO);

    // 3. Настраиваем и инициализируем Bluetooth-модуль
    btModule.setLogCallback(onBtLog);
    btModule.setMetadataCallback(onBtMetadata);
    btModule.setStateChangeCallback(onBtStateChange);
    
    // Включаем BT модуль
    btModule.begin(115200, BT_RX_PIN, BT_TX_PIN, BT_SYS_CTRL_PIN);
    
    webUI_log("[SYS] Setup Complete!", LogLevel::INFO);
}

void loop() {
    // Вся работа происходит внутри классов (FreeRTOS)
    // loop() пустой, но мы даем FreeRTOS переключать контекст
    updateStatusLed();
    delay(50);
}
