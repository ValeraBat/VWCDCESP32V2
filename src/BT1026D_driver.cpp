#include "BT1026D_driver.h"
#include <stdarg.h> // Это нужно для форматирования строк (va_list)

// Конструктор: просто сохраняем ссылку на Serial и задаем начальные состояния
BT1026D::BT1026D(HardwareSerial& serial) 
    : _serial(serial), 
      _opState(BTOperationalState::UNINITIALIZED), 
      _lastCmdTime(0),
      _connState(BTConnState::DISCONNECTED),
      _rxIndex(0),
      _taskHandle(nullptr),
      _cmdQueue(nullptr) 
{
    memset(_rxBuffer, 0, sizeof(_rxBuffer));
}

bool BT1026D::begin(uint32_t baudrate, int rxPin, int txPin, int sysCtrlPin) {
    if (_opState != BTOperationalState::UNINITIALIZED) return false;

    pinMode(sysCtrlPin, OUTPUT);
    digitalWrite(sysCtrlPin, LOW);
    delay(150);
    digitalWrite(sysCtrlPin, HIGH);

    // 1. Инициализируем железный UART
    _serial.begin(baudrate, SERIAL_8N1, rxPin, txPin);

    // 2. Создаем очередь команд (например, на 10 команд максимум)
    _cmdQueue = xQueueCreate(10, sizeof(BTCommand));
    if (_cmdQueue == nullptr) return false;

    // 3. Создаем задачу FreeRTOS, которая будет крутиться постоянно.
    // Параметры: Функция задачи, Имя для дебага, Размер стека(в байтах), 
    // Параметр (передаем указатель на себя), Приоритет, Указатель на хэндл, Привязка к Ядру
    BaseType_t success = xTaskCreatePinnedToCore(
        _taskRunner,
        "BT1026D_Task",
        4096,
        this,
        1,
        &_taskHandle,
        0   // Core 0 - PRO_CPU (освобождаем Core 1 для CDC)
    );

    if (success != pdPASS) return false;

    _opState = BTOperationalState::IDLE;
    return true;
}

// Статическая обертка (мост между C-style интерфейсом FreeRTOS и нашим C++ классом)
void BT1026D::_taskRunner(void* pvParameters) {
    BT1026D* instance = static_cast<BT1026D*>(pvParameters);
    instance->_taskLoop(); // Уходим в нормальный метод класса
}

void BT1026D::_setConnState(BTConnState newState) {
    if (_connState != newState) {
        BTConnState oldState = _connState;
        _connState = newState;
        if (_stateCb != nullptr) {
            _stateCb(newState, oldState);
        }
    }
}

// Поместить команду в очередь (вызывается извне, например, из прерываний или основного кода)
bool BT1026D::enqueueCommand(BTCmdType cmd, int param) {
    if (_cmdQueue == nullptr) return false;
    
    BTCommand newCmd;
    newCmd.type = cmd;
    newCmd.param = param;

    // xQueueSendToBack: кладем в конец. Если очередь полна, ждем максимум 10 тиков (миллисекунд).
    // Если вызывается из ISR (прерывания), нужно использовать xQueueSendToBackFromISR! (Пока оставим обычный)
    return (xQueueSendToBack(_cmdQueue, &newCmd, pdMS_TO_TICKS(10)) == pdTRUE);
}

// Прямая отправка произвольной AT-команды (например, из WebUI) мимо очереди
void BT1026D::sendRawCommand(const char* cmd) {
    _log("[BT RAW TX] %s", cmd);
    _serial.println(cmd);
    _lastCmdTime = millis();
    _opState = BTOperationalState::WAIT_RESPONSE;
}

void BT1026D::_log(const char* format, ...) {
    char locBuf[256]; // Временный буфер для лога
    va_list args;
    va_start(args, format);
    vsnprintf(locBuf, sizeof(locBuf), format, args);
    va_end(args);

    // Если кто-то "подписался" на наши логи - отдаем ему строку
    if (_logCb != nullptr) {
        _logCb(locBuf);
    }
}

void BT1026D::_processIncomingChar(char c) {
    // Твоя задача:
    // 1. Если c == '\r' - игнорируем (просто return).
    // 2. Если c == '\n' - конец строки! Ставим в конце _rxBuffer нуль-терминатор ('\0'),
    //    и если длина (индекс) больше 0, вызываем _parseLine(_rxBuffer). 
    //    После этого обязательно сбрасываем _rxIndex в 0.
    // 3. Иначе (если это обычный символ) - кладем его в _rxBuffer и увеличиваем _rxIndex.
    //    Важно: проверь, чтобы не выйти за пределы буфера (ограничь _rxIndex размером буфера - 1)!
    if (c == '\r') {
        return;
    } else if (c == '\n') {
        _rxBuffer[_rxIndex] = '\0'; // Нуль-терминатор
        if (_rxIndex > 0) {
            _parseLine(_rxBuffer); // Обрабатываем строку
        }
        _rxIndex = 0;
    } else {
        if (_rxIndex < sizeof(_rxBuffer) - 1) {
            _rxBuffer[_rxIndex++] = c;
        }
    }
}

void BT1026D::_sendPhysicalCommand(BTCommand cmd) {
    char cmdStr[64];
    memset(cmdStr, 0, sizeof(cmdStr));

    switch (cmd.type) {
        case BTCmdType::PLAYPAUSE: strcpy(cmdStr, "AT+PLAYPAUSE\r\n"); break;
        case BTCmdType::STOP:      strcpy(cmdStr, "AT+STOP\r\n"); break;
        case BTCmdType::FORWARD:   strcpy(cmdStr, "AT+FORWARD\r\n"); break;
        case BTCmdType::BACKWARD:  strcpy(cmdStr, "AT+BACKWARD\r\n"); break;
        case BTCmdType::HFPANSW:   strcpy(cmdStr, "AT+HFPANSW\r\n"); break;
        case BTCmdType::HFPCHUP:   strcpy(cmdStr, "AT+HFPCHUP\r\n"); break;
        case BTCmdType::STAT:      strcpy(cmdStr, "AT+STAT\r\n"); break;
        case BTCmdType::REBOOT:    strcpy(cmdStr, "AT+REBOOT\r\n"); break;
        case BTCmdType::FACTORYRESET: strcpy(cmdStr, "AT+RESTORE\r\n"); break;
        case BTCmdType::DSCA:      strcpy(cmdStr, "AT+DSCA\r\n"); break;
        case BTCmdType::A2DPDISC:  strcpy(cmdStr, "AT+A2DPDISC\r\n"); break;
        case BTCmdType::HFPDISC:   strcpy(cmdStr, "AT+HFPDISC\r\n"); break;
        case BTCmdType::NAME:      strcpy(cmdStr, "AT+NAME\r\n"); break; // Read name
        case BTCmdType::SCAN:      snprintf(cmdStr, sizeof(cmdStr), "AT+SCAN=%d\r\n", cmd.param); break;
        case BTCmdType::MICMUTE:   snprintf(cmdStr, sizeof(cmdStr), "AT+MICMUTE=%d\r\n", cmd.param); break;
        case BTCmdType::MICGAIN:   snprintf(cmdStr, sizeof(cmdStr), "AT+MICGAIN=%d\r\n", cmd.param); break;
        case BTCmdType::SPKVOL:    snprintf(cmdStr, sizeof(cmdStr), "AT+SPKVOL=%d\r\n", cmd.param); break;
        case BTCmdType::PROFILE:   snprintf(cmdStr, sizeof(cmdStr), "AT+PROFILE=%d\r\n", cmd.param); break;
        case BTCmdType::AUTOCONN:  snprintf(cmdStr, sizeof(cmdStr), "AT+AUTOCONN=%d\r\n", cmd.param); break;
        case BTCmdType::SSP:       snprintf(cmdStr, sizeof(cmdStr), "AT+SSP=%d\r\n", cmd.param); break;
        case BTCmdType::PLIST:     snprintf(cmdStr, sizeof(cmdStr), "AT+PLIST=%d\r\n", cmd.param); break;
        case BTCmdType::AUXCFG:    snprintf(cmdStr, sizeof(cmdStr), "AT+AUXCFG=%d\r\n", cmd.param); break;
        case BTCmdType::TONEPLAY:  snprintf(cmdStr, sizeof(cmdStr), "AT+TONEPLAY=%d\r\n", cmd.param); break;
        case BTCmdType::A2DPCFG:   snprintf(cmdStr, sizeof(cmdStr), "AT+A2DPCFG=%d\r\n", cmd.param); break;
        case BTCmdType::AVRCPCFG:  snprintf(cmdStr, sizeof(cmdStr), "AT+AVRCPCFG=%d\r\n", cmd.param); break;
            
        default:
            _log("[BT ERR] Unknown command type");
            return;
    }

    _log("[BT TX] %s", cmdStr); // Логируем (без \r\n, но для логов сойдет, или можно отрезать)
    _serial.print(cmdStr);
}

void BT1026D::_handleEvent(const char* eventStr) {
    int param1, param2, param3, param4, param5, param6, param7, param8;
    
    // ПАРСИНГ СОСТОЯНИЯ ПЛЕЕРА
    if (sscanf(eventStr, "+PLAYSTAT=%d", &param1) == 1) {
        if (param1 == 1) {
            _setConnState(BTConnState::PLAYING);
            _log("[BT] Music PLAYING");
        } else if (param1 == 2) {
            _setConnState(BTConnState::PAUSED);
            _log("[BT] Music PAUSED");
        } else if (param1 == 0) {
             _setConnState(BTConnState::A2DP_CONNECTED); 
             _log("[BT] Music STOPPED");
        }
    } 
    // ПАРСИНГ СОСТОЯНИЯ A2DP (МУЗЫКИ)
    else if (sscanf(eventStr, "+A2DPSTAT=%d", &param1) == 1) {
        switch(param1) {
            case 3: 
                _setConnState(BTConnState::A2DP_CONNECTED); 
                _log("[BT] A2DP Connected"); 
                break;
            case 4: 
                _setConnState(BTConnState::PLAYING); 
                _log("[BT] A2DP Streaming"); 
                break;
            case 1: 
            case 2:
                if (_connState == BTConnState::PLAYING || _connState == BTConnState::A2DP_CONNECTED) {
                   _setConnState(BTConnState::DISCONNECTED);
                   _log("[BT] A2DP Disconnected");
                }
                break;
        }
    }
    // ПАРСИНГ СОСТОЯНИЯ HFP (ЗВОНКОВ)
    else if (sscanf(eventStr, "+HFPSTAT=%d", &param1) == 1) {
        switch(param1) {
            case 3: // Connected (просто подключились ИЛИ закончился звонок)
                _setConnState(BTConnState::HFP_CONNECTED);
                _log("[BT] HFP Connected / Call Ended");
                break;
            case 5:
                _setConnState(BTConnState::CALL_INCOMING);
                _log("[BT] Incoming Call!");
                break;
            case 4:
            case 6:
                _setConnState(BTConnState::CALL_ACTIVE);
                _log("[BT] Call Active!");
                break;
            case 1: // Standby (устройство полностью отключилось от профиля HFP)
            case 2: // Connecting
                if (_connState == BTConnState::CALL_ACTIVE || _connState == BTConnState::CALL_INCOMING || _connState == BTConnState::HFP_CONNECTED) {
                    _setConnState(BTConnState::DISCONNECTED);
                    _log("[BT] HFP Disconnected");
                }
                break;
        }
    }
    // ПАРСИНГ СОСТОЯНИЯ AVRCP (УПРАВЛЕНИЯ ПЛЕЕРОМ)
    else if (sscanf(eventStr, "+AVRCPSTAT=%d", &param1) == 1) {
        if (param1 == 3 || param1 == 4) { // 3 = Connected, 4 = Playing
            if (_metaCb != nullptr) {
                _metaCb("AVRCP_READY", "1");
            }
        }
    }
    // ОБРАБОТКА AT+STAT (может быть разное число параметров в зависимости от прошивки)
    else if (strncmp(eventStr, "+STAT=", 6) == 0) {
        param1 = param2 = param3 = param4 = param5 = param6 = param7 = param8 = 0;
        int parsed = sscanf(eventStr, "+STAT=%d,%d,%d,%d,%d,%d,%d,%d", 
                            &param1, &param2, &param3, &param4, &param5, &param6, &param7, &param8);
        
        if (parsed >= 5) { // Главное, чтобы хватило параметров хотя бы до A2DPSTAT (он 5-й)
            _log("[BT] STAT parsed: %d params (HFP=%d, A2DP=%d)", parsed, param4, param5);
             
            // HFP = param4
            if (param4 == 3) _setConnState(BTConnState::HFP_CONNECTED);
            else if (param4 == 4 || param4 == 6) _setConnState(BTConnState::CALL_ACTIVE);
            else if (param4 == 5) _setConnState(BTConnState::CALL_INCOMING);
            
            // A2DP = param5 (если нет активного звонка, он приоритетнее)
            if (param4 <= 3 && param5 == 3) _setConnState(BTConnState::A2DP_CONNECTED);
            else if (param4 <= 3 && param5 == 4) _setConnState(BTConnState::PLAYING);
            
            // Если ничего не подключено
            if (param4 <= 2 && param5 <= 2) _setConnState(BTConnState::DISCONNECTED);
        }
    }
    // ПАРСИНГ МЕТАДАННЫХ ТРЕКА (+TRACKINFO=...)
    else if (strncmp(eventStr, "+TRACKINFO=", 11) == 0) {
        if (_metaCb != nullptr) {
            // Передаем сырую строку дальше, пускай AppLogic / WebUI сам парсит название песни
            _metaCb("TRACKINFO", eventStr + 11);
        }
    }
    // ПАРСИНГ СТАТУСА ВОСПРОИЗВЕДЕНИЯ (+TRACKSTAT=...)
    else if (strncmp(eventStr, "+TRACKSTAT=", 11) == 0) {
        if (_metaCb != nullptr) {
            _metaCb("TRACKSTAT", eventStr + 11);
        }
    }
    // ПАРСИНГ НОМЕРА ЗВОНЯЩЕГО (+HFPCID=...)
    else if (strncmp(eventStr, "+HFPCID=", 8) == 0) {
        if (_metaCb != nullptr) {
            _metaCb("CALLER_ID", eventStr + 8);
        }
    }
}

void BT1026D::_parseLine(const char* line) {
    if (strncmp(line, "+TRACKSTAT=", 11) != 0 && strncmp(line, "+TRACKINFO=", 11) != 0) {
        _log("[BT RX] %s", line);
    }
    if (strcmp(line, "OK") == 0 || strcmp(line, "ERROR") == 0) {
        _opState = BTOperationalState::IDLE;
        return;
    }
    
    // Если строка начинается с '+', значит это какое-то событие или данные
    if (line[0] == '+') {
        _handleEvent(line);
    }
}

// ==========================================================
// ОСНОВНОЙ ЦИКЛ BLUETOOTH ДРАЙВЕРА (ТА ЗАДАЧА, ЧТО КРУТИТСЯ)
// ==========================================================
void BT1026D::_taskLoop() {
    BTCommand currentCmd;

    while (true) {
        // 1. Читаем все, что пришло из UART (максимально быстро)
        while (_serial.available()) {
            char c = _serial.read();
            _processIncomingChar(c);
        }

        // 2. Логика автомата (FSM)
        switch (_opState) {
            case BTOperationalState::IDLE:
                // Мы свободны. Проверяем, есть ли что-то в очереди?
                // Ждем 50мс (блокируем задачу, экономя процессор).
                if (xQueueReceive(_cmdQueue, &currentCmd, pdMS_TO_TICKS(50)) == pdTRUE) {
                    
                    _sendPhysicalCommand(currentCmd); 
                    
                    // Переходим в состояние ожидания ответа
                    _opState = BTOperationalState::WAIT_RESPONSE;
                    _lastCmdTime = millis();
                }
                break;

            case BTOperationalState::WAIT_RESPONSE:
                // Ждем ОК или ERROR от модуля. 
                // А как мы из него выйдем? Это сделает _parseLine(), когда увидит "OK\r\n".
                // Но нам нужен таймаут! Вдруг модуль завис или мы отправили плохую команду?
                if (millis() - _lastCmdTime > 2000) { // Таймаут 2 секунды
                    _log("[BT] Command Timeout!");
                    _opState = BTOperationalState::IDLE; // Сдаемся, возвращаемся в IDLE
                }
                vTaskDelay(pdMS_TO_TICKS(10)); // Даем подышать процессору
                break;

            default:
                vTaskDelay(pdMS_TO_TICKS(50));
                break;
        }
        
    }
}
