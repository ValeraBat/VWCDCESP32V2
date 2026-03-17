#include "webUI.h"
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <ElegantOTA.h>
#include <ESPmDNS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <Preferences.h>

bool g_debugMode = true; // Сделаем пока true для отладки

WebServer server(80);
WebSocketsServer webSocket = WebSocketsServer(81);
WebUICallback _atCommandCallback = nullptr;

// Кольцевой буфер для логов (чтобы не фрагментировать память вектором)
const size_t MAX_LOG_HISTORY = 40;
String logHistory[MAX_LOG_HISTORY];
size_t logHead = 0;
size_t logCount = 0;
const size_t MAX_WS_QUEUE = 64;
String wsQueue[MAX_WS_QUEUE];
size_t wsQueueHead = 0;
size_t wsQueueCount = 0;
volatile uint8_t g_wsClientCount = 0;
SemaphoreHandle_t g_logMutex = NULL;

static void queueWsMessage(const String& line) {
    if (g_logMutex != NULL && xSemaphoreTake(g_logMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        wsQueue[(wsQueueHead + wsQueueCount) % MAX_WS_QUEUE] = line;
        if (wsQueueCount < MAX_WS_QUEUE) {
            wsQueueCount++;
        } else {
            wsQueueHead = (wsQueueHead + 1) % MAX_WS_QUEUE;
        }
        xSemaphoreGive(g_logMutex);
    }
}

void webUI_setATCallback(WebUICallback cb) {
    _atCommandCallback = cb;
}

// --- HTML и CSS (Адаптивный дизайн для ПК и Телефона) ---
static const char MAIN_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="ru">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>VW CDC V2 Debugger</title>
    <style>
        :root { --bg: #121212; --panel: #1e1e1e; --text: #e0e0e0; --accent: #0288D1; --accent-hover: #0277bd; }
        * { box-sizing: border-box; }
        body { background: var(--bg); color: var(--text); font-family: -apple-system, system-ui, sans-serif; margin: 0; padding: 0; display: flex; flex-direction: column; height: 100vh; }
        
        header { background: var(--panel); padding: 10px 15px; box-shadow: 0 2px 6px rgba(0,0,0,0.4); z-index: 10; display: flex; flex-wrap: wrap; gap: 10px; align-items: center; justify-content: space-between; }
        .title { margin: 0; font-size: 1.1rem; font-weight: 600; color: #fff; }
        
        .tab-btn { background: #333; color: #ccc; border: none; padding: 8px 16px; border-radius: 6px; cursor: pointer; font-size: 0.9rem; transition: 0.2s; flex-grow: 1; text-align: center; }
        .tab-btn.active { background: var(--accent); color: white; font-weight: 600; }
        
        .controls { display: flex; gap: 8px; flex-wrap: wrap; width: 100%; }
        @media(min-width: 600px) {
            .controls { width: auto; flex-wrap: nowrap; }
            .tab-btn { flex-grow: 0; }
            .title { flex-grow: 1; }
        }

        .tab-content { display: none; flex: 1; flex-direction: column; overflow: hidden; }
        .tab-content.active { display: flex; }
        
        .log-box { flex: 1; padding: 12px; overflow-y: auto; background: #000; font-family: 'Consolas', 'Courier New', monospace; font-size: 0.9rem; line-height: 1.4; scroll-behavior: smooth; border-bottom: 2px solid #333; }
        .log-line { margin: 0 0 4px 0; border-bottom: 1px dashed #222; padding-bottom: 2px; word-wrap: break-word; }
        .log-info { color: #81C784; }
        .log-debug { color: #FFB74D; }
        .log-sys { color: #64B5F6; }
        .log-raw { color: #9E9E9E; font-size: 0.85rem; }
        .log-tx { color: #E57373; font-weight: bold; }
        
        .terminal-input-area { background: #1a1a1a; display: flex; padding: 10px; gap: 10px; align-items: center; }
        .terminal-prompt { color: #64B5F6; font-family: monospace; font-weight: bold; font-size: 1.1rem; }
        input[type="text"] { flex: 1; padding: 10px; background: #000; border: 1px solid #444; color: #81C784; font-family: monospace; border-radius: 4px; font-size: 1rem; }
        input[type="text"]:focus { outline: none; border-color: var(--accent); }
        button.action { background: var(--accent); color: white; border: none; padding: 10px 20px; border-radius: 4px; cursor: pointer; font-family: monospace; font-size: 1rem; font-weight: bold; }
        button.action:hover { background: var(--accent-hover); }
        button.danger { background: #d32f2f; color: white; border: none; padding: 6px 12px; border-radius: 6px; cursor: pointer; font-size: 0.8rem;}

        .panel-area { background: #1a1a1a; border-bottom: 2px solid #333; flex: 0 0 auto; max-height: 45vh; overflow-y: auto; padding: 10px; }
        details { background: #222; padding: 8px 12px; border-radius: 6px; margin-bottom: 8px; border: 1px solid #333; }
        summary { color:#64B5F6; cursor:pointer; font-weight:bold; outline: none; }
        .detail-row { display: flex; gap: 10px; margin-top: 10px; align-items: center; flex-wrap: wrap; }

        ::-webkit-scrollbar { width: 8px; }
        ::-webkit-scrollbar-track { background: #000; }
        ::-webkit-scrollbar-thumb { background: #444; border-radius: 4px; }
    </style>
</head>
<body>
    <header>
        <div class="title">CDCESP32 V2</div>
        <div class="controls">
            <button class="tab-btn active" onclick="switchTab('main')">System</button>
            <button class="tab-btn" onclick="switchTab('cdc')">CDC Raw</button>
            <button class="danger" onclick="clearCurrentLog()">Clear Log</button>
            <a href="/update" style="color:#fa0; font-weight:bold; margin-left:10px; text-decoration:none; padding:8px;">OTA UPDATE</a>
        </div>
    </header>

    <div id="tab-main" class="tab-content active">
        <!-- Панель управления (сверху) -->
        <div class="panel-area">
            <div style="display:flex; justify-content: space-between; align-items:center; margin-bottom: 10px; padding: 0 5px;">
                <span style="color:#aaa;">State: <strong id="ui_state" style="color:#fff;">UNKNOWN</strong></span>
                <div style="display:flex; gap:5px;">
                    <button class="action" style="padding:6px 10px; font-size:0.8rem;" onclick="sendCustomAT('AT+STAT')">Refresh</button>
                    <button class="action" style="padding:6px 10px; font-size:0.8rem;" onclick="sendCustomAT('AT+BACKWARD')">⏮</button>
                    <button class="action" style="padding:6px 10px; font-size:0.8rem;" onclick="sendCustomAT('AT+PLAYPAUSE')">⏯</button>
                    <button class="action" style="padding:6px 10px; font-size:0.8rem;" onclick="sendCustomAT('AT+FORWARD')">⏭</button>
                </div>
            </div>

            <details>
                <summary>Paired Devices</summary>
                <div style="margin-top: 10px;">
                    <button class="action" style="padding: 4px 10px; font-size: 0.8rem; margin-bottom: 10px;" onclick="requestPairedList()">Refresh List</button>
                    <div id="pairedList" style="font-size: 0.9rem;"></div>
                </div>
            </details>
            
            <details>
                <summary>Configuration & Hardware</summary>
                <div class="detail-row">
                    <label style="color:#aaa; width:60px;">Name:</label>
                    <input type="text" id="btName" value="VW_BT1026" style="flex:1; padding:6px;">
                    <button class="action" style="padding:6px 12px; font-size:0.9rem;" onclick="sendCustomAT('AT+NAME=' + document.getElementById('btName').value + ',0')">Set</button>
                </div>
                <div class="detail-row">
                    <label style="color:#aaa; width:60px;">COD:</label>
                    <input type="text" id="btCod" value="240404" style="flex:1; padding:6px;">
                    <button class="action" style="padding:6px 12px; font-size:0.9rem;" onclick="sendCustomAT('AT+COD=' + document.getElementById('btCod').value)">Set</button>
                </div>
                <div class="detail-row" style="margin-top:15px; border-top: 1px dashed #444; padding-top: 10px;">
                    <button class="action" style="padding:6px 10px; font-size:0.8rem;" onclick="sendCustomAT('AT+MAC?')">Get MAC</button>
                    <button class="action" style="padding:6px 10px; font-size:0.8rem;" onclick="sendCustomAT('AT+VOL=12')">Vol Max</button>
                    <button class="danger" style="margin-left:auto;" onclick="if(confirm('Reboot BT Module?')) sendCustomAT('AT+REBOOT')">Reboot BT</button>
                    <button class="danger" onclick="if(confirm('Run Factory Setup on BT?')) sendCustomAT('AT+FACTORY')">Factory BT</button>
                    <button class="danger" style="background:#b71c1c;" onclick="if(confirm('Reboot ESP32?')) { fetch('/api/reboot_esp').then(()=>setTimeout(()=>location.reload(),3000)); }">Reboot ESP32</button>
                </div>
            </details>

            <details>
                <summary>WiFi Setup (Connect to Home Network)</summary>
                <div class="detail-row">
                    <label style="color:#aaa; width:60px;">SSID:</label>
                    <input type="text" id="wifiSsid" placeholder="Home WiFi Name" style="flex:1; padding:6px;">
                </div>
                <div class="detail-row">
                    <label style="color:#aaa; width:60px;">PASS:</label>
                    <input type="password" id="wifiPass" placeholder="Password" style="flex:1; padding:6px;">
                </div>
                <div class="detail-row" style="margin-top:15px;">
                    <button class="action" style="padding:6px 15px; width:100%;" onclick="connectWiFi()">Save & Connect</button>
                </div>
            </details>
        </div>

        <!-- Терминал логов (снизу) -->
        <div id="log-sys" class="log-box"></div>
        <div class="terminal-input-area">
            <span class="terminal-prompt">$></span>
            <input type="text" id="atInput" autocomplete="off" autocorrect="off" autocapitalize="off" spellcheck="false" placeholder="AT+VER" onkeypress="if(event.key === 'Enter') sendAT()">
            <button class="action" onclick="sendAT()">SEND</button>
        </div>
    </div>

    <!-- Вкладка 2: CDC RAW логи -->
    <div id="tab-cdc" class="tab-content">
        <div class="panel-area" style="max-height:none; border-bottom:1px solid #333;">
            <div style="display:grid; grid-template-columns: repeat(auto-fit, minmax(180px, 1fr)); gap:8px; font-family:Consolas, monospace; font-size:0.85rem;">
                <div>State: <strong id="cdc_state" style="color:#fff;">-</strong></div>
                <div>Valid: <strong id="cdc_valid" style="color:#81C784;">0</strong></div>
                <div>BadPrefix: <strong id="cdc_badprefix" style="color:#FFB74D;">0</strong></div>
                <div>BadCsum: <strong id="cdc_badcsum" style="color:#FFB74D;">0</strong></div>
                <div>BadFmt: <strong id="cdc_badfmt" style="color:#FFB74D;">0</strong></div>
                <div>LastCmd: <strong id="cdc_cmd" style="color:#64B5F6;">0x00</strong></div>
                <div>Low: <strong id="cdc_low" style="color:#aaa;">0</strong> us</div>
                <div>Min/Max: <strong id="cdc_minmax" style="color:#aaa;">0 / 0</strong> us</div>
            </div>
        </div>
        <div id="log-cdc" class="log-box"></div>
    </div>

    <script>
        const elLogs = document.getElementById('log-sys');
        const elCdc = document.getElementById('log-cdc');
        const elCdcState = document.getElementById('cdc_state');
        const elCdcValid = document.getElementById('cdc_valid');
        const elCdcBadPrefix = document.getElementById('cdc_badprefix');
        const elCdcBadCsum = document.getElementById('cdc_badcsum');
        const elCdcBadFmt = document.getElementById('cdc_badfmt');
        const elCdcCmd = document.getElementById('cdc_cmd');
        const elCdcLow = document.getElementById('cdc_low');
        const elCdcMinMax = document.getElementById('cdc_minmax');
        let currentTabId = 'main';
        let ws;

        function switchTab(tabId) {
            currentTabId = tabId;
            document.querySelectorAll('.tab-content').forEach(el => el.classList.remove('active'));
            document.querySelectorAll('.tab-btn').forEach(el => el.classList.remove('active'));
            
            document.getElementById('tab-' + tabId).classList.add('active');
            event.currentTarget.classList.add('active');
        }

        function clearCurrentLog() {
            if(currentTabId === 'main') elLogs.innerHTML = '';
            if(currentTabId === 'cdc') elCdc.innerHTML = '';
        }

        function requestPairedList() {
            document.getElementById('pairedList').innerHTML = '<div style="color:#fa0;">Fetching devices...</div>';
            sendCustomAT('AT+PLIST');
        }

        function deleteDevice(mac) {
            if(confirm('Delete device ' + mac + '?')) {
                // BT1026 uses AT+PLIST=<MAC|index|0> for delete/clear operations.
                sendCustomAT('AT+PLIST=' + mac);
                setTimeout(requestPairedList, 1000);
            }
        }

        function sendCustomAT(cmd) {
            const input = document.getElementById('atInput');
            input.value = cmd;
            sendAT(); 
        }

        function sendAT() {
            const input = document.getElementById('atInput');
            const cmd = input.value.trim();
            if(!cmd) return;
            
            const pEcho = document.createElement('div');
            pEcho.className = 'log-line log-tx';
            pEcho.textContent = "> " + cmd;
            elLogs.appendChild(pEcho);
            elLogs.scrollTop = elLogs.scrollHeight;

            fetch('/api/at_cmd?cmd=' + encodeURIComponent(cmd)).then(res => {
                input.value = '';
                input.focus();
            });
        }

        function connectWiFi() {
            const ssid = document.getElementById('wifiSsid').value.trim();
            const psk = document.getElementById('wifiPass').value;
            if(!ssid) return alert("SSID required");
            fetch('/api/wifi/connect', {
                method: 'POST',
                headers: {'Content-Type': 'application/x-www-form-urlencoded'},
                body: 'ssid=' + encodeURIComponent(ssid) + '&psk=' + encodeURIComponent(psk)
            }).then(r => alert('WiFi settings saved! Attempting to connect...'));
        }

        function handleWSMessage(text) {
            // 1. Парсинг статуса
            if(text.includes('Music PLAYING')) document.getElementById('ui_state').innerText = "PLAYING";
            if(text.includes('Music PAUSED')) document.getElementById('ui_state').innerText = "PAUSED";
            if(text.includes('Music STOPPED') || text.includes('A2DP Connected')) document.getElementById('ui_state').innerText = "A2DP CONNECTED";
            if(text.includes('Call Active!')) document.getElementById('ui_state').innerText = "CALL ACTIVE";
            if(text.includes('Incoming Call!')) document.getElementById('ui_state').innerText = "INCOMING CALL";
            if(text.includes('Disconnected') && text.includes('[BT]')) document.getElementById('ui_state').innerText = "DISCONNECTED";

            // 2. Список устройств
            if(text.includes('+PLIST=') || text.includes('+PLIST:')) {
                const match = text.match(/\+PLIST[:=]\s*(\d+),([^,]+),(.*)/);
                if(match && match.length >= 4) {
                    const mac = match[2];
                    const name = match[3];
                    
                    const listContainer = document.getElementById('pairedList');
                    if(listContainer.innerHTML.includes('Fetching')) {
                        listContainer.innerHTML = '';
                    }

                    const item = document.createElement('div');
                    item.style.cssText = "display:flex; justify-content:space-between; padding:8px 0; border-bottom:1px dashed #444; align-items:center;";
                    item.innerHTML = `
                        <div>
                            <strong style="color:#81C784;">${name}</strong>
                            <div style="color:#888; font-size:0.8rem;">MAC: ${mac}</div>
                        </div>
                        <button class="danger" style="padding:4px 8px;" onclick="deleteDevice('${mac}')">Delete</button>
                    `;
                    listContainer.appendChild(item);
                } else if (text.includes('+PLIST=E') || text.includes('+PLIST:E')) {
                    const listContainer = document.getElementById('pairedList');
                    if(listContainer.innerHTML.includes('Fetching')) {
                        listContainer.innerHTML = '<div style="color:#aaa;">No paired devices found</div>';
                    }
                }
                return; // Не выводим PLIST в общие логи
            }

            // 3. Вывод лога
            const p = document.createElement('div');
            if(text.startsWith('[CDC RAW]')) {
                const rawText = text.replace('[CDC RAW] ', '');
                if(rawText.startsWith('[CDC DIAG]')) {
                    const state = rawText.match(/state=([^\s]+)/);
                    const valid = rawText.match(/valid=(\d+)/);
                    const badPrefix = rawText.match(/badPrefix=(\d+)/);
                    const badCsum = rawText.match(/badCsum=(\d+)/);
                    const badFmt = rawText.match(/badFmt=(\d+)/);
                    const cmd = rawText.match(/cmd=0x([0-9a-fA-F]+)/);
                    const low = rawText.match(/low=(\d+)/);
                    const min = rawText.match(/min=(\d+)/);
                    const max = rawText.match(/max=(\d+)/);

                    if(state) elCdcState.textContent = state[1];
                    if(valid) elCdcValid.textContent = valid[1];
                    if(badPrefix) elCdcBadPrefix.textContent = badPrefix[1];
                    if(badCsum) elCdcBadCsum.textContent = badCsum[1];
                    if(badFmt) elCdcBadFmt.textContent = badFmt[1];
                    if(cmd) elCdcCmd.textContent = '0x' + cmd[1].toUpperCase();
                    if(low) elCdcLow.textContent = low[1];
                    if(min && max) elCdcMinMax.textContent = min[1] + ' / ' + max[1];
                }

                p.className = 'log-line log-raw';
                p.textContent = rawText;
                elCdc.appendChild(p);
                if(elCdc.childNodes.length > 200) elCdc.removeChild(elCdc.firstChild);
                elCdc.scrollTop = elCdc.scrollHeight;
            } else {
                let cssClass = 'log-info';
                if(text.includes('[DEBUG]')) cssClass = 'log-debug';
                if(text.includes('[SYS]')) cssClass = 'log-sys';
                if(text.includes('[BT TX]')) cssClass = 'log-tx';
                
                p.className = 'log-line ' + cssClass;
                p.textContent = text;
                elLogs.appendChild(p);
                if(elLogs.childNodes.length > 200) elLogs.removeChild(elLogs.firstChild);
                elLogs.scrollTop = elLogs.scrollHeight;
            }
        }

        function connectWS() {
            ws = new WebSocket('ws://' + window.location.hostname + ':81/');
            ws.onopen = function() { 
                elLogs.innerHTML = ''; // Очищаем экраны при переподключении, чтобы не дублировать историю
                elCdc.innerHTML = '';
            };
            ws.onmessage = function(e) { handleWSMessage(e.data); };
            ws.onclose = function() { setTimeout(connectWS, 2000); };
        }

        window.onload = connectWS;
    </script>
</body>
</html>
)rawliteral";

// --- Обработчики HTTP и WebSockets ---

void handleRoot() {
    server.send(200, "text/html", MAIN_PAGE);
}

void handleAtCmd() {
    if (server.hasArg("cmd")) {
        String cmd = server.arg("cmd");
        // Вызываем коллбэк, который мы привязали в main.cpp
        if (_atCommandCallback != nullptr) {
            _atCommandCallback(cmd);
            server.send(200, "text/plain", "OK");
        } else {
            server.send(500, "text/plain", "Callback not set");
        }
    } else {
        server.send(400, "text/plain", "Bad Request");
    }
}

void handleRebootESP() {
    server.send(200, "text/plain", "ESP Rebooting...");
    delay(500);
    ESP.restart();
}

void handleWifiConnect() {
    if (server.hasArg("ssid")) {
        String ssid = server.arg("ssid");
        String psk = server.arg("psk");
        Preferences prefs;
        prefs.begin("vw_bt", false);
        prefs.putString("sta_ssid", ssid);
        prefs.putString("sta_psk", psk);
        prefs.end();
        
        server.send(200, "text/plain", "OK");
        
        // Переключаем в режим AP+STA и пробуем подключиться
        WiFi.mode(WIFI_AP_STA);
        WiFi.begin(ssid.c_str(), psk.c_str());
        webUI_log("[SYS] Connecting to WiFi: " + ssid, LogLevel::INFO);
    } else {
        server.send(400, "text/plain", "Missing SSID");
    }
}

void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
    if(type == WStype_CONNECTED) {
        g_wsClientCount++;
        if (g_logMutex != NULL && xSemaphoreTake(g_logMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            const size_t replayCount = (logCount > 12) ? 12 : logCount;
            const size_t startOffset = (logCount > replayCount) ? (logCount - replayCount) : 0;
            for(size_t i = 0; i < replayCount; i++) {
                size_t idx = (logHead + startOffset + i) % MAX_LOG_HISTORY;
                webSocket.sendTXT(num, logHistory[idx]);
            }
            xSemaphoreGive(g_logMutex);
        }
        
        webSocket.sendTXT(num, "[SYS] WebSocket Connected. V2 Architecture.");
    } else if (type == WStype_DISCONNECTED) {
        if (g_wsClientCount > 0) {
            g_wsClientCount--;
        }
    }
}

// --- Публичные функции логирования ---

void webUI_log(const String &line, LogLevel level) {
    if ((level == LogLevel::DEBUG || level == LogLevel::VERBOSE) && !g_debugMode) {
        return; 
    }

#ifdef USE_HW_SERIAL
    Serial.println(line); 
#endif

    // Если Wi-Fi выключен, нет смысла копить историю логов и забивать очередь (разве что нужен Serial)
    if (!g_wifiActive) return;

    if (g_logMutex != NULL && xSemaphoreTake(g_logMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        logHistory[(logHead + logCount) % MAX_LOG_HISTORY] = line;
        if (logCount < MAX_LOG_HISTORY) {
            logCount++;
        } else {
            logHead = (logHead + 1) % MAX_LOG_HISTORY;
        }
        xSemaphoreGive(g_logMutex);
    }

    queueWsMessage(line);
}

void webUI_broadcastCdcRaw(const String &line) {
    if (!g_debugMode) return;
#ifdef USE_HW_SERIAL
    Serial.println(line);
#endif

    if (!g_wifiActive) return;

    queueWsMessage("[CDC RAW] " + line);
}

bool g_wifiActive = true;

// --- Фоновая задача FreeRTOS (Сервер) ---

void _webUITask(void *pvParameters) {
    Preferences prefs;
    prefs.begin("vw_bt", true); // true = read-only mode
    g_wifiActive = prefs.getBool("wifi_on", true); // Получаем сохраненное состояние, по умолчанию true
    String staSsid = prefs.getString("sta_ssid", "");
    String staPsk  = prefs.getString("sta_psk", "");
    prefs.end();

    if (g_wifiActive) {
        if(staSsid.length() > 0) {
            WiFi.mode(WIFI_AP_STA);
            WiFi.softAP("VW-BT1026D", "12345678");
            WiFi.begin(staSsid.c_str(), staPsk.c_str());
            webUI_log("[SYS] WebUI Started. AP: VW-BT1026D, STA: " + staSsid, LogLevel::INFO);
        } else {
            WiFi.mode(WIFI_AP);
            WiFi.softAP("VW-BT1026D", "12345678");
            webUI_log("[SYS] WebUI Started. WiFi AP: VW-BT1026D (12345678)", LogLevel::INFO);
        }
        WiFi.setSleep(false); // Отключаем сон WiFi для стабильности
        
        // Запускаем mDNS: устройство будет доступно по адресу http://vw-bt.local
        if (MDNS.begin("vw-bt")) {
            MDNS.addService("http", "tcp", 80);
            webUI_log("[SYS] mDNS started: http://vw-bt.local", LogLevel::INFO);
        }
    } else {
        WiFi.mode(WIFI_OFF);
    }

    server.on("/", handleRoot);
    server.on("/api/at_cmd", HTTP_GET, handleAtCmd); // Регистрация эндпоинта для AT Команд!
    server.on("/api/wifi/connect", HTTP_POST, handleWifiConnect); // Регистрация эндпоинта для WiFi
    server.on("/api/reboot_esp", HTTP_GET, handleRebootESP); // Эндпоинт перезагрузки ESP

    ElegantOTA.begin(&server); // Инициализация OTA
    server.begin();

    webSocket.begin();
    webSocket.onEvent(webSocketEvent);
    webSocket.enableHeartbeat(15000, 3000, 2);

    while(true) {
        if (g_wifiActive) {
            server.handleClient();
            ElegantOTA.loop(); // Фоновый цикл OTA
            webSocket.loop();

            if (g_wsClientCount > 0) {
                String outBuf[8];
                size_t outCount = 0;
                if (g_logMutex != NULL && xSemaphoreTake(g_logMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                    while (wsQueueCount > 0 && outCount < 8) {
                        outBuf[outCount++] = wsQueue[wsQueueHead];
                        wsQueueHead = (wsQueueHead + 1) % MAX_WS_QUEUE;
                        wsQueueCount--;
                    }
                    xSemaphoreGive(g_logMutex);
                }

                for (size_t i = 0; i < outCount; i++) {
                    webSocket.broadcastTXT(outBuf[i]);
                }
            } else if (g_logMutex != NULL && xSemaphoreTake(g_logMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                wsQueueHead = 0;
                wsQueueCount = 0;
                xSemaphoreGive(g_logMutex);
            }
            
            // Если мы пытаемся подключиться к STA (домашней сети), проверяем статус и логируем IP один раз
            static bool ipLogged = false;
            if (WiFi.getMode() == WIFI_AP_STA) {
                if (WiFi.status() == WL_CONNECTED && !ipLogged) {
                    ipLogged = true;
                    webUI_log("[SYS] Connected to HOME WiFi! IP: " + WiFi.localIP().toString(), LogLevel::INFO);
                } else if (WiFi.status() != WL_CONNECTED && ipLogged) {
                    ipLogged = false; // Сброс флага, если связь потеряна
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10)); 
    }
}

void webUI_init() {
    if (g_logMutex == NULL) {
        g_logMutex = xSemaphoreCreateMutex();
    }
    xTaskCreatePinnedToCore(
        _webUITask, 
        "WebUI_Task", 
        4096, 
        NULL, 
        1, 
        NULL, 
        0   // Core 0 - PRO_CPU (сеть, WiFi)
    );
}

bool webUI_isWiFiActive() {
    return g_wifiActive;
}

void webUI_toggleWiFi() {
    g_wifiActive = !g_wifiActive;
    
    // Сохраняем новое состояние в энергонезависимую память локальным объектом
    Preferences prefs;
    prefs.begin("vw_bt", false); // false = read/write
    prefs.putBool("wifi_on", g_wifiActive);
    prefs.end();

    if (g_wifiActive) {
        // Включаем обратно
        WiFi.mode(WIFI_AP);
        WiFi.softAP("VW-BT1026D", "12345678");
        webUI_log("[SYS] WiFi AP Enabled: VW-BT1026D", LogLevel::INFO);
    } else {
        // Выключаем (для экономии батареи/уменьшения помех)
        webUI_log("[SYS] WiFi AP Disabled by User", LogLevel::INFO);
        // Небольшая задержка, чтобы лог успел улететь в WebSocket до закрытия
        vTaskDelay(pdMS_TO_TICKS(100)); 
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_OFF);
    }
}