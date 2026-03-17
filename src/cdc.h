#pragma once

#include <Arduino.h>
#include <SPI.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

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

enum class DecoderState{
    UNUSED
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
        void sendStatus(); // Спрятали внутрь, так как теперь он вызывается автоматически
        void _scanVwPackets();
        void _handleVwCommand(uint8_t cmdCode);


        SPIClass* _spi = nullptr;

        static void IRAM_ATTR _isr(void* arg);

        uint8_t _currentMinutes = 0;
        uint8_t _currentSeconds = 0;

        // VW pulse-width decoder constants (vwcdpic model)
        static constexpr uint32_t VW_START_THRESHOLD = 3200;
        static constexpr uint32_t VW_HIGH_THRESHOLD  = 1248;
        static constexpr uint32_t VW_LOW_THRESHOLD   = 256;
        static constexpr uint8_t  VW_PKTSIZE         = 32;

        static constexpr uint8_t VW_CAPBUFFER_SIZE = 24;
        volatile uint8_t _capBuffer[VW_CAPBUFFER_SIZE] = {0};
        volatile uint8_t _capPtr = 0;
        volatile uint8_t _scanPtr = 0;
        volatile bool _capBusy = false;
        volatile uint8_t _capBit = 8;
        volatile uint8_t _capBitPacket = 0;
        volatile uint8_t _currentByte = 0;
        volatile uint32_t _lastFallingEdge = 0;
        volatile bool _measuringLow = false;
        volatile uint16_t _lastLowDuration = 0;
        volatile uint16_t _minLowDuration = 0xFFFF;
        volatile uint16_t _maxLowDuration = 0;

        CDCButtonCallback _buttonCb = nullptr;
        CDCDiagCallback _diagCb = nullptr;
        TaskHandle_t _taskHandle;
        int _dataOutPin = -1;

        uint8_t _currentTrack = 1;
        uint8_t _currentDisc = 1;
        uint8_t _modeByte = 0x00;
        uint8_t _scanByte = 0xCF;
        uint32_t _validPackets = 0;
        uint32_t _badPrefixPackets = 0;
        uint32_t _badChecksumPackets = 0;
        uint32_t _badCmdFormatPackets = 0;
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

        static void _taskRunner(void* pvParameters);
        void _taskLoop();
        void _sendSpiPacket(const uint8_t frame[8]);
};
