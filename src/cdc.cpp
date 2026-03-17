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

    // Initialize state
    _validPackets = 0;
    _badPrefixPackets = 0;
    _badChecksumPackets = 0;
    _lastCmdCode = 0;
    _lastDiagMs = millis();

    _spiState = SpiState::IDLE_THEN_PLAY;
    _stateCounter = 0;
    _initStarted = false;
    _discLoad = 0x2E;
    _lastSpiMs = millis();

    // RMT Hardware Configuration for capturing IR-like NEC pulses
    rmt_config_t rmt_cfg;
    rmt_cfg.channel = RMT_CHANNEL_0;
    rmt_cfg.gpio_num = (gpio_num_t)_dataOutPin;
    rmt_cfg.clk_div = 80;                // 80MHz/80 = 1 tick = 1us
    rmt_cfg.mem_block_num = 1;           // 64 memory units 
    rmt_cfg.rmt_mode = RMT_MODE_RX;

    // Filter to suppress small spikes (glitches < 150us)
    // and wait for ~12ms of idle bus to finish a packet
    rmt_cfg.rx_config.filter_en = true;
    rmt_cfg.rx_config.filter_ticks_thresh = 150; 
    rmt_cfg.rx_config.idle_threshold = 12000;

    rmt_config(&rmt_cfg);
    rmt_driver_install(rmt_cfg.channel, 2048, 0); // 2048 bytes rx buffer
    rmt_get_ringbuf_handle(rmt_cfg.channel, &_rmt_ringbuf);
    rmt_rx_start(rmt_cfg.channel, true); // True clears RX buffer on start

    // Create the CDC Core Task
    // Notice we can lower priority now because RMT does the heavy lifting!
    BaseType_t success = xTaskCreatePinnedToCore(
        _taskRunner,
        "CDC_Task",
        4096,
        this,
        2,  
        &_taskHandle,
        1   // Core 1
    );

    return (success == pdPASS);
}

// Custom NEC pulse to 32-bit integer decoder tailored to Volkswagen protocol
bool CDC::_rmtDecoder(const rmt_item32_t* item, size_t num_items, uint32_t* decoded_cmd) {
    // A packet needs at least 32 data bits (some leading noise is possible)
    if (num_items < 32) return false;

    uint32_t cmd = 0;
    int bit_pos = 0; 

    for (size_t i = 0; i < num_items; i++) {
        // Find LOW pulse duration (since line is HIGH idle, LOW is the active logic)
        uint32_t lowDuration = 0;
        if (item[i].level0 == 0) {
            lowDuration = item[i].duration0;
        } else if (item[i].level1 == 0) {
            // Started capturing on rising edge, meaning level1 is the LOW pulse
            lowDuration = item[i].duration1;
        } else {
            continue; // Not a valid transition pair
        }

        // vwcdpic timing specs in microseconds:
        // Start bit:  LOW > 3200µs
        // Bit '1':    LOW > 1248µs
        // Bit '0':    LOW < 1248µs
        // Noise filter: LOW > 256µs minimum

        if (lowDuration < 256) {
            continue; // Noise
        }

        if (lowDuration >= 3200) {
            // START Bit found. Reset tracking for a fresh 32-bit packet
            cmd = 0;
            bit_pos = 0;
            continue;
        }

        // Data bit
        if (bit_pos < 32) {
            cmd <<= 1; // MSB first for each byte, pushed chronologically correctly
            if (lowDuration >= 1248) {
                cmd |= 0x01; // Logic 1
            } else {
                cmd |= 0x00; // Logic 0
            }
            bit_pos++;

            if (bit_pos == 32) {
                *decoded_cmd = cmd;
                return true; 
            }
        }
    }

    return false;
}

void CDC::_processRmtData() {
    size_t rx_size = 0;
    // Non-blocking or minimal block to allow SPI updates (10ms wait max)
    rmt_item32_t* item = (rmt_item32_t*) xRingbufferReceive(_rmt_ringbuf, &rx_size, pdMS_TO_TICKS(10));

    if (item) {
        uint32_t decoded_cmd = 0;
        
        if (_rmtDecoder(item, rx_size / sizeof(rmt_item32_t), &decoded_cmd)) {
            // Unpack 4 bytes
            uint8_t byte0 = (decoded_cmd >> 24) & 0xFF; 
            uint8_t byte1 = (decoded_cmd >> 16) & 0xFF; 
            uint8_t byte2 = (decoded_cmd >> 8) & 0xFF;  
            uint8_t byte3 = decoded_cmd & 0xFF;

            if (byte0 != 0x53 || byte1 != 0x2C) {
                _badPrefixPackets++;
            } else if (byte3 != (0xFF ^ byte2)) {
                _badChecksumPackets++;
            } else {
                // Success!
                _validPackets++;
                _lastCmdCode = byte2;
                _handleVwCommand(byte2);
            }
        }
        
        vRingbufferReturnItem(_rmt_ringbuf, (void*) item);
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
            0x74, (uint8_t)(0xBF - disc), (uint8_t)(0xFF - track),
            0xFF, 0xFF, 0xFF, 0x8F, 0x7C
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
                0x34, _discLoad, (uint8_t)(0xFF - 0x99), (uint8_t)(0xFF - 0x99),
                (uint8_t)(0xFF - 0x59), 0xB7, 0xFF, 0x3C
            };
            _sendSpiPacket(frame);
            if (_discLoad == 0x29) _discLoad = 0x2E;
            else _discLoad--;
        } else {
            uint8_t frame[8] = {
                0x34, (uint8_t)(0xBF - disc), (uint8_t)(0xFF - track),
                0xFF, 0xFF, 0xFF, 0xEF, 0x3C
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
                0x34, (uint8_t)((disc & 0x0F) | 0x20), (uint8_t)(0xFF - 0x99),
                (uint8_t)(0xFF - 0x99), (uint8_t)(0xFF - 0x59), 0xB7, 0xFF, 0x3C
            };
            _sendSpiPacket(frame);
        } else {
            uint8_t frame[8] = {
                0x34, (uint8_t)(0xBF - disc), (uint8_t)(0xFF - track),
                0xFF, 0xFF, 0xFF, 0xAE, 0x3C
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
        // Read decoded RMT signals from the buffer and parse button presses
        _processRmtData();
        
        // Ensure SPI packet reaches radio consistently
        sendStatus();

        uint32_t now = millis();
        if (now - _lastDiagMs >= 1000) {
            _lastDiagMs = now;

            const char* state = "PLAY";
            if (_spiState == SpiState::IDLE_THEN_PLAY) state = "IDLE_THEN_PLAY";
            else if (_spiState == SpiState::INIT_PLAY) state = "INIT_PLAY";
            else if (_spiState == SpiState::PLAY_LEAD_IN) state = "PLAY_LEAD_IN";

            if (_diagCb != nullptr) {
                char diag[128];
                snprintf(
                    diag, sizeof(diag),
                    "[CDC RMT] state=%s val=%lu errPre=%lu errCsm=%lu cmd=0x%02X",
                    state, _validPackets, _badPrefixPackets, _badChecksumPackets, _lastCmdCode
                );
                _diagCb(diag);
            }
        }
    }
}
