#pragma once

#include <Arduino.h>
#include <SPI.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/ringbuf.h>
#include <driver/rmt.h>

enum class Button{
    PREV,
    NEXT,
    CD1,
    CD2,
    CD3,
    CD4,
    CD5,
    CD6,
    ASMIX,
    SCAN,
    NONE
};

typedef void (*CDCButtonCallback)(Button button);
typedef void (*CDCDiagCallback)(const char* line);

class CDC {
    public:
        CDC();

        bool begin(int DataOutPin, int SCKPin, int MOSIPin, int MISOPin);
        void setButtonCallback(CDCButtonCallback cb) { _buttonCb = cb; }
        void setDiagCallback(CDCDiagCallback cb) { _diagCb = cb; }
        void setTrack(u_int8_t trackNum);
        void setDisc(u_int8_t discNum);
        void setPlayTime(uint8_t minutes, uint8_t seconds);
    private:
        SPIClass* _spi = nullptr;

        void sendStatus();
        void _handleVwCommand(uint8_t cmdCode);
        void _sendSpiPacket(const uint8_t frame[8]);
        
        static void _taskRunner(void* pvParameters);
        void _taskLoop();
        
        // RMT Hardware Decoding
        RingbufHandle_t _rmt_ringbuf = NULL;
        void _processRmtData();
        bool _rmtDecoder(const rmt_item32_t* item, size_t num_items, uint32_t* decoded_cmd);

        uint8_t _currentMinutes = 0;
        uint8_t _currentSeconds = 0;

        CDCButtonCallback _buttonCb = nullptr;
        CDCDiagCallback _diagCb = nullptr;
        TaskHandle_t _taskHandle;
        int _dataOutPin = -1;

        uint8_t _currentTrack = 1;
        uint8_t _currentDisc = 1;
        uint8_t _modeByte = 0xFF;
        uint8_t _scanByte = 0xCF; // Mode bytes for PLAY
        
        uint32_t _validPackets = 0;
        uint32_t _badPrefixPackets = 0;
        uint32_t _badChecksumPackets = 0;
        uint8_t _lastCmdCode = 0;
        uint32_t _lastDiagMs = 0;

        enum class SpiState {
            IDLE_THEN_PLAY,
            INIT_PLAY,
            PLAY_LEAD_IN,
            PLAY
        };

        SpiState _spiState = SpiState::IDLE_THEN_PLAY;
        int _stateCounter = 0;
        bool _initStarted = false;
        uint8_t _discLoad = 0x2E;
        uint32_t _lastSpiMs = 0;
};
