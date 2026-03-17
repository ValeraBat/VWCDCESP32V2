#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

//BT1026D STATES
enum class BTOperationalState {
    UNINITIALIZED,
    INIT_SEQ,      // Проходим цепочку стартовых команд
    IDLE,          // Ждем команд от пользователя или событий от модуля
    WAIT_RESPONSE, // Отправили команду, ждем OK, ERROR или таймаут
    ERROR_STATE
};

enum class BTConnState {
    DISCONNECTED,
    HFP_CONNECTED,
    A2DP_CONNECTED,
    A2DP_HFP_CONNECTED,
    PLAYING,
    PAUSED,
    CALL_INCOMING,
    CALL_ACTIVE
};

// Тип функции для передачи логов наружу (например, в WebUI)
typedef void (*BTLogCallback)(const char* msg);

// Callback вызывается, когда модуль меняет статус (например, с PAUSED на PLAYING)
typedef void (*BTStateChangeCallback)(BTConnState newState, BTConnState oldState);

// Callback для передачи метаинформации (Имя трека, номер при звонке и т.д.)
typedef void (*BTMetadataCallback)(const char* type, const char* data);

//AT COMMANDS
enum class BTCmdType {
    PLAYPAUSE,
    STOP,
    FORWARD,
    BACKWARD,
    HFPANSW,
    HFPCHUP,
    MICMUTE,
    MICGAIN,
    SPKVOL,
    REBOOT,
    FACTORYRESET, //AT+RESTORE
    PROFILE, //Bluetooth profile selection
    AUTOCONN,
    STAT,
    NAME,
    SSP,  //Get/Set BR/EDR Pairing Mode
    COD, //Get/Set Class of Device COD 
    SCAN, //Scan Nearvy Bluetooth Devices
    PLIST, //Get/Delete Paired Device List
    DSCA, //Disconnect All
    A2DPDISC,
    HFPDISC,
    AUXCFG, //AUXILIARY CONFIGURATION
    TONEPLAY, //Play Tone
    A2DPCFG,
    AVRCPCFG,
    // добавь другие команды, которые тебе нужны
};

struct BTCommand {
    BTCmdType type;
    int param;     // Для простых числовых параметров (например, 1 или 0 для MICMUTE)
    // char strParam[16]; // Если понадобится передавать строки вроде мак-адреса, можно раскомментировать
};

class BT1026D {
public:
    BT1026D(HardwareSerial& serial);
    
    // Инициализация (настройка UART, создание Task во FreeRTOS)
    bool begin(uint32_t baudrate, int rxPin, int txPin, int sysCtrlPin);
    
    // Асинхронная отправка команды (положить в очередь)
    bool enqueueCommand(BTCmdType cmd, int param = 0); // По умолчанию параметр 0

    // Отправка кастомной (Raw) команды напрямую для отладки из WebUI
    void sendRawCommand(const char* cmd);
    
    // Получить текущее состояние плеера/звонков
    BTConnState getConnState() const { return _connState; }

    // Установить callback для логов
    void setLogCallback(BTLogCallback cb) { _logCb = cb; }

    // Установить callback для изменения состояния
    void setStateChangeCallback(BTStateChangeCallback cb) { _stateCb = cb; }

    // Установить callback для метаинформации (треки, номера)
    void setMetadataCallback(BTMetadataCallback cb) { _metaCb = cb; }


private:
    HardwareSerial& _serial;
    volatile BTOperationalState _opState; // Make volatile since it's changed from another thread
    volatile uint32_t _lastCmdTime;       // Track time of last sent command
    BTConnState _connState;
    
    BTLogCallback _logCb = nullptr;
    BTStateChangeCallback _stateCb = nullptr;
    BTMetadataCallback _metaCb = nullptr;
    
    // Буфер для накопления строки из UART
    char _rxBuffer[128];
    uint16_t _rxIndex;
    
    // FreeRTOS элементы
    TaskHandle_t _taskHandle;
    QueueHandle_t _cmdQueue; // Очередь для отправки команд

    // Внутренние методы

    // Вспомогательная функция для удобной отправки логов
    void _log(const char* format, ...);

    // Вспомогательная функция для смены состояния
    void _setConnState(BTConnState newState);

    static void _taskRunner(void* pvParameters); // FreeRTOS требует статический метод
    void _taskLoop(); // А здесь будет крутиться сама задача
    
    void _processIncomingChar(char c);
    void _parseLine(const char* line);
    void _handleEvent(const char* eventStr);
    
    void _sendPhysicalCommand(BTCommand cmd);
    void _sendPhysicalCommand(const char* rawCmd); // ТЕПЕРЬ ПРИНИМАЕТ СТРОКУ
    void _processCommandQueue();
};