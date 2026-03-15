#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <SD.h>
#include <SPI.h>
#include <Wire.h>
#include "Adafruit_VL53L0X.h"

// --- НОВІ АУДІО БІБЛІОТЕКИ ---
#include "AudioFileSourceSD.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2S.h"

// --- Піни ---
#define SD_CS      5
#define SPI_MOSI   23
#define SPI_MISO   19
#define SPI_SCK    18
#define I2S_BCLK   26  
#define I2S_LRC    25  
#define I2S_DOUT   27  
#define I2C_SDA    21
#define I2C_SCL    22
#define BUTTON_PIN 0

AsyncWebServer server(80);
DNSServer dnsServer;
Preferences prefs;
Adafruit_VL53L0X lox = Adafruit_VL53L0X();

// --- АУДІО ОБ'ЄКТИ ---
AudioGeneratorMP3 *mp3 = nullptr;
AudioFileSourceSD *file = nullptr;
AudioOutputI2S *out = nullptr;

bool isConfigured = false;
String ssid, password, webUser, webPass;
String activeTrack = "SEQ";
int triggerDistance = 1000;
int volumeLevel = 15;
std::vector<String> fileCache;
unsigned long buttonPressTime = 0;
bool buttonActive = false;
File uploadFile;
const byte DNS_PORT = 53;
int currentPlaylistIndex = -1;

// Діагностика
bool sd_ok = false;
bool tof_ok = false;
std::vector<String> sysLogs;

void addLog(String msg) {
  String safeMsg = msg;
  safeMsg.replace("\"", "'"); 
  safeMsg.replace("\\", "/");
  String timeStr = "[" + String(millis() / 1000) + "s] ";
  sysLogs.push_back(timeStr + safeMsg);
  if (sysLogs.size() > 15) sysLogs.erase(sysLogs.begin());
  Serial.println(msg);
}

// ==========================================
// КЕРУВАННЯ АУДІО
// ==========================================
void stopAudio() {
  if (mp3 && mp3->isRunning()) {
    mp3->stop();
  }
  if (file) {
    file->close();
    delete file;
    file = nullptr;
  }
}

void playAudio(String path) {
  stopAudio();
  addLog("Запуск треку: " + path);
  file = new AudioFileSourceSD(path.c_str());
  if (!mp3) mp3 = new AudioGeneratorMP3();
  mp3->begin(file, out);
}

void setAudioVolume(int vol) {
  if (out) {
    // Конвертуємо нашу шкалу 0-21 у шкалу 0.0 - 1.0 для нової бібліотеки
    float gain = (float)vol / 21.0;
    out->SetGain(gain);
  }
}

// ==========================================
// 1. СТОРІНКА НАЛАШТУВАННЯ (CAPTIVE PORTAL)
// ==========================================
const char setup_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Налаштування StoryPointYK</title>
<style>
  :root { --primary: #007AFF; --bg: #F2F2F7; --card: #FFFFFF; --text: #1C1C1E; --muted: #8E8E93; --border: #E5E5EA; }
  body { font-family: -apple-system, sans-serif; background: var(--bg); color: var(--text); display: flex; justify-content: center; align-items: center; min-height: 100vh; margin: 0; padding: 20px; box-sizing: border-box; }
  .card { background: var(--card); border-radius: 20px; box-shadow: 0 10px 30px rgba(0,0,0,0.08); padding: 30px; width: 100%; max-width: 420px; }
  h2 { margin-top: 0; text-align: center; font-size: 24px; font-weight: 600; }
  .subtitle { text-align: center; color: var(--muted); font-size: 14px; margin-bottom: 25px; }
  .form-group { margin-bottom: 20px; }
  label { display: block; font-size: 14px; font-weight: 500; margin-bottom: 8px; }
  input[type="text"], input[type="password"] { width: 100%; padding: 12px 15px; border: 1px solid var(--border); border-radius: 12px; font-size: 16px; box-sizing: border-box; background: #FAFAFA; }
  .radio-group { display: flex; gap: 10px; margin-bottom: 20px; background: #F3F3F5; padding: 5px; border-radius: 14px; }
  .radio-group label { flex: 1; text-align: center; padding: 10px; border-radius: 10px; cursor: pointer; font-size: 14px; font-weight: 600; margin: 0; transition: 0.2s; }
  .radio-group input { display: none; }
  .radio-group input:checked + label { background: #FFF; box-shadow: 0 2px 8px rgba(0,0,0,0.1); color: var(--primary); }
  button { width: 100%; padding: 14px; background: var(--primary); color: white; border: none; border-radius: 12px; font-size: 16px; font-weight: 600; cursor: pointer; margin-top: 10px; }
</style>
</head><body>
<div class="card">
  <h2>StoryPointYK Setup</h2>
  <div class="subtitle">Оберіть режим роботи пристрою</div>
  <form action="/setup" method="POST">
    <div class="radio-group">
      <input type="radio" id="m_wifi" name="mode" value="wifi" checked onchange="toggleMode()">
      <label for="m_wifi">Домашній Wi-Fi</label>
      <input type="radio" id="m_auto" name="mode" value="auto" onchange="toggleMode()">
      <label for="m_auto">Автономно</label>
    </div>
    <div id="wifi-fields">
      <div class="form-group"><label>Назва мережі (SSID)</label><input type="text" name="ssid"></div>
      <div class="form-group"><label>Пароль Wi-Fi</label><input type="password" name="pass"></div>
    </div>
    <div style="font-size:12px; color:#8E8E93; margin:20px 0 10px; text-transform:uppercase; font-weight:bold;">Доступ та Безпека</div>
    <p style="font-size:13px; color:var(--muted); margin-top:0;">Цей пароль буде використовуватися для входу в веб-інтерфейс, а також <b>стане паролем від Wi-Fi точки StoryPointYK</b>.</p>
    <div class="form-group"><label>Логін</label><input type="text" name="w_user" required value="admin"></div>
    <div class="form-group"><label>Спільний пароль (мінімум 8 символів)</label><input type="password" name="w_pass" required minlength="8" value="admin123"></div>
    <button type="submit">Зберегти та запустити</button>
  </form>
</div>
<script>
  function toggleMode() {
    let mode = document.querySelector('input[name="mode"]:checked').value;
    document.getElementById('wifi-fields').style.display = (mode === 'wifi') ? 'block' : 'none';
  }
</script>
</body></html>
)rawliteral";

// ==========================================
// 2. ГОЛОВНА СТОРІНКА (DASHBOARD)
// ==========================================
const char main_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>StoryPointYK Dashboard</title>
<style>
  :root { --primary: #007AFF; --bg: #F2F2F7; --card: #FFFFFF; --text: #1C1C1E; --muted: #8E8E93; --border: #E5E5EA; --success: #34C759; --danger: #FF3B30; --warn: #FF9500; }
  body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; background: var(--bg); color: var(--text); margin: 0; padding: 15px; padding-bottom: 50px; }
  .container { max-width: 600px; margin: 0 auto; }
  .header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 20px; }
  h1 { font-size: 24px; font-weight: 700; margin: 0; }
  .badge { background: #E5F0FF; color: var(--primary); padding: 5px 10px; border-radius: 8px; font-size: 13px; font-weight: 600; }
  
  .card { background: var(--card); border-radius: 16px; padding: 20px; margin-bottom: 20px; box-shadow: 0 2px 10px rgba(0,0,0,0.03); border: 1px solid rgba(0,0,0,0.05); }
  .card-title { font-size: 16px; font-weight: 600; margin: 0 0 15px 0; color: var(--muted); text-transform: uppercase; letter-spacing: 0.5px; }
  
  .stats-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; }
  .stat-box { background: #FAFAFA; padding: 12px; border-radius: 12px; border: 1px solid var(--border); display: flex; flex-direction: column; text-align: center; }
  .stat-lbl { font-size: 12px; color: var(--muted); margin-bottom: 4px; }
  .stat-val { font-size: 15px; font-weight: 700; color: var(--text); }

  .control-group { margin-bottom: 18px; }
  .control-group:last-child { margin-bottom: 0; }
  .control-header { display: flex; justify-content: space-between; font-size: 14px; font-weight: 500; margin-bottom: 8px; }
  .val-badge { color: var(--primary); font-weight: 600; background: #F3F3F5; padding: 2px 8px; border-radius: 6px; }
  
  input[type="range"] { -webkit-appearance: none; width: 100%; height: 6px; background: var(--border); border-radius: 3px; outline: none; margin: 10px 0; }
  input[type="range"]::-webkit-slider-thumb { -webkit-appearance: none; width: 22px; height: 22px; background: #fff; border-radius: 50%; box-shadow: 0 2px 5px rgba(0,0,0,0.2); cursor: pointer; border: 1px solid #E5E5EA; }
  select { width: 100%; padding: 12px; border: 1px solid var(--border); border-radius: 10px; font-size: 15px; background: #FAFAFA; outline: none; color: var(--text); font-weight: 500; }

  .file-area { border: 2px dashed var(--primary); border-radius: 12px; padding: 25px 15px; text-align: center; background: rgba(0,122,255,0.02); margin-bottom: 15px; position: relative; transition: 0.2s; }
  input[type="file"] { opacity: 0; position: absolute; top: 0; left: 0; width: 100%; height: 100%; cursor: pointer; }
  .file-lbl { font-weight: 600; color: var(--primary); font-size: 15px; display: block; }
  .file-name { display: block; margin-top: 5px; font-size: 13px; color: var(--text); font-weight: 500; }
  
  .btn { width: 100%; padding: 14px; border: none; border-radius: 12px; font-size: 15px; font-weight: 600; cursor: pointer; transition: 0.2s; display: flex; justify-content: center; align-items: center; gap: 8px; }
  .btn-upload { background: var(--success); color: white; display: none; }
  .btn-danger { background: rgba(255,59,48,0.1); color: var(--danger); }
  .btn-warn { background: rgba(255,149,0,0.1); color: var(--warn); }
  
  .file-list { margin-top: 15px; }
  .file-item { display: flex; justify-content: space-between; align-items: center; padding: 12px; background: #FAFAFA; border: 1px solid var(--border); border-radius: 10px; margin-bottom: 8px; font-size: 14px; font-weight: 500; }
  .del-btn { background: none; border: none; color: var(--danger); font-size: 18px; cursor: pointer; padding: 0 5px; }
  
  .sys-controls { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; }
  .log-box { background: #1C1C1E; color: #34C759; font-family: monospace; font-size: 12px; padding: 12px; border-radius: 12px; height: 160px; overflow-y: auto; margin-top: 15px; text-align: left; line-height: 1.5; }
  #toast { visibility: hidden; background: #333; color: #fff; text-align: center; border-radius: 8px; padding: 12px 20px; position: fixed; left: 50%; bottom: 30px; transform: translateX(-50%); font-size: 14px; font-weight: 500; opacity: 0; transition: 0.3s; z-index: 100; box-shadow: 0 4px 12px rgba(0,0,0,0.15); }
  #toast.show { visibility: visible; opacity: 1; bottom: 50px; }
</style>
</head><body>

<div class="container">
  <div class="header">
    <h1>StoryPointYK</h1>
    <div class="badge" id="uptime">--:--</div>
  </div>
  
  <div class="card">
    <div class="card-title">Діагностика</div>
    <div class="stats-grid">
      <div class="stat-box"><span class="stat-lbl">SD Карта</span><span class="stat-val" id="sdState">--</span></div>
      <div class="stat-box"><span class="stat-lbl">TOF Датчик</span><span class="stat-val" id="tofState">--</span></div>
      <div class="stat-box" style="grid-column: span 2;"><span class="stat-lbl">Аудіо</span><span class="stat-val" id="audioState">--</span></div>
    </div>
    <div class="log-box" id="logConsole">Очікування логів...</div>
  </div>
  
  <div class="card">
    <div class="card-title">Налаштування відтворення</div>
    <div class="control-group">
      <div class="control-header"><span>Що грати при спрацюванні:</span></div>
      <select id="trackSelect" onchange="saveSettings()">
        <optgroup label="Плейлисти">
          <option value="SEQ">🔁 Всі файли (По черзі)</option>
          <option value="RND">🔀 Всі файли (Випадково)</option>
        </optgroup>
        <optgroup label="Окремі файли" id="fileOptions"></optgroup>
      </select>
    </div>
    <div class="control-group">
      <div class="control-header"><span>Гучність</span><span class="val-badge" id="volVal">15</span></div>
      <input type="range" id="volume" min="0" max="21" onchange="saveSettings()" oninput="document.getElementById('volVal').innerText=this.value">
    </div>
    <div class="control-group">
      <div class="control-header"><span>Дистанція датчика</span><span class="val-badge" id="distVal">1000 мм</span></div>
      <input type="range" id="distance" min="50" max="2000" step="50" onchange="saveSettings()" oninput="document.getElementById('distVal').innerText=this.value+' мм'">
    </div>
  </div>

  <div class="card">
    <div class="card-title">Менеджер файлів (SD)</div>
    <div class="file-area" id="dropZone">
      <span class="file-lbl">📁 Натисніть, щоб обрати MP3/WAV</span>
      <span class="file-name" id="fileNameDisp">Файл не вибрано</span>
      <input type="file" id="fileInput" accept=".mp3,.wav" onchange="handleFileSelect()">
    </div>
    <button class="btn btn-upload" id="uploadBtn" onclick="uploadFile()">⬆️ Завантажити на пристрій</button>
    <div class="file-list" id="filesListContainer"></div>
  </div>

  <div class="card">
    <div class="card-title" style="color: var(--danger);">Система</div>
    <div class="sys-controls">
      <button class="btn btn-warn" onclick="if(confirm('Перезавантажити пристрій?')) fetch('/reboot', {method:'POST'});">🔄 Рестарт</button>
      <button class="btn btn-danger" onclick="if(confirm('Увага! Всі налаштування мережі будуть стерті. Продовжити?')) fetch('/reset', {method:'POST'});">⚠️ Скинути</button>
    </div>
  </div>
</div>

<div id="toast">Повідомлення</div>

<script>
  function showToast(msg) {
    let t = document.getElementById("toast"); t.innerText = msg; t.className = "show";
    setTimeout(()=> t.className = t.className.replace("show", ""), 3000);
  }

  function formatTime(sec) {
    let h = Math.floor(sec / 3600); let m = Math.floor((sec % 3600) / 60);
    if(h > 0) return h + "г " + m + "хв"; return m + "хв " + (sec % 60) + "с";
  }

  function handleFileSelect() {
    let file = document.getElementById('fileInput').files[0];
    let disp = document.getElementById('fileNameDisp');
    let btn = document.getElementById('uploadBtn');
    let area = document.getElementById('dropZone');
    
    if(file) {
      disp.innerText = file.name; disp.style.color = 'var(--primary)';
      btn.style.display = 'flex'; area.style.borderColor = 'var(--success)';
    } else {
      disp.innerText = 'Файл не вибрано'; disp.style.color = 'var(--text)';
      btn.style.display = 'none'; area.style.borderColor = 'var(--primary)';
    }
  }

  function loadData() {
    fetch('/api/data').then(r=>r.json()).then(d=>{
      document.getElementById('uptime').innerText = formatTime(d.up);
      
      let sd = document.getElementById('sdState');
      sd.innerText = d.sd_ok ? "✅ Підключено" : "❌ Помилка";
      sd.style.color = d.sd_ok ? "var(--success)" : "var(--danger)";
      
      let tof = document.getElementById('tofState');
      tof.innerText = d.tof_ok ? "✅ Працює" : "❌ Помилка";
      tof.style.color = d.tof_ok ? "var(--success)" : "var(--danger)";
      
      let aud = document.getElementById('audioState');
      if (d.playing) {
          aud.innerText = "🔊 Відтворюється трек...";
          aud.style.color = "var(--primary)";
      } else {
          aud.innerText = "⏸️ Тиша (Очікування)";
          aud.style.color = "var(--text)";
      }

      let logBox = document.getElementById('logConsole');
      let isScrolledToBottom = logBox.scrollHeight - logBox.clientHeight <= logBox.scrollTop + 1;
      logBox.innerHTML = d.logs.join("<br>");
      if(isScrolledToBottom) logBox.scrollTop = logBox.scrollHeight;
      
      let v = document.getElementById('volume'); if(document.activeElement !== v) { v.value = d.vol; document.getElementById('volVal').innerText = d.vol; }
      let dist = document.getElementById('distance'); if(document.activeElement !== dist) { dist.value = d.dist; document.getElementById('distVal').innerText = d.dist + " мм"; }
      
      let sel = document.getElementById('trackSelect');
      let optGroup = document.getElementById('fileOptions');
      optGroup.innerHTML = '';
      let listCont = document.getElementById('filesListContainer');
      listCont.innerHTML = '';

      if(d.files.length === 0) listCont.innerHTML = '<div style="text-align:center; padding:10px; color:var(--muted); font-size:13px;">Немає файлів на SD-карті</div>';

      d.files.forEach(f => {
        let opt = document.createElement('option'); opt.value = f; opt.innerHTML = "🎵 " + f; optGroup.appendChild(opt);
        let item = document.createElement('div'); item.className = 'file-item';
        item.innerHTML = '<span>🎵 ' + f + '</span> <button class="del-btn" onclick="deleteFile(\'' + f + '\')">✕</button>';
        listCont.appendChild(item);
      });
      if(d.track) sel.value = d.track;
    });
  }

  function saveSettings() {
    let vol = document.getElementById('volume').value; let dist = document.getElementById('distance').value; let trk = document.getElementById('trackSelect').value;
    let fd = new FormData(); fd.append("vol", vol); fd.append("dist", dist); fd.append("track", trk);
    fetch('/api/settings', {method: 'POST', body: fd}).then(()=> showToast("✅ Налаштування збережено"));
  }

  function uploadFile() {
    let file = document.getElementById('fileInput').files[0];
    if(!file) return;
    document.getElementById('uploadBtn').innerHTML = "⏳ Завантаження...";
    document.getElementById('uploadBtn').style.opacity = "0.7";
    let fd = new FormData(); fd.append("file", file);
    fetch('/upload', {method: 'POST', body: fd}).then(()=> { 
      showToast("✅ Завантажено"); 
      document.getElementById('fileInput').value = "";
      document.getElementById('uploadBtn').innerHTML = "⬆️ Завантажити на пристрій";
      handleFileSelect(); loadData(); 
    }).catch(()=> {
      showToast("❌ Помилка");
      document.getElementById('uploadBtn').innerHTML = "⬆️ Завантажити на пристрій";
      document.getElementById('uploadBtn').style.opacity = "1";
    });
  }

  function deleteFile(fname) {
    if(confirm("Видалити " + fname + " ?")) {
      let fd = new FormData(); fd.append("filename", fname);
      fetch('/delete', {method: 'POST', body: fd}).then(()=> { showToast("🗑️ Видалено"); loadData(); });
    }
  }

  loadData(); setInterval(loadData, 2000); 
</script>
</body></html>
)rawliteral";

// ==========================================
// ЛОГІКА ДОДАТКУ
// ==========================================

void updateFileCache() {
  fileCache.clear();
  File root = SD.open("/");
  if (!root) { addLog("Помилка читання SD кореня"); return; }
  File file = root.openNextFile();
  while (file) {
    if (!file.isDirectory()) {
      String fname = String(file.name());
      if (fname.endsWith(".mp3") || fname.endsWith(".MP3") || fname.endsWith(".wav") || fname.endsWith(".WAV")) {
        fileCache.push_back(fname);
      }
    }
    file = root.openNextFile();
  }
  addLog("SD проскановано. Знайдено треків: " + String(fileCache.size()));
}

void handleUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
  if (!index) {
    addLog("Початок завантаження: " + filename);
    if (!filename.startsWith("/")) filename = "/" + filename;
    if (SD.exists(filename)) SD.remove(filename);
    uploadFile = SD.open(filename, FILE_WRITE);
  }
  if (uploadFile) uploadFile.write(data, len);
  if (final) {
    if (uploadFile) uploadFile.close();
    addLog("Файл успішно завантажено!");
    updateFileCache(); 
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  addLog("=== Запуск системи ===");

  prefs.begin("storyframe", false);
  isConfigured = prefs.getBool("conf", false);
  ssid = prefs.getString("ssid", "");
  password = prefs.getString("pass", "");
  webUser = prefs.getString("w_user", "admin");
  webPass = prefs.getString("w_pass", "admin123"); 
  triggerDistance = prefs.getInt("dist", 1000);
  volumeLevel = prefs.getInt("vol", 15);
  activeTrack = prefs.getString("track", "SEQ");

  // Ініціалізація SD
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, SD_CS);
  if (SD.begin(SD_CS)) {
    sd_ok = true;
    addLog("Модуль SD карти ініціалізовано.");
    updateFileCache();
  } else {
    sd_ok = false;
    addLog("ПОМИЛКА: SD карту не знайдено або пошкоджено!");
  }

  // Ініціалізація I2S (Звук)
  out = new AudioOutputI2S();
  out->SetPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  setAudioVolume(volumeLevel);
  addLog("Аудіо інтерфейс I2S налаштовано.");

  // Ініціалізація TOF (I2C)
  Wire.begin(I2C_SDA, I2C_SCL);
  if (lox.begin()) {
    tof_ok = true;
    addLog("Лазерний датчик TOF200C знайдено.");
  } else {
    tof_ok = false;
    addLog("ПОМИЛКА: TOF датчик не відповідає на шині I2C!");
  }

  if (!isConfigured) {
    WiFi.softAP("StoryPointYK");
    dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());

    server.on("/", HTTP_GET, [](AsyncWebServerRequest *req){ req->send_P(200, "text/html", setup_html); });
    server.onNotFound([](AsyncWebServerRequest *req){ req->redirect("http://" + WiFi.softAPIP().toString() + "/"); });

    server.on("/setup", HTTP_POST, [](AsyncWebServerRequest *req){
      String mode = "wifi";
      if(req->hasParam("mode", true)) mode = req->getParam("mode", true)->value();
      
      if(mode == "auto") { prefs.putString("ssid", ""); prefs.putString("pass", ""); } 
      else {
        if(req->hasParam("ssid", true)) prefs.putString("ssid", req->getParam("ssid", true)->value());
        if(req->hasParam("pass", true)) prefs.putString("pass", req->getParam("pass", true)->value());
      }
      if(req->hasParam("w_user", true)) prefs.putString("w_user", req->getParam("w_user", true)->value());
      if(req->hasParam("w_pass", true)) prefs.putString("w_pass", req->getParam("w_pass", true)->value());
      
      prefs.putBool("conf", true);
      req->send(200, "text/plain", "Saved. Restarting...");
      delay(1000); ESP.restart();
    });
  } else {
    const char* apPassword = (webPass.length() >= 8) ? webPass.c_str() : NULL;

    if (ssid == "") {
      WiFi.softAP("StoryPointYK", apPassword); 
      addLog("Мережа: Автономна точка StoryPointYK");
    } else {
      WiFi.setHostname("StoryPointYK"); 
      WiFi.begin(ssid.c_str(), password.c_str());
      int tries = 0;
      while (WiFi.status() != WL_CONNECTED && tries < 20) { delay(500); tries++; }
      
      if(WiFi.status() != WL_CONNECTED) {
        WiFi.softAP("StoryPointYK", apPassword);
        addLog("Немає Wi-Fi роутера. Створено автономну точку.");
      } else {
        addLog("Підключено до роутера. IP: " + WiFi.localIP().toString());
      }
    }

    server.on("/", HTTP_GET, [](AsyncWebServerRequest *req){
      if(!req->authenticate(webUser.c_str(), webPass.c_str())) return req->requestAuthentication();
      req->send_P(200, "text/html", main_html);
    });

    server.on("/api/data", HTTP_GET, [](AsyncWebServerRequest *req){
      if(!req->authenticate(webUser.c_str(), webPass.c_str())) return req->send(401);
      
      uint32_t sd_total = SD.totalBytes() / (1024 * 1024);
      uint32_t sd_used = SD.usedBytes() / (1024 * 1024);
      unsigned long uptime_sec = millis() / 1000;
      bool isPlaying = (mp3 && mp3->isRunning());

      String json = "{\"rssi\":" + String(WiFi.RSSI()) + ",\"heap\":" + String(ESP.getFreeHeap()) + 
                    ",\"up\":" + String(uptime_sec) + ",\"sd_t\":" + String(sd_total) + ",\"sd_u\":" + String(sd_used) +
                    ",\"vol\":" + String(volumeLevel) + ",\"dist\":" + String(triggerDistance) + 
                    ",\"track\":\"" + activeTrack + "\",\"tof_ok\":" + (tof_ok ? "true" : "false") + 
                    ",\"sd_ok\":" + (sd_ok ? "true" : "false") + ",\"playing\":" + (isPlaying ? "true" : "false") + 
                    ",\"logs\":[";
      
      for (size_t i = 0; i < sysLogs.size(); i++) {
        json += "\"" + sysLogs[i] + "\"";
        if (i < sysLogs.size() - 1) json += ",";
      }
      
      json += "],\"files\":[";
      for (size_t i = 0; i < fileCache.size(); i++) {
        json += "\"" + fileCache[i] + "\"";
        if (i < fileCache.size() - 1) json += ",";
      }
      json += "]}";
      req->send(200, "application/json", json);
    });

    server.on("/api/settings", HTTP_POST, [](AsyncWebServerRequest *req){
      if(!req->authenticate(webUser.c_str(), webPass.c_str())) return req->send(401);
      if(req->hasParam("vol", true)) { 
        volumeLevel = req->getParam("vol", true)->value().toInt(); 
        prefs.putInt("vol", volumeLevel); 
        setAudioVolume(volumeLevel); 
        addLog("Змінено гучність: " + String(volumeLevel));
      }
      if(req->hasParam("dist", true)) { 
        triggerDistance = req->getParam("dist", true)->value().toInt(); 
        prefs.putInt("dist", triggerDistance); 
        addLog("Змінено дистанцію датчика: " + String(triggerDistance));
      }
      if(req->hasParam("track", true)) { 
        activeTrack = req->getParam("track", true)->value(); 
        prefs.putString("track", activeTrack); 
        addLog("Змінено активний трек: " + activeTrack);
      }
      req->send(200);
    });

    server.on("/upload", HTTP_POST, [](AsyncWebServerRequest *req){ req->send(200); }, handleUpload);

    server.on("/delete", HTTP_POST, [](AsyncWebServerRequest *req){
      if(!req->authenticate(webUser.c_str(), webPass.c_str())) return req->send(401);
      if(req->hasParam("filename", true)) {
        String fname = "/" + req->getParam("filename", true)->value();
        if(SD.exists(fname)) { 
          SD.remove(fname); 
          addLog("Видалено файл: " + fname);
          updateFileCache(); 
        }
      }
      req->send(200);
    });

    server.on("/reboot", HTTP_POST, [](AsyncWebServerRequest *req){
      if(!req->authenticate(webUser.c_str(), webPass.c_str())) return req->send(401);
      req->send(200);
      addLog("Запит на перезавантаження...");
      delay(500); ESP.restart();
    });

    server.on("/reset", HTTP_POST, [](AsyncWebServerRequest *req){
      if(!req->authenticate(webUser.c_str(), webPass.c_str())) return req->send(401);
      req->send(200);
      prefs.clear();
      delay(500); ESP.restart();
    });
  }
  server.begin();
}

void loop() {
  // --- АУДІО ДВИГУН ---
  if (mp3 && mp3->isRunning()) {
    if (!mp3->loop()) {
      mp3->stop();
      addLog("Трек завершено.");
    }
  }

  // --- МЕРЕЖА ---
  if (!isConfigured) dnsServer.processNextRequest();

  // --- КНОПКА СКИНУТТЯ ---
  if (digitalRead(BUTTON_PIN) == LOW) {
    if (!buttonActive) { buttonPressTime = millis(); buttonActive = true; } 
    else if (millis() - buttonPressTime > 5000) {
      Serial.println("Скидання..."); prefs.clear(); delay(1000); ESP.restart();
    }
  } else { buttonActive = false; }

  // --- ДАТЧИК ВІДСТАНІ ---
  static unsigned long lastTof = 0;
  if (isConfigured && tof_ok && millis() - lastTof > 200) {
    lastTof = millis();
    VL53L0X_RangingMeasurementData_t measure;
    lox.rangingTest(&measure, false);

    if (measure.RangeStatus != 4) {
      if (measure.RangeMilliMeter < triggerDistance) {
        
        // Перевіряємо, чи ЗАРАЗ НЕ грає музика
        bool isPlaying = (mp3 && mp3->isRunning());
        
        if (!isPlaying && fileCache.size() > 0) {
          String pathToPlay = "";
          
          if (activeTrack == "SEQ") {
            currentPlaylistIndex++;
            if (currentPlaylistIndex >= fileCache.size()) currentPlaylistIndex = 0;
            pathToPlay = "/" + fileCache[currentPlaylistIndex];
          } else if (activeTrack == "RND") {
            currentPlaylistIndex = random(0, fileCache.size());
            pathToPlay = "/" + fileCache[currentPlaylistIndex];
          } else if (activeTrack != "") {
            pathToPlay = "/" + activeTrack;
          }
          
          if (pathToPlay != "") {
            addLog("Датчик: " + String(measure.RangeMilliMeter) + "мм");
            playAudio(pathToPlay);
          }
        }
      }
    }
  }
}