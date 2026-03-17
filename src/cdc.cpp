#include "cdc.h"

CDC::CDC() : _taskHandle(nullptr) {
    _spi = new SPIClass(VSPI);
}

void CDC::_taskRunner(void* pvParameters) {
    CDC* instance = static_cast<CDC*>(pvParameters);
    instance->_taskLoop();
}

bool CDC::begin(int DataOutPin, int SCKPin, int MOSIPin, int MISOPin) {
    _dataOutPin = DataOutPin;
    pinMode(_dataOutPin, INPUT);

    _spi->begin(SCKPin, MISOPin, MOSIPin, -1);

    _capPtr = 0;
    _scanPtr = 0;
    _capBusy = false;
    _capBit = 8;
    _capBitPacket = 0;
    _currentByte = 0;
    _measuringLow = false;
    _lastFallingEdge = 0;
    _lastLowDuration = 0;
    _minLowDuration = 0xFFFF;
    _maxLowDuration = 0;
    _validPackets = 0;
    _badPrefixPackets = 0;
    _badChecksumPackets = 0;
    _badCmdFormatPackets = 0;
    _lastCmdCode = 0;
    _lastDiagMs = millis();

    _spiState = SpiState::IDLE_THEN_PLAY;
    _stateCounter = 0;
    _initStarted = false;
    _discLoad = 0x2E;
    _lastSpiMs = millis();

    attachInterruptArg(digitalPinToInterrupt(_dataOutPin), _isr, this, CHANGE);

    BaseType_t success = xTaskCreatePinnedToCore(
        _taskRunner,
        "CDC_Task",
        4096,
        this,
        2,  // Приоритет выше для безотказности (2 вместо 1)
        &_taskHandle,
        1   // Core 1 - APP_CPU
    );

    return (success == pdPASS);
}

void IRAM_ATTR CDC::_isr(void* arg) {
    CDC* instance = static_cast<CDC*>(arg);

    uint32_t now = micros();
    bool level = digitalRead(instance->_dataOutPin);

    if (!level) {
        instance->_lastFallingEdge = now;
        instance->_measuringLow = true;
        return;
    }

    if (!instance->_measuringLow) {
        return;
    }

    uint32_t lowDuration = now - instance->_lastFallingEdge;
    instance->_measuringLow = false;

    uint16_t clamped = (lowDuration > 65535) ? 65535 : (uint16_t)lowDuration;
    instance->_lastLowDuration = clamped;
    if (clamped > instance->_maxLowDuration) instance->_maxLowDuration = clamped;
    if (clamped < instance->_minLowDuration) instance->_minLowDuration = clamped;

    if (lowDuration < VW_LOW_THRESHOLD) {
        return;
    }

    if (lowDuration >= VW_START_THRESHOLD) {
        instance->_capBusy = true;
        instance->_capBitPacket = VW_PKTSIZE;
        instance->_capBit = 8;
        instance->_currentByte = 0;
        return;
    }

    if (!instance->_capBusy || instance->_capBitPacket == 0) {
        return;
    }

    uint8_t bitValue = (lowDuration >= VW_HIGH_THRESHOLD) ? 1 : 0;

    instance->_currentByte <<= 1;
    if (bitValue) {
        instance->_currentByte |= 0x01;
    }

    instance->_capBit--;
    instance->_capBitPacket--;

    if (instance->_capBit == 0) {
        instance->_capBuffer[instance->_capPtr] = instance->_currentByte;
        instance->_capPtr = (instance->_capPtr + 1) % VW_CAPBUFFER_SIZE;
        instance->_capBit = 8;
        instance->_currentByte = 0;
    }

    if (instance->_capBitPacket == 0) {
        instance->_capBusy = false;
    }
}

void CDC::_handleVwCommand(uint8_t cmdCode) {
    Button btn = Button::NONE;

    switch (cmdCode) {
        case 0xF8: btn = Button::NEXT; break;
        case 0x78: btn = Button::PREV; break;

        case 0x0C: btn = Button::CD1; break;
        case 0x8C: btn = Button::CD2; break;
        case 0x4C: btn = Button::CD3; break;
        case 0xCC: btn = Button::CD4; break;
        case 0x2C: btn = Button::CD5; break;
        case 0xAC: btn = Button::CD6; break;

        case 0xA0: btn = Button::SCAN; break;
        case 0xE0: btn = Button::ASMIX; break;

        default: break;
    }

    if (btn != Button::NONE && _buttonCb != nullptr) {
        _buttonCb(btn);
    }
}

void CDC::_scanVwPackets() {
    while (_scanPtr != _capPtr) {
        uint8_t byte1 = _capBuffer[_scanPtr];

        if (byte1 != 0x53) {
            _badPrefixPackets++;
            _scanPtr = (_scanPtr + 1) % VW_CAPBUFFER_SIZE;
            continue;
        }

        uint8_t available = (_capPtr >= _scanPtr)
            ? (_capPtr - _scanPtr)
            : (VW_CAPBUFFER_SIZE - _scanPtr + _capPtr);

        if (available < 4) {
            return;
        }

        uint8_t byte2 = _capBuffer[(_scanPtr + 1) % VW_CAPBUFFER_SIZE];
        uint8_t byte3 = _capBuffer[(_scanPtr + 2) % VW_CAPBUFFER_SIZE];
        uint8_t byte4 = _capBuffer[(_scanPtr + 3) % VW_CAPBUFFER_SIZE];

        if (byte2 != 0x2C) {
            _badPrefixPackets++;
            _scanPtr = (_scanPtr + 1) % VW_CAPBUFFER_SIZE;
            continue;
        }

        if ((uint8_t)(byte3 + byte4) != 0xFF) {
            _badChecksumPackets++;
            _scanPtr = (_scanPtr + 1) % VW_CAPBUFFER_SIZE;
            continue;
        }

        if ((byte3 & 0x03) != 0) {
            _badCmdFormatPackets++;
            _scanPtr = (_scanPtr + 1) % VW_CAPBUFFER_SIZE;
            continue;
        }

        _validPackets++;
        _lastCmdCode = byte3;
        _handleVwCommand(byte3);
        _scanPtr = (_scanPtr + 4) % VW_CAPBUFFER_SIZE;
    }
}

void CDC::_sendSpiPacket(const uint8_t frame[8]) {
    _spi->beginTransaction(SPISettings(62500, MSBFIRST, SPI_MODE1));
    for (int i = 0; i < 8; ++i) {
        _spi->transfer(frame[i]);
        delayMicroseconds(874);
    }
    _spi->endTransaction();
}

static inline uint8_t toBCD(uint8_t val) {
    if (val > 99) val = 99;
    return ((val / 10) << 4) | (val % 10);
}

void CDC::setPlayTime(uint8_t minutes, uint8_t seconds) {
    if (minutes > 99) minutes = 99;
    if (seconds > 59) seconds = 59;
    _currentMinutes = minutes;
    _currentSeconds = seconds;
}

void CDC::setTrack(uint8_t trackNum) {
    _currentTrack = trackNum;
}

void CDC::setDisc(uint8_t discNum) {
    _currentDisc = discNum;
}

void CDC::sendStatus() {
    uint32_t now = millis();
    if (now - _lastSpiMs < 50) {
        return;
    }
    _lastSpiMs = now;

    uint8_t disc = _currentDisc;
    if (disc < 1) disc = 1;
    if (disc > 6) disc = 6;

    uint8_t track = _currentTrack;
    if (track < 1) track = 1;
    if (track > 99) track = 99;

    if (!_initStarted) {
        _initStarted = true;
        _spiState = SpiState::IDLE_THEN_PLAY;
        _stateCounter = -20;
    }

    if (_spiState == SpiState::IDLE_THEN_PLAY) {
        uint8_t idle[8] = {
            0x74,
            (uint8_t)(0xBF - disc),
            (uint8_t)(0xFF - track),
            0xFF, 0xFF, 0xFF,
            0x8F, 0x7C
        };

        _sendSpiPacket(idle);

        _stateCounter++;
        if (_stateCounter >= 0) {
            _spiState = SpiState::INIT_PLAY;
            _stateCounter = -24;
            _discLoad = 0x2E;
        }
        return;
    }

    if (_spiState == SpiState::INIT_PLAY) {
        bool isAnnounce = ((_stateCounter & 1) == 0);

        if (isAnnounce) {
            uint8_t frame[8] = {
                0x34,
                _discLoad,
                (uint8_t)(0xFF - 0x99),
                (uint8_t)(0xFF - 0x99),
                (uint8_t)(0xFF - 0x59),
                0xB7,
                0xFF,
                0x3C
            };
            _sendSpiPacket(frame);

            if (_discLoad == 0x29) {
                _discLoad = 0x2E;
            } else {
                _discLoad--;
            }
        } else {
            uint8_t frame[8] = {
                0x34,
                (uint8_t)(0xBF - disc),
                (uint8_t)(0xFF - track),
                0xFF, 0xFF, 0xFF,
                0xEF,
                0x3C
            };
            _sendSpiPacket(frame);
        }

        _stateCounter++;
        if (_stateCounter >= 0) {
            _spiState = SpiState::PLAY_LEAD_IN;
            _stateCounter = -10;
        }
        return;
    }

    if (_spiState == SpiState::PLAY_LEAD_IN) {
        bool isAnnounce = ((_stateCounter & 1) == 0);

        if (isAnnounce) {
            uint8_t frame[8] = {
                0x34,
                (uint8_t)((disc & 0x0F) | 0x20),
                (uint8_t)(0xFF - 0x99),
                (uint8_t)(0xFF - 0x99),
                (uint8_t)(0xFF - 0x59),
                0xB7,
                0xFF,
                0x3C
            };
            _sendSpiPacket(frame);
        } else {
            uint8_t frame[8] = {
                0x34,
                (uint8_t)(0xBF - disc),
                (uint8_t)(0xFF - track),
                0xFF, 0xFF, 0xFF,
                0xAE,
                0x3C
            };
            _sendSpiPacket(frame);
        }

        _stateCounter++;
        if (_stateCounter >= 0) {
            _spiState = SpiState::PLAY;
        }
        return;
    }

    uint8_t trackBCD = toBCD(track);
    uint8_t minBCD = toBCD(_currentMinutes);
    uint8_t secBCD = toBCD(_currentSeconds);

    uint8_t frame[8] = {
        0x34,
        (uint8_t)(0xBF - disc),
        (uint8_t)(0xFF - trackBCD),
        (uint8_t)(0xFF - minBCD),
        (uint8_t)(0xFF - secBCD),
        _modeByte,
        _scanByte,
        0x3C
    };

    _sendSpiPacket(frame);
}

void CDC::_taskLoop() {
    while (true) {
        _scanVwPackets();
        sendStatus();

        uint32_t now = millis();
        if (now - _lastDiagMs >= 1000) {
            _lastDiagMs = now;

            const char* state = "PLAY";
            if (_spiState == SpiState::IDLE_THEN_PLAY) state = "IDLE_THEN_PLAY";
            else if (_spiState == SpiState::INIT_PLAY) state = "INIT_PLAY";
            else if (_spiState == SpiState::PLAY_LEAD_IN) state = "PLAY_LEAD_IN";

            if (_diagCb != nullptr) {
                char diag[192];
                snprintf(
                    diag,
                    sizeof(diag),
                    "[CDC DIAG] state=%s valid=%lu badPrefix=%lu badCsum=%lu badFmt=%lu cmd=0x%02X low=%u min=%u max=%u",
                    state,
                    (unsigned long)_validPackets,
                    (unsigned long)_badPrefixPackets,
                    (unsigned long)_badChecksumPackets,
                    (unsigned long)_badCmdFormatPackets,
                    (unsigned int)_lastCmdCode,
                    (unsigned int)_lastLowDuration,
                    (unsigned int)_minLowDuration,
                    (unsigned int)_maxLowDuration
                );
                _diagCb(diag);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
