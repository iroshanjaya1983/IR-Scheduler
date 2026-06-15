#include <Arduino.h>
#include <Wire.h>          
#include <IRrecv.h>
#include <IRsend.h>
#include <IRutils.h>
#include <RTClib.h>
#include <WiFi.h>
#include <esp_wifi.h> // Required for TX power control to reduce heat
#include <WebServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <vector>
#include <algorithm>
#include <cstring>

// ---------- Pin Definitions for ESP32-C3 Super Mini ----------
#define IR_LED_PIN      4     
#define IR_RECV_PIN     6     
#define MODE_SWITCH_PIN 21    
#define BUILTIN_LED     10     // C3 Super Mini Built-in LED

// ---------- Remapped I2C Pins ----------
#define I2C_SDA       8    
#define I2C_SCL       9

// ---------- IR objects ----------
IRrecv irrecv(IR_RECV_PIN);
IRsend irsend(IR_LED_PIN);
decode_results irResults;

// ---------- RTC ----------
RTC_DS3231 rtc;

// ---------- WiFi AP ----------
const char* ap_ssid = "irauto";
const char* ap_password = "12345678"; // MUST BE 8+ characters
WebServer server(80);

// ---------- Preferences ----------
Preferences prefs;
#define PREF_NAMESPACE "ir_sched"

// ---------- Data structures ----------
struct IrCode {
  uint16_t id;
  String name;
  bool isRaw;
  decode_type_t protocol;
  uint64_t value;
  uint16_t bits;
  std::vector<uint16_t> rawbuf;
};

struct ScheduleItem {
  uint16_t id;
  uint16_t codeId;
  uint8_t hour;
  uint8_t minute;
  uint8_t dayMask;
  bool enabled;
};

// ---------- Global containers ----------
std::vector<IrCode> irCodes;
std::vector<ScheduleItem> schedules;
uint16_t nextCodeId = 1;
uint16_t nextScheduleId = 1;

// ---------- Mode & time ----------
bool receiverMode = true;
unsigned long lastModeCheck = 0;
unsigned long lastSecondTick = 0;
int lastLoggedMinute = -1;

// Execution log ring buffer
#define LOG_SIZE 10
String executionLog[LOG_SIZE];
uint8_t logIndex = 0;

// ---------- Helper functions ----------
void blinkLED(int times, int onMs = 50) {
  for (int i = 0; i < times; i++) {
    digitalWrite(BUILTIN_LED, LOW); // Active LOW on C3
    vTaskDelay(pdMS_TO_TICKS(onMs));
    digitalWrite(BUILTIN_LED, HIGH);
    vTaskDelay(pdMS_TO_TICKS(onMs));
  }
}

String rawToString(const std::vector<uint16_t>& raw) {
  String out;
  out.reserve(raw.size() * 5); // Prevent heap fragmentation
  for (size_t i = 0; i < raw.size(); i++) {
    if (i > 0) out += ',';
    out += String(raw[i]);
  }
  return out;
}

std::vector<uint16_t> stringToRaw(const String& str) {
  std::vector<uint16_t> res;
  int start = 0;
  int comma;
  do {
    comma = str.indexOf(',', start);
    String numStr = (comma == -1) ? str.substring(start) : str.substring(start, comma);
    if (numStr.length() > 0) {
      res.push_back((uint16_t)numStr.toInt());
    }
    start = comma + 1;
  } while (comma != -1);
  return res;
}

// ---------- Storage: load / save ----------
void loadIrCodes() {
  prefs.begin(PREF_NAMESPACE, false);
  int count = prefs.getInt("irCount", 0);
  irCodes.clear();
  nextCodeId = 1;
  for (int i = 0; i < count; i++) {
    String key = "ir_" + String(i);
    String json = prefs.getString(key.c_str(), "");
    if (json.length() == 0) continue;
    
    DynamicJsonDocument doc(1536); 
    DeserializationError err = deserializeJson(doc, json);
    if (err) continue;
    
    IrCode code;
    code.id = doc["id"];
    code.name = doc["name"].as<String>();
    code.isRaw = doc["isRaw"] | false;
    
    if (!code.isRaw) {
      code.protocol = (decode_type_t)doc["protocol"].as<int>();
      code.value = doc["value"];
      code.bits = doc["bits"];
    } else {
      String rawStr = doc["raw"].as<String>();
      code.rawbuf = stringToRaw(rawStr);
    }
    irCodes.push_back(code);
    if (code.id >= nextCodeId) nextCodeId = code.id + 1;
  }
  prefs.end();
}

void saveIrCodes() {
  prefs.begin(PREF_NAMESPACE, false);
  prefs.putInt("irCount", irCodes.size());
  for (size_t i = 0; i < irCodes.size(); i++) {
    DynamicJsonDocument doc(1536);
    IrCode& code = irCodes[i];
    doc["id"] = code.id;
    doc["name"] = code.name;
    doc["isRaw"] = code.isRaw;
    if (!code.isRaw) {
      doc["protocol"] = (int)code.protocol;
      doc["value"] = (uint32_t)code.value;
      doc["bits"] = code.bits;
    } else {
      doc["raw"] = rawToString(code.rawbuf);
    }
    String json;
    serializeJson(doc, json);
    prefs.putString(("ir_" + String(i)).c_str(), json);
  }
  prefs.end();
}

void loadSchedules() {
  prefs.begin(PREF_NAMESPACE, false);
  int count = prefs.getInt("schedCount", 0);
  schedules.clear();
  nextScheduleId = 1;
  for (int i = 0; i < count; i++) {
    String key = "sched_" + String(i);
    String json = prefs.getString(key.c_str(), "");
    if (json.length() == 0) continue;
    
    DynamicJsonDocument doc(256);
    deserializeJson(doc, json);
    ScheduleItem s;
    s.id = doc["id"];
    s.codeId = doc["codeId"];
    s.hour = doc["hour"];
    s.minute = doc["minute"];
    s.dayMask = doc["dayMask"];
    s.enabled = doc["enabled"];
    schedules.push_back(s);
    if (s.id >= nextScheduleId) nextScheduleId = s.id + 1;
  }
  prefs.end();
}

void saveSchedules() {
  prefs.begin(PREF_NAMESPACE, false);
  prefs.putInt("schedCount", schedules.size());
  for (size_t i = 0; i < schedules.size(); i++) {
    DynamicJsonDocument doc(256);
    ScheduleItem& s = schedules[i];
    doc["id"] = s.id;
    doc["codeId"] = s.codeId;
    doc["hour"] = s.hour;
    doc["minute"] = s.minute;
    doc["dayMask"] = s.dayMask;
    doc["enabled"] = s.enabled;
    String json;
    serializeJson(doc, json);
    prefs.putString(("sched_" + String(i)).c_str(), json);
  }
  prefs.end();
}

void appendLog(String entry) {
  executionLog[logIndex] = entry;
  logIndex = (logIndex + 1) % LOG_SIZE;
  Serial.println("[LOG] " + entry);
}

// ---------- IR Learning ----------
bool learnIrCode(String name, bool wantRaw, IrCode &outCode) {
  Serial.printf("Learning %s mode, point remote...\n", wantRaw ? "RAW" : "DECODED");
  blinkLED(2, 100);
  irrecv.enableIRIn();
  unsigned long start = millis();
  while (millis() - start < 10000) {
    if (irrecv.decode(&irResults)) {
      outCode.id = nextCodeId++;
      outCode.name = name;
      
      if (!wantRaw && irResults.decode_type != decode_type_t::UNKNOWN) {
        outCode.isRaw = false;
        outCode.protocol = irResults.decode_type;
        outCode.value = irResults.value;
        outCode.bits = irResults.bits;
        appendLog("Learned DECODED: " + name);
        irrecv.disableIRIn();
        blinkLED(1, 200);
        return true;
      } else {
        outCode.isRaw = true;
        uint16_t rawLen = irResults.rawlen;
        if (rawLen > 0 && rawLen < 500) {
          outCode.rawbuf.clear();
          for (uint16_t i = 0; i < rawLen; i++) {
            outCode.rawbuf.push_back((uint16_t)irResults.rawbuf[i]);
          }
          appendLog("Learned RAW: " + name + " (len=" + String(rawLen) + ")");
          irrecv.disableIRIn();
          blinkLED(1, 200);
          return true;
        }
      }
      irrecv.resume();
      continue;
    }
    vTaskDelay(pdMS_TO_TICKS(10)); // Yield to prevent crash
  }
  irrecv.disableIRIn();
  blinkLED(3, 100);
  return false;
}

void sendIrCode(const IrCode &code) {
  Serial.printf("Sending IR: %s\n", code.name.c_str());
  if (!code.isRaw) {
    irsend.send(code.protocol, code.value, code.bits);
  } else {
    if (code.rawbuf.size() > 0) {
      irsend.sendRaw(code.rawbuf.data(), code.rawbuf.size(), 38);
    } else {
      Serial.println("Error: empty raw buffer");
      return;
    }
  }
  blinkLED(1, 50);
  appendLog("Sent: " + code.name);
}

// ---------- Schedule execution ----------
bool isDayMatch(uint8_t dayMask, DateTime now) {
  if (dayMask == 0xFF) return true;
  int wday = now.dayOfTheWeek();
  int bitPos = (wday == 0) ? 6 : wday - 1;
  return (dayMask >> bitPos) & 1;
}

void checkAndExecuteSchedules(DateTime now) {
  if (receiverMode) return;
  int currentMinute = now.hour() * 60 + now.minute();
  if (currentMinute == lastLoggedMinute) return;
  lastLoggedMinute = currentMinute;
  
  for (auto &sched : schedules) {
    //if (!sched.enabled) continue;
    int schedMinute = sched.hour * 60 + sched.minute;
    if (schedMinute == currentMinute && isDayMatch(sched.dayMask, now)) {
      for (auto &code : irCodes) {
        if (code.id == sched.codeId) {
          sendIrCode(code);
          appendLog("Auto exec: " + code.name);
          break;
        }
      }
    }
  }
}

// ---------- Web Server API ----------
String getCurrentTimeJson() {
  DateTime now = rtc.now();
  DynamicJsonDocument doc(128);
  doc["year"] = now.year();
  doc["month"] = now.month();
  doc["day"] = now.day();
  doc["hour"] = now.hour();
  doc["minute"] = now.minute();
  doc["second"] = now.second();
  doc["epoch"] = now.unixtime();
  String out;
  serializeJson(doc, out);
  return out;
}

String getAllDataJson() {
  DynamicJsonDocument doc(8192); // Adjusted for stability
  JsonArray codesArr = doc.createNestedArray("irCodes");
  for (auto &c : irCodes) {
    JsonObject obj = codesArr.createNestedObject();
    obj["id"] = c.id;
    obj["name"] = c.name;
    obj["isRaw"] = c.isRaw;
    if (!c.isRaw) {
      obj["protocol"] = (int)c.protocol;
      obj["value"] = (uint32_t)c.value;
      obj["bits"] = c.bits;
    } else {
      obj["rawLen"] = (int)c.rawbuf.size();
    }
  }
  
  JsonArray schedArr = doc.createNestedArray("schedules");
  for (auto &s : schedules) {
    JsonObject obj = schedArr.createNestedObject();
    obj["id"] = s.id;
    obj["codeId"] = s.codeId;
    obj["hour"] = s.hour;
    obj["minute"] = s.minute;
    obj["dayMask"] = s.dayMask;
    obj["enabled"] = s.enabled;
  }
  
  JsonArray logArr = doc.createNestedArray("log");
  for (int i = 0; i < LOG_SIZE; i++) {
    int idx = (logIndex + i) % LOG_SIZE;
    if (executionLog[idx].length()) logArr.add(executionLog[idx]);
  }
  
  String out;
  serializeJson(doc, out);
  return out;
}

// HTML Page with Delete, Manual Time, and Dropdown State Fix
const char HTML_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>IR Scheduler Pro v3.1</title>
    <style>
        * { box-sizing: border-box; font-family: 'Segoe UI', Roboto, sans-serif; }
        body { background: linear-gradient(145deg, #0a0f1e 0%, #0c1222 100%); margin: 0; padding: 20px; color: #eef4ff; }
        .glass { background: rgba(255,255,255,0.08); backdrop-filter: blur(12px); border-radius: 32px; padding: 24px; margin-bottom: 24px; border: 1px solid rgba(255,255,255,0.2); }
        .time-card { font-size: 2.5rem; font-weight: 600; font-family: monospace; background: #00000044; border-radius: 60px; padding: 12px 24px; display: inline-block; margin-bottom: 15px;}
        button, .btn { background: #2a6df0; border: none; padding: 10px 20px; border-radius: 40px; color: white; font-weight: bold; cursor: pointer; margin: 4px; transition: 0.2s; }
        button:hover { background: #1e54b8; transform: scale(1.02); }
        .raw-btn { background: #9c4dcc; }
        .delete-btn { background: #d32f2f; }
        input, select { background: #1e2a3a; border: 1px solid #3a4a5a; padding: 10px 14px; border-radius: 28px; color: white; margin: 6px 0; width: 100%; max-width: 300px; }
        input[type="datetime-local"] { max-width: 240px; display: inline-block; }
        table { width: 100%; border-collapse: collapse; background: rgba(0,0,0,0.3); border-radius: 24px; overflow: hidden; margin-top: 10px;}
        th, td { padding: 12px 10px; text-align: left; border-bottom: 1px solid #2a3a4a; }
        .badge-raw { background: #aa2e6b; padding: 4px 12px; border-radius: 20px; font-size: 0.75rem; display: inline-block; }
        .badge-dec { background: #2a6df0; padding: 4px 12px; border-radius: 20px; font-size: 0.75rem; display: inline-block; }
        .flex { display: flex; flex-wrap: wrap; gap: 12px; align-items: center; }
        .time-controls { display: flex; flex-wrap: wrap; gap: 10px; justify-content: center; align-items: center; background: rgba(0,0,0,0.2); padding: 15px; border-radius: 20px;}
        @media (max-width: 640px) { .glass { padding: 16px; } th, td { font-size: 12px; } }
    </style>
</head>
<body>
<div class="glass" style="text-align:center">
    <h1>IR Scheduler Pro</h1>
    <div class="time-card" id="liveClock">--:--:--</div>
    
    <div class="time-controls">
        <button id="syncTimeBtn">Sync Browser Time</button>
        <span style="color:#888;">|</span>
        <input type="datetime-local" id="manualTimeInput">
        <button id="setManualTimeBtn">Set Manual Time</button>
    </div>
</div>
<div class="glass">
    <h2>IR Code Manager</h2>
    <div class="flex">
        <input type="text" id="newIrName" placeholder="Code name (e.g., TV Power)">
        <button id="learnDecBtn">Learn Decoded</button>
        <button id="learnRawBtn" class="raw-btn">Learn Raw</button>
    </div>
    <table id="irTable">
        <thead><tr><th>ID</th><th>Name</th><th>Type</th><th>Details</th><th>Actions</th></tr></thead>
        <tbody></tbody>
    </table>
</div>
<div class="glass">
    <h2>Schedules</h2>
    <div class="flex">
        <select id="schedCodeSelect"></select>
        <input type="number" id="schedHour" placeholder="Hour (0-23)" min="0" max="23">
        <input type="number" id="schedMinute" placeholder="Minute (0-59)" min="0" max="59">
        <select id="schedDays">
            <option value="255">Daily</option>
            <option value="1">Monday only</option>
            <option value="2">Tuesday only</option>
            <option value="4">Wednesday only</option>
            <option value="8">Thursday only</option>
            <option value="16">Friday only</option>
            <option value="32">Saturday only</option>
            <option value="64">Sunday only</option>
        </select>
        <button id="addScheduleBtn">Add Schedule</button>
    </div>
    <table id="schedTable">
        <thead><tr><th>Time</th><th>IR Code</th><th>Days</th><th>Action</th></tr></thead>
        <tbody></tbody>
    </table>
</div>
<div class="glass">
    <h2>Execution Log</h2>
    <div id="logArea" style="font-family:monospace; font-size:0.9rem; max-height:200px; overflow-y:auto;"></div>
</div>
<script>
    let irCodes = [], schedules = [];
    
    async function fetchData() {
        try {
            const res = await fetch('/api/data');
            const data = await res.json();
            irCodes = data.irCodes || [];
            schedules = data.schedules || [];
            renderIrTable();
            renderSchedules();
            renderLog(data.log);
            populateCodeSelect();
        } catch(e) { console.error('Fetch error:', e); }
    }
    
    function renderIrTable() {
        let html = '';
        for(let c of irCodes) {
            let typeBadge = c.isRaw ? `<span class="badge-raw">RAW (${c.rawLen||0})</span>` : `<span class="badge-dec">DEC</span>`;
            let details = c.isRaw ? `Len: ${c.rawLen||0}` : `Proto: ${c.protocol}`;
            html += `<tr>
                        <td>${c.id}</td>
                        <td>${escapeHtml(c.name)}</td>
                        <td>${typeBadge}</td>
                        <td>${escapeHtml(details)}</td>
                        <td>
                            <button onclick="testIr(${c.id})" style="padding: 6px 12px; margin:2px;">Test</button>
                            <button class="delete-btn" onclick="delIr(${c.id})" style="padding: 6px 12px; margin:2px;">Del</button>
                        </td>
                     </tr>`;
        }
        document.querySelector('#irTable tbody').innerHTML = html;
    }
    
    function renderSchedules() {
        let html = '';
        for(let s of schedules) {
            let code = irCodes.find(c=>c.id===s.codeId);
            let codeName = code ? code.name : '[Deleted]';
            let days = s.dayMask===255 ? 'Daily' : ['Mon','Tue','Wed','Thu','Fri','Sat','Sun'].filter((_,i)=> (s.dayMask>>i)&1).join(',');
            html += `<tr>
                        <td>${pad(s.hour)}:${pad(s.minute)}</td>
                        <td>${escapeHtml(codeName)}</td>
                        <td>${days}</td>
                        <td><button class="delete-btn" onclick="delSched(${s.id})">Del</button></td>
                     </tr>`;
        }
        document.querySelector('#schedTable tbody').innerHTML = html;
    }
    
    function populateCodeSelect() {
        let selectEl = document.getElementById('schedCodeSelect');
        // FIX: Save the user's current selection before rebuilding the list
        let currentSelection = selectEl.value; 
        
        let opts = '<option value="">Select IR Code</option>';
        for(let c of irCodes) {
            opts += `<option value="${c.id}">${escapeHtml(c.name)}</option>`;
        }
        selectEl.innerHTML = opts;
        
        // FIX: Restore the selection so it doesn't get wiped out every 3 seconds
        if (currentSelection) {
            selectEl.value = currentSelection;
        }
    }
    
    function renderLog(logArr) {
        let logDiv = document.getElementById('logArea');
        if(logArr && logArr.length) {
            logDiv.innerHTML = logArr.map(l=>`<div>Log- ${escapeHtml(l)}</div>`).reverse().join('');
        } else {
            logDiv.innerHTML = '<div>No events yet</div>';
        }
    }
    
    async function testIr(codeId) {
        let fd = new URLSearchParams();
        fd.append('codeId', codeId);
        await fetch('/api/send_test', {method:'POST', body:fd});
        fetchData();
    }

    async function delIr(id) {
        if(confirm('Delete this IR Code? Any schedules using this code will also be deleted.')) {
            let fd = new URLSearchParams();
            fd.append('id', id);
            await fetch('/api/delete_ir', {method:'POST', body:fd});
            fetchData();
        }
    }
    
    async function delSched(id) {
        if(confirm('Delete this schedule?')) {
            let fd = new URLSearchParams();
            fd.append('id', id);
            await fetch('/api/delete_schedule', {method:'POST', body:fd});
            fetchData();
        }
    }
    
    async function learn(name, rawMode) {
        if(!name.trim()) { alert("Enter a name for the IR code"); return; }
        let fd = new URLSearchParams();
        fd.append('name', name);
        fd.append('rawMode', rawMode);
        alert(rawMode ? "RAW mode - press remote button now (10s timeout)" : "DECODED mode - press remote button now");
        let res = await fetch('/api/learn', {method:'POST', body:fd});
        if(res.ok) {
            document.getElementById('newIrName').value = '';
            fetchData();
        } else {
            let txt = await res.text();
            alert("Learning failed: " + txt);
        }
    }
    
    document.getElementById('learnDecBtn').onclick = () => learn(document.getElementById('newIrName').value, false);
    document.getElementById('learnRawBtn').onclick = () => learn(document.getElementById('newIrName').value, true);
    
    document.getElementById('addScheduleBtn').onclick = async () => {
        let codeId = document.getElementById('schedCodeSelect').value;
        let hour = parseInt(document.getElementById('schedHour').value);
        let minute = parseInt(document.getElementById('schedMinute').value);
        let dayMask = parseInt(document.getElementById('schedDays').value);
        
        // Ensure values are picked correctly
        if(!codeId || isNaN(hour) || isNaN(minute) || hour<0 || hour>23 || minute<0 || minute>59) { 
            alert("Invalid input parameters. Please check the code, hour, and minute."); return; 
        }
        
        let payload = { codeId: parseInt(codeId), hour, minute, dayMask, enabled: true };
        let fd = new URLSearchParams();
        fd.append('json', JSON.stringify(payload));
        await fetch('/api/add_schedule', {method:'POST', body:fd});
        
        // Reset inputs after adding
        document.getElementById('schedHour').value = '';
        document.getElementById('schedMinute').value = '';
        document.getElementById('schedCodeSelect').value = '';
        fetchData();
    };

 document.getElementById('syncTimeBtn').onclick = async () => {
        let d = new Date();
        // Convert local time components into a raw epoch without timezone shifting
        let localEpoch = Math.floor(Date.UTC(d.getFullYear(), d.getMonth(), d.getDate(), d.getHours(), d.getMinutes(), d.getSeconds()) / 1000);
        let fd = new URLSearchParams();
        fd.append('epoch', localEpoch);
        await fetch('/api/set_time', {method:'POST', body:fd});
        fetchData();
    };

document.getElementById('setManualTimeBtn').onclick = async () => {
        let val = document.getElementById('manualTimeInput').value;
        if(!val) { alert("Please select a date and time first."); return; }
        let d = new Date(val);
        // Do the same for manual time
        let localEpoch = Math.floor(Date.UTC(d.getFullYear(), d.getMonth(), d.getDate(), d.getHours(), d.getMinutes(), d.getSeconds()) / 1000);
        let fd = new URLSearchParams();
        fd.append('epoch', localEpoch);
        await fetch('/api/set_time', {method:'POST', body:fd});
        alert("RTC manually updated!");
        fetchData();
    };

    function pad(n){ return n<10 ? '0'+n : n; }
    
 function updateClock() {
        fetch('/api/time').then(r=>r.json()).then(data=>{
            // Display the exact internal components from the ESP to avoid browser timezone auto-correction
            let timeStr = `${data.year}-${pad(data.month)}-${pad(data.day)} ${pad(data.hour)}:${pad(data.minute)}:${pad(data.second)}`;
            document.getElementById('liveClock').innerText = timeStr;
        }).catch(e=>console.error('Clock error:',e));
    }
    
    function escapeHtml(str) { 
        if(!str) return '';
        return str.replace(/[&<>]/g, function(m){ 
            if(m==='&') return '&amp;'; if(m==='<') return '&lt;'; if(m==='>') return '&gt;'; 
            return m;
        });
    }
    
    setInterval(updateClock, 1000);
    setInterval(fetchData, 3000);
    fetchData();
    updateClock();
</script>
</body>
</html>
)rawliteral";

// ---------- Web Server Route Handlers ----------
void handleRoot() {
  server.send(200, "text/html", HTML_PAGE);
}

void handleGetTime() {
  server.send(200, "application/json", getCurrentTimeJson());
}

void handleSetTime() {
  if (server.hasArg("epoch")) {
    uint32_t epoch = server.arg("epoch").toInt();
    rtc.adjust(DateTime(epoch));
    appendLog("Time set via web interface");
    server.send(200, "text/plain", "OK");
  } else {
    server.send(400, "text/plain", "Missing epoch");
  }
}

void handleGetData() {
  server.send(200, "application/json", getAllDataJson());
}

void handleLearn() {
  if (server.hasArg("name") && server.hasArg("rawMode")) {
    String name = server.arg("name");
    bool rawMode = (server.arg("rawMode") == "true");
    IrCode newCode;
    
    if (learnIrCode(name, rawMode, newCode)) {
      irCodes.push_back(newCode);
      saveIrCodes();
      server.send(200, "application/json", "{\"success\":true, \"id\":" + String(newCode.id) + "}");
    } else {
      server.send(500, "application/json", "{\"success\":false, \"msg\":\"timeout or no signal\"}");
    }
  } else {
    server.send(400, "text/plain", "Missing name or rawMode");
  }
}

void handleSendTest() {
  if (server.hasArg("codeId")) {
    uint16_t cid = server.arg("codeId").toInt();
    for (auto &c : irCodes) {
      if (c.id == cid) {
        sendIrCode(c);
        server.send(200, "text/plain", "OK");
        return;
      }
    }
    server.send(404, "text/plain", "Code not found");
  } else {
    server.send(400, "text/plain", "Missing codeId");
  }
}

void handleDeleteIr() {
  if (server.hasArg("id")) {
    uint16_t cid = server.arg("id").toInt();
    
    // 1. Remove the IR code
    irCodes.erase(std::remove_if(irCodes.begin(), irCodes.end(),
      [cid](IrCode &c) { return c.id == cid; }), irCodes.end());
      
    // 2. Cascade delete: Remove any schedules attached to this code to prevent crashes
    schedules.erase(std::remove_if(schedules.begin(), schedules.end(),
      [cid](ScheduleItem &s) { return s.codeId == cid; }), schedules.end());
      
    saveIrCodes();
    saveSchedules();
    server.send(200, "text/plain", "OK");
  } else {
    server.send(400, "text/plain", "Missing id");
  }
}

void handleAddSchedule() {
  if (server.hasArg("json")) {
    String json = server.arg("json");
    DynamicJsonDocument doc(256);
    deserializeJson(doc, json);
    ScheduleItem s;
    s.id = nextScheduleId++;
    s.codeId = doc["codeId"];
    s.hour = doc["hour"];
    s.minute = doc["minute"];
    s.dayMask = doc["dayMask"];
    if (s.dayMask == 0) s.dayMask = 0xFF;
    s.enabled = doc["enabled"];
    schedules.push_back(s);
    saveSchedules();
    server.send(200, "text/plain", "OK");
  } else {
    server.send(400, "text/plain", "Missing json");
  }
}

void handleDeleteSchedule() {
  if (server.hasArg("id")) {
    uint16_t sid = server.arg("id").toInt();
    schedules.erase(std::remove_if(schedules.begin(), schedules.end(),
      [sid](ScheduleItem &s) { return s.id == sid; }), schedules.end());
    saveSchedules();
    server.send(200, "text/plain", "OK");
  } else {
    server.send(400, "text/plain", "Missing id");
  }
}

void setupWebServer() {
  server.on("/", handleRoot);
  server.on("/api/time", HTTP_GET, handleGetTime);
  server.on("/api/set_time", HTTP_POST, handleSetTime);
  server.on("/api/data", HTTP_GET, handleGetData);
  server.on("/api/learn", HTTP_POST, handleLearn);
  server.on("/api/send_test", HTTP_POST, handleSendTest);
  server.on("/api/delete_ir", HTTP_POST, handleDeleteIr);
  server.on("/api/add_schedule", HTTP_POST, handleAddSchedule);
  server.on("/api/delete_schedule", HTTP_POST, handleDeleteSchedule);
  
  server.begin();
}

// ---------- Mode handling ----------
void updateMode() {
  int switchState = digitalRead(MODE_SWITCH_PIN);
  bool newMode = (switchState == LOW);  // LOW = GND = Receiver Mode
  
  if (newMode == receiverMode) return;
  receiverMode = newMode;
  
  if (receiverMode) {
    WiFi.disconnect(true);
    vTaskDelay(pdMS_TO_TICKS(100));
    WiFi.mode(WIFI_AP);
    vTaskDelay(pdMS_TO_TICKS(100));
    
    if (strlen(ap_password) >= 8) {
        WiFi.softAP(ap_ssid, ap_password, 1, 0, 4);
    } else {
        WiFi.softAP(ap_ssid, NULL, 1, 0, 4);
    }

    // THERMAL FIX: Throttle the TX power of the Wi-Fi Radio.
    // Range is typically 8 to 84. ~40 (10dBm) is excellent for a room and runs cool.
    esp_wifi_set_max_tx_power(40); 

    setupWebServer();
    appendLog("Switched to RECEIVER mode");
    blinkLED(2, 100);
  } else {
    WiFi.mode(WIFI_OFF);
    server.stop();
    appendLog("Switched to AUTO mode - WiFi off");
    blinkLED(1, 200);
  }
}

// ---------- Setup ----------
void setup() {
  Serial.begin(115200);
  vTaskDelay(pdMS_TO_TICKS(1000));
  
  pinMode(IR_LED_PIN, OUTPUT);
  pinMode(MODE_SWITCH_PIN, INPUT_PULLUP);
  pinMode(BUILTIN_LED, OUTPUT);
  digitalWrite(BUILTIN_LED, HIGH);

  irsend.begin();
  irrecv.enableIRIn();
  
  Wire.begin(I2C_SDA, I2C_SCL);
  if (!rtc.begin()) {
    while (1) {
      blinkLED(5, 100);
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
  }
  
  if (rtc.lostPower()) rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));

  loadIrCodes();
  loadSchedules();
  
  int switchState = digitalRead(MODE_SWITCH_PIN);
  receiverMode = (switchState == LOW);  
  
  if (receiverMode) {
    WiFi.disconnect(true);
    vTaskDelay(pdMS_TO_TICKS(100));
    WiFi.mode(WIFI_AP);
    vTaskDelay(pdMS_TO_TICKS(100));
    
    if (strlen(ap_password) >= 8) WiFi.softAP(ap_ssid, ap_password, 1, 0, 4);
    else WiFi.softAP(ap_ssid, NULL, 1, 0, 4);
    
    esp_wifi_set_max_tx_power(40); // Heat reduction protocol applied here too
    setupWebServer();
  } else {
    WiFi.mode(WIFI_OFF);
  }
  
  blinkLED(3, 100);
}

// ---------- Main loop ----------
void loop() {
  if (receiverMode) {
    server.handleClient();
  }
  
  if (millis() - lastModeCheck > 500) {
    updateMode();
    lastModeCheck = millis();
  }
  
  if (millis() - lastSecondTick >= 1000) {
    DateTime now = rtc.now();
    checkAndExecuteSchedules(now);
    lastSecondTick = millis();
  }
  
  // CRITICAL FIX: vTaskDelay passes idle time back to FreeRTOS.
  // Standard delay() keeps the CPU spinning internally on some ESP cores.
  vTaskDelay(pdMS_TO_TICKS(50)); 
}