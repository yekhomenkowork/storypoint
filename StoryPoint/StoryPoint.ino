#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <SD.h>
#include <SPI.h>
#include <Wire.h>
#include <Update.h>
#include <time.h>
#include <sys/time.h>
#include <ESPmDNS.h>
#include "Adafruit_VL53L0X.h"

#include "AudioFileSourceSD.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2S.h"

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

const String FIRMWARE_VERSION = "v.0.3.1-beta";

AsyncWebServer server(80);
DNSServer dnsServer;
Preferences prefs;
Adafruit_VL53L0X lox = Adafruit_VL53L0X();

AudioGeneratorMP3 *mp3 = nullptr;
AudioFileSourceSD *file = nullptr;
AudioOutputI2S *out = nullptr;

bool isConfigured = false;
String ssid, password, webUser, webPass;
String activeTrack = "SEQ";
int triggerDistance = 1000;
int volumeLevel = 15;
int startHour = 0;
int endHour = 0;

// Мережеві налаштування
String mdnsName = "storypointyk";
bool useStaticIP = false;
String staticIP = "";
String staticGW = "";
String staticSN = "255.255.255.0";
String staticDNS = "";

bool systemArmed = true;
bool reqPlay = false;
bool reqStop = false;
bool reqToggleArm = false;

struct FileInfo {
  String name;
  size_t size;
};

std::vector<FileInfo> fileCache;
unsigned long buttonPressTime = 0;
bool buttonActive = false;
File uploadFile;
const byte DNS_PORT = 53;
int currentPlaylistIndex = -1;

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

IPAddress parseIP(String ipStr) {
  IPAddress ip;
  ip.fromString(ipStr);
  return ip;
}

void stopAudioSafe() {
  if (mp3 && mp3->isRunning()) {
    mp3->stop();
    addLog("Відтворення зупинено");
  }
  if (file) {
    file->close();
    delete file;
    file = nullptr;
  }
}

void playAudioSafe(String path) {
  stopAudioSafe();
  addLog("Запуск треку: " + path);
  file = new AudioFileSourceSD(path.c_str());
  if (!mp3) mp3 = new AudioGeneratorMP3();
  mp3->begin(file, out);
}

void triggerPlaybackSafe() {
  if (fileCache.size() == 0) return;
  String pathToPlay = "";
  
  if (activeTrack == "SEQ") {
    currentPlaylistIndex++;
    if (currentPlaylistIndex >= fileCache.size()) currentPlaylistIndex = 0;
    pathToPlay = "/" + fileCache[currentPlaylistIndex].name;
  } else if (activeTrack == "RND") {
    currentPlaylistIndex = random(0, fileCache.size());
    pathToPlay = "/" + fileCache[currentPlaylistIndex].name;
  } else if (activeTrack != "") {
    pathToPlay = "/" + activeTrack;
  }
  
  if (pathToPlay != "") {
    playAudioSafe(pathToPlay);
  }
}

void setAudioVolume(int vol) {
  if (out) {
    float gain = (float)vol / 21.0;
    out->SetGain(gain);
  }
}

bool isTimeActive() {
  if (startHour == endHour) return true;
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 0)) return true;
  int h = timeinfo.tm_hour;
  if (startHour < endHour) {
    return (h >= startHour && h < endHour);
  } else {
    return (h >= startHour || h < endHour);
  }
}

const char setup_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Налаштування StoryPointYK</title>
<style>
  :root { --bg: #0B0E14; --card: #131720; --text: #E2E8F0; --primary: #3B82F6; --border: #1E222D; }
  body { font-family: 'Segoe UI', sans-serif; background: var(--bg); color: var(--text); display: flex; justify-content: center; align-items: center; min-height: 100vh; margin: 0; padding: 20px; box-sizing: border-box; }
  .card { background: var(--card); border: 1px solid var(--border); border-radius: 12px; padding: 30px; width: 100%; max-width: 420px; box-shadow: 0 10px 30px rgba(0,0,0,0.5); }
  h2 { margin-top: 0; text-align: center; font-size: 24px; color: white; }
  .form-group { margin-bottom: 20px; }
  label { display: block; font-size: 13px; color: #94A3B8; margin-bottom: 8px; text-transform: uppercase; letter-spacing: 0.5px; }
  input[type="text"], input[type="password"] { width: 100%; padding: 12px 15px; border: 1px solid var(--border); border-radius: 8px; font-size: 15px; box-sizing: border-box; background: #0B0E14; color: white; outline: none; }
  input:focus { border-color: var(--primary); }
  .radio-group { display: flex; gap: 10px; margin-bottom: 20px; background: #0B0E14; padding: 5px; border-radius: 8px; border: 1px solid var(--border); }
  .radio-group label { flex: 1; text-align: center; padding: 10px; border-radius: 6px; cursor: pointer; font-size: 13px; font-weight: 600; margin: 0; transition: 0.2s; color: #94A3B8; }
  .radio-group input { display: none; }
  .radio-group input:checked + label { background: #1E222D; color: white; }
  button { width: 100%; padding: 14px; background: var(--primary); color: white; border: none; border-radius: 8px; font-size: 15px; font-weight: 600; cursor: pointer; margin-top: 10px; transition: 0.2s; }
  button:hover { background: #2563EB; }
  .link-btn { display: block; text-align: center; color: var(--primary); font-size: 13px; margin-top: 15px; cursor: pointer; font-weight: 600; }
  .link-btn:hover { text-decoration: underline; }
</style>
</head><body>
<div class="card">
  <h2>StoryPointYK Setup</h2>
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
      
      <span class="link-btn" onclick="toggleAdv()">Розширені налаштування мережі ▼</span>
      <div id="adv-fields" style="display:none; margin-top: 15px; padding: 15px; background: #0B0E14; border: 1px solid var(--border); border-radius: 8px;">
        <div class="form-group"><label>mDNS Ім'я</label><input type="text" name="mdns" value="storypointyk"></div>
        <label style="display:flex; align-items:center; gap:10px; color:white; cursor:pointer;">
          <input type="checkbox" name="use_static" onchange="document.getElementById('static-box').style.display = this.checked ? 'block' : 'none'" style="width:auto; margin:0;">
          Статичний IP
        </label>
        <div id="static-box" style="display:none; margin-top:15px;">
          <div class="form-group"><label>IP Адреса</label><input type="text" name="ip" placeholder="192.168.1.100"></div>
          <div class="form-group"><label>Шлюз (Gateway)</label><input type="text" name="gw" placeholder="192.168.1.1"></div>
          <div class="form-group"><label>Маска (Subnet)</label><input type="text" name="sn" placeholder="255.255.255.0"></div>
          <div class="form-group"><label>DNS (опціонально)</label><input type="text" name="dns" placeholder="8.8.8.8"></div>
        </div>
      </div>
    </div>

    <hr style="border: 0; border-top: 1px solid var(--border); margin: 25px 0;">
    <div class="form-group"><label>Логін Адміністратора</label><input type="text" name="w_user" required value="admin"></div>
    <div class="form-group"><label>Пароль (мін. 8 символів)</label><input type="password" name="w_pass" required minlength="8" value="admin123"></div>
    <button type="submit">Зберегти та запустити</button>
  </form>
</div>
<script>
  function toggleMode() {
    let mode = document.querySelector('input[name="mode"]:checked').value;
    document.getElementById('wifi-fields').style.display = (mode === 'wifi') ? 'block' : 'none';
  }
  function toggleAdv() {
    let el = document.getElementById('adv-fields');
    el.style.display = el.style.display === 'none' ? 'block' : 'none';
  }
</script>
</body></html>
)rawliteral";

const char main_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>StoryPointYK Дашборд</title>
<style>
  :root { 
    --bg: #0B0E14; --panel: #131720; --border: #1E222D; --text: #E2E8F0; --muted: #64748B; 
    --primary: #3B82F6; --primary-hover: #2563EB; --success: #10B981; --danger: #EF4444; --warning: #F59E0B;
    --sidebar: #06080A;
  }
  body { font-family: 'Segoe UI', Roboto, sans-serif; background: var(--bg); color: var(--text); margin: 0; display: flex; height: 100vh; overflow: hidden; }
  ::-webkit-scrollbar { width: 8px; }
  ::-webkit-scrollbar-track { background: var(--bg); }
  ::-webkit-scrollbar-thumb { background: var(--border); border-radius: 4px; }
  ::-webkit-scrollbar-thumb:hover { background: var(--muted); }

  .sidebar { width: 250px; background: var(--sidebar); border-right: 1px solid var(--border); padding: 25px 20px; display: flex; flex-direction: column; z-index: 10; }
  .logo { font-size: 20px; font-weight: 800; color: white; margin-bottom: 35px; display: flex; align-items: center; gap: 10px; letter-spacing: 0.5px; }
  .profile { padding: 15px; background: var(--panel); border: 1px solid var(--border); border-radius: 10px; margin-bottom: 30px; }
  .profile-name { font-size: 14px; font-weight: 600; color: var(--primary); }
  .profile-role { font-size: 10px; color: var(--muted); letter-spacing: 1px; margin-top: 4px; text-transform: uppercase; }
  .nav { list-style: none; padding: 0; margin: 0; display: flex; flex-direction: column; gap: 8px; }
  .nav-item { padding: 12px 15px; border-radius: 8px; cursor: pointer; font-size: 14px; font-weight: 500; color: var(--muted); transition: 0.2s; display: flex; align-items: center; gap: 10px; border: 1px solid transparent; }
  .nav-item:hover { background: rgba(255,255,255,0.03); color: white; }
  .nav-item.active { background: rgba(59, 130, 246, 0.1); color: var(--primary); border-color: var(--primary); }

  .main-content { flex: 1; padding: 30px 40px; overflow-y: auto; }
  .header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 30px; }
  .header h1 { font-size: 26px; margin: 0; color: white; font-weight: 700; }
  .header .subtitle { font-size: 13px; color: var(--muted); margin-top: 5px; font-family: monospace; }
  
  .tab-pane { display: none; animation: fadeIn 0.3s; }
  .tab-pane.active { display: block; }
  @keyframes fadeIn { from { opacity: 0; transform: translateY(5px); } to { opacity: 1; transform: translateY(0); } }

  .stats-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(200px, 1fr)); gap: 15px; margin-bottom: 30px; }
  .stat-card { background: var(--panel); border: 1px solid var(--border); border-radius: 10px; padding: 20px; position: relative; overflow: hidden; }
  .stat-card::before { content: ''; position: absolute; top: 0; left: 0; width: 100%; height: 2px; background: var(--border); }
  .stat-card.active-green::before { background: var(--success); }
  .stat-card.active-blue::before { background: var(--primary); }
  .stat-title { font-size: 11px; color: var(--muted); text-transform: uppercase; letter-spacing: 1px; font-weight: 600; margin-bottom: 10px; display: flex; align-items: center; gap: 6px; }
  .stat-value { font-size: 24px; font-weight: 700; color: white; }
  .stat-sub { font-size: 12px; color: var(--muted); margin-top: 5px; }

  .panel { background: var(--panel); border: 1px solid var(--border); border-radius: 12px; padding: 25px; margin-bottom: 20px; }
  .panel-header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 20px; padding-bottom: 15px; border-bottom: 1px solid var(--border); }
  .panel-title { font-size: 14px; font-weight: 700; color: white; text-transform: uppercase; letter-spacing: 1px; display: flex; align-items: center; gap: 8px; }
  
  .list-item { display: flex; justify-content: space-between; align-items: center; padding: 12px 0; border-bottom: 1px solid rgba(255,255,255,0.05); }
  .list-item:last-child { border-bottom: none; }
  .list-name { font-size: 14px; font-weight: 600; color: white; margin-bottom: 4px; }
  .list-meta { font-size: 11px; color: var(--muted); }
  
  label { font-size: 12px; color: var(--muted); font-weight: 600; margin-bottom: 8px; display: block; text-transform: uppercase; }
  input[type="text"], input[type="number"], input[type="password"], input[type="time"], select { 
    width: 100%; padding: 10px 12px; background: var(--bg); border: 1px solid var(--border); border-radius: 6px; 
    color: white; font-size: 14px; box-sizing: border-box; outline: none; margin-bottom: 15px;
  }
  input:focus, select:focus { border-color: var(--primary); }
  
  .custom-select-list { 
    display: none; position: absolute; top: 100%; left: 0; right: 0; 
    background: var(--panel); border: 1px solid var(--border); border-radius: 6px; 
    margin-top: 4px; max-height: 200px; overflow-y: auto; z-index: 100; 
    box-shadow: 0 4px 15px rgba(0,0,0,0.5); 
  }
  .custom-select-item { padding: 10px 12px; font-size: 14px; color: white; cursor: pointer; transition: 0.2s; }
  .custom-select-item:hover { background: var(--primary); }
  
  input[type="range"] { -webkit-appearance: none; width: 100%; height: 4px; background: var(--border); border-radius: 2px; outline: none; margin: 15px 0; }
  input[type="range"]::-webkit-slider-thumb { -webkit-appearance: none; width: 16px; height: 16px; background: var(--primary); border-radius: 50%; cursor: pointer; box-shadow: 0 0 10px rgba(59,130,246,0.5); }
  
  .btn { padding: 10px 16px; border: none; border-radius: 6px; font-size: 13px; font-weight: 600; cursor: pointer; transition: 0.2s; display: inline-flex; align-items: center; justify-content: center; gap: 6px; }
  .btn-primary { background: var(--primary); color: white; }
  .btn-primary:hover { background: var(--primary-hover); }
  .btn-outline { background: transparent; border: 1px solid var(--border); color: var(--text); }
  .btn-outline:hover { border-color: var(--muted); color: white; }
  .btn-danger { background: rgba(239, 68, 68, 0.1); color: var(--danger); border: 1px solid rgba(239, 68, 68, 0.2); }
  .btn-danger:hover { background: var(--danger); color: white; }
  .btn-success { background: rgba(16, 185, 129, 0.1); color: var(--success); border: 1px solid rgba(16, 185, 129, 0.2); }
  .btn-success:hover { background: var(--success); color: white; }
  .btn-block { width: 100%; }

  .upload-area { border: 1px dashed var(--muted); border-radius: 8px; padding: 20px; text-align: center; position: relative; background: rgba(0,0,0,0.2); margin-bottom: 15px; transition: 0.2s; }
  .upload-area:hover { border-color: var(--primary); }
  input[type="file"] { position: absolute; top: 0; left: 0; width: 100%; height: 100%; opacity: 0; cursor: pointer; }
  .upload-text { color: var(--primary); font-weight: 600; font-size: 13px; }
  
  .log-console { background: #010409; border: 1px solid var(--border); border-radius: 8px; padding: 15px; height: 300px; overflow-y: auto; font-family: 'Consolas', monospace; font-size: 12px; color: var(--success); line-height: 1.6; }
  
  #toast { position: fixed; bottom: -50px; left: 50%; transform: translateX(-50%); background: var(--success); color: white; padding: 10px 20px; border-radius: 6px; font-size: 13px; font-weight: 600; transition: 0.3s; z-index: 1000; box-shadow: 0 5px 15px rgba(0,0,0,0.3); }
  #toast.show { bottom: 30px; }
  #toast.error { background: var(--danger); }
</style>
</head><body>

<aside class="sidebar">
  <div class="logo">StoryPointYK</div>
  <div class="profile">
    <div class="profile-name" id="wifiName">StoryPointYK Node</div>
    <div class="profile-role">SYSTEM ADMIN</div>
  </div>
  <nav class="nav">
    <div class="nav-item active" onclick="switchTab('tab-dashboard', this)">Системний Дашборд</div>
    <div class="nav-item" onclick="switchTab('tab-audio', this)">Управління аудіо</div>
    <div class="nav-item" onclick="switchTab('tab-system', this)">Системні налаштування</div>
  </nav>
  <div style="margin-top: auto;">
    <button class="btn btn-outline btn-block" onclick="rebootSystem()">Рестарт Системи</button>
  </div>
</aside>

<main class="main-content">
  <header class="header">
    <div>
      <h1 id="pageTitle">Статус системи</h1>
      <div class="subtitle" id="networkInfo">IP: -- | mDNS: --.local</div>
    </div>
    <div class="top-actions">
      <button class="btn btn-success" id="armBtn" onclick="toggleArm()">Запустити систему</button>
    </div>
  </header>

  <div id="tab-dashboard" class="tab-pane active">
    <div class="stats-grid">
      <div class="stat-card active-blue">
        <div class="stat-title">SD CARD</div>
        <div class="stat-value" id="sdState">--</div>
        <div class="stat-sub" id="sdSize">-- / -- MB</div>
      </div>
      <div class="stat-card active-green">
        <div class="stat-title">TOF SENSOR</div>
        <div class="stat-value" id="tofState">--</div>
        <div class="stat-sub">I2C Підключення</div>
      </div>
      <div class="stat-card active-blue">
        <div class="stat-title">AUDIO ENGINE</div>
        <div class="stat-value" id="audioState" style="font-size: 18px; margin-top: 5px;">--</div>
        <div class="stat-sub">I2S MAX98357A</div>
      </div>
      <div class="stat-card">
        <div class="stat-title">UPTIME</div>
        <div class="stat-value" id="uptime">--:--</div>
        <div class="stat-sub" id="devTime">Системний час: --:--</div>
      </div>
    </div>

    <div class="panel">
      <div class="panel-header">
        <div class="panel-title">АУДІО ФАЙЛИ</div>
        <span style="font-size: 11px; color: var(--muted);">SD Storage</span>
      </div>
      
      <div class="upload-area" id="dropZone">
        <span class="upload-text" id="fileNameDisp">Натисніть для завантаження .mp3 / .wav</span>
        <input type="file" id="fileInput" accept=".mp3,.wav" onchange="handleFileSelect()">
      </div>
      <button class="btn btn-success btn-block" id="uploadBtn" style="display:none; margin-bottom:15px;" onclick="uploadFile()">Завантажити в систему</button>

      <div id="filesListContainer"></div>
    </div>
  </div>

  <div id="tab-audio" class="tab-pane">
    <div class="panel">
      <div class="panel-header">
        <div class="panel-title">ПАРАМЕТРИ СЕРЕДОВИЩА ТА ДАТЧИКА</div>
        <button class="btn btn-primary" onclick="saveSettings()">Зберегти конфіг</button>
      </div>
      
      <div style="display: grid; grid-template-columns: 1fr 1fr; gap: 30px;">
        <div>
          <label>Алгоритм спрацювання (Трек):</label>
          <select id="trackSelect" onchange="saveSettings()">
            <optgroup label="Системні алгоритми">
              <option value="SEQ">Послідовно (SEQ_MODE)</option>
              <option value="RND">Випадково (RND_MODE)</option>
            </optgroup>
            <optgroup label="Статичний файл" id="fileOptions"></optgroup>
          </select>
          
          <label style="margin-top: 20px;">Активне вікно роботи (Години):</label>
          <div style="display:flex; align-items:center; gap:10px;">
            <input type="number" id="startH" min="0" max="23" placeholder="Від" onchange="saveSettings()" style="margin:0;">
            <span style="color:var(--muted);">-</span>
            <input type="number" id="endH" min="0" max="23" placeholder="До" onchange="saveSettings()" style="margin:0;">
          </div>
          <div style="font-size:11px; color:var(--muted); margin-top:5px;">0 - 0 означає цілодобово</div>
          
          <button class="btn btn-outline" style="margin-top: 30px;" onclick="togglePlayTest()">Тестовий запуск / Зупинка</button>
        </div>

        <div>
          <label>Дистанція датчика: <span id="distVal" style="color:white;">100</span> см</label>
          <input type="range" id="distance" min="5" max="200" step="5" onchange="saveSettings()" oninput="document.getElementById('distVal').innerText=this.value">
          
          <label style="margin-top: 30px;">Master Гучність: <span id="volVal" style="color:white;">15</span></label>
          <input type="range" id="volume" min="0" max="21" onchange="saveSettings()" oninput="document.getElementById('volVal').innerText=this.value">
        </div>
      </div>
    </div>
  </div>

  <div id="tab-system" class="tab-pane">
    <div style="display: grid; grid-template-columns: 1fr 1fr; gap: 20px;">
      
      <div class="panel" style="border-top: 3px solid var(--warning);">
        <div class="panel-header" style="margin-bottom:10px; padding-bottom:10px;"><div class="panel-title" style="color:var(--warning);">ІНЦИДЕНТИ (ЛОГИ)</div></div>
        <div class="log-console" id="logConsole">System boot...</div>
      </div>

      <div>
        <div class="panel">
          <div class="panel-header"><div class="panel-title">БЕЗПЕКА ТА ДОСТУП</div></div>
          <label>Новий Логін Адміна:</label>
          <input type="text" id="newWebUser" placeholder="admin">
          <label>Новий Пароль (мін 8 символів):</label>
          <input type="password" id="newWebPass" placeholder="********">
          <button class="btn btn-primary btn-block" onclick="changeAuth()">Оновити дані доступу</button>
        </div>

        <div class="panel">
          <div class="panel-header"><div class="panel-title">ПЛАНУВАЛЬНИК ТА ЧАС</div></div>
          <div style="display:flex; gap:10px; margin-bottom: 15px;">
            <input type="time" id="manualTime" style="margin:0;">
            <button class="btn btn-outline" onclick="setDeviceTime()">Set</button>
          </div>
          <button class="btn btn-outline btn-block" onclick="syncBrowserTime()">Синхронізувати з ПК</button>
        </div>

        <div class="panel" style="border-top: 3px solid var(--primary);">
          <div class="panel-header"><div class="panel-title">ПІДКЛЮЧЕННЯ WI-FI</div></div>
          
          <label>SSID (Назва мережі):</label>
          <div style="display:flex; gap:10px; margin-bottom: 15px; position:relative;">
            <div style="position: relative; flex: 1;">
              <input type="text" id="wifiSSID" autocomplete="off" placeholder="Назва мережі" style="margin:0; width: 100%;">
              <div id="custom-wifi-list" class="custom-select-list"></div>
            </div>
            <button class="btn btn-outline" style="width: auto; margin:0; padding: 0 15px;" onclick="scanWifi()" id="scanBtn">Scan</button>
          </div>
          
          <label>Пароль:</label>
          <input type="password" id="wifiPass" placeholder="Пароль від Wi-Fi">
          
          <div style="margin-top: 20px; padding-top: 15px; border-top: 1px dashed rgba(255,255,255,0.1);">
            <label>mDNS Ім'я (без .local):</label>
            <input type="text" id="wifiMDNS" placeholder="storypointyk">
            
            <label style="display:flex; align-items:center; gap:10px; color:white; cursor:pointer; margin-top:15px; margin-bottom:15px; text-transform:none;">
              <input type="checkbox" id="wifiStaticToggle" onchange="document.getElementById('staticConfig').style.display = this.checked ? 'block' : 'none'" style="width:auto; margin:0;">
              Використовувати статичний IP
            </label>
            
            <div id="staticConfig" style="display:none; padding-left:15px; border-left:2px solid var(--primary);">
              <label>IP Адреса:</label> <input type="text" id="wifiIP" placeholder="192.168.1.100">
              <label>Шлюз (Gateway):</label> <input type="text" id="wifiGW" placeholder="192.168.1.1">
              <label>Маска підмережі:</label> <input type="text" id="wifiSN" placeholder="255.255.255.0">
              <label>DNS (опціонально):</label> <input type="text" id="wifiDNS" placeholder="8.8.8.8">
            </div>
          </div>
          
          <button class="btn btn-primary btn-block" onclick="saveWifi()">Підключитись (Рестарт)</button>
        </div>

        <div class="panel" style="border-top: 3px solid var(--danger);">
          <div class="panel-header">
            <div class="panel-title">SYSTEM OTA UPDATE</div>
            <span style="font-size: 11px; color: var(--muted);">Версія: <span id="fwVersion">--</span></span>
          </div>
          <div class="upload-area" id="otaDropZone" style="border-color:var(--danger); padding:10px;">
            <span class="upload-text" id="otaFileNameDisp" style="color:var(--danger);">Оберіть .bin прошивку</span>
            <input type="file" id="otaInput" accept=".bin" onchange="handleOtaSelect()">
          </div>
          <input type="password" id="otaPass" placeholder="Root пароль (OTA)" style="background:#010409;">
          <button class="btn btn-danger btn-block" id="otaBtn" style="display:none;" onclick="updateFirmware()">Ініціалізувати прошивку</button>
        </div>
      </div>

    </div>
  </div>
</main>

<div id="toast">Повідомлення</div>

<script>
  function showToast(msg, isErr=false) {
    let t = document.getElementById("toast"); t.innerText = msg; 
    t.className = isErr ? "show error" : "show";
    setTimeout(()=> t.className = "", 3000);
  }

  function formatTime(sec) {
    let h = Math.floor(sec / 3600); let m = Math.floor((sec % 3600) / 60);
    return h + "h " + m + "m";
  }

  function switchTab(tabId, elem) {
    document.querySelectorAll('.tab-pane').forEach(el => el.classList.remove('active'));
    document.querySelectorAll('.nav-item').forEach(el => el.classList.remove('active'));
    document.getElementById(tabId).classList.add('active');
    elem.classList.add('active');
    let titles = {'tab-dashboard': 'Статус системи', 'tab-audio': 'Управління аудіо', 'tab-system': 'Системні налаштування'};
    document.getElementById('pageTitle').innerText = titles[tabId];
  }

  function handleFileSelect() {
    let file = document.getElementById('fileInput').files[0];
    let disp = document.getElementById('fileNameDisp');
    let btn = document.getElementById('uploadBtn');
    if(file) { disp.innerText = file.name; disp.style.color = 'white'; btn.style.display = 'block'; } 
    else { disp.innerText = 'Натисніть для завантаження .mp3 / .wav'; disp.style.color = 'var(--primary)'; btn.style.display = 'none'; }
  }

  function handleOtaSelect() {
    let file = document.getElementById('otaInput').files[0];
    let disp = document.getElementById('otaFileNameDisp');
    let btn = document.getElementById('otaBtn');
    if(file && file.name.endsWith('.bin')) { disp.innerText = file.name; disp.style.color = 'white'; btn.style.display = 'block'; } 
    else { disp.innerText = 'Тільки файли .bin'; btn.style.display = 'none'; }
  }

  function loadData() {
    fetch('/api/data').then(r=>r.json()).then(d=>{
      document.getElementById('uptime').innerText = formatTime(d.up);
      document.getElementById('devTime').innerText = "Системний час: " + d.time;
      document.getElementById('wifiName').innerText = d.ssid !== "" ? d.ssid : "Autonomous AP";
      document.getElementById('fwVersion').innerText = d.version;
      document.getElementById('networkInfo').innerText = "IP: " + d.ip + " | mDNS: " + d.mdns + ".local";
      
      let sd = document.getElementById('sdState');
      sd.innerText = d.sd_ok ? "ONLINE" : "OFFLINE";
      sd.style.color = d.sd_ok ? "white" : "var(--danger)";
      document.getElementById('sdSize').innerText = d.sd_u + " / " + d.sd_t + " MB";
      
      let tof = document.getElementById('tofState');
      tof.innerText = d.tof_ok ? "RUNNING" : "ERROR";
      tof.style.color = d.tof_ok ? "white" : "var(--danger)";
      
      let aud = document.getElementById('audioState');
      aud.innerText = d.playing ? "Відтворюється..." : "Очікування";
      aud.style.color = d.playing ? "var(--success)" : "var(--muted)";

      let armBtn = document.getElementById('armBtn');
      if (d.armed) {
        armBtn.innerHTML = "Зупинити систему (Disarm)";
        armBtn.className = "btn btn-danger";
      } else {
        armBtn.innerHTML = "Запустити систему (Arm)";
        armBtn.className = "btn btn-success";
      }

      let logBox = document.getElementById('logConsole');
      let isScrolledToBottom = logBox.scrollHeight - logBox.clientHeight <= logBox.scrollTop + 1;
      logBox.innerHTML = d.logs.join("<br>");
      if(isScrolledToBottom) logBox.scrollTop = logBox.scrollHeight;
      
      let v = document.getElementById('volume'); if(document.activeElement !== v) { v.value = d.vol; document.getElementById('volVal').innerText = d.vol; }
      let dist = document.getElementById('distance'); if(document.activeElement !== dist) { dist.value = Math.round(d.dist / 10); document.getElementById('distVal').innerText = Math.round(d.dist / 10); }
      let sH = document.getElementById('startH'); if(document.activeElement !== sH) sH.value = d.sh;
      let eH = document.getElementById('endH'); if(document.activeElement !== eH) eH.value = d.eh;

      // Оновлення полів Wi-Fi налаштувань
      if(document.activeElement !== document.getElementById('wifiMDNS')) document.getElementById('wifiMDNS').value = d.mdns;
      if(document.activeElement !== document.getElementById('wifiIP')) document.getElementById('wifiIP').value = d.static_ip;
      if(document.activeElement !== document.getElementById('wifiGW')) document.getElementById('wifiGW').value = d.static_gw;
      if(document.activeElement !== document.getElementById('wifiSN')) document.getElementById('wifiSN').value = d.static_sn;
      if(document.activeElement !== document.getElementById('wifiDNS')) document.getElementById('wifiDNS').value = d.static_dns;
      
      let tgl = document.getElementById('wifiStaticToggle');
      if(document.activeElement !== tgl) {
         tgl.checked = d.use_static;
         document.getElementById('staticConfig').style.display = d.use_static ? 'block' : 'none';
      }

      let sel = document.getElementById('trackSelect');
      let optGroup = document.getElementById('fileOptions');
      optGroup.innerHTML = '';
      let listCont = document.getElementById('filesListContainer');
      listCont.innerHTML = '';

      if(d.files.length === 0) listCont.innerHTML = '<div style="text-align:center; padding:20px; color:var(--muted); font-size:13px;">Сховище порожнє</div>';

      d.files.forEach(f => {
        let opt = document.createElement('option'); opt.value = f.name; opt.innerHTML = f.name; optGroup.appendChild(opt);
        let sizeMB = (f.size / (1024 * 1024)).toFixed(2);

        let item = document.createElement('div'); item.className = 'list-item';
        item.innerHTML = `
          <div><div class="list-name">${f.name}</div><div class="list-meta">Розмір: ${sizeMB} MB</div></div>
          <div style="display:flex; align-items:center; gap:15px;">
            <button class="btn btn-danger" style="padding:6px 12px; font-size:11px;" onclick="deleteFile('${f.name}')">Видалити</button>
          </div>
        `;
        listCont.appendChild(item);
      });
      if(d.track) sel.value = d.track;
    });
  }

  function setDeviceTime() {
    let tVal = document.getElementById('manualTime').value;
    if(!tVal) return;
    let fd = new FormData(); fd.append("time", tVal);
    fetch('/api/time', {method: 'POST', body: fd}).then(()=> { showToast("Час системи синхронізовано"); loadData(); });
  }

  function syncBrowserTime() {
    let d = new Date();
    let tVal = String(d.getHours()).padStart(2, '0') + ":" + String(d.getMinutes()).padStart(2, '0');
    document.getElementById('manualTime').value = tVal;
    setDeviceTime();
  }

  function saveSettings() {
    let vol = document.getElementById('volume').value; 
    let dist = document.getElementById('distance').value * 10; 
    let trk = document.getElementById('trackSelect').value;
    let sh = document.getElementById('startH').value;
    let eh = document.getElementById('endH').value;
    let fd = new FormData(); fd.append("vol", vol); fd.append("dist", dist); fd.append("track", trk); fd.append("sh", sh); fd.append("eh", eh);
    fetch('/api/settings', {method: 'POST', body: fd}).then(()=> showToast("Конфігурацію збережено"));
  }

  function toggleArm() {
    let fd = new FormData(); fd.append("cmd", "toggle_arm");
    fetch('/api/control', {method: 'POST', body: fd}).then(()=> { loadData(); });
  }

  function togglePlayTest() {
    let fd = new FormData(); fd.append("cmd", "toggle");
    fetch('/api/control', {method: 'POST', body: fd}).then(()=> { loadData(); });
  }

  function scanWifi() {
    let btn = document.getElementById('scanBtn');
    btn.innerHTML = "Scanning..."; btn.disabled = true;
    fetch('/api/scan').then(r=>r.json()).then(d=>{
      if(d.status === "scanning") {
        setTimeout(scanWifi, 2000);
      } else {
        let dl = document.getElementById('custom-wifi-list');
        dl.innerHTML = '';
        let unique = [...new Set(d.networks)];
        if(unique.length === 0) {
           dl.innerHTML = '<div class="custom-select-item" style="color:var(--muted);">Мереж не знайдено</div>';
        } else {
          unique.forEach(net => { 
            let div = document.createElement('div');
            div.className = 'custom-select-item';
            div.innerText = net;
            div.onclick = () => {
              document.getElementById('wifiSSID').value = net;
              dl.style.display = 'none';
            };
            dl.appendChild(div); 
          });
        }
        dl.style.display = 'block';
        btn.innerHTML = "Scan"; btn.disabled = false;
        showToast("Мережі знайдено");
      }
    }).catch(()=>{ btn.innerHTML = "Scan"; btn.disabled = false; });
  }

  document.addEventListener('click', function(e) {
    let list = document.getElementById('custom-wifi-list');
    let input = document.getElementById('wifiSSID');
    let btn = document.getElementById('scanBtn');
    if (list && e.target !== input && e.target !== list && e.target !== btn) {
      list.style.display = 'none';
    }
  });

  document.getElementById('wifiSSID').addEventListener('click', function() {
     let list = document.getElementById('custom-wifi-list');
     if(list && list.innerHTML !== '') list.style.display = 'block';
  });

  function saveWifi() {
    let s = document.getElementById('wifiSSID').value;
    let p = document.getElementById('wifiPass').value;
    let m = document.getElementById('wifiMDNS').value;
    let us = document.getElementById('wifiStaticToggle').checked ? "1" : "0";
    let ip = document.getElementById('wifiIP').value;
    let gw = document.getElementById('wifiGW').value;
    let sn = document.getElementById('wifiSN').value;
    let dns = document.getElementById('wifiDNS').value;
    
    let fd = new FormData(); 
    if(s) fd.append("ssid", s); 
    if(p) fd.append("pass", p);
    fd.append("mdns", m);
    fd.append("use_static", us);
    fd.append("ip", ip);
    fd.append("gw", gw);
    fd.append("sn", sn);
    fd.append("dns", dns);
    
    fetch('/api/wifi', {method: 'POST', body: fd}).then(() => {
      showToast("Мережеві налаштування збережено! Рестарт...");
      setTimeout(() => location.reload(), 5000);
    });
  }

  function changeAuth() {
    let u = document.getElementById('newWebUser').value;
    let p = document.getElementById('newWebPass').value;
    if(!u || p.length < 8) { showToast("Логін та пароль (від 8 символів) обов'язкові", true); return; }
    let fd = new FormData(); fd.append("new_user", u); fd.append("new_pass", p);
    fetch('/api/auth', {method: 'POST', body: fd}).then(r => {
      if(r.ok) { showToast("Дані змінено. Авторизуйтесь знову."); setTimeout(()=>location.reload(), 2000); }
      else { showToast("Помилка", true); }
    });
  }

  function uploadFile() {
    let file = document.getElementById('fileInput').files[0];
    if(!file) return;
    let btn = document.getElementById('uploadBtn');
    btn.innerHTML = "Deploying..."; btn.style.opacity = "0.7"; btn.disabled = true;
    let fd = new FormData(); fd.append("file", file);
    fetch('/upload', {method: 'POST', body: fd}).then(()=> { 
      showToast("Файл успішно розгорнуто"); 
      document.getElementById('fileInput').value = "";
      btn.innerHTML = "Завантажити в систему"; btn.style.opacity = "1"; btn.disabled = false;
      handleFileSelect(); loadData(); 
    }).catch(()=> { showToast("Помилка завантаження", true); btn.innerHTML = "Завантажити в систему"; btn.disabled = false; btn.style.opacity = "1"; });
  }

  function deleteFile(fname) {
    if(confirm("Знищити аудіофайл " + fname + " ?")) {
      let fd = new FormData(); fd.append("filename", fname);
      fetch('/delete', {method: 'POST', body: fd}).then(()=> { showToast("Файл видалено"); loadData(); });
    }
  }

  function updateFirmware() {
    let file = document.getElementById('otaInput').files[0];
    let pass = document.getElementById('otaPass').value;
    if(!file) return;
    if(!pass) { showToast("Потрібна авторизація (Root Password)", true); return; }
    
    let btn = document.getElementById('otaBtn');
    btn.innerHTML = "FLASHING: 0%"; btn.disabled = true;
    
    let fd = new FormData(); fd.append("update", file);
    let xhr = new XMLHttpRequest();
    xhr.open('POST', '/update?pwd=' + encodeURIComponent(pass), true);
    
    xhr.upload.addEventListener('progress', function(e) {
      if (e.lengthComputable) {
        let percent = Math.round((e.loaded / e.total) * 100);
        btn.innerHTML = "FLASHING: " + percent + "% (DO NOT UNPLUG)";
      }
    });

    xhr.onload = function() {
      if (xhr.status === 200) {
        btn.innerHTML = "SUCCESS! Rebooting...";
        showToast("Прошивку оновлено. Система перезавантажується...");
        setTimeout(() => location.reload(), 8000);
      } else {
        btn.innerHTML = "Ініціалізувати прошивку"; btn.disabled = false;
        showToast("Access Denied або помилка файлу", true);
      }
    };
    xhr.send(fd);
  }

  function rebootSystem() {
    if(confirm('Виконати жорсткий рестарт ESP32?')) {
      fetch('/reboot', {method:'POST'}).then(() => {
        showToast("Перезавантаження... Очікуйте 5 сек");
        setTimeout(() => location.reload(), 5000);
      });
    }
  }

  loadData(); setInterval(loadData, 2000); 
</script>
</body></html>
)rawliteral";

void updateFileCache() {
  fileCache.clear();
  File root = SD.open("/");
  if (!root) { addLog("Помилка читання SD кореня"); return; }
  File file = root.openNextFile();
  while (file) {
    if (!file.isDirectory()) {
      String fname = String(file.name());
      if (fname.endsWith(".mp3") || fname.endsWith(".MP3") || fname.endsWith(".wav") || fname.endsWith(".WAV")) {
        FileInfo info;
        info.name = fname;
        info.size = file.size();
        fileCache.push_back(info);
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
  addLog("Запуск системи StoryPointYK");

  prefs.begin("storyframe", false);
  isConfigured = prefs.getBool("conf", false);
  ssid = prefs.getString("ssid", "");
  password = prefs.getString("pass", "");
  webUser = prefs.getString("w_user", "admin");
  webPass = prefs.getString("w_pass", "admin123"); 
  triggerDistance = prefs.getInt("dist", 1000);
  volumeLevel = prefs.getInt("vol", 15);
  activeTrack = prefs.getString("track", "SEQ");
  startHour = prefs.getInt("sh", 0);
  endHour = prefs.getInt("eh", 0);
  
  mdnsName = prefs.getString("mdns", "storypointyk");
  useStaticIP = prefs.getBool("use_static", false);
  staticIP = prefs.getString("ip", "");
  staticGW = prefs.getString("gw", "");
  staticSN = prefs.getString("sn", "255.255.255.0");
  staticDNS = prefs.getString("dns", "");

  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, SD_CS);
  if (SD.begin(SD_CS)) {
    sd_ok = true;
    addLog("Модуль SD карти ініціалізовано.");
    updateFileCache();
  } else {
    sd_ok = false;
    addLog("ПОМИЛКА: SD карту не знайдено!");
  }

  out = new AudioOutputI2S();
  out->SetPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  setAudioVolume(volumeLevel);
  addLog("Аудіо інтерфейс I2S налаштовано.");

  Wire.begin(I2C_SDA, I2C_SCL);
  if (lox.begin()) {
    tof_ok = true;
    addLog("Лазерний датчик TOF200C знайдено.");
  } else {
    tof_ok = false;
    addLog("ПОМИЛКА: TOF датчик не знайдено!");
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
      
      if(req->hasParam("mdns", true)) prefs.putString("mdns", req->getParam("mdns", true)->value());
      prefs.putBool("use_static", req->hasParam("use_static", true));
      if(req->hasParam("ip", true)) prefs.putString("ip", req->getParam("ip", true)->value());
      if(req->hasParam("gw", true)) prefs.putString("gw", req->getParam("gw", true)->value());
      if(req->hasParam("sn", true)) prefs.putString("sn", req->getParam("sn", true)->value());
      if(req->hasParam("dns", true)) prefs.putString("dns", req->getParam("dns", true)->value());

      prefs.putBool("conf", true);
      req->send(200, "text/plain", "Saved. Restarting...");
      delay(1000); ESP.restart();
    });
  } else {
    const char* apPassword = (webPass.length() >= 8) ? webPass.c_str() : NULL;
    
    if (ssid == "") {
      WiFi.mode(WIFI_AP);
      WiFi.softAP("StoryPointYK", apPassword); 
      addLog("Мережа: Автономна точка");
    } else {
      WiFi.setHostname(mdnsName.c_str()); 
      WiFi.mode(WIFI_STA); 
      
      if (useStaticIP && staticIP != "" && staticGW != "" && staticSN != "") {
         IPAddress lip = parseIP(staticIP);
         IPAddress lgw = parseIP(staticGW);
         IPAddress lsn = parseIP(staticSN);
         if (staticDNS != "") {
           WiFi.config(lip, lgw, lsn, parseIP(staticDNS));
         } else {
           WiFi.config(lip, lgw, lsn);
         }
      }

      WiFi.begin(ssid.c_str(), password.c_str());
      int tries = 0;
      while (WiFi.status() != WL_CONNECTED && tries < 20) { delay(500); tries++; }
      
      if(WiFi.status() != WL_CONNECTED) {
        WiFi.mode(WIFI_AP_STA);
        WiFi.softAP("StoryPointYK", apPassword);
        addLog("Немає Wi-Fi роутера. Резервна AP.");
      } else {
        WiFi.mode(WIFI_STA);
        addLog("Підключено до роутера. IP: " + WiFi.localIP().toString());
        configTzTime("EET-2EEST,M3.5.0/3,M10.5.0/4", "pool.ntp.org");
      }
    }

    if(MDNS.begin(mdnsName.c_str())) {
      MDNS.addService("http", "tcp", 80);
      addLog("mDNS запущено: " + mdnsName + ".local");
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

      String curTime = "--:--";
      struct tm timeinfo;
      if (getLocalTime(&timeinfo, 0)) {
        char timeStr[6];
        sprintf(timeStr, "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
        curTime = String(timeStr);
      }

      String currentIP = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : WiFi.softAPIP().toString();

      String json = "{\"rssi\":" + String(WiFi.RSSI()) + ",\"heap\":" + String(ESP.getFreeHeap()) + 
                    ",\"up\":" + String(uptime_sec) + ",\"sd_t\":" + String(sd_total) + ",\"sd_u\":" + String(sd_used) +
                    ",\"vol\":" + String(volumeLevel) + ",\"dist\":" + String(triggerDistance) + 
                    ",\"track\":\"" + activeTrack + "\",\"sh\":" + String(startHour) + ",\"eh\":" + String(endHour) +
                    ",\"tof_ok\":" + (tof_ok ? "true" : "false") + ",\"sd_ok\":" + (sd_ok ? "true" : "false") + 
                    ",\"playing\":" + (isPlaying ? "true" : "false") + ",\"armed\":" + (systemArmed ? "true" : "false") + 
                    ",\"time\":\"" + curTime + "\",\"ssid\":\"" + ssid + "\",\"version\":\"" + FIRMWARE_VERSION + "\"" +
                    ",\"ip\":\"" + currentIP + "\",\"mdns\":\"" + mdnsName + "\",\"use_static\":" + (useStaticIP ? "true" : "false") +
                    ",\"static_ip\":\"" + staticIP + "\",\"static_gw\":\"" + staticGW + "\",\"static_sn\":\"" + staticSN + "\",\"static_dns\":\"" + staticDNS + "\"" +
                    ",\"logs\":[";
      
      for (size_t i = 0; i < sysLogs.size(); i++) {
        json += "\"" + sysLogs[i] + "\"";
        if (i < sysLogs.size() - 1) json += ",";
      }
      
      json += "],\"files\":[";
      for (size_t i = 0; i < fileCache.size(); i++) {
        json += "{\"name\":\"" + fileCache[i].name + "\",\"size\":" + String(fileCache[i].size) + "}";
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
      }
      if(req->hasParam("dist", true)) { 
        triggerDistance = req->getParam("dist", true)->value().toInt(); 
        prefs.putInt("dist", triggerDistance); 
      }
      if(req->hasParam("track", true)) { 
        activeTrack = req->getParam("track", true)->value(); 
        prefs.putString("track", activeTrack); 
      }
      if(req->hasParam("sh", true)) { 
        startHour = req->getParam("sh", true)->value().toInt(); 
        prefs.putInt("sh", startHour); 
      }
      if(req->hasParam("eh", true)) { 
        endHour = req->getParam("eh", true)->value().toInt(); 
        prefs.putInt("eh", endHour); 
      }
      req->send(200);
    });

    server.on("/api/wifi", HTTP_POST, [](AsyncWebServerRequest *req){
      if(!req->authenticate(webUser.c_str(), webPass.c_str())) return req->send(401);
      
      if(req->hasParam("ssid", true)) prefs.putString("ssid", req->getParam("ssid", true)->value());
      if(req->hasParam("pass", true)) prefs.putString("pass", req->getParam("pass", true)->value());
      if(req->hasParam("mdns", true)) prefs.putString("mdns", req->getParam("mdns", true)->value());
      
      prefs.putBool("use_static", req->getParam("use_static", true)->value() == "1");
      if(req->hasParam("ip", true)) prefs.putString("ip", req->getParam("ip", true)->value());
      if(req->hasParam("gw", true)) prefs.putString("gw", req->getParam("gw", true)->value());
      if(req->hasParam("sn", true)) prefs.putString("sn", req->getParam("sn", true)->value());
      if(req->hasParam("dns", true)) prefs.putString("dns", req->getParam("dns", true)->value());
      
      addLog("Мережеві налаштування збережено. Рестарт...");
      req->send(200);
      delay(500); ESP.restart();
    });

    server.on("/api/auth", HTTP_POST, [](AsyncWebServerRequest *req){
      if(!req->authenticate(webUser.c_str(), webPass.c_str())) return req->send(401);
      if(req->hasParam("new_user", true) && req->hasParam("new_pass", true)) {
        String nu = req->getParam("new_user", true)->value();
        String np = req->getParam("new_pass", true)->value();
        if (np.length() >= 8 && nu.length() > 0) {
           prefs.putString("w_user", nu);
           prefs.putString("w_pass", np);
           webUser = nu;
           webPass = np;
           addLog("Облікові дані оновлено.");
           req->send(200);
           return;
        }
      }
      req->send(400);
    });

    server.on("/api/scan", HTTP_GET, [](AsyncWebServerRequest *req){
      if(!req->authenticate(webUser.c_str(), webPass.c_str())) return req->send(401);
      int n = WiFi.scanComplete();
      if(n == -2){
        WiFi.scanNetworks(true);
        req->send(200, "application/json", "{\"status\":\"scanning\"}");
      } else if(n == -1){
        req->send(200, "application/json", "{\"status\":\"scanning\"}");
      } else {
        String json = "{\"status\":\"done\",\"networks\":[";
        for (int i = 0; i < n; ++i) {
          json += "\"" + WiFi.SSID(i) + "\"";
          if (i < n - 1) json += ",";
        }
        json += "]}";
        WiFi.scanDelete();
        req->send(200, "application/json", json);
      }
    });

    server.on("/api/time", HTTP_POST, [](AsyncWebServerRequest *req){
      if(!req->authenticate(webUser.c_str(), webPass.c_str())) return req->send(401);
      if(req->hasParam("time", true)) {
        String t = req->getParam("time", true)->value();
        int h = t.substring(0, 2).toInt();
        int m = t.substring(3, 5).toInt();
        
        struct tm tm_info;
        if(!getLocalTime(&tm_info, 0)) {
           tm_info.tm_year = 2026 - 1900;
           tm_info.tm_mon = 0;
           tm_info.tm_mday = 1;
        }
        tm_info.tm_hour = h;
        tm_info.tm_min = m;
        tm_info.tm_sec = 0;
        
        time_t t_of_day = mktime(&tm_info);
        struct timeval tv = { .tv_sec = t_of_day, .tv_usec = 0 };
        settimeofday(&tv, NULL);
        addLog("Час встановлено вручну: " + t);
      }
      req->send(200);
    });

    server.on("/api/control", HTTP_POST, [](AsyncWebServerRequest *req){
      if(!req->authenticate(webUser.c_str(), webPass.c_str())) return req->send(401);
      if(req->hasParam("cmd", true)) {
        String cmd = req->getParam("cmd", true)->value();
        if (cmd == "toggle_arm") {
          reqToggleArm = true;
        } else if (cmd == "toggle") {
          if (mp3 && mp3->isRunning()) reqStop = true;
          else reqPlay = true;
        }
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

    server.on("/update", HTTP_POST, [](AsyncWebServerRequest *req){
      if(!req->authenticate(webUser.c_str(), webPass.c_str())) return req->send(401);
      bool error = Update.hasError();
      if (error) {
        req->send(500, "text/plain", "Update Failed");
      } else {
        req->send(200, "text/plain", "Update Success. Rebooting...");
        delay(1000); ESP.restart();
      }
    }, [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
      if(!request->authenticate(webUser.c_str(), webPass.c_str())) return;
      
      if (!request->hasParam("pwd") || request->getParam("pwd")->value() != "13795OTA") {
        if (!index) addLog("OTA Відхилено: Невірний пароль!");
        return;
      }

      if (!index) {
        addLog("Початок OTA оновлення...");
        if (!Update.begin((ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000)) {
          Update.printError(Serial);
        }
      }
      if (!Update.hasError()) {
        if (Update.write(data, len) != len) {
          Update.printError(Serial);
        }
      }
      if (final) {
        if (Update.end(true)) {
          addLog("OTA оновлення успішне. Перезавантаження...");
        } else {
          Update.printError(Serial);
        }
      }
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
  
  if (reqStop) {
    stopAudioSafe();
    reqStop = false;
  }
  if (reqPlay) {
    triggerPlaybackSafe();
    reqPlay = false;
  }
  if (reqToggleArm) {
    systemArmed = !systemArmed;
    if (!systemArmed) reqStop = true;
    addLog(systemArmed ? "СИСТЕМУ АКТИВОВАНО" : "СИСТЕМУ ПРИЗУПИНЕНО");
    reqToggleArm = false;
  }

  if (mp3 && mp3->isRunning()) {
    if (!mp3->loop()) {
      mp3->stop();
      addLog("Трек завершено.");
    }
  }

  if (!isConfigured) dnsServer.processNextRequest();

  if (digitalRead(BUTTON_PIN) == LOW) {
    if (!buttonActive) { buttonPressTime = millis(); buttonActive = true; } 
    else if (millis() - buttonPressTime > 5000) {
      Serial.println("Скидання..."); prefs.clear(); delay(1000); ESP.restart();
    }
  } else { buttonActive = false; }

  static unsigned long lastTof = 0;
  if (isConfigured && tof_ok && millis() - lastTof > 200) {
    lastTof = millis();
    VL53L0X_RangingMeasurementData_t measure;
    lox.rangingTest(&measure, false);

    if (measure.RangeStatus != 4 && systemArmed) {
      if (measure.RangeMilliMeter < triggerDistance) {
        bool isPlaying = (mp3 && mp3->isRunning());
        
        if (!isPlaying && fileCache.size() > 0) {
          if (isTimeActive()) {
            addLog("Датчик: " + String(measure.RangeMilliMeter / 10) + " см");
            triggerPlaybackSafe();
          }
        }
      }
    }
  }
}