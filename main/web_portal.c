#include "web_portal.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>

#include "wifi_config_manager.h"
#include "user_azure.h"
#include "user_system.h"
#include "user_ouput.h"
#include "user_external_flash.h"
#include "user_fram.h"
#include <sys/stat.h>
#include <sys/unistd.h>
#include <sys/param.h>
#include <dirent.h>
#include "esp_spiffs.h"

static const char *PORTAL_TAG = "web_portal";

/* HTTP Basic Auth: mebieco.admin : 68686868@ */
#define CONFIG_HTTP_AUTH_USER "mebieco.admin"
#define CONFIG_HTTP_AUTH_PASS "68686868@"
#define CONFIG_HTTP_AUTH_PIN  "686868"
#define CONFIG_HTTP_AUTH_B64  "Basic bWViaWVjby5hZG1pbjo2ODY4Njg2OEA="
#define CONFIG_PORTAL_DOMAIN  "mebieco.local"

static bool prvIsAuthenticated(httpd_req_t *req)
{
    size_t hdr_len = httpd_req_get_hdr_value_len(req, "Authorization");
    if (hdr_len == 0) return false;

    char *auth_hdr = malloc(hdr_len + 1);
    if (!auth_hdr) return false;

    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_hdr, hdr_len + 1) == ESP_OK) {
        if (strcmp(auth_hdr, CONFIG_HTTP_AUTH_B64) == 0) {
            free(auth_hdr);
            return true;
        }
    }
    free(auth_hdr);
    return false;
}

static esp_err_t prvSendUnauthorized(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"Mebieco Config Portal\"");
    return httpd_resp_send(req, "401 Unauthorized", HTTPD_RESP_USE_STRLEN);
}

#include "esp_mac.h"

static esp_err_t prvSendDomainRedirect(httpd_req_t *req, const char* domain)
{
    ESP_LOGI(PORTAL_TAG, "Redirecting IP access to domain: %s", domain);
    httpd_resp_set_status(req, "302 Found");
    char location[128];
    snprintf(location, sizeof(location), "http://%s", domain);
    httpd_resp_set_hdr(req, "Location", location);
    return httpd_resp_send(req, NULL, 0);
}


static const char *portal_html =
"<!DOCTYPE html><html><head><meta charset='utf-8'/>"
"<title>Mebieco Setup</title>"
"<style>"
":root{--p:#3b82f6;--s:#10b981;--d:#ef4444;--bg:#f3f4f6;--c:#ffffff;--sub:#eff6ff;--t:#1f2937;--b:#e5e7eb;--sh:rgba(0,0,0,0.1);}"
"body.dark{--bg:#0f172a;--c:#1e293b;--sub:#334155;--t:#f8fafc;--b:#475569;--sh:rgba(0,0,0,0.3);}"
"body{font-family:'Segoe UI',Roboto,Arial,sans-serif;background:var(--bg);color:var(--text);margin:0;line-height:1.5;transition:background 0.3s, color 0.3s;}"
".layout{display:flex;min-height:100vh;flex-direction:column;}"
"@media(min-width:768px){.layout{flex-direction:row;}}"
".sidebar{background:var(--c);padding:20px;border-right:1px solid var(--b);min-width:220px;display:flex;flex-direction:column;}"
"@media(max-width:767px){.sidebar{border-right:none;border-bottom:1px solid var(--b);flex-direction:row;overflow-x:auto;padding:10px 20px;gap:10px;}}"
".brand{font-size:20px;font-weight:700;color:var(--p);margin-bottom:20px;}"
"@media(max-width:767px){.brand{display:none;}}"
".menu-item{padding:12px 16px;border-radius:8px;cursor:pointer;font-weight:600;color:var(--t);opacity:0.7;transition:all 0.2s;white-space:nowrap;margin-bottom:8px; display:flex; gap:8px; align-items:center;}"
"@media(max-width:767px){.menu-item{margin-bottom:0;}}"
".menu-item:hover{background:var(--bg);opacity:1;}"
".menu-item.active{background:var(--p);color:#fff;opacity:1;box-shadow:0 4px 12px rgba(59,130,246,0.3);}"
".dark-toggle{margin-top:auto;padding:12px;border-radius:8px;cursor:pointer;display:flex;align-items:center;gap:8px;font-weight:600;color:var(--t);opacity:0.8;border:1px solid var(--b);justify-content:center;transition:0.2s;}"
"@media(max-width:767px){.dark-toggle{margin-top:0;min-width:44px;padding:8px;}}"
".dark-toggle:hover{background:var(--p);color:#fff;}"
".content{flex:1;padding:24px;overflow-y:auto;}"
".page{display:none;max-width:600px;margin:0 auto;animation:fadeIn 0.3s;}"
".page.active{display:block;}"
"@keyframes fadeIn{from{opacity:0;transform:translateY(10px);}to{opacity:1;transform:translateY(0);}}"
".card{background:var(--c);padding:24px;border-radius:16px;box-shadow:0 4px 6px -1px var(--sh);border:1px solid var(--b);margin-bottom:20px;color:var(--t);}"
".section-title{font-size:18px;font-weight:600;margin:0 0 16px;padding-bottom:8px;border-bottom:2px solid var(--bg);}"
"select,input{width:100%;box-sizing:border-box;padding:10px 12px;border:1px solid var(--b);border-radius:8px;margin-top:6px;font-size:14px;background:var(--bg);color:var(--t);transition:0.2s;}"
"select:focus,input:focus{outline:none;border-color:var(--p);box-shadow:0 0 0 3px rgba(59,130,246,0.15);}"
"button{width:100%;padding:12px;border:none;border-radius:8px;font-weight:600;font-size:15px;cursor:pointer;color:#fff;transition:0.2s;}"
".btn-primary{background:var(--s);}"
".btn-secondary{background:var(--p);margin-top:16px;}"
".btn-danger{background:var(--d);}"
".btn-group{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin-top:20px;}"
".btn-group .btn-primary{grid-column:span 2;}"
".pw-wrap{position:relative;margin-top:6px;}"
".pw-toggle{position:absolute;right:10px;top:50%;transform:translateY(-50%);cursor:pointer;opacity:0.6;font-size:18px;width:30px;height:30px;display:flex;align-items:center;justify-content:center;z-index:2;}"
".info-item{display:flex;justify-content:space-between;border-bottom:1px dashed var(--b);padding:8px 0;font-size:14px;}"
".sch-table{width:100%;border-collapse:collapse;margin-top:10px;font-size:13px;}"
".sch-table th{text-align:left;color:var(--t);opacity:0.6;padding:8px;border-bottom:1px solid var(--b);}"
".sch-table td{padding:8px;border-bottom:1px solid var(--b);}"
".status-tag{padding:2px 8px;border-radius:12px;font-weight:600;font-size:11px;}"
".status-done{background:#dcfce7;color:#166534;}"
"body.dark .status-done{background:#064e3b;color:#34d399;}"
".status-wait{background:#fef9c3;color:#854d0e;}"
"body.dark .status-wait{background:#422006;color:#fbbf24;}"
"@keyframes spin{to{transform:rotate(360deg);}}"
".spinning{display:inline-block;animation:spin 0.8s linear infinite;}"
".status-card{cursor:pointer;transition:transform 0.2s,box-shadow 0.2s,background-color 0.2s,border-color 0.2s,filter 0.2s;user-select:none;}"
".status-card:hover{transform:translateY(-2px);box-shadow:0 6px 16px var(--sh);filter:brightness(1.05);}"
".status-card:active{transform:translateY(0);box-shadow:0 2px 6px var(--sh);}"
"/* Switch & Overlay styling */"
".switch { position: relative; display: inline-block; width: 44px; height: 24px; }"
".switch input { opacity: 0; width: 0; height: 0; }"
".slider { position: absolute; cursor: pointer; top: 0; left: 0; right: 0; bottom: 0; background-color: var(--b); transition: .3s; border-radius: 24px; }"
".slider:before { position: absolute; content: \"\"; height: 18px; width: 18px; left: 3px; bottom: 3px; background-color: #fff; transition: .3s; border-radius: 50%; box-shadow: 0 1px 3px rgba(0,0,0,0.3); }"
"input:checked + .slider { background-color: var(--s); }"
"input:checked + .slider:before { transform: translateX(20px); }"
".perf-overlay { display: none; background: rgba(0,0,0,0.03); border: 2px dashed var(--b); border-radius: 12px; padding: 30px; text-align: center; margin-top: 15px; font-weight: 500; }"
"body.dark .perf-overlay { background: rgba(255,255,255,0.02); }"
"</style></head><body>"
"<div class='layout'>"
"  <div class='sidebar'>"
"    <div class='brand'>Mebieco Device Setup</div>"
"    <div class='menu-item active' onclick='showTab(\"config\", this)'>⚙️ Config</div>"
"    <div class='menu-item' onclick='showTab(\"schedule\", this)'>📅 Schedule</div>"
"    <div class='menu-item' onclick='showTab(\"status\", this)'>📊 Status</div>"
"    <div class='menu-item' onclick='showTab(\"vfd\", this)'>⚡ VFD</div>"
"    <div class='menu-item' onclick='showTab(\"fram\", this)'>💾 FRAM Map</div>"
"    <div class='menu-item' onclick='showTab(\"flash\", this)'>📁 Flash Storage</div>"
"    <div class='menu-item' onclick='showTab(\"info\", this)'>ℹ️ Information</div>"
"    <div class='menu-item' onclick='showTab(\"perf\", this)'>📈 Performance</div>"
"    <div class='menu-item' onclick='showTab(\"console\", this)'>💻 Console</div>"
"    <div class='dark-toggle' onclick='toggleDark()'><span id='dk_icon'>🌙</span> <span id='dk_text'>Dark Mode</span></div>"
"  </div>"
"  <div class='content'>"
"    <div id='page_config' class='page active'>"
"      <div id='cfg_pin_area' class='card' style='text-align:center;padding:20px;border-radius:12px;'>"
"        <p style='margin-top:0;color:var(--t);font-weight:600;'>Authentication Required to Configure</p>"
"        <div class='pw-wrap' style='max-width:300px;margin:10px auto;'>"
"          <input id='cfg_input_pin' type='password' placeholder='Enter PIN' style='text-align:center;'/>"
"          <span class='pw-toggle' onclick='togglePw(this)'>👁️</span>"
"        </div>"
"        <button class='btn-primary' style='max-width:300px;' onclick='unlockCfg()'>Unlock</button>"
"      </div>"
"      <div id='cfg_content' style='display:none;'>"
"      <div class='card'>"
"        <div class='section-title'>📶 WiFi Configuration</div>"
"        <select id='ssid'><option value=''>Scanning...</option></select>"
"        <input id='ssid_manual' placeholder='Hidden SSID (Optional)'/>"
"        <div class='pw-wrap'><input id='pass' type='password' placeholder='WiFi Password'/><span class='pw-toggle' onclick='togglePw(this)'>👁️</span></div>"
"        <div class='btn-group'>"
"          <button class='btn-danger' onclick='clr(\"wifi\")'>Clear</button>"
"          <button class='btn-primary' onclick='save(\"wifi\")'>Save & Restart</button>"
"        </div>"
"        <button class='btn-secondary' onclick='scan()'>Scan Networks</button>"
"      </div>"
"      <div class='card'>"
"        <div class='section-title'>☁️ Azure IoT Hub</div>"
"        <input id='host_name' placeholder='Host Name'/>"
"        <input id='dev_id' placeholder='Device ID'/>"
"        <input id='sym_key' placeholder='Symmetric Key'/>"
"        <div class='btn-group'>"
"          <button class='btn-danger' onclick='clr(\"azure\")'>Clear</button>"
"          <button class='btn-primary' onclick='save(\"azure\")'>Save Config</button>"
"        </div>"
"      </div>"
"      <div class='card'>"
"        <div class='section-title'>⚙️ Cấu hình Hệ thống (System Configuration)</div>"
"        <div style='font-size:13px;margin-bottom:12px;opacity:0.8;'>Thiết lập Chế độ Ao nuôi và trạng thái Biến tần VFD.<br/>"
"        <strong style='color:var(--d);'>Lưu ý: Khi đổi chế độ ao (Pond Mode), toàn bộ lịch trình trong FRAM sẽ bị xóa và ESP sẽ khởi động lại!</strong></div>"
"        "
"        <label style='font-size:13px;font-weight:600;'>Chế độ ao nuôi:</label>"
"        <select id='pond_mode_sel' style='margin-top:6px;margin-bottom:14px;'>"
"          <option value='0'>Mode 10 thiết bị (11,12,13,14,21,22,31,32,41,42)</option>"
"          <option value='1'>Mode 4 thiết bị (11, 12, 21, 41)</option>"
"        </select>"
"        "
"        <label style='font-size:13px;font-weight:600;'>Trạng thái biến tần VFD:</label>"
"        <select id='vfd_enabled_sel' style='margin-top:6px;'>"
"          <option value='1'>Kích hoạt (Enable)</option>"
"          <option value='0'>Vô hiệu hóa (Disable)</option>"
"        </select>"
"        "
"        <div id='sys_cfg_current' style='margin-top:12px;font-size:12px;opacity:0.65;line-height:1.6;'></div>"
"        <button class='btn-primary' style='margin-top:16px;background:#8b5cf6;' onclick='saveSystemConfig()'>Lưu Cấu hình &amp; Khởi động lại</button>"
"      </div>"
"      </div>"
"    </div>"
"    <div id='page_status' class='page'>"
"      <div class='card'>"
"        <div class='section-title'>📊 Live Device Status</div>"
"        <div id='status_grid' style='display:grid; grid-template-columns: repeat(auto-fit, minmax(140px, 1fr)); gap:16px;'>"
"          <!-- Status cards injected by JS -->"
"        </div>"
"        <div style='margin-top:20px; font-size:12px; opacity:0.6; text-align:center;'>Updates automatically every 2 seconds</div>"
"      </div>"
"    </div>"
"    <div id='page_schedule' class='page'>"
"      <div class='card' id='sch_list_container'>"
"        <div class='section-title'>📅 Active Schedules</div>"
"        <div style='display:flex;gap:10px;margin-bottom:12px;'>"
"          <select id='sch_dev_select' onchange='renderSchedules()' style='margin-top:0;'></select>"
"          <button id='sch_reload_btn' class='btn-secondary' style='margin-top:0;width:auto;padding:0 16px;white-space:nowrap;' onclick='loadSchedules(this)'><span>↻</span> Reload</button>"
"        </div>"
"        <div id='sch_list'><p style='text-align:center;color:#6b7280;'>Loading schedules...</p></div>"
"      </div>"
"    </div>"
"    <div id='page_vfd' class='page'>"
"      <div class='card'>"
"        <div class='section-title'>⚡ VFD Monitor (Biến tần GD200A)</div>"
"        <div id='vfd_status_banner' style='display:flex; justify-content:space-between; align-items:center; background:var(--sub); padding:14px 20px; border-radius:12px; margin-bottom:20px; border:1px solid var(--b);'>"
"          <div>"
"            <div style='font-size:12px; opacity:0.6; text-transform:uppercase; font-weight:700;'>Connection</div>"
"            <div id='vfd_conn' style='font-size:18px; font-weight:700; color:var(--d);'>Disconnected</div>"
"          </div>"
"          <div>"
"            <div style='font-size:12px; opacity:0.6; text-transform:uppercase; font-weight:700;'>State</div>"
"            <div id='vfd_running' style='font-size:18px; font-weight:700; color:#64748b;'>STOPPED</div>"
"          </div>"
"          <div>"
"            <div style='font-size:12px; opacity:0.6; text-transform:uppercase; font-weight:700;'>Mode</div>"
"            <div id='vfd_mode' style='font-size:18px; font-weight:700; color:var(--p);'>MANUAL</div>"
"          </div>"
"        </div>"
"        <div id='vfd_fault_banner' style='display:none; background:rgba(239,68,68,0.1); border:1px solid var(--d); padding:12px; border-radius:12px; margin-bottom:20px; color:var(--d); font-size:14px;'>"
"          <strong>⚠️ VFD FAULT!</strong> <span id='vfd_fault_detail'>Error Code 0</span>"
"        </div>"
"        <div style='display:grid; grid-template-columns: repeat(auto-fit, minmax(130px, 1fr)); gap:16px;'>"
"          <div style='background:var(--sub); padding:16px; border-radius:12px; border:1px solid var(--b); text-align:center;'>"
"            <div style='font-size:12px; opacity:0.6; font-weight:600; color:var(--t);'>Actual Frequency</div>"
"            <div id='vfd_act_freq' style='font-size:26px; font-weight:800; color:var(--s); margin:8px 0;'>0.00</div>"
"            <div style='font-size:11px; opacity:0.8;'>Hz</div>"
"          </div>"
"          <div style='background:var(--sub); padding:16px; border-radius:12px; border:1px solid var(--b); text-align:center;'>"
"            <div style='font-size:12px; opacity:0.6; font-weight:600; color:var(--t);'>Set Frequency</div>"
"            <div id='vfd_set_freq' style='font-size:26px; font-weight:800; color:var(--p); margin:8px 0;'>0.00</div>"
"            <div style='font-size:11px; opacity:0.8;'>Hz</div>"
"          </div>"
"          <div style='background:var(--sub); padding:16px; border-radius:12px; border:1px solid var(--b); text-align:center;'>"
"            <div style='font-size:12px; opacity:0.6; font-weight:600; color:var(--t);'>Output Current</div>"
"            <div id='vfd_out_curr' style='font-size:26px; font-weight:800; color:#e11d48; margin:8px 0;'>0.0</div>"
"            <div style='font-size:11px; opacity:0.8;'>A</div>"
"          </div>"
"          <div style='background:var(--sub); padding:16px; border-radius:12px; border:1px solid var(--b); text-align:center;'>"
"            <div style='font-size:12px; opacity:0.6; font-weight:600; color:var(--t);'>Motor Speed</div>"
"            <div id='vfd_motor_speed' style='font-size:26px; font-weight:800; color:#8b5cf6; margin:8px 0;'>0</div>"
"            <div style='font-size:11px; opacity:0.8;'>RPM</div>"
"          </div>"
"          <div style='background:var(--sub); padding:16px; border-radius:12px; border:1px solid var(--b); text-align:center;'>"
"            <div style='font-size:12px; opacity:0.6; font-weight:600; color:var(--t);'>Output Voltage</div>"
"            <div id='vfd_out_volt' style='font-size:22px; font-weight:700; color:var(--t); margin:8px 0;'>0</div>"
"            <div style='font-size:11px; opacity:0.8;'>V</div>"
"          </div>"
"          <div style='background:var(--sub); padding:16px; border-radius:12px; border:1px solid var(--b); text-align:center;'>"
"            <div style='font-size:12px; opacity:0.6; font-weight:600; color:var(--t);'>Bus Voltage</div>"
"            <div id='vfd_bus_volt' style='font-size:22px; font-weight:700; color:var(--t); margin:8px 0;'>0</div>"
"            <div style='font-size:11px; opacity:0.8;'>V</div>"
"          </div>"
"          <div style='background:var(--sub); padding:16px; border-radius:12px; border:1px solid var(--b); text-align:center;'>"
"            <div style='font-size:12px; opacity:0.6; font-weight:600; color:var(--t);'>Output Power</div>"
"            <div id='vfd_out_power' style='font-size:22px; font-weight:700; color:var(--t); margin:8px 0;'>0.0</div>"
"            <div style='font-size:11px; opacity:0.8;'>%</div>"
"          </div>"
"          <div style='background:var(--sub); padding:16px; border-radius:12px; border:1px solid var(--b); text-align:center;'>"
"            <div style='font-size:12px; opacity:0.6; font-weight:600; color:var(--t);'>Daily Energy</div>"
"            <div id='vfd_daily_energy' style='font-size:22px; font-weight:700; color:#06b6d4; margin:8px 0;'>0.0</div>"
"            <div style='font-size:11px; opacity:0.8;'>kWh</div>"
"          </div>"
"          <div style='background:var(--sub); padding:16px; border-radius:12px; border:1px solid var(--b); text-align:center;'>"
"            <div style='font-size:12px; opacity:0.6; font-weight:600; color:var(--t);'>Yesterday Saved</div>"
"            <div id='vfd_last_energy' style='font-size:22px; font-weight:700; color:#8b5cf6; margin:8px 0;'>0.0</div>"
"            <div style='font-size:11px; opacity:0.8;'>kWh</div>"
"          </div>"
"          <div style='background:var(--sub); padding:16px; border-radius:12px; border:1px solid var(--b); text-align:center;'>"
"            <div style='font-size:12px; opacity:0.6; font-weight:600; color:var(--t);'>Total Energy</div>"
"            <div id='vfd_total_energy' style='font-size:22px; font-weight:700; color:#10b981; margin:8px 0;'>0.0</div>"
"            <div style='font-size:11px; opacity:0.8;'>kWh</div>"
"          </div>"
"          <div style='background:var(--sub); padding:16px; border-radius:12px; border:1px solid var(--b); text-align:center;'>"
"            <div style='font-size:12px; opacity:0.6; font-weight:600; color:var(--t);'>Accel / Decel</div>"
"            <div style='font-size:20px; font-weight:700; color:var(--t); margin:8px 0;'><span id='vfd_acc_time'>0</span>s / <span id='vfd_dec_time'>0</span>s</div>"
"            <div style='font-size:11px; opacity:0.8;'>Ramp Times</div>"
"          </div>"
"          <div style='background:var(--sub); padding:16px; border-radius:12px; border:1px solid var(--b); text-align:center;'>"
"            <div style='font-size:12px; opacity:0.6; font-weight:600; color:var(--t);'>Auto Run After Power Back</div>"
"            <div style='font-size:18px; font-weight:700; color:var(--t); margin:8px 0;'><span id='vfd_auto_run_enable'>Disable</span> / <span id='vfd_auto_run_delay'>0</span>s</div>"
"            <div style='font-size:11px; opacity:0.8;'>Power Recovery</div>"
"          </div>"
"        </div>"
"        <div style='display:flex; gap:12px; margin-top:20px;'>"
"          <button class='btn-secondary' style='margin-top:0; flex:1;' onclick='loadVfd(this)'><span>↻</span> Refresh Now</button>"
"        </div>"
"        <div style='margin-top:15px; font-size:11px; opacity:0.5; text-align:center;'>Auto-updates every 3 seconds when this tab is open</div>"
"      </div>"
"    </div>"
"    <div id='page_fram' class='page'>"
"      <div class='card'>"
"        <div class='section-title' style='display:flex; justify-content:space-between; align-items:center;'>"
"          <span>💾 FRAM Memory Map (32KB)</span>"
"          <button class='btn-secondary' style='margin:0; width:auto; padding:6px 12px; font-size:12px;' onclick='loadFramMap(this)'><span>↻</span> Refresh</button>"
"        </div>"
"        <div style='display:grid; grid-template-columns:repeat(auto-fit, minmax(110px, 1fr)); gap:12px; margin-bottom:16px;'>"
"          <div style='background:var(--sub); padding:12px; border-radius:10px; text-align:center; border:1px solid var(--b);'>"
"            <div style='font-size:11px; opacity:0.6;'>Total Capacity</div>"
"            <div id='fram_total_cap' style='font-size:18px; font-weight:700; color:var(--p);'>8 KB</div>"
"          </div>"
"          <div style='background:var(--sub); padding:12px; border-radius:10px; text-align:center; border:1px solid var(--b);'>"
"            <div style='font-size:11px; opacity:0.6;'>Used Memory</div>"
"            <div id='fram_used_bytes' style='font-size:18px; font-weight:700; color:var(--s);'>-</div>"
"          </div>"
"          <div style='background:var(--sub); padding:12px; border-radius:10px; text-align:center; border:1px solid var(--b);'>"
"            <div style='font-size:11px; opacity:0.6;'>Free Space</div>"
"            <div id='fram_free_bytes' style='font-size:18px; font-weight:700; color:#64748b;'>-</div>"
"          </div>"
"        </div>"
"        <div style='margin-bottom:20px;'>"
"          <div style='display:flex; justify-content:space-between; font-size:12px; font-weight:600; margin-bottom:6px;'>"
"            <span>Memory Allocation Bar</span>"
"            <span id='fram_usage_pct'>0%</span>"
"          </div>"
"          <div style='background:var(--sub); height:16px; border-radius:8px; overflow:hidden; display:flex; border:1px solid var(--b);'>"
"            <div id='bar_dev' style='background:#3b82f6; width:0%; transition:0.3s;' title='Devices & Schedules'></div>"
"            <div id='bar_wifi' style='background:#10b981; width:0%; transition:0.3s;' title='WiFi Credentials'></div>"
"            <div id='bar_azure' style='background:#f59e0b; width:0%; transition:0.3s;' title='Azure IoT'></div>"
"            <div id='bar_runtime' style='background:#ec4899; width:0%; transition:0.3s;' title='Daily Runtimes'></div>"
"            <div id='bar_energy' style='background:#06b6d4; width:0%; transition:0.3s;' title='VFD Energy'></div>"
"            <div id='bar_backup' style='background:#f97316; width:0%; transition:0.3s;' title='VFD & Runtimes Backup'></div>"
"            <div id='bar_sys' style='background:#8b5cf6; width:0%; transition:0.3s;' title='System Modes'></div>"
"          </div>"
"          <div style='display:flex; flex-wrap:wrap; gap:10px; font-size:11px; margin-top:8px; opacity:0.8;'>"
"            <span><span style='color:#3b82f6;'>■</span> Devices (5.1KB)</span>"
"            <span><span style='color:#10b981;'>■</span> WiFi (128B)</span>"
"            <span><span style='color:#f59e0b;'>■</span> Azure (256B)</span>"
"            <span><span style='color:#ec4899;'>■</span> Runtimes (40B)</span>"
"            <span><span style='color:#06b6d4;'>■</span> VFD Energy (16B)</span>"
"            <span><span style='color:#f97316;'>■</span> Backup (48B)</span>"
"            <span><span style='color:#8b5cf6;'>■</span> System (22B)</span>"
"            <span><span style='color:#64748b;'>■</span> Free (<span id='fram_free_legend'>-</span>)</span>"
"          </div>"
"        </div>"
"        <div id='fram_blocks_list' style='font-size:13px;'>Loading memory map...</div>"
"      </div>"
"    </div>"
"    <div id='page_info' class='page'>"
"      <div class='card'>"
"        <div class='section-title'>ℹ️ System Information</div>"
"        <div id='pin_area' style='background:var(--sub);padding:20px;border-radius:12px;text-align:center;border:1px solid var(--b);'>"
"          <p style='margin-top:0;color:var(--t);font-weight:600;'>Authentication Required</p>"
"          <div class='pw-wrap' style='max-width:300px;margin:10px auto;'>"
"            <input id='view_pin' type='password' placeholder='Enter PIN' style='text-align:center;'/>"
"            <span class='pw-toggle' onclick='togglePw(this)'>👁️</span>"
"          </div>"
"          <button class='btn-secondary' style='max-width:300px;' onclick='unlockInfo()'>Unlock</button>"
"        </div>"
"        <div id='info_res' style='display:none;'>"
"          <div style='background:var(--sub);padding:16px;border-radius:12px;border:1px solid var(--b);'>"
"            <div class='info-item'><span>Firmware:</span><strong id='cur_ver' style='color:var(--p);'>-</strong></div>"
"            <div class='info-item'><span>Local Domain:</span><strong id='cur_mdns' style='color:var(--s);'>-</strong></div>"
"            <div class='info-item'><span>WiFi SSD:</span><strong id='cur_wifi' style='color:var(--p);'>-</strong></div>"
"            <div class='info-item'><span>WiFi Pass:</span><strong id='cur_wifipass' style='color:var(--d);'>-</strong></div>"
"            <div class='info-item'><span>Azure Host:</span><strong id='cur_host' style='color:var(--p);word-break:break-all;'>-</strong></div>"
"            <div class='info-item'><span>Device ID:</span><strong id='cur_dev' style='color:var(--p);'>-</strong></div>"
"            <div class='info-item'><span>Azure Key:</span><strong id='cur_sym' style='color:var(--d);word-break:break-all;font-size:11px;'>-</strong></div>"
"            <div class='info-item'><span>Oxy Mode:</span><strong id='cur_oxy' style='color:var(--p);'>-</strong></div>"
"            <div class='info-item'><span>Feeder Mode:</span><strong id='cur_feeder' style='color:var(--p);'>-</strong></div>"
"            <div class='info-item'><span>Pond Mode:</span><strong id='cur_pond' style='color:var(--p);'>-</strong></div>"
"            <div class='info-item'><span>VFD Enable:</span><strong id='cur_vfd' style='color:var(--s);'>-</strong></div>"
"          </div>"
"          <div style='display:flex;flex-wrap:wrap;gap:12px;margin-top:16px;'>"
"            <button class='btn-secondary' style='margin-top:0;flex:1;' onclick='unlockInfo()'>↻ Refresh</button>"
"            <button class='btn-primary' style='margin-top:0;background:#8b5cf6;flex:1;' onclick='searchDev()'>🔍 Search Device</button>"
"            <button class='btn-danger' style='margin-top:0;flex:1;' onclick='restartEsp()'>🔄 Restart ESP</button>"
"          </div>"
"        </div>"
"      </div>"
"    </div>"
"    <div id='page_flash' class='page'>"
"      <div class='card' style='padding:12px;'>"
"        <iframe src='/update_web' style='width:100%; height:700px; border:none; border-radius:12px;'></iframe>"
"      </div>"
"    </div>"
"    <div id='page_perf' class='page' style='max-width:800px;'>"
"      <div class='card' style='display:flex; justify-content:space-between; align-items:center; padding:15px 20px; margin-bottom:15px; color:var(--t);'>"
"        <div style='display:flex; flex-direction:column; gap:4px;'>"
"          <strong style='font-size:15px;'>📈 Giám sát hiệu năng (Performance Monitor)</strong>"
"          <span style='font-size:11px; opacity:0.6;'>Bật chức năng này để theo dõi biểu đồ RAM và danh sách Task trực quan.</span>"
"        </div>"
"        <label class='switch'>"
"          <input type='checkbox' id='perf_toggle_switch' onchange='togglePerfMonitor(this)'>"
"          <span class='slider'></span>"
"        </label>"
"      </div>"
"      <div id='perf_disabled_card' class='card' style='padding:40px 20px; text-align:center; display:none;'>"
"        <span style='font-size:32px;'>⚠️</span>"
"        <h3 style='margin:10px 0 5px 0; font-size:16px;'>Chức năng giám sát hiệu năng đang tắt</h3>"
"        <p style='font-size:13px; opacity:0.7; margin:0;'>Để tiết kiệm RAM hệ thống khi chạy thực tế, tác vụ quét hiệu năng chi tiết đã được tạm dừng. Vui lòng gạt công tắc phía trên để kích hoạt.</p>"
"      </div>"
"      <div id='perf_main_card' class='card' style='display:flex; flex-wrap:wrap; gap:20px; padding:20px; min-height:400px; color:var(--t);'>"
"        <div id='perf_sidebar' style='flex:1 1 180px; display:flex; flex-direction:column; gap:10px; border-right:1px solid var(--b); padding-right:15px; max-width:200px;'>"
"          <div id='btn_perf_internal' class='menu-item active' style='margin-bottom:0; justify-content:space-between; padding:8px 12px; font-size:13px;' onclick='selectPerfTab(\"internal\")'>"
"            <span>SRAM (Nội bộ)</span>"
"            <span id='sidebar_pct_internal'>0%</span>"
"          </div>"
"          <div id='btn_perf_external' class='menu-item' style='margin-bottom:0; justify-content:space-between; padding:8px 12px; font-size:13px;' onclick='selectPerfTab(\"external\")'>"
"            <span>PSRAM (Ngoại vi)</span>"
"            <span id='sidebar_pct_external'>0%</span>"
"          </div>"
"        </div>"
"        <div style='flex:2 1 350px; display:flex; flex-direction:column; gap:15px; min-width:0;'>"
"          <div style='display:flex; justify-content:space-between; align-items:center;'>"
"            <strong style='font-size:16px; color:var(--t);' id='perf_title'>SRAM (Bộ nhớ nội bộ)</strong>"
"            <span style='font-size:11px; opacity:0.6;' id='perf_update_time'>Real-time</span>"
"          </div>"
"          <div style='background:#0f172a; border-radius:8px; padding:10px; border:1px solid #1e293b; overflow:hidden;'>"
"            <canvas id='perfCanvas' width='450' height='200' style='width:100%; height:200px; display:block;'></canvas>"
"          </div>"
"          <div>"
"            <div style='display:flex; justify-content:space-between; font-size:11px; font-weight:600; margin-bottom:4px;'>"
"              <span>Phân bổ bộ nhớ</span>"
"              <span id='perf_bar_text'>0% Used</span>"
"            </div>"
"            <div style='background:var(--b); height:12px; border-radius:6px; overflow:hidden; border:1px solid var(--b);'>"
"              <div id='perf_bar_used' style='background:#8b5cf6; width:0%; height:100%; transition:0.3s;'></div>"
"            </div>"
"          </div>"
"          <div style='display:grid; grid-template-columns:1fr 1fr; gap:10px; font-size:13px;'>"
"            <div style='border-bottom:1px dashed var(--b); padding:4px 0; display:flex; justify-content:space-between;'>"
"              <span style='opacity:0.6;'>Đang sử dụng:</span>"
"              <strong id='perf_val_used'>0 KB</strong>"
"            </div>"
"            <div style='border-bottom:1px dashed var(--b); padding:4px 0; display:flex; justify-content:space-between;'>"
"              <span style='opacity:0.6;'>Khả dụng (Trống):</span>"
"              <strong id='perf_val_free'>0 KB</strong>"
"            </div>"
"            <div style='border-bottom:1px dashed var(--b); padding:4px 0; display:flex; justify-content:space-between;'>"
"              <span style='opacity:0.6;'>Cực cận trống:</span>"
"              <strong id='perf_val_min'>0 KB</strong>"
"            </div>"
"            <div style='border-bottom:1px dashed var(--b); padding:4px 0; display:flex; justify-content:space-between;'>"
"              <span style='opacity:0.6;'>Tổng dung lượng:</span>"
"              <strong id='perf_val_total'>0 KB</strong>"
"            </div>"
"          </div>"
"        </div>"
"      </div>"
"      <div id='perf_tasks_card' class='card' style='margin-top:20px; padding:20px; color:var(--t);'>"
"        <div style='display:flex; justify-content:space-between; align-items:center; border-bottom:1px solid var(--b); padding-bottom:10px; margin-bottom:15px;'>"
"          <strong style='font-size:16px;'>📋 Chi tiết các tác vụ (Active Tasks)</strong>"
"        </div>"
"        <div style='overflow-x:auto;'>"
"          <table style='width:100%; border-collapse:collapse; font-size:13px; text-align:left;'>"
"            <thead>"
"              <tr style='border-bottom:2px solid var(--b); opacity:0.8;'>"
"                <th style='padding:8px 4px;'>Tên Task</th>"
"                <th style='padding:8px 4px; text-align:center;'>Core</th>"
"                <th style='padding:8px 4px; text-align:center;'>Độ ưu tiên</th>"
"                <th style='padding:8px 4px;'>Vùng nhớ Stack</th>"
"                <th style='padding:8px 4px; text-align:right;'>Allocated Stack</th>"
"                <th style='padding:8px 4px; text-align:right;'>Stack đã dùng (Max)</th>"
"                <th style='padding:8px 4px; text-align:right;'>Tỷ lệ sử dụng</th>"
"              </tr>"
"            </thead>"
"            <tbody id='perf_tasks_list'>"
"              <tr><td colspan='7' style='text-align:center; padding:20px; opacity:0.5;'>Đang tải dữ liệu tác vụ...</td></tr>"
"            </tbody>"
"          </table>"
"        </div>"
"      </div>"
"    </div>"
"    <div id='page_console' class='page' style='max-width:1200px;'>"
"      <div class='card' style='display:flex; justify-content:space-between; align-items:center; padding:15px 20px; margin-bottom:15px; color:var(--t);'>"
"        <div style='display:flex; flex-direction:column; gap:4px;'>"
"          <strong style='font-size:15px;'>💻 Live System Console</strong>"
"          <span style='font-size:11px; opacity:0.6;'>Bật chức năng này để ghi và theo dõi log hệ thống thời gian thực.</span>"
"        </div>"
"        <label class='switch'>"
"          <input type='checkbox' id='console_toggle_switch' onchange='toggleConsoleMonitor(this)'>"
"          <span class='slider'></span>"
"        </label>"
"      </div>"
"      <div id='console_disabled_card' class='card' style='padding:40px 20px; text-align:center; display:none;'>"
"        <span style='font-size:32px;'>⚠️</span>"
"        <h3 style='margin:10px 0 5px 0; font-size:16px;'>Chức năng Live Console đang tắt</h3>"
"        <p style='font-size:13px; opacity:0.7; margin:0;'>Để tiết kiệm RAM và giảm tải CPU xử lý log, Live Console đã được tạm dừng. Vui lòng gạt công tắc phía trên để kích hoạt.</p>"
"      </div>"
"      <div id='console_main_card' class='card' style='background:#0f172a; border-radius:16px; padding:20px; color:#f8fafc; border:1px solid #334155; display:flex; flex-direction:column; gap:15px; height:750px; box-sizing:border-box;'>"
"        <div style='display:flex; justify-content:space-between; align-items:center; border-bottom:1px solid #1e293b; padding-bottom:10px;'>"
"          <strong style='font-size:16px; color:#38bdf8; display:flex; align-items:center; gap:8px;'>💻 Live System Console</strong>"
"          <div style='display:flex; gap:8px;'>"
"            <button class='btn-secondary' style='margin-top:0; background:#334155; font-size:12px; padding:6px 12px; width:auto; border:1px solid #475569;' onclick='clearConsole()'>Clear</button>"
"            <button id='btn_pause_log' class='btn-primary' style='margin-top:0; font-size:12px; padding:6px 12px; width:auto;' onclick='togglePauseConsole()'>Pause</button>"
"            <button class='btn-secondary' style='margin-top:0; background:#059669; font-size:12px; padding:6px 12px; width:auto;' onclick='downloadConsoleLog()'>Download</button>"
"          </div>"
"        </div>"
"        <div id='console_output' style='flex:1; overflow-y:auto; font-family:Consolas, Monaco, \"Courier New\", monospace; font-size:13.5px; -webkit-font-smoothing:antialiased; -moz-osx-font-smoothing:grayscale; line-height:1.5; white-space:pre-wrap; word-break:break-all; background:#020617; border-radius:8px; padding:15px; border:1px solid #1e293b; color:#cbd5e1;'>"
"          <span style='color:#64748b;'>-- System log stream started --</span>"
"        </div>"
"        <div style='display:flex; justify-content:space-between; font-size:11px; opacity:0.6; padding-top:5px; border-top:1px solid #1e293b;'>"
"          <span id='console_scroll_status' style='color:#34d399;'>Auto-scroll: Active</span>"
"          <span id='console_line_count'>Lines: 1</span>"
"        </div>"
"      </div>"
"    </div>"
"  </div>"
"</div>"
"<script>"
"let global_pin = '';"
"const f=(t)=>{const d=new Date(t*1000);return `${d.getHours().toString().padStart(2,'0')}:${d.getMinutes().toString().padStart(2,'0')} (${d.getDate()}/${d.getMonth()+1})`;};"
"function showTab(id, el){"
"  document.querySelectorAll('.page').forEach(p=>p.classList.remove('active'));"
"  document.getElementById('page_'+id).classList.add('active');"
"  document.querySelectorAll('.menu-item').forEach(m=>m.classList.remove('active'));"
"  el.classList.add('active');"
"  if(window.statusInterval) { clearInterval(window.statusInterval); window.statusInterval = null; }"
"  if(window.vfdInterval) { clearInterval(window.vfdInterval); window.vfdInterval = null; }"
"  if(window.perfInterval) { clearInterval(window.perfInterval); window.perfInterval = null; }"
"  if(window.consoleInterval) { clearInterval(window.consoleInterval); window.consoleInterval = null; }"
"  if(id==='schedule') loadSchedules(document.getElementById('sch_reload_btn'));"
"  if(id==='status') { loadStatus(); window.statusInterval = setInterval(loadStatus, 2000); }"
"  if(id==='vfd') { loadVfd(); window.vfdInterval = setInterval(loadVfd, 3000); }"
"  if(id==='fram') loadFramMap();"
"  if(id==='perf') startPerfMonitor();"
"  if(id==='console') startConsoleMonitor();"
"}"
"function loadFramMap(btn){"
"  const icon=btn?btn.querySelector('span'):null; if(icon)icon.classList.add('spinning');"
"  fetch('/api/fram/map').then(r=>r.json()).then(d=>{"
"    const total = d.total_capacity || 8192;"
"    const used = d.used_bytes || 0;"
"    const free = d.free_bytes || 0;"
"    const pct = ((used / total) * 100).toFixed(1);"
"    document.getElementById('fram_total_cap').innerText = (total / 1024).toFixed(0) + ' KB';"
"    document.getElementById('fram_used_bytes').innerText = (used / 1024).toFixed(2) + ' KB';"
"    document.getElementById('fram_free_bytes').innerText = (free / 1024).toFixed(1) + ' KB';"
"    document.getElementById('fram_usage_pct').innerText = pct + '% Used';"
"    document.getElementById('fram_free_legend').innerText = (free / 1024).toFixed(1) + ' KB';"
"    document.getElementById('bar_dev').style.width = ((5120 / total) * 100) + '%';"
"    document.getElementById('bar_wifi').style.width = ((128 / total) * 100) + '%';"
"    document.getElementById('bar_azure').style.width = ((256 / total) * 100) + '%';"
"    document.getElementById('bar_runtime').style.width = ((40 / total) * 100) + '%';"
"    document.getElementById('bar_energy').style.width = ((16 / total) * 100) + '%';"
"    document.getElementById('bar_backup').style.width = ((48 / total) * 100) + '%';"
"    document.getElementById('bar_sys').style.width = ((22 / total) * 100) + '%';"
"    let html = '';"
"    d.blocks.forEach(b => {"
"      let icon = '📦'; let color = 'var(--p)';"
"      if(b.type === 'devices') { icon = '🏷️'; color = '#3b82f6'; }"
"      else if(b.type === 'wifi') { icon = '📶'; color = '#10b981'; }"
"      else if(b.type === 'azure') { icon = '☁️'; color = '#f59e0b'; }"
"      else if(b.type === 'runtime') { icon = '⏱️'; color = '#ec4899'; }"
"      else if(b.type === 'energy') { icon = '⚡'; color = '#06b6d4'; }"
"      else if(b.type === 'backup') { icon = '💾'; color = '#f97316'; }"
"      else if(b.type === 'system') { icon = '⚙️'; color = '#8b5cf6'; }"
"      else if(b.type === 'free') { icon = '⏹️'; color = '#64748b'; }"
"      html += `<div style=\"background:var(--sub); border:1px solid var(--b); border-radius:12px; padding:16px; margin-bottom:14px;\">"
"        <div style=\"display:flex; justify-content:space-between; align-items:center; border-bottom:1px solid var(--b); padding-bottom:8px; margin-bottom:10px;\">"
"          <div style=\"font-weight:700; color:${color}; font-size:15px;\">${icon} ${b.name}</div>"
"          <div style=\"font-family:monospace; font-weight:700; font-size:12px; background:var(--c); padding:2px 8px; border-radius:6px; border:1px solid var(--b);\">${b.addr} (${b.size} B)</div>"
"        </div>`;"
"      if(b.type === 'devices') {"
"        html += `<div style=\"display:grid; grid-template-columns:repeat(auto-fit, minmax(230px, 1fr)); gap:10px;\">`;"
"        b.items.forEach(item => {"
"          html += `<div style=\"background:var(--c); padding:10px; border-radius:8px; border:1px solid var(--b); font-size:12px;\">"
"            <div style=\"display:flex; justify-content:space-between; font-weight:700; color:var(--t);\">"
"              <span>${item.name} (ID: ${item.id})</span>"
"              <span style=\"font-family:monospace; color:#64748b; font-size:10px;\">Slot ${item.index}</span>"
"            </div>"
"            <div style=\"margin-top:4px; opacity:0.8; font-size:11px;\">Addr: <code>${item.addr}</code></div>"
"            <div style=\"margin-top:4px; font-weight:600;\">Schedules: <span style=\"color:var(--p);\">${item.sch_count}</span> active</div>`;"
"          if(item.schedules && item.schedules.length > 0) {"
"            html += `<div style=\"margin-top:6px; background:var(--sub); padding:6px; border-radius:6px; font-size:10px;\">`;"
"            item.schedules.forEach(s => {"
"              html += `<div>#${s.idx}: ${f(s.start)} → ${f(s.stop)} (${s.done?'Done':'Wait'})</div>`;"
"            });"
"            html += `</div>`;"
"          }"
"          html += `</div>`;"
"        });"
"        html += `</div>`;"
"      }"
"      else if(b.type === 'wifi') {"
"        html += `<div style=\"font-size:13px;\"><div>SSID: <strong>${b.ssid}</strong></div><div>Status: <span style=\"color:${b.valid?'var(--s)':'var(--d)'}; font-weight:700;\">${b.valid?'Valid (Configured)':'Empty'}</span></div></div>`;"
"      }"
"      else if(b.type === 'azure') {"
"        html += `<div style=\"font-size:13px;\"><div>Host: <strong>${b.host}</strong></div><div>Device ID: <strong>${b.dev_id||'-'}</strong></div><div>Status: <span style=\"color:${b.valid?'var(--s)':'var(--d)'}; font-weight:700;\">${b.valid?'Valid (Configured)':'Empty'}</span></div></div>`;"
"      }"
"      else if(b.type === 'system') {"
"        html += `<div style=\"font-size:13px; display:grid; grid-template-columns:repeat(auto-fit, minmax(180px, 1fr)); gap:8px;\"><div>🏖️ Pond Mode: <strong style=\"color:var(--p);\">${b.pond_mode===1?'Mode 4 thiết bị':'Mode 10 thiết bị'}</strong></div><div>⚡ VFD Enabled: <strong style=\"color:var(--s);\">${b.vfd_enabled===1?'Enable':'Disable'}</strong></div><div>⚙️ Feeder Mode: <strong>${b.feeder_mode===1?'1 Contactor':'2 Contactor'}</strong> (ID: ${b.feeder_active_id})</div><div>💨 Oxy Mode: <strong>${b.oxy_mode===1?'1 Máy sục khí':'2 Máy sục khí'}</strong> (ID: ${b.oxy_active_id})</div></div>`;"
"      }"
"      else if(b.type === 'free') {"
"        html += `<div style=\"font-size:12px; opacity:0.7;\">Unallocated space ready for future feature expansion</div>`;"
"      }"
"      html += `</div>`;"
"    });"
"    document.getElementById('fram_blocks_list').innerHTML = html;"
"  }).catch(e=>{"
"    document.getElementById('fram_blocks_list').innerHTML = '<p style=\"color:var(--d)\">Failed to load FRAM memory map</p>';"
"  }).finally(()=>{if(icon)icon.classList.remove('spinning');});"
"}"
"function unlockInfo(){"
"  const pin=document.getElementById('view_pin').value; if(!pin) return alert('Enter PIN');"
"  fetch('/info?pin='+pin).then(r=>{if(r.status!==200)throw new Error('Invalid PIN'); return r.json();}).then(d=>{"
"    global_pin=pin;"
"    document.getElementById('cur_ver').innerText=d.version||'Not Set';"
"    document.getElementById('cur_mdns').innerText=d.mdns||'Not Set';"
"    document.getElementById('cur_wifi').innerText=d.wifi||'Not Set';"
"    document.getElementById('cur_wifipass').innerText=d.wifipass||'Not Set';"
"    document.getElementById('cur_host').innerText=d.host||'Not Set';"
"    document.getElementById('cur_dev').innerText=d.dev||'Not Set';"
"    document.getElementById('cur_sym').innerText=d.sym||'Not Set';"
"    document.getElementById('cur_oxy').innerText=d.oxy_mode===1?'1 Máy sục khí (ID: '+d.oxy_active_id+')':'2 Máy sục khí';"
"    document.getElementById('cur_feeder').innerText=d.feeder_mode===1?'1 Contactor (ID: '+d.feeder_active_id+')':'2 Contactor';"
"    document.getElementById('cur_pond').innerText=d.pond_mode===1?'Mode 4 thiết bị':'Mode 10 thiết bị';"
"    document.getElementById('cur_vfd').innerText=d.vfd_enabled===1?'Enabled':'Disabled';"
"    document.getElementById('pin_area').style.display='none'; document.getElementById('info_res').style.display='block';"
"  }).catch(e=>alert(e.message));"
"}"
"window.statusInterval = null;"
"window.vfdInterval = null;"
"function loadVfd(btn){"
"  const icon=btn?btn.querySelector('span'):null; if(icon)icon.classList.add('spinning');"
"  fetch('/api/vfd').then(r=>r.json()).then(d=>{"
"    if(d.connected){"
"      document.getElementById('vfd_conn').innerText = 'Connected';"
"      document.getElementById('vfd_conn').style.color = 'var(--s)';"
"      document.getElementById('vfd_running').innerText = d.running ? (d.fwd ? 'RUNNING FWD' : 'RUNNING REV') : 'STOPPED';"
"      document.getElementById('vfd_running').style.color = d.running ? 'var(--s)' : '#64748b';"
"      document.getElementById('vfd_mode').innerText = d.mode.toUpperCase();"
"      document.getElementById('vfd_act_freq').innerText = d.actual_freq.toFixed(2);"
"      document.getElementById('vfd_set_freq').innerText = d.set_freq.toFixed(2);"
"      document.getElementById('vfd_out_curr').innerText = d.output_current.toFixed(1);"
"      document.getElementById('vfd_motor_speed').innerText = d.motor_speed;"
"      document.getElementById('vfd_out_volt').innerText = d.output_voltage;"
"      document.getElementById('vfd_bus_volt').innerText = d.bus_voltage.toFixed(1);"
"      document.getElementById('vfd_out_power').innerText = d.output_power.toFixed(1);"
"      document.getElementById('vfd_daily_energy').innerText = d.vfd_daily_energy.toFixed(1);"
"      document.getElementById('vfd_last_energy').innerText = d.vfd_last_energy.toFixed(1);"
"      document.getElementById('vfd_total_energy').innerText = d.vfd_total_energy >= 0 ? d.vfd_total_energy.toFixed(1) : '-';"
"      document.getElementById('vfd_acc_time').innerText = d.accel_time.toFixed(1);"
"      document.getElementById('vfd_dec_time').innerText = d.decel_time.toFixed(1);"
"      document.getElementById('vfd_auto_run_enable').innerText = d.auto_run_enable ? 'Enable' : 'Disable';"
"      document.getElementById('vfd_auto_run_enable').style.color = d.auto_run_enable ? 'var(--s)' : '#64748b';"
"      document.getElementById('vfd_auto_run_delay').innerText = d.auto_run_delay.toFixed(1);"
"      if(d.fault){"
"        document.getElementById('vfd_fault_banner').style.display = 'block';"
"        document.getElementById('vfd_fault_detail').innerText = d.fault_str + ' (Code: ' + d.fault_code + ')';"
"      } else {"
"        document.getElementById('vfd_fault_banner').style.display = 'none';"
"      }"
"    } else {"
"      document.getElementById('vfd_conn').innerText = 'Disconnected';"
"      document.getElementById('vfd_conn').style.color = 'var(--d)';"
"      document.getElementById('vfd_running').innerText = 'UNKNOWN';"
"      document.getElementById('vfd_running').style.color = '#64748b';"
"      document.getElementById('vfd_mode').innerText = 'UNKNOWN';"
"      document.getElementById('vfd_daily_energy').innerText = d.vfd_daily_energy !== undefined ? d.vfd_daily_energy.toFixed(1) : '-';"
"      document.getElementById('vfd_last_energy').innerText = d.vfd_last_energy !== undefined ? d.vfd_last_energy.toFixed(1) : '-';"
"      document.getElementById('vfd_total_energy').innerText = '-';"
"      document.getElementById('vfd_fault_banner').style.display = 'block';"
"      document.getElementById('vfd_fault_detail').innerText = d.message || 'Communication Error';"
"    }"
"  }).catch(e=>{"
"    document.getElementById('vfd_conn').innerText = 'Error';"
"    document.getElementById('vfd_conn').style.color = 'var(--d)';"
"  }).finally(()=>{if(icon)icon.classList.remove('spinning');});"
"}"
"function loadStatus(){"
"  fetch('/api/status').then(r=>r.json()).then(data=>{"
"    const grid = document.getElementById('status_grid');"
"    if(!grid) return;"
"    grid.innerHTML = '';"
"    data.forEach(dev => {"
"      const isOn = dev.state === 1;"
"      const isFault = dev.state === 2;"
"      const color = isFault ? 'var(--d)' : (isOn ? 'var(--s)' : '#64748b');"
"      const bg = isFault ? 'rgba(239,68,68,0.1)' : (isOn ? 'rgba(16,185,129,0.1)' : 'var(--bg)');"
"      const pausedColor = dev.isSchedulePaused ? 'var(--d)' : 'var(--s)';"
"      const rtSec = dev.runtime || 0;"
"      const hours = Math.floor(rtSec / 3600);"
"      const minutes = Math.floor((rtSec % 3600) / 60);"
"      const runtimeStr = hours + 'h ' + minutes + 'p';"
"      grid.innerHTML += `<div class=\"status-card\" style=\"background:${bg}; padding:16px; border-radius:12px; border:2px solid ${color}; text-align:center;\" onclick=\"toggleDev(${dev.id}, ${isOn ? 0 : 1})\">"
"        <div style=\"font-size:24px; margin-bottom:8px;\">${isFault?'⚠️':(isOn?'⚡':'💤')}</div>"
"        <div style=\"font-weight:700; font-size:13px; color:var(--t);\">${dev.name}</div>"
"        <div style=\"font-size:11px; margin-top:4px; font-weight:800; color:${color};\">${isFault?'FAULT':(isOn?'RUNNING':'OFF')}</div>"
"        <div style=\"font-size:10px; margin-top:6px; opacity:0.8; color:var(--t);\">isSchedulePaused: <span style=\"color:${pausedColor};font-weight:bold;\">${dev.isSchedulePaused ? 'True' : 'False'}</span></div>"
"        <div style=\"font-size:10px; margin-top:4px; opacity:0.8; color:var(--t);\">Chạy trong ngày: <span style=\"font-weight:bold; color:var(--p);\">${runtimeStr}</span></div>"
"      </div>`;"
"    });"
"  }).catch(()=>{});"
"}"
"function toggleDev(id, val){"
"  fetch('/api/control',{"
"    method:'POST',"
"    body:JSON.stringify({Data:{DeviceId:id,Value:val}})"
"  }).then(r=>{"
"    if(r.ok) loadStatus();"
"  });"
"}"
"function unlockCfg(){"
"  const pin=document.getElementById('cfg_input_pin').value; if(!pin) return alert('Enter PIN');"
"  fetch('/info?pin='+pin).then(r=>{if(r.status!==200)throw new Error('Invalid PIN'); return r.json();}).then(()=>{global_pin=pin; document.getElementById('cfg_pin_area').style.display='none'; document.getElementById('cfg_content').style.display='block';}).catch(e=>alert(e.message));"
"}"
"let allSchedules = [];"
"function loadSchedules(btn){"
"  const icon=btn?btn.querySelector('span'):null; if(icon)icon.classList.add('spinning');"
"  const sel=document.getElementById('sch_dev_select'); const prev=sel.value;"
"  fetch('/api/schedules').then(r=>r.json()).then(data=>{"
"    allSchedules = data; sel.innerHTML='';"
"    if(data.length===0){document.getElementById('sch_list').innerHTML='<p style=\"text-align:center;color:#6b7280;\">No schedules found</p>'; sel.style.display='none'; return;}"
"    sel.style.display='block';"
"    data.forEach((dev, index)=>{"
"      sel.innerHTML+=`<option value=\"${index}\">${dev.name} (ID: ${dev.id})</option>`;"
"    });"
"    if(prev!=='' && data[prev]) sel.value=prev;"
"    renderSchedules();"
"  }).catch(()=>document.getElementById('sch_list').innerHTML='<p style=\"color:red\">Failed to load</p>')"
"  .finally(()=>{if(icon)icon.classList.remove('spinning');});"
"}"
"function renderSchedules(){"
"  const devIdx = document.getElementById('sch_dev_select').value;"
"  if(!allSchedules[devIdx]) return;"
"  const dev = allSchedules[devIdx];"
"  const root = document.getElementById('sch_list');"
"  let h=`<div style=\"background:var(--sub);padding:12px;border-radius:8px;border:1px solid var(--b);overflow-x:auto;\"><table class=\"sch-table\"><tr><th>Idx</th><th>Run(s)</th><th>Pause(s)</th><th>Start</th><th>End</th><th>Status</th></tr>`;"
"  if(dev.items.length===0) h+='<tr><td colspan=\"6\" style=\"text-align:center;opacity:0.6\">No active schedules</td></tr>';"
"  else dev.items.forEach(s=>{"
"    h+=`<tr><td>${s.idx}</td><td>${s.run}</td><td>${s.pause}</td><td>${f(s.start)}</td><td>${f(s.stop)}</td><td><span class=\"status-tag ${s.done?'status-done':'status-wait'}\">${s.done?'Done':'Wait'}</span></td></tr>`;"
"  });"
"  root.innerHTML = h+'</table></div>';"
"}"
"function scan(){fetch('/scan').then(r=>r.json()).then(d=>{const s=document.getElementById('ssid');s.innerHTML='';d.forEach(n=>s.innerHTML+=`<option value=\"${n.ssid}\">${n.ssid} (${n.rssi}dBm)</option>`);});}"
"function save(type){"
"  const pin=global_pin; if(!pin) return alert('Not unlocked!');"
"  const d=type==='wifi'?{ssid:document.getElementById('ssid').value||document.getElementById('ssid_manual').value,pass:document.getElementById('pass').value,pin:pin}:{host:document.getElementById('host_name').value,dev:document.getElementById('dev_id').value,key:document.getElementById('sym_key').value,pin:pin};"
"  fetch('/save_'+type,{method:'POST',body:JSON.stringify(d)}).then(r=>r.ok?alert('Saved!'):alert('Error/Invalid PIN'));"
"}"
"function clr(t){"
"  const pin=global_pin; if(!pin) return alert('Not unlocked!');"
"  if(confirm('Clear '+t+'?'))fetch('/clear_'+t,{method:'POST',body:JSON.stringify({pin:pin})}).then(()=>alert('Cleared'));"
"}"
"function togglePw(btn){const i=btn.previousElementSibling;i.type=i.type==='password'?'text':'password';}"
"function toggleDark(){"
"  const d = document.body.classList.toggle('dark');"
"  localStorage.setItem('theme', d?'dark':'light');"
"  document.getElementById('dk_icon').innerText = d?'☀️':'🌙';"
"  document.getElementById('dk_text').innerText = d?'Light Mode':'Dark Mode';"
"}"
"function searchDev(){"
"  fetch('/search',{method:'POST'}).then(r=>{if(r.ok)alert('Đã phát tín hiệu!\\nĐèn LED tín hiệu trên thiết bị này sẽ SÁNG LIÊN TỤC trong 5 giây.\\n👉 Anh vui lòng kiểm tra tại ao.'); else alert('Có lỗi xảy ra khi gửi lệnh!');});"
"}"
"function restartEsp(){"
"  const pin=global_pin; if(!pin) return alert('Chưa nhập mã PIN!');"
"  if(confirm('Anh có chắc chắn muốn khởi động lại thiết bị không?')){"
"    fetch('/restart',{method:'POST',body:JSON.stringify({pin:pin})}).then(r=>{"
"      if(r.ok) alert('Đang khởi động lại thiết bị...');"
"      else alert('Lỗi: PIN không hợp lệ hoặc kết nối thất bại!');"
"    });"
"  }"
"}"
"if(localStorage.getItem('theme')==='dark')toggleDark();"
"scan();"
"fetch('/api/config').then(r=>r.json()).then(d=>{"
"  if(d.pond_mode !== undefined && d.vfd_enabled !== undefined) {"
"    document.getElementById('pond_mode_sel').value = d.pond_mode;"
"    document.getElementById('vfd_enabled_sel').value = d.vfd_enabled;"
"    const pondStr = d.pond_mode===1 ? 'Mode 4 thiết bị (11,12,21,41)' : 'Mode 10 thiết bị (đầy đủ)';"
"    const vfdStr = d.vfd_enabled===1 ? 'Đang kích hoạt (Enable)' : 'Đang vô hiệu hóa (Disable)';"
"    document.getElementById('sys_cfg_current').innerHTML = `Hiện tại:<br/>• Ao nuôi: <strong>${pondStr}</strong><br/>• Biến tần: <strong>${vfdStr}</strong>`;"
"  }"
"}).catch(()=>{});"
"function saveSystemConfig(){"
"  const pin=global_pin; if(!pin) return alert('Chưa nhập mã PIN Config!');"
"  const pondMode=parseInt(document.getElementById('pond_mode_sel').value);"
"  const vfdEnabled=parseInt(document.getElementById('vfd_enabled_sel').value);"
"  const pondStr = pondMode===1 ? 'Mode 4 thiết bị (11,12,21,41)' : 'Mode 10 thiết bị (đầy đủ)';"
"  const vfdStr = vfdEnabled===1 ? 'Kích hoạt (Enable)' : 'Vô hiệu hóa (Disable)';"
"  let warning = `Anh có chắc muốn lưu cấu hình mới và khởi động lại ESP?\\n\\n• Chế độ ao: ${pondStr}\\n• Biến tần: ${vfdStr}\\n\\n⚠️ LƯU Ý: Nếu thay đổi Chế độ ao nuôi, toàn bộ lịch trình trong FRAM sẽ bị XÓA!`;"
"  if(!confirm(warning)) return;"
"  fetch('/api/set_system_config',{method:'POST',body:JSON.stringify({pond_mode:pondMode,vfd_enabled:vfdEnabled,pin:pin})}).then(r=>{"
"    if(r.ok) {"
"      r.text().then(t => {"
"        if(t==='OK_NO_CHANGE') alert('Không có thay đổi nào cần lưu.');"
"        else alert('Đã lưu! ESP đang khởi động lại...');"
"      });"
"    }"
"    else r.text().then(t=>alert('Lỗi: ' + t));"
"  }).catch(()=>alert('Kết nối thất bại!'));"
"}"
"window.perfInterval = null;"
"window.currentPerfTab = 'internal';"
"window.perfHistory = { internal: [], external: [] };"
"const MAX_HISTORY = 30;"
"function selectPerfTab(tab) {"
"  window.currentPerfTab = tab;"
"  document.getElementById('btn_perf_internal').classList.toggle('active', tab === 'internal');"
"  document.getElementById('btn_perf_external').classList.toggle('active', tab === 'external');"
"  const title = tab === 'internal' ? 'SRAM (Bộ nhớ nội bộ)' : 'PSRAM (Bộ nhớ ngoài)';"
"  document.getElementById('perf_title').innerText = title;"
"  updatePerfUI();"
"}"
"function updatePerfUI() {"
"  const tab = window.currentPerfTab;"
"  const history = window.perfHistory[tab];"
"  if (history.length === 0) return;"
"  const lastData = history[history.length - 1];"
"  const total = lastData.total;"
"  const free = lastData.free;"
"  const used = lastData.used;"
"  const min_free = lastData.min_free;"
"  const pct = total > 0 ? ((used / total) * 100).toFixed(1) : '0';"
"  const toMB = (bytes) => (bytes / 1024 / 1024).toFixed(2) + ' MB';"
"  const toKB = (bytes) => (bytes / 1024).toFixed(1) + ' KB';"
"  const formatSize = (bytes) => { if (bytes >= 1024 * 1024) return toMB(bytes); return toKB(bytes); };"
"  document.getElementById('perf_val_used').innerText = formatSize(used) + ' (' + pct + '%)';"
"  document.getElementById('perf_val_free').innerText = formatSize(free);"
"  document.getElementById('perf_val_min').innerText = formatSize(min_free);"
"  document.getElementById('perf_val_total').innerText = formatSize(total);"
"  document.getElementById('perf_bar_used').style.width = pct + '%';"
"  document.getElementById('perf_bar_text').innerText = pct + '% Used';"
"  drawPerfChart();"
"}"
"function drawPerfChart() {"
"  const canvas = document.getElementById('perfCanvas'); if (!canvas) return;"
"  const ctx = canvas.getContext('2d'); const w = canvas.width; const h = canvas.height;"
"  ctx.fillStyle = '#0f172a'; ctx.fillRect(0, 0, w, h);"
"  ctx.strokeStyle = '#1e293b'; ctx.lineWidth = 1;"
"  for (let i = 1; i < 5; i++) { const y = (h / 5) * i; ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(w, y); ctx.stroke(); }"
"  for (let i = 1; i < 10; i++) { const x = (w / 10) * i; ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, h); ctx.stroke(); }"
"  const tab = window.currentPerfTab; const history = window.perfHistory[tab];"
"  if (history.length < 2) return;"
"  ctx.beginPath(); ctx.strokeStyle = '#8b5cf6'; ctx.lineWidth = 2.5;"
"  const step = w / (MAX_HISTORY - 1);"
"  const getX = (index) => index * step;"
"  const getY = (used, total) => { if (total === 0) return h; const ratio = used / total; return h - (ratio * h * 0.8) - (h * 0.1); };"
"  const grad = ctx.createLinearGradient(0, 0, 0, h);"
"  grad.addColorStop(0, 'rgba(139, 92, 246, 0.4)'); grad.addColorStop(1, 'rgba(139, 92, 246, 0.0)');"
"  ctx.beginPath(); let firstX = 0; let firstY = h;"
"  for (let i = 0; i < history.length; i++) {"
"    const x = getX(i + (MAX_HISTORY - history.length));"
"    const y = getY(history[i].used, history[i].total);"
"    if (i === 0) { ctx.moveTo(x, y); firstX = x; firstY = y; } else { ctx.lineTo(x, y); }"
"  }"
"  ctx.stroke();"
"  if (history.length > 0) {"
"    const lastX = getX(MAX_HISTORY - 1);"
"    ctx.lineTo(lastX, h); ctx.lineTo(firstX, h);"
"    ctx.fillStyle = grad; ctx.fill();"
"  }"
"}"
"function startPerfMonitor() {"
"  loadPerfData();"
"  if (window.perfInterval) clearInterval(window.perfInterval);"
"  window.perfInterval = setInterval(loadPerfData, 2000);"
"}"
"function togglePerfMonitor(el) {"
"  const enabled = el.checked;"
"  fetch('/api/system/perf_config', {"
"    method: 'POST',"
"    headers: { 'Content-Type': 'application/json' },"
"    body: JSON.stringify({ enabled: enabled })"
"  }).then(r=>r.json()).then(d=>{"
"    if (d.status === 'ok') {"
"      startPerfMonitor();"
"    }"
"  }).catch(()=>{});"
"}"
"function loadPerfData() {"
"  fetch('/api/system/ram').then(r=>r.json()).then(d=>{"
"    const sw = document.getElementById('perf_toggle_switch');"
"    if (sw) sw.checked = d.perf_enabled;"
"    if (!d.perf_enabled) {"
"      document.getElementById('perf_disabled_card').style.display = 'block';"
"      document.getElementById('perf_main_card').style.display = 'none';"
"      document.getElementById('perf_tasks_card').style.display = 'none';"
"      if (window.perfInterval) {"
"        clearInterval(window.perfInterval);"
"        window.perfInterval = null;"
"      }"
"      return;"
"    }"
"    document.getElementById('perf_disabled_card').style.display = 'none';"
"    document.getElementById('perf_main_card').style.display = 'flex';"
"    document.getElementById('perf_tasks_card').style.display = 'block';"
"    if (d.internal) {"
"      window.perfHistory.internal.push(d.internal);"
"      if (window.perfHistory.internal.length > MAX_HISTORY) window.perfHistory.internal.shift();"
"      const intPct = d.internal.total > 0 ? ((d.internal.used / d.internal.total) * 100).toFixed(0) : '0';"
"      document.getElementById('sidebar_pct_internal').innerText = intPct + '%';"
"    }"
"    if (d.external && d.external.total > 0) {"
"      window.perfHistory.external.push(d.external);"
"      if (window.perfHistory.external.length > MAX_HISTORY) window.perfHistory.external.shift();"
"      const extPct = d.external.total > 0 ? ((d.external.used / d.external.total) * 100).toFixed(0) : '0';"
"      document.getElementById('sidebar_pct_external').innerText = extPct + '%';"
"      document.getElementById('btn_perf_external').style.display = 'flex';"
"    } else {"
"      document.getElementById('btn_perf_external').style.display = 'none';"
"    }"
"    if (d.tasks && d.tasks.length > 0) {"
"      let html = '';"
"      d.tasks.sort((a,b) => {"
"        if (b.priority !== a.priority) return b.priority - a.priority;"
"        return a.name.localeCompare(b.name);"
"      }).forEach(t => {"
"        const minFree = t.stack_min_free;"
"        const total = t.stack_total;"
"        let used = total - minFree;"
"        if (used < 0) used = 0;"
"        if (used > total) used = total;"
"        const pct = total > 0 ? ((used / total) * 100).toFixed(1) : '0.0';"
"        let pctColor = '#10b981';"
"        if (pct >= 85) pctColor = '#ef4444';"
"        else if (pct >= 70) pctColor = '#f59e0b';"
"        const coreText = t.core === -1 ? 'Any' : t.core;"
"        const ramType = t.is_psram ?"
"          `<span style=\"color:#a78bfa; font-weight:700; font-size:11px; background:rgba(167,139,250,0.1); padding:2px 6px; border-radius:4px; border:1px solid rgba(167,139,250,0.2);\">PSRAM</span>` :"
"          `<span style=\"color:#60a5fa; font-weight:700; font-size:11px; background:rgba(96,165,250,0.1); padding:2px 6px; border-radius:4px; border:1px solid rgba(96,165,250,0.2);\">SRAM</span>`;"
"        html += `<tr style=\"border-bottom:1px solid var(--b);\">"
"          <td style=\"padding:10px 4px; font-weight:700;\">${t.name}</td>"
"          <td style=\"padding:10px 4px; text-align:center;\">${coreText}</td>"
"          <td style=\"padding:10px 4px; text-align:center;\">${t.priority}</td>"
"          <td style=\"padding:10px 4px;\">${ramType}</td>"
"          <td style=\"padding:10px 4px; text-align:right; font-family:monospace;\">${total} B</td>"
"          <td style=\"padding:10px 4px; text-align:right; font-family:monospace;\">${used} B</td>"
"          <td style=\"padding:10px 4px; text-align:right; font-weight:bold; color:${pctColor}; font-family:monospace;\">${pct}%</td>"
"        </tr>`;"
"      });"
"      const tbody = document.getElementById('perf_tasks_list');"
"      if (tbody) tbody.innerHTML = html;"
"    }"
"    updatePerfUI();"
"  }).catch(()=>{});"
"}"
"window.consoleInterval = null;"
"window.lastLogSeq = 0;"
"window.consolePaused = false;"
"window.consoleLines = [];"
"function startConsoleMonitor() {"
"  loadConsoleLogs();"
"  if (window.consoleInterval) clearInterval(window.consoleInterval);"
"  window.consoleInterval = setInterval(loadConsoleLogs, 1500);"
"  const outputDiv = document.getElementById('console_output');"
"  if (outputDiv && !outputDiv.scrollListenerAdded) {"
"    outputDiv.scrollListenerAdded = true;"
"    outputDiv.addEventListener('scroll', () => {"
"      const isAtBottom = (outputDiv.scrollHeight - outputDiv.clientHeight - outputDiv.scrollTop) < 35;"
"      const statusSpan = document.getElementById('console_scroll_status');"
"      if (statusSpan) {"
"        statusSpan.innerText = isAtBottom ? 'Auto-scroll: Active' : 'Auto-scroll: Suspended (Cuộn xuống cuối để tiếp tục)';"
"        statusSpan.style.color = isAtBottom ? '#34d399' : '#f59e0b';"
"      }"
"    });"
"  }"
"}"
"function toggleConsoleMonitor(el) {"
"  const enabled = el.checked;"
"  fetch('/api/system/console_config', {"
"    method: 'POST',"
"    headers: { 'Content-Type': 'application/json' },"
"    body: JSON.stringify({ enabled: enabled })"
"  }).then(r=>r.json()).then(d=>{"
"    if (d.status === 'ok') {"
"      startConsoleMonitor();"
"    }"
"  }).catch(()=>{});"
"}"
"function parseAnsiColor(text) {"
"  let output = text;"
"  output = output.replace(/&/g, \'&amp;\').replace(/</g, \'&lt;\').replace(/>/g, \'&gt;\');"
"  output = output.replace(/\\x1b\\[([0-9;]*)m/g, (match, p1) => {"
"    if (p1 === \'0\' || p1 === \'\') return \'</span>\';"
"    const codes = p1.split(\';\');"
"    let style = \'\';"
"    let hasColor = false;"
"    codes.forEach(code => {"
"      if (code === \'1\') style += \'font-weight:bold;\';"
"      else if (code === \'31\') { style += \'color:#f87171;\'; hasColor = true; }"
"      else if (code === \'32\') { style += \'color:#4ade80;\'; hasColor = true; }"
"      else if (code === \'33\') { style += \'color:#facc15;\'; hasColor = true; }"
"      else if (code === \'34\') { style += \'color:#60a5fa;\'; hasColor = true; }"
"      else if (code === \'35\') { style += \'color:#c084fc;\'; hasColor = true; }"
"      else if (code === \'36\') { style += \'color:#2dd4bf;\'; hasColor = true; }"
"      else if (code === \'37\') { style += \'color:#f8fafc;\'; hasColor = true; }"
"    });"
"    if (hasColor || style !== \'\') return \'<span style=\\\"\' + style + \'\\\">\';"
"    return \'\';"
"  });"
"  return output;"
"}"
"function loadConsoleLogs() {"
"  if (window.consolePaused) return;"
"  fetch('/api/system/logs?last_seq=' + window.lastLogSeq).then(r=>{"
"    const isEnabled = r.headers.get('X-Console-Enabled') !== 'false';"
"    const sw = document.getElementById('console_toggle_switch');"
"    if (sw) sw.checked = isEnabled;"
"    if (!isEnabled) {"
"      document.getElementById('console_disabled_card').style.display = 'block';"
"      document.getElementById('console_main_card').style.display = 'none';"
"      if (window.consoleInterval) {"
"        clearInterval(window.consoleInterval);"
"        window.consoleInterval = null;"
"      }"
"      return null;"
"    }"
"    document.getElementById('console_disabled_card').style.display = 'none';"
"    document.getElementById('console_main_card').style.display = 'flex';"
"    return r.json();"
"  }).then(d=>{"
"    if (!d) return;"
"    if (d.length > 0) {"
"      const outputDiv = document.getElementById('console_output'); if (!outputDiv) return;"
"      const isAtBottom = (outputDiv.scrollHeight - outputDiv.clientHeight - outputDiv.scrollTop) < 35;"
"      let newHtml = '';"
"      d.forEach(item => {"
"        const cleanText = parseAnsiColor(item.text);"
"        newHtml += cleanText + '\\n';"
"        window.consoleLines.push(item.text);"
"        if (item.seq > window.lastLogSeq) window.lastLogSeq = item.seq;"
"      });"
"      if (window.consoleLines.length > 5000) window.consoleLines.splice(0, window.consoleLines.length - 5000);"
"      outputDiv.innerHTML += newHtml;"
"      const lines = outputDiv.innerHTML.split('\\n').length;"
"      document.getElementById('console_line_count').innerText = 'Lines: ' + lines;"
"      const maxDomLines = 600;"
"      const domLines = outputDiv.innerHTML.split('\\n');"
"      if (domLines.length > maxDomLines) {"
"        const heightBeforeSlice = outputDiv.scrollHeight;"
"        outputDiv.innerHTML = domLines.slice(domLines.length - maxDomLines).join(\'\\n\');"
"        if (!isAtBottom) {"
"          const heightDifference = outputDiv.scrollHeight - heightBeforeSlice;"
"          outputDiv.scrollTop += heightDifference;"
"        }"
"      }"
"      if (isAtBottom) {"
"        outputDiv.scrollTop = outputDiv.scrollHeight;"
"      }"
"    }"
"  }).catch(()=>{});"
"}"
"function clearConsole() {"
"  const outputDiv = document.getElementById('console_output');"
"  if (outputDiv) outputDiv.innerHTML = \"<span style='color:#64748b;'>-- System log stream cleared --</span>\\n\";"
"  window.consoleLines = [];"
"  document.getElementById('console_line_count').innerText = 'Lines: 1';"
"}"
"function togglePauseConsole() {"
"  window.consolePaused = !window.consolePaused;"
"  const btn = document.getElementById('btn_pause_log');"
"  if (btn) {"
"    btn.innerText = window.consolePaused ? 'Resume' : 'Pause';"
"    btn.style.background = window.consolePaused ? 'var(--s)' : 'var(--p)';"
"  }"
"}"
"function downloadConsoleLog() {"
"  if (window.consoleLines.length === 0) {"
"    alert('Không có dữ liệu log để tải về!'); return;"
"  }"
"  const cleanLines = window.consoleLines.map(line => line.replace(/\033\[[0-9;]*[a-zA-Z]/g, ''));"
"  const blob = new Blob([cleanLines.join('\\r\\n')], { type: 'text/plain;charset=utf-8' });"
"  const url = URL.createObjectURL(blob);"
"  const a = document.createElement('a'); a.href = url;"
"  const nowSecs = Math.floor(Date.now() / 1000);"
"  a.download = 'mebieco_system_log_' + nowSecs + '.txt';"
"  document.body.appendChild(a); a.click(); document.body.removeChild(a);"
"  URL.revokeObjectURL(url);"
"}"
"</script></body></html>";

static const char *update_web_html =
"<!DOCTYPE html>"
"<html>"
"<head>"
"    <meta charset=\"UTF-8\">"
"    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">"
"    <title>TomIOT - Winbond Flash File Manager</title>"
"    <style>"
"        :root {"
"            --bg: #0f172a;"
"            --card: #1e293b;"
"            --text: #f8fafc;"
"            --accent: #3b82f6;"
"            --accent-hover: #2563eb;"
"            --border: #334155;"
"            --success: #10b981;"
"            --danger: #ef4444;"
"            --danger-hover: #dc2626;"
"        }"
"        body {"
"            font-family: system-ui, -apple-system, sans-serif;"
"            background-color: var(--bg);"
"            color: var(--text);"
"            margin: 0;"
"            padding: 20px;"
"            display: flex;"
"            justify-content: center;"
"            align-items: center;"
"            min-height: 100vh;"
"        }"
"        .container {"
"            width: 100%;"
"            max-width: 680px;"
"            background: var(--card);"
"            border: 1px solid var(--border);"
"            border-radius: 16px;"
"            padding: 28px;"
"            box-shadow: 0 10px 25px -5px rgba(0, 0, 0, 0.4);"
"        }"
"        .header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 20px; }"
"        h2 { margin: 0; font-size: 22px; }"
"        .sub { color: #94a3b8; font-size: 13px; margin-top: 4px; }"
"        .storage-card {"
"            background: rgba(15, 23, 42, 0.6);"
"            border: 1px solid var(--border);"
"            border-radius: 12px;"
"            padding: 16px;"
"            margin-bottom: 20px;"
"        }"
"        .storage-title { display: flex; justify-content: space-between; font-weight: 600; font-size: 14px; margin-bottom: 8px; }"
"        .usage-bar { background: var(--border); height: 10px; border-radius: 5px; overflow: hidden; }"
"        .usage-fill { background: var(--accent); height: 100%; width: 0%; transition: width 0.3s; }"
"        .dropzone {"
"            border: 2px dashed var(--border);"
"            border-radius: 12px;"
"            padding: 24px;"
"            text-align: center;"
"            cursor: pointer;"
"            transition: 0.2s;"
"            background: rgba(255,255,255,0.02);"
"            margin-bottom: 16px;"
"        }"
"        .dropzone:hover { border-color: var(--accent); background: rgba(59, 130, 246, 0.08); }"
"        input[type=\"file\"] { display: none; }"
"        .btn {"
"            background: var(--accent);"
"            color: white;"
"            border: none;"
"            border-radius: 8px;"
"            padding: 10px 18px;"
"            font-weight: 600;"
"            cursor: pointer;"
"            font-size: 13px;"
"            transition: 0.2s;"
"        }"
"        .btn:hover { background: var(--accent-hover); }"
"        .btn-danger { background: var(--danger); }"
"        .btn-danger:hover { background: var(--danger-hover); }"
"        .btn-sm { padding: 5px 10px; font-size: 12px; }"
"        .progress-bar { background: var(--border); height: 6px; border-radius: 3px; margin-top: 12px; overflow: hidden; display: none; }"
"        .progress-fill { background: var(--success); height: 100%; width: 0%; transition: 0.1s; }"
"        #status { margin-top: 10px; font-size: 13px; font-weight: 500; text-align: center; }"
"        table { width: 100%; border-collapse: collapse; margin-top: 20px; font-size: 14px; }"
"        th { text-align: left; padding: 10px; border-bottom: 2px solid var(--border); color: #94a3b8; }"
"        td { padding: 12px 10px; border-bottom: 1px solid var(--border); }"
"        tr:hover td { background: rgba(255,255,255,0.02); }"
"        .actions { display: flex; gap: 8px; }"
"        a.dl-btn { color: var(--accent); text-decoration: none; font-weight: 600; padding: 4px 8px; border-radius: 4px; border: 1px solid var(--accent); font-size: 12px; }"
"        a.dl-btn:hover { background: var(--accent); color: white; }"
"    </style>"
"</head>"
"<body>"
"    <div class=\"container\">"
"        <div class=\"header\">"
"            <div>"
"                <h2>📁 Winbond Flash File Manager</h2>"
"                <div class=\"sub\">Quản lý giao diện Web & Tệp tin trên phân vùng SPIFFS (/web)</div>"
"            </div>"
"            <button class=\"btn btn-danger btn-sm\" onclick=\"formatFlash()\">Format Flash</button>"
"        </div>"
"        <div class=\"storage-card\">"
"            <div class=\"storage-title\">"
"                <span>Dung lượng Flash Ngoại</span>"
"                <span id=\"storageStats\">0 B / 16 MB (0%)</span>"
"            </div>"
"            <div class=\"usage-bar\">"
"                <div class=\"usage-fill\" id=\"usageFill\"></div>"
"            </div>"
"        </div>"
"        <div class=\"dropzone\" onclick=\"document.getElementById('fileInput').click()\">"
"            <span style=\"font-size: 32px;\">📤</span>"
"            <p id=\"fileName\" style=\"margin: 8px 0 0 0; font-weight: 500;\">Bấm hoặc Kéo & Thả file vào đây để Upload</p>"
"        </div>"
"        <input type=\"file\" id=\"fileInput\" onchange=\"fileSelected()\">"
"        <button class=\"btn\" style=\"width: 100%;\" onclick=\"uploadFile()\">Upload File Lên Flash</button>"
"        <div class=\"progress-bar\" id=\"progBar\">"
"            <div class=\"progress-fill\" id=\"progFill\"></div>"
"        </div>"
"        <div id=\"status\"></div>"
"        <div style=\"display: flex; justify-content: space-between; align-items: center; margin-top: 28px; margin-bottom: 10px;\">"
"            <h3 style=\"margin: 0; font-size: 16px;\">Danh Sách Tệp</h3>"
"            <button id=\"reloadBtn\" class=\"btn btn-sm\" style=\"background: #334155; border: 1px solid var(--border);\" onclick=\"loadFiles()\">🔄 Làm mới</button>"
"        </div>"
"        <table>"
"            <thead>"
"                <tr>"
"                    <th>Tên Tệp</th>"
"                    <th>Kích Thước</th>"
"                    <th style=\"width: 130px;\">Hành Động</th>"
"                </tr>"
"            </thead>"
"            <tbody id=\"fileTableBody\">"
"                <tr><td colspan=\"3\" style=\"text-align: center; color: #94a3b8;\">Chưa có tệp nào trên Flash ngoại</td></tr>"
"            </tbody>"
"        </table>"
"    </div>"
"    <script>"
"        function fmtSize(bytes) {"
"            if (bytes === 0) return '0 B';"
"            const k = 1024, sizes = ['B', 'KB', 'MB'];"
"            const i = Math.floor(Math.log(bytes) / Math.log(k));"
"            return parseFloat((bytes / Math.pow(k, i)).toFixed(2)) + ' ' + sizes[i];"
"        }"
"        function loadFiles() {"
"            const btn = document.getElementById('reloadBtn');"
"            if (btn) btn.innerText = '⌛ Đang tải...';"
"            fetch('/api/files')"
"                .then(r => r.json())"
"                .then(data => {"
"                    if (btn) btn.innerText = '🔄 Làm mới';"
"                    const u = data.used_bytes || 0;"
"                    let t = data.total_bytes || (16 * 1024 * 1024);"
"                    if (!t || t <= 0) t = 16 * 1024 * 1024;"
"                    const pct = t > 0 ? ((u / t) * 100).toFixed(1) : 0;"
"                    document.getElementById('storageStats').innerText = fmtSize(u) + ' / ' + fmtSize(t) + ' (' + pct + '%)';"
"                    document.getElementById('usageFill').style.width = pct + '%';"
"                    const tbody = document.getElementById('fileTableBody');"
"                    tbody.innerHTML = '';"
"                    if (!data.files || data.files.length === 0) {"
"                        tbody.innerHTML = '<tr><td colspan=\"3\" style=\"text-align: center; color: #94a3b8;\">Chưa có tệp nào trên Flash ngoại</td></tr>';"
"                        return;"
"                    }"
"                    data.files.forEach(f => {"
"                        const tr = document.createElement('tr');"
"                        tr.innerHTML = `<td><strong>${f.name}</strong></td><td>${fmtSize(f.size)}</td><td><div class=\"actions\"><a class=\"dl-btn\" href=\"/api/download?file=${encodeURIComponent(f.name)}\" download=\"${f.name}\">Tải về</a><button class=\"btn btn-danger btn-sm\" onclick=\"deleteFile('${f.name}')\">Xóa</button></div></td>`;"
"                        tbody.appendChild(tr);"
"                    });"
"                })"
"                .catch(e => {"
"                    if (btn) btn.innerText = '🔄 Làm mới';"
"                    document.getElementById('storageStats').innerText = 'Lỗi kết nối';"
"                });"
"        }"
"        function fileSelected() {"
"            const file = document.getElementById('fileInput').files[0];"
"            if (file) {"
"                document.getElementById('fileName').innerText = file.name + ' (' + fmtSize(file.size) + ')';"
"            }"
"        }"
"        function uploadFile() {"
"            const fileInput = document.getElementById('fileInput');"
"            const file = fileInput.files[0];"
"            if (!file) return alert('Vui lòng chọn một tệp!');"
"            const bar = document.getElementById('progBar');"
"            const fill = document.getElementById('progFill');"
"            const status = document.getElementById('status');"
"            bar.style.display = 'block';"
"            fill.style.width = '0%';"
"            status.style.color = '#94a3b8';"
"            status.innerText = 'Đang tải lên...';"
"            const xhr = new XMLHttpRequest();"
"            xhr.open('POST', '/api/upload', true);"
"            xhr.setRequestHeader('X-File-Name', file.name);"
"            xhr.upload.onprogress = function(e) {"
"                if (e.lengthComputable) {"
"                    fill.style.width = ((e.loaded / e.total) * 100) + '%';"
"                }"
"            };"
"            xhr.onload = function() {"
"                if (xhr.status === 200) {"
"                    status.style.color = 'var(--success)';"
"                    status.innerText = 'Tải lên thành công ' + file.name + '!';"
"                    fileInput.value = '';"
"                    document.getElementById('fileName').innerText = 'Bấm hoặc Kéo & Thả file vào đây để Upload';"
"                    loadFiles();"
"                } else {"
"                    status.style.color = 'var(--danger)';"
"                    status.innerText = 'Lỗi: ' + xhr.responseText;"
"                }"
"            };"
"            xhr.onerror = function() {"
"                status.style.color = 'var(--danger)';"
"                status.innerText = 'Kết nối thất bại!';"
"            };"
"            xhr.send(file);"
"        }"
"        function deleteFile(name) {"
"            if (!confirm('Bạn có chắc chắn muốn xóa file \"' + name + '\" không?')) return;"
"            fetch('/api/delete', {"
"                method: 'POST',"
"                headers: { 'Content-Type': 'application/json' },"
"                body: JSON.stringify({ filename: name })"
"            })"
"            .then(r => {"
"                if (r.ok) {"
"                    loadFiles();"
"                } else {"
"                    alert('Xóa file thất bại!');"
"                }"
"            });"
"        }"
"        function formatFlash() {"
"            if (!confirm('CẢNH BÁO: Hành động này sẽ XÓA SẠCH toàn bộ file trên Flash ngoại!\\nBạn có chắc chắn muốn tiếp tục?')) return;"
"            fetch('/api/format_flash', { method: 'POST' })"
"            .then(r => {"
"                if (r.ok) {"
"                    alert('Đã Format Flash ngoại thành công!');"
"                    loadFiles();"
"                } else {"
"                    alert('Format Flash thất bại!');"
"                }"
"            });"
"        }"
"        loadFiles();"
"    </script>"
"</body>"
"</html>";

static esp_err_t list_files_get_handler(httpd_req_t *req)
{
    if (!prvIsAuthenticated(req)) return prvSendUnauthorized(req);

    size_t total = 0, used = 0;
    if (esp_spiffs_info("storage", &total, &used) != ESP_OK || total == 0)
    {
        total = 16 * 1024 * 1024; // 16 MB Winbond W25Q128
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "total_bytes", total);
    cJSON_AddNumberToObject(root, "used_bytes", used);
    cJSON_AddNumberToObject(root, "free_bytes", (total > used) ? (total - used) : 0);

    cJSON *files_arr = cJSON_CreateArray();

    DIR *dir = opendir("/web");
    if (dir)
    {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL)
        {
            if (entry->d_type == DT_REG || entry->d_type == DT_UNKNOWN)
            {
                cJSON *file_obj = cJSON_CreateObject();
                cJSON_AddStringToObject(file_obj, "name", entry->d_name);

                char filepath[300];
                snprintf(filepath, sizeof(filepath), "/web/%s", entry->d_name);
                struct stat st;
                if (stat(filepath, &st) == 0)
                {
                    cJSON_AddNumberToObject(file_obj, "size", st.st_size);
                }
                else
                {
                    cJSON_AddNumberToObject(file_obj, "size", 0);
                }
                cJSON_AddItemToArray(files_arr, file_obj);
            }
        }
        closedir(dir);
    }

    cJSON_AddItemToObject(root, "files", files_arr);

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));

    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t delete_file_post_handler(httpd_req_t *req)
{
    if (!prvIsAuthenticated(req)) return prvSendUnauthorized(req);

    char buf[256];
    int ret = httpd_req_recv(req, buf, req->content_len);
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root)
    {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    }

    cJSON *filename_obj = cJSON_GetObjectItem(root, "filename");
    if (!filename_obj || !filename_obj->valuestring)
    {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing filename");
    }

    const char *filename = filename_obj->valuestring;
    if (strchr(filename, '/') != NULL || strchr(filename, '\\') != NULL || strstr(filename, "..") != NULL)
    {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filename");
    }

    char filepath[300];
    snprintf(filepath, sizeof(filepath), "/web/%s", filename);

    if (unlink(filepath) == 0)
    {
        ESP_LOGI(PORTAL_TAG, "Deleted file: %s", filepath);
        cJSON_Delete(root);
        httpd_resp_sendstr(req, "OK");
        return ESP_OK;
    }
    else
    {
        ESP_LOGE(PORTAL_TAG, "Failed to delete file: %s", filepath);
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to delete file");
    }
}

static esp_err_t format_flash_post_handler(httpd_req_t *req)
{
    if (!prvIsAuthenticated(req)) return prvSendUnauthorized(req);

    ESP_LOGW(PORTAL_TAG, "Formatting external Flash SPIFFS storage...");
    esp_err_t err = User_External_Flash_Format();
    if (err == ESP_OK)
    {
        httpd_resp_sendstr(req, "OK");
        return ESP_OK;
    }
    else
    {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Format failed");
    }
}

static const char* get_content_type(const char *filepath)
{
    if (strstr(filepath, ".html")) return "text/html";
    if (strstr(filepath, ".css")) return "text/css";
    if (strstr(filepath, ".js")) return "application/javascript";
    if (strstr(filepath, ".png")) return "image/png";
    if (strstr(filepath, ".jpg") || strstr(filepath, ".jpeg")) return "image/jpeg";
    if (strstr(filepath, ".ico")) return "image/x-icon";
    if (strstr(filepath, ".json")) return "application/json";
    if (strstr(filepath, ".csv")) return "text/csv";
    return "text/plain";
}

static esp_err_t download_file_get_handler(httpd_req_t *req)
{
    if (!prvIsAuthenticated(req)) return prvSendUnauthorized(req);

    char filename[128] = {0};
    char query[256] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK)
    {
        httpd_query_key_value(query, "file", filename, sizeof(filename));
    }

    if (strlen(filename) == 0 || strchr(filename, '/') != NULL || strchr(filename, '\\') != NULL)
    {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filename");
    }

    char filepath[300];
    snprintf(filepath, sizeof(filepath), "/web/%s", filename);

    FILE *fd = fopen(filepath, "r");
    if (!fd)
    {
        ESP_LOGE(PORTAL_TAG, "Download failed: File not found %s", filepath);
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
    }

    struct stat st;
    if (stat(filepath, &st) == 0)
    {
        char len_str[32];
        snprintf(len_str, sizeof(len_str), "%ld", (long)st.st_size);
        httpd_resp_set_hdr(req, "Content-Length", len_str);
    }

    httpd_resp_set_type(req, get_content_type(filepath));

    char disposition[300];
    snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s\"", filename);
    httpd_resp_set_hdr(req, "Content-Disposition", disposition);

    char *chunk = malloc(1024);
    if (chunk != NULL)
    {
        size_t read_bytes;
        while ((read_bytes = fread(chunk, 1, 1024, fd)) > 0) {
            if (httpd_resp_send_chunk(req, chunk, read_bytes) != ESP_OK)
            {
                break;
            }
        }
        free(chunk);
    }

    fclose(fd);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t stream_spiffs_file(httpd_req_t *req, const char *filepath)
{
    FILE *fd = fopen(filepath, "r");
    if (!fd)
    {
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, get_content_type(filepath));

    const char *filename = strrchr(filepath, '/');
    if (filename) filename++; else filename = filepath;

    char disposition[300];
    snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s\"", filename);
    httpd_resp_set_hdr(req, "Content-Disposition", disposition);

    char *chunk = malloc(1024);
    esp_err_t err = ESP_OK;
    if (chunk != NULL)
    {
        size_t read_bytes;
        do {
            read_bytes = fread(chunk, 1, 1024, fd);
            if (read_bytes > 0)
            {
                err = httpd_resp_send_chunk(req, chunk, read_bytes);
                if (err != ESP_OK)
                {
                    break;
                }
            }
        } while (read_bytes > 0);
        free(chunk);
    }
    else
    {
        err = ESP_ERR_NO_MEM;
    }

    fclose(fd);
    httpd_resp_send_chunk(req, NULL, 0); // End of response
    return err;
}

static esp_err_t update_web_get_handler(httpd_req_t *req)
{
    if(!prvIsAuthenticated(req)) return prvSendUnauthorized(req);
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, update_web_html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t upload_post_handler(httpd_req_t *req)
{
    if(!prvIsAuthenticated(req)) return prvSendUnauthorized(req);

    char filename[64] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-File-Name", filename, sizeof(filename)) != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing X-File-Name header");
        return ESP_FAIL;
    }

    if (strchr(filename, '/') != NULL || strchr(filename, '\\') != NULL)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filename");
        return ESP_FAIL;
    }

    char filepath[300];
    snprintf(filepath, sizeof(filepath), "/web/%s", filename);
    ESP_LOGI(PORTAL_TAG, "Receiving file: %s (%d bytes)", filepath, req->content_len);

    FILE *fd = fopen(filepath, "w");
    if (!fd)
    {
        ESP_LOGE(PORTAL_TAG, "Failed to open file for writing: %s", filepath);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to write file");
        return ESP_FAIL;
    }

    char buffer[1024];
    int remaining = req->content_len;
    int received;

    while (remaining > 0) {
        if ((received = httpd_req_recv(req, buffer, MIN(remaining, sizeof(buffer)))) <= 0)
        {
            if (received == HTTPD_SOCK_ERR_TIMEOUT)
            {
                continue; 
            }
            fclose(fd);
            unlink(filepath); 
            ESP_LOGE(PORTAL_TAG, "Socket receive failed");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Socket receive error");
            return ESP_FAIL;
        }

        fwrite(buffer, 1, received, fd);
        remaining -= received;
    }

    fclose(fd);
    ESP_LOGI(PORTAL_TAG, "File %s written successfully", filename);
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

static esp_err_t portal_get_handler(httpd_req_t *req)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char dynamic_domain[64];
    snprintf(dynamic_domain, sizeof(dynamic_domain), "mebieco-%02x%02x%02x.local", mac[3], mac[4], mac[5]);

    // 1. Force Domain Redirect if access by IP or different domain
    char host[64] = {0};
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) == ESP_OK)
    {
        if (strstr(host, "mebieco") == NULL)
        {
            return prvSendDomainRedirect(req, dynamic_domain);
        }
    }

    // 2. Auth Check
    if(!prvIsAuthenticated(req)) return prvSendUnauthorized(req);
    
    // Map request to SPIFFS path
    char filepath[300];
    if (strcmp(req->uri, "/") == 0)
    {
        snprintf(filepath, sizeof(filepath), "/web/index.html");
    }
    else
    {
        char clean_uri[64] = {0};
        char *q = strchr(req->uri, '?');
        if (q != NULL)
        {
            int len = q - req->uri;
            if (len >= sizeof(clean_uri)) len = sizeof(clean_uri) - 1;
            strncpy(clean_uri, req->uri, len);
        }
        else
        {
            strncpy(clean_uri, req->uri, sizeof(clean_uri) - 1);
        }

        if (strcmp(clean_uri, "/") == 0)
        {
            snprintf(filepath, sizeof(filepath), "/web/index.html");
        }
        else
        {
            snprintf(filepath, sizeof(filepath), "/web%s", clean_uri);
        }
    }

    // Check if file exists on SPIFFS and stream it
    struct stat st;
    if (stat(filepath, &st) == 0)
    {
        ESP_LOGI(PORTAL_TAG, "Serving file from external Flash SPIFFS: %s", filepath);
        return stream_spiffs_file(req, filepath);
    }

    // Fallback: If root index.html is requested or file not found, fall back to embedded C-string
    if (strcmp(req->uri, "/") == 0 || strstr(req->uri, "/index.html")) {
        ESP_LOGI(PORTAL_TAG, "File index.html not found, falling back to embedded portal_html");
        httpd_resp_set_type(req, "text/html");
        return httpd_resp_send(req, portal_html, HTTPD_RESP_USE_STRLEN);
    }

    // Otherwise, it's a 404
    return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File Not Found");
}

static esp_err_t scan_get_handler(httpd_req_t *req)
{
    wifi_scan_config_t scan_config = { .ssid = NULL, .bssid = NULL, .channel = 0, .show_hidden = true };
    ESP_ERROR_CHECK(esp_wifi_scan_start(&scan_config, true));

    uint16_t ap_count = 0;
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&ap_count));
    wifi_ap_record_t *ap_records = malloc(sizeof(wifi_ap_record_t) * ap_count);
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&ap_count, ap_records));

    cJSON *root = cJSON_CreateArray();
    for (int i = 0; i < ap_count; i++)
    {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", (char *)ap_records[i].ssid);
        cJSON_AddNumberToObject(item, "rssi", ap_records[i].rssi);
        cJSON_AddItemToArray(root, item);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));

    free(ap_records);
    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

#include "esp_app_desc.h"

static esp_err_t info_get_handler(httpd_req_t *req)
{
    // Lấy query string từ URI thay vì toàn bộ URI
    char buf[64];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK)
    {
        char pin[16];
        if (httpd_query_key_value(buf, "pin", pin, sizeof(pin)) == ESP_OK)
        {
            if (strcmp(pin, CONFIG_HTTP_AUTH_PIN) != 0)
            {
                return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Invalid PIN");
            }
        }
        else
        {
            return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "PIN Required");
        }
    }
    else
    {
         return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "PIN Required");
    }

    // Nếu PIN đúng, trả về thông tin
    wifi_config_t cur_wifi;
    esp_wifi_get_config(WIFI_IF_STA, &cur_wifi);

    const esp_app_desc_t *desc = esp_app_get_description();

    cJSON *root = cJSON_CreateObject();
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char mdns_str[64];
    snprintf(mdns_str, sizeof(mdns_str), "mebieco-%02x%02x%02x.local", mac[3], mac[4], mac[5]);

    cJSON_AddStringToObject(root, "version", desc->version);
    cJSON_AddStringToObject(root, "mdns", mdns_str);
    cJSON_AddStringToObject(root, "wifi", (char *)cur_wifi.sta.ssid);
    cJSON_AddStringToObject(root, "wifipass", (char *)cur_wifi.sta.password);
    cJSON_AddStringToObject(root, "host", IoTHubHandle.hostName);
    cJSON_AddStringToObject(root, "dev", IoTHubHandle.deviceId);
    cJSON_AddStringToObject(root, "sym", IoTHubHandle.symmetricKey);
    cJSON_AddNumberToObject(root, "oxy_mode", Sys_Info.oxyMode);
    cJSON_AddNumberToObject(root, "oxy_active_id", Sys_Info.activeOxyId);
    cJSON_AddNumberToObject(root, "feeder_mode", Sys_Info.feederMode);
    cJSON_AddNumberToObject(root, "feeder_active_id", Sys_Info.activeFeederId);
    cJSON_AddNumberToObject(root, "pond_mode", Sys_Info.pondMode);
    cJSON_AddNumberToObject(root, "vfd_enabled", Sys_Info.vfdEnabled);

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));

    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t save_wifi_handler(httpd_req_t *req)
{
    char buf[256];
    int ret = httpd_req_recv(req, buf, req->content_len);
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    cJSON *pin_obj = cJSON_GetObjectItem(root, "pin");
    if (!pin_obj || strcmp(pin_obj->valuestring, CONFIG_HTTP_AUTH_PIN) != 0) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Invalid PIN");
    }

    const char *ssid = cJSON_GetObjectItem(root, "ssid")->valuestring;
    const char *pass = cJSON_GetObjectItem(root, "pass")->valuestring;

    wifi_config_manager_save(ssid, pass);
    
    cJSON_Delete(root);
    httpd_resp_sendstr(req, "OK");

    // Đợi 1 giây rồi khởi động lại
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;
}

static esp_err_t save_azure_handler(httpd_req_t *req)
{
    char buf[512];
    int ret = httpd_req_recv(req, buf, req->content_len);
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    cJSON *pin_obj = cJSON_GetObjectItem(root, "pin");
    if (!pin_obj || strcmp(pin_obj->valuestring, CONFIG_HTTP_AUTH_PIN) != 0)
    {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Invalid PIN");
    }

    const char *host = cJSON_GetObjectItem(root, "host")->valuestring;
    const char *dev  = cJSON_GetObjectItem(root, "dev")->valuestring;
    const char *key  = cJSON_GetObjectItem(root, "key")->valuestring;

    azure_config_manager_save(host, dev, key);

    /* Nạp lại cấu hình từ FRAM vào RAM ngay lập tức và yêu cầu Task Azure tái kết nối */
    User_Azure_LoadConfig();
    IoTHubHandle.isNeedReinit = true;
    ESP_LOGI(PORTAL_TAG, "AZURE CONFIG SAVED, RECONNECT TO NEW AZURE CONFIG");

    cJSON_Delete(root);
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

static esp_err_t clear_handler(httpd_req_t *req)
{
    char buf[256];
    int ret = httpd_req_recv(req, buf, req->content_len);
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    cJSON *pin_obj = cJSON_GetObjectItem(root, "pin");
    if (!pin_obj || strcmp(pin_obj->valuestring, CONFIG_HTTP_AUTH_PIN) != 0)
    {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Invalid PIN");
    }
    cJSON_Delete(root);

    if (strstr(req->uri, "wifi"))
    {
        wifi_config_manager_clear();
    }
    else if (strstr(req->uri, "azure"))
    {
        azure_config_manager_clear();
        /* Nạp lại từ FRAM (sẽ rỗng) → xoá credentials trong RAM và dừng kết nối Azure */
        User_Azure_LoadConfig();
        IoTHubHandle.isAzureInitialized = false; // Tắt LED ngay lập tức, không chờ Task Azure xử lý
        IoTHubHandle.isNeedReinit = true;
        ESP_LOGW(PORTAL_TAG, "AZURE CONFIG CLEARED, DISCONNECT TO CLEARED AZURE");
    }
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

static esp_err_t search_post_handler(httpd_req_t *req)
{
    ESP_LOGI(PORTAL_TAG, "Search Device trigged - Blinking LED 2");
    User_System_Search_Device();
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

/**
 * @brief GET /api/config – Trả về cấu hình hiện tại của thiết bị (bao gồm pond_mode)
 */
static esp_err_t api_config_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");

    cJSON_AddNumberToObject(root, "pond_mode", Sys_Info.pondMode);
    cJSON_AddNumberToObject(root, "vfd_enabled", Sys_Info.vfdEnabled);
    cJSON_AddNumberToObject(root, "oxy_mode", Sys_Info.oxyMode);
    cJSON_AddNumberToObject(root, "feeder_mode", Sys_Info.feederMode);

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

/**
 * @brief POST /api/set_system_config – Nhận cấu hình hệ thống (pond_mode & vfd_enabled) từ Web Portal.
 *        Xác thực PIN rồi ghi vào FRAM và reboot thiết bị.
 */
static esp_err_t set_system_config_post_handler(httpd_req_t *req)
{
    if (req->content_len > 256)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large");

    char buf[256] = {0};
    int ret = httpd_req_recv(req, buf, req->content_len);
    if (ret <= 0)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");

    // Xác thực PIN
    cJSON *pin_obj = cJSON_GetObjectItem(root, "pin");
    if (!pin_obj || strcmp(pin_obj->valuestring, CONFIG_HTTP_AUTH_PIN) != 0)
    {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Invalid PIN");
    }

    // Lấy giá trị pond_mode và vfd_enabled
    cJSON *pond_mode_obj = cJSON_GetObjectItem(root, "pond_mode");
    cJSON *vfd_enabled_obj = cJSON_GetObjectItem(root, "vfd_enabled");

    if (!pond_mode_obj || !cJSON_IsNumber(pond_mode_obj) || !vfd_enabled_obj || !cJSON_IsNumber(vfd_enabled_obj))
    {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing pond_mode or vfd_enabled field");
    }

    uint8_t new_pond_mode = (uint8_t)pond_mode_obj->valueint;
    uint8_t new_vfd_enabled = (uint8_t)vfd_enabled_obj->valueint;
    cJSON_Delete(root);

    if (new_pond_mode != POND_MODE_10_DEV && new_pond_mode != POND_MODE_4_DEV)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid pond_mode value");

    if (new_vfd_enabled != 0 && new_vfd_enabled != 1)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid vfd_enabled value");

    bool pond_changed = (new_pond_mode != Sys_Info.pondMode);
    bool vfd_changed = (new_vfd_enabled != Sys_Info.vfdEnabled);

    if (!pond_changed && !vfd_changed)
    {
        return httpd_resp_sendstr(req, "OK_NO_CHANGE");
    }

    ESP_LOGW(PORTAL_TAG, "System configuration change requested. PondMode: %d->%d, VFD: %d->%d",
             Sys_Info.pondMode, new_pond_mode, Sys_Info.vfdEnabled, new_vfd_enabled);

    // Gửi phản hồi trước khi reboot
    httpd_resp_sendstr(req, "OK");

    // Chờ 1 giây để HTTP response được gửi xong rồi mới thực hiện ghi & reboot
    vTaskDelay(pdMS_TO_TICKS(1000));

    if (vfd_changed)
    {
        uint8_t val = new_vfd_enabled;
        Fram_Write_Data(FRAM_VFD_ENABLED_ADDR, &val, 1);
        Sys_Info.vfdEnabled = new_vfd_enabled;
    }

    if (pond_changed)
    {
        User_Device_Switch_Pond_Mode(new_pond_mode);
    }
    else
    {
        esp_restart();
    }

    return ESP_OK;
}

static esp_err_t restart_post_handler(httpd_req_t *req)
{
    char buf[256];
    int ret = httpd_req_recv(req, buf, req->content_len);
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    cJSON *pin_obj = cJSON_GetObjectItem(root, "pin");
    if (!pin_obj || strcmp(pin_obj->valuestring, CONFIG_HTTP_AUTH_PIN) != 0) {
        if (root) cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Invalid PIN");
    }
    cJSON_Delete(root);

    ESP_LOGW(PORTAL_TAG, "Restart ESP trigged from portal");
    httpd_resp_sendstr(req, "OK");

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

void web_portal_register_handlers(httpd_handle_t server)
{
    httpd_uri_t portal = { .uri = "/", .method = HTTP_GET, .handler = portal_get_handler };
    httpd_uri_t scan = { .uri = "/scan", .method = HTTP_GET, .handler = scan_get_handler };
    httpd_uri_t info = { .uri = "/info", .method = HTTP_GET, .handler = info_get_handler };
    httpd_uri_t save_w = { .uri = "/save_wifi", .method = HTTP_POST, .handler = save_wifi_handler };
    httpd_uri_t save_a = { .uri = "/save_azure", .method = HTTP_POST, .handler = save_azure_handler };
    httpd_uri_t clr_w = { .uri = "/clear_wifi", .method = HTTP_POST, .handler = clear_handler };
    httpd_uri_t clr_a = { .uri = "/clear_azure", .method = HTTP_POST, .handler = clear_handler };
    httpd_uri_t search = { .uri = "/search", .method = HTTP_POST, .handler = search_post_handler };
    httpd_uri_t restart = { .uri = "/restart", .method = HTTP_POST, .handler = restart_post_handler };
    httpd_uri_t update_w = { .uri = "/update_web", .method = HTTP_GET, .handler = update_web_get_handler };
    httpd_uri_t upload_api = { .uri = "/api/upload", .method = HTTP_POST, .handler = upload_post_handler };
    httpd_uri_t files_api = { .uri = "/api/files", .method = HTTP_GET, .handler = list_files_get_handler };
    httpd_uri_t delete_api = { .uri = "/api/delete", .method = HTTP_POST, .handler = delete_file_post_handler };
    httpd_uri_t format_api = { .uri = "/api/format_flash", .method = HTTP_POST, .handler = format_flash_post_handler };
    httpd_uri_t download_api = { .uri = "/api/download", .method = HTTP_GET, .handler = download_file_get_handler };
    httpd_uri_t portal_wildcard = { .uri = "/*", .method = HTTP_GET, .handler = portal_get_handler };
    httpd_uri_t api_config = { .uri = "/api/config", .method = HTTP_GET, .handler = api_config_get_handler };
    httpd_uri_t set_system_config = { .uri = "/api/set_system_config", .method = HTTP_POST, .handler = set_system_config_post_handler };

    httpd_register_uri_handler(server, &portal);
    httpd_register_uri_handler(server, &scan);
    httpd_register_uri_handler(server, &info);
    httpd_register_uri_handler(server, &save_w);
    httpd_register_uri_handler(server, &save_a);
    httpd_register_uri_handler(server, &clr_w);
    httpd_register_uri_handler(server, &clr_a);
    httpd_register_uri_handler(server, &search);
    httpd_register_uri_handler(server, &restart);
    httpd_register_uri_handler(server, &update_w);
    httpd_register_uri_handler(server, &upload_api);
    httpd_register_uri_handler(server, &files_api);
    httpd_register_uri_handler(server, &download_api);
    httpd_register_uri_handler(server, &delete_api);
    httpd_register_uri_handler(server, &format_api);
    httpd_register_uri_handler(server, &api_config);
    httpd_register_uri_handler(server, &set_system_config);
    httpd_register_uri_handler(server, &portal_wildcard);
}

