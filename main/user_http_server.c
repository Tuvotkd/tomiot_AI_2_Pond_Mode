#include "user_http_server.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_check.h"
#include <stdlib.h>
#include <sys/param.h>
#include "user_system.h"
#include "mdns.h"
#include "cJSON.h"
#include "user_ouput.h"
#include "time.h"
#include "RS485.h"
#include <math.h>
#include "web_portal.h"
#include "user_fram.h"
#include "wifi_config_manager.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "user_log_stream.h"

#define WIFI_CRED_MAGIC 0x57494649u
#define AZURE_CRED_MAGIC 0x415A5552u

typedef struct __attribute__((packed))
{
    uint32_t magic;
    char ssid[32];
    char pass[64];
    uint8_t reserved[WIFI_FRAM_SIZE - 4 - 32 - 64];
} fram_map_wifi_t;

typedef struct __attribute__((packed))
{
    uint32_t magic;
    char hostName[64];
    char deviceId[64];
    char symmetricKey[64];
    uint8_t reserved[AZURE_FRAM_SIZE - 4 - 64 - 64 - 64];
} fram_map_azure_t;

static const char* html_page = 
"<!DOCTYPE html><html><head><meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width, initial-scale=1'>"
"<style>"
"  body{font-family:'Segoe UI',Arial,sans-serif;text-align:center;background:#f0f2f5;margin:0;padding:20px;}"
"  .container{display:flex;flex-wrap:wrap;justify-content:center;gap:20px;}"
"  .card{background:white;padding:20px;border-radius:20px;box-shadow:0 8px 20px rgba(0,0,0,0.1);width:300px;transition:0.3s;}"
"  .bulb{width:70px;fill:#d1d1d1;transition:0.5s;margin:10px 0;filter:drop-shadow(0 0 2px #ccc);}"
"  .active{fill:#ffdb13;filter:drop-shadow(0 0 15px #ffdb13) drop-shadow(0 0 30px #f1c40f);}"
"  button{width:45%;padding:12px;margin:5px;border:none;border-radius:12px;color:white;font-weight:bold;cursor:pointer;font-size:14px;transition:all 0.1s;}"
"  .on{background:#2ecc71;box-shadow:0 4px #27ae60;} .on:active{box-shadow:0 0 #27ae60; transform:translateY(4px);}"
"  .off{background:#e74c3c;box-shadow:0 4px #c0392b;} .off:active{box-shadow:0 0 #c0392b; transform:translateY(4px);}"
"  .save{background:#3498db;width:94%;box-shadow:0 4px #2980b9;margin-top:10px;}"
"  .save:active{box-shadow:0 0 #2980b9; transform:translateY(4px);}"
"  input{width:85%;padding:12px;margin:8px 0;border:2px solid #eee;border-radius:10px;outline:none;transition:0.3s;}"
"  h3{margin:10px 0;color:#2c3e50;} hr{border:0;border-top:1px solid #eee;margin:15px 0;}"
"</style></head><body>"
"  <h2>HỆ THỐNG AO TÔM THÔNG MINH</h2>"
"  <div class='container'>"
"    <div class='card'><h3>MÁY CHO ĂN (11)</h3>"
"      <svg id='b11' class='bulb' viewBox='0 0 24 24'><path d='M12,2A7,7 0 0,0 5,9C5,11.38 6.19,13.47 8,14.74V17A1,1 0 0,0 9,18H15A1,1 0 0,0 16,17V14.74C17.81,13.47 19,11.38 19,9A7,7 0 0,0 12,2M9,21A1,1 0 0,0 10,22H14A1,1 0 0,0 15,21V20H9V21Z'/></svg><br>"
"      <button class='on' onclick='send(1,11)'>BẬT</button><button class='off' onclick='send(0,11)'>TẮT</button>"
"      <hr><input type='time' id='t11'><input type='number' id='d11' value='30'><button class='save' onclick='save(11)'>LƯU LỊCH</button></div>"
"    <div class='card'><h3>MÁY SỤC OXY (21)</h3>"
"      <svg id='b21' class='bulb' viewBox='0 0 24 24'><path d='M12,2A7,7 0 0,0 5,9C5,11.38 6.19,13.47 8,14.74V17A1,1 0 0,0 9,18H15A1,1 0 0,0 16,17V14.74C17.81,13.47 19,11.38 19,9A7,7 0 0,0 12,2M9,21A1,1 0 0,0 10,22H14A1,1 0 0,0 15,21V20H9V21Z'/></svg><br>"
"      <button class='on' onclick='send(1,21)'>BẬT</button><button class='off' onclick='send(0,21)'>TẮT</button>"
"      <hr><input type='time' id='t21'><input type='number' id='d21' value='30'><button class='save' onclick='save(21)'>LƯU LỊCH</button></div>"
"    <div class='card'><h3>QUẠT OXY (22)</h3>"
"      <svg id='b22' class='bulb' viewBox='0 0 24 24'><path d='M12,2A7,7 0 0,0 5,9C5,11.38 6.19,13.47 8,14.74V17A1,1 0 0,0 9,18H15A1,1 0 0,0 16,17V14.74C17.81,13.47 19,11.38 19,9A7,7 0 0,0 12,2M9,21A1,1 0 0,0 10,22H14A1,1 0 0,0 15,21V20H9V21Z'/></svg><br>"
"      <button class='on' onclick='send(1,22)'>BẬT</button><button class='off' onclick='send(0,22)'>TẮT</button>"
"      <hr><input type='time' id='t22'><input type='number' id='d22' value='30'><button class='save' onclick='save(22)'>LƯU LỊCH</button></div>"
"  </div>"
"<script>"
"function update(){"
"  fetch('/api/status').then(r=>r.json()).then(d=>{"
"    for(let id in d){"
"      let b=document.getElementById('b'+id);"
"      if(b) d[id]?b.classList.add('active'):b.classList.remove('active');"
"    }"
"  }).catch(e=>{});"
"}"
"setInterval(update, 500);" 
"function send(v,id){"
"  fetch('/api/control',{method:'POST',body:JSON.stringify({Data:{DeviceId:id,Value:v}})});"
"}"
"function save(id){"
"  const t=document.getElementById('t'+id).value, d=document.getElementById('d'+id).value;"
"  if(!t)return;"
"  const n=new Date(), [h,m]=t.split(':'); let s=new Date(); s.setHours(h,m,0,0);"
"  if(s<=n)s.setDate(s.getDate()+1);"
"  const st=parseInt(String(s.getDate()).padStart(2,'0')+String(s.getMonth()+1).padStart(2,'0')+String(s.getFullYear()).slice(-2)+h+m+'00');"
"  fetch('/api/schedule',{method:'POST',body:JSON.stringify({Data:{StartTime:st,Duration:parseInt(d),DeviceId:id}})});"
"}"
"update();"
"</script></body></html>";

#define EXAMPLE_HTTP_QUERY_KEY_MAX_LEN  (64)

static const char *TAG = "example";

/* An HTTP GET handler */
static esp_err_t hello_get_handler(httpd_req_t *req)
{
    char*  buf;
    size_t buf_len;

    buf_len = httpd_req_get_hdr_value_len(req, "Host") + 1;
    if (buf_len > 1)
    {
        buf = malloc(buf_len);
        ESP_RETURN_ON_FALSE(buf, ESP_ERR_NO_MEM, TAG, "buffer alloc failed");
        if (httpd_req_get_hdr_value_str(req, "Host", buf, buf_len) == ESP_OK)
        {
            ESP_LOGI(TAG, "Found header => Host: %s", buf);
        }
        free(buf);
    }
    httpd_resp_send(req, html_page, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t device_control_post_handler(httpd_req_t *req)
{
    char content[256];
    size_t recv_size = MIN(req->content_len, sizeof(content));
    int ret = httpd_req_recv(req, content, recv_size);
    if (ret <= 0) return ESP_FAIL;
    content[ret] = '\0';
    cJSON *root = cJSON_Parse(content);
    if (root)
    {
        cJSON *data = cJSON_GetObjectItem(root, "Data");
        if (data)
        {
            int device_id = cJSON_GetObjectItem(data, "DeviceId")->valueint;
            int value = cJSON_GetObjectItem(data, "Value")->valueint;
            uint8_t hiNibble = (device_id / 10);
            uint8_t loNiblle = (device_id % 10);
            uint8_t shift = (hiNibble*4) - (4-loNiblle) - 1;
            if (shift < DEVICE_MAX_NUM && !DeviceHandle.Device[shift].isActived)
            {
                ESP_LOGW("HTTP", "Ignore control command for disabled DeviceId %d (shift %d)", device_id, shift);
                cJSON_Delete(root);
                httpd_resp_set_hdr(req, "Connection", "close");
                const char* resp_err = "{\"status\":\"error\",\"message\":\"Device disabled in current Pond Mode\"}";
                httpd_resp_send(req, resp_err, HTTPD_RESP_USE_STRLEN);
                return ESP_OK;
            }
            if(value == 0)
            {
                DeviceHandle.outputBuf &= ~(1 << shift);
                DeviceHandle.Device[shift].state = DEVICE_STATE_OFF;
            }
            else
            {
                DeviceHandle.outputBuf |= (1 << shift);
                DeviceHandle.Device[shift].state = DEVICE_STATE_ON;
            }
            DeviceHandle.activeType = DEVICE_ACTIVE_TYPE_TRIGGER;
        }
        cJSON_Delete(root);
    }
    httpd_resp_set_hdr(req, "Connection", "close");
    const char* resp = "{\"status\":\"ok\"}";
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t schedule_post_handler(httpd_req_t *req)
{
    char buf[256];
    int ret = httpd_req_recv(req, buf, req->content_len);
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';
    cJSON *root = cJSON_Parse(buf);
    if (root)
    {
        cJSON *data = cJSON_GetObjectItem(root, "Data");
        if (data)
        {
            double st = cJSON_GetObjectItem(data, "StartTime")->valuedouble;
            long long full = (long long)st;
            int sec  = full % 100;
            int min  = (full / 100) % 100;
            int hour = (full / 10000) % 100;
            int year = 2000 + ((full / 1000000) % 100);
            int mon  = (full / 100000000) % 100;
            int day  = (full / 10000000000);
            int deviceId = cJSON_GetObjectItem(data, "DeviceId")->valueint;
            int duration = cJSON_GetObjectItem(data, "Duration")->valueint;
            struct tm t = {0};
            t.tm_year = year - 1900;
            t.tm_mon = mon - 1;
            t.tm_mday = day;
            t.tm_hour = hour;
            t.tm_min = min;
            t.tm_sec = sec;
            t.tm_isdst = -1; 
            time_t epoch = mktime(&t);
            if(epoch < Sys_Info.epochtime)
            {
                httpd_resp_set_status(req, "400 Bad Request");
            }
            else
            {
                uint8_t hiNibble = (deviceId / 10);
                uint8_t loNiblle = (deviceId % 10);
                uint8_t shift = (hiNibble*4) - (4-loNiblle) - 1;
                DeviceHandle.activeType = DEVICE_ACTIVE_TYPE_SCHEDULE;
                DeviceHandle.Device[shift].isScheduled = true;
                DeviceHandle.Device[shift].duration =  duration;
                DeviceHandle.Device[shift].startTime = epoch;
                DeviceHandle.Device[shift].stopTime = DeviceHandle.Device[shift].startTime + duration;
                httpd_resp_set_status(req, "200 OK");
            }
        }
        cJSON_Delete(root);
    }
    httpd_resp_send(req, "Response", strlen("Response"));
    return ESP_OK;
}

esp_err_t status_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateArray();
    for (int i = 0; i < DEVICE_MAX_NUM; i++)
    {
        if (!DeviceHandle.Device[i].isActived) continue;
        cJSON *device = cJSON_CreateObject();
        cJSON_AddStringToObject(device, "name", DeviceHandle.Device[i].name);
        cJSON_AddNumberToObject(device, "id", DeviceHandle.Device[i].id);
        cJSON_AddNumberToObject(device, "state", DeviceHandle.Device[i].state);
        cJSON_AddBoolToObject(device, "isSchedulePaused", DeviceHandle.Device[i].isSchedulePaused);
        cJSON_AddNumberToObject(device, "runtime", DeviceHandle.Device[i].runtime);
        cJSON_AddItemToArray(root, device);
    }
    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t schedules_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateArray();
    for (int i = 0; i < DEVICE_MAX_NUM; i++)
    {
        if (!DeviceHandle.Device[i].isActived) continue;
        cJSON *device = cJSON_CreateObject();
        cJSON_AddStringToObject(device, "name", DeviceHandle.Device[i].name);
        cJSON_AddNumberToObject(device, "id", DeviceHandle.Device[i].id);
        cJSON *schedules = cJSON_CreateArray();
        for (int j = 0; j < DEVICE_SCHEDULE_MAX; j++)
        {
            if (DeviceHandle.Device[i].schedules[j].startTime == 0) continue;
            cJSON *sch = cJSON_CreateObject();
            cJSON_AddNumberToObject(sch, "idx", j);
            cJSON_AddNumberToObject(sch, "start", (double)DeviceHandle.Device[i].schedules[j].startTime);
            cJSON_AddNumberToObject(sch, "stop", (double)DeviceHandle.Device[i].schedules[j].stopTime);
            cJSON_AddNumberToObject(sch, "run", 0);
            cJSON_AddNumberToObject(sch, "pause", 0);
            cJSON_AddNumberToObject(sch, "done", DeviceHandle.Device[i].schedules[j].isFinished);
            cJSON_AddItemToArray(schedules, sch);
        }
        cJSON_AddItemToObject(device, "items", schedules);
        cJSON_AddItemToArray(root, device);
    }
    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t vfd_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (Sys_Info.vfdEnabled == 0)
    {
        cJSON_AddBoolToObject(root, "connected", false);
        cJSON_AddStringToObject(root, "message", "VFD is disabled in System Config");
        char *json_str = cJSON_PrintUnformatted(root);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, json_str, strlen(json_str));
        free(json_str);
        cJSON_Delete(root);
        return ESP_OK;
    }
    
    GD200A_Status_t vfd_status;
    RS485_Status_t ret = GD200A_ReadStatus(GD200A_SLAVE_ADDR, &vfd_status);
    
    if (ret == RS485_OK)
    {
        Update_Vfd_Energy(vfd_status.cumulative_energy_kwh);

        cJSON_AddBoolToObject(root, "connected", true);
        cJSON_AddStringToObject(root, "mode", (vfd_status.run_command_channel == 2) ? "auto" : "manual");
        cJSON_AddBoolToObject(root, "running", vfd_status.is_running);
        cJSON_AddBoolToObject(root, "fwd", vfd_status.is_fwd);
        cJSON_AddBoolToObject(root, "fault", vfd_status.is_fault);
        cJSON_AddNumberToObject(root, "fault_code", vfd_status.fault_code);
        cJSON_AddStringToObject(root, "fault_str", GD200A_GetFaultString(vfd_status.fault_code));
        cJSON_AddNumberToObject(root, "actual_freq", round(vfd_status.freq_actual_hz * 10.0) / 10.0);
        cJSON_AddNumberToObject(root, "set_freq", round(vfd_status.freq_set_hz * 10.0) / 10.0);
        cJSON_AddNumberToObject(root, "accel_time", round(vfd_status.accel_time_s * 10.0) / 10.0);
        cJSON_AddNumberToObject(root, "decel_time", round(vfd_status.decel_time_s * 10.0) / 10.0);
        cJSON_AddNumberToObject(root, "output_current", round(vfd_status.output_current_a * 10.0) / 10.0);
        cJSON_AddNumberToObject(root, "output_voltage", vfd_status.output_voltage_v);
        cJSON_AddNumberToObject(root, "bus_voltage", round(vfd_status.bus_voltage_v * 10.0) / 10.0);
        cJSON_AddNumberToObject(root, "motor_speed", vfd_status.motor_speed_rpm);
        cJSON_AddNumberToObject(root, "output_power", round(vfd_status.output_power_pct * 10.0) / 10.0);
        cJSON_AddBoolToObject(root, "auto_run_enable", vfd_status.auto_run_enable == 1);
        cJSON_AddNumberToObject(root, "auto_run_delay", round(vfd_status.auto_run_delay_s * 10.0) / 10.0);

        cJSON_AddNumberToObject(root, "vfd_last_energy", round(vfd_last_energy * 10.0) / 10.0);
        cJSON_AddNumberToObject(root, "vfd_daily_energy", round(vfd_daily_energy * 10.0) / 10.0);
        cJSON_AddNumberToObject(root, "vfd_total_energy", round(vfd_status.cumulative_energy_kwh * 10.0) / 10.0);
    }
    else
    {
        cJSON_AddBoolToObject(root, "connected", false);
        cJSON_AddNumberToObject(root, "error_code", (int)ret);
        cJSON_AddStringToObject(root, "message", "485 cable breakage or VFD power loss");

        cJSON_AddNumberToObject(root, "vfd_last_energy", round(vfd_last_energy * 10.0) / 10.0);
        cJSON_AddNumberToObject(root, "vfd_daily_energy", round(vfd_daily_energy * 10.0) / 10.0);
        cJSON_AddNumberToObject(root, "vfd_total_energy", -1.0);
    }
    
    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t fram_map_get_handler(httpd_req_t *req)
{
    uint32_t total_capacity = 8192;
    Fram_Get_Capacity(&total_capacity);

    uint32_t used_bytes = 5566;
    uint32_t free_bytes = (total_capacity > used_bytes) ? (total_capacity - used_bytes) : 0;

    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_FAIL;

    cJSON_AddNumberToObject(root, "total_capacity", total_capacity);
    cJSON_AddNumberToObject(root, "used_bytes", used_bytes);
    cJSON_AddNumberToObject(root, "free_bytes", free_bytes);

    cJSON *blocks = cJSON_CreateArray();

    /* 1. Devices & Schedules block (0x0000 - 0x13FF) */
    cJSON *devBlock = cJSON_CreateObject();
    cJSON_AddStringToObject(devBlock, "name", "Devices & Schedules Storage");
    cJSON_AddStringToObject(devBlock, "addr", "0x0000 - 0x13FF");
    cJSON_AddNumberToObject(devBlock, "size", 5120);
    cJSON_AddStringToObject(devBlock, "type", "devices");

    cJSON *devList = cJSON_CreateArray();
    for (int i = 0; i < DEVICE_MAX_NUM; i++)
    {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddNumberToObject(d, "index", i);
        char addrStr[32];
        snprintf(addrStr, sizeof(addrStr), "0x%04X - 0x%04X", i * 512, (i + 1) * 512 - 1);
        cJSON_AddStringToObject(d, "addr", addrStr);
        cJSON_AddNumberToObject(d, "id", DeviceHandle.Device[i].id);
        cJSON_AddStringToObject(d, "name", DeviceHandle.Device[i].name);
        cJSON_AddBoolToObject(d, "active", DeviceHandle.Device[i].isActived);
        cJSON_AddBoolToObject(d, "scheduled", DeviceHandle.Device[i].isScheduled);
        cJSON_AddNumberToObject(d, "sch_count", DeviceHandle.Device[i].scheduleCount);

        cJSON *schItems = cJSON_CreateArray();
        for (int j = 0; j < DeviceHandle.Device[i].scheduleCount && j < DEVICE_SCHEDULE_MAX; j++)
        {
            cJSON *s = cJSON_CreateObject();
            cJSON_AddNumberToObject(s, "idx", j + 1);
            cJSON_AddNumberToObject(s, "start", DeviceHandle.Device[i].schedules[j].startTime);
            cJSON_AddNumberToObject(s, "stop", DeviceHandle.Device[i].schedules[j].stopTime);
            cJSON_AddBoolToObject(s, "done", DeviceHandle.Device[i].schedules[j].isFinished);
            cJSON_AddItemToArray(schItems, s);
        }
        cJSON_AddItemToObject(d, "schedules", schItems);
        cJSON_AddItemToArray(devList, d);
    }
    cJSON_AddItemToObject(devBlock, "items", devList);
    cJSON_AddItemToArray(blocks, devBlock);

    /* 2. WiFi block (0x1400 - 0x147F) */
    cJSON *wifiBlock = cJSON_CreateObject();
    cJSON_AddStringToObject(wifiBlock, "name", "WiFi Credentials");
    cJSON_AddStringToObject(wifiBlock, "addr", "0x1400 - 0x147F");
    cJSON_AddNumberToObject(wifiBlock, "size", 128);
    cJSON_AddStringToObject(wifiBlock, "type", "wifi");

    fram_map_wifi_t wifi_cred;
    if (Fram_Read_Data(WIFI_FRAM_ADDR, (uint8_t *)&wifi_cred, sizeof(wifi_cred)) && wifi_cred.magic == WIFI_CRED_MAGIC)
    {
        cJSON_AddBoolToObject(wifiBlock, "valid", true);
        cJSON_AddStringToObject(wifiBlock, "ssid", wifi_cred.ssid);
    }
    else
    {
        cJSON_AddBoolToObject(wifiBlock, "valid", false);
        cJSON_AddStringToObject(wifiBlock, "ssid", "Empty / Not Configured");
    }
    cJSON_AddItemToArray(blocks, wifiBlock);

    /* 3. Azure block (0x1480 - 0x157F) */
    cJSON *azureBlock = cJSON_CreateObject();
    cJSON_AddStringToObject(azureBlock, "name", "Azure IoT Credentials");
    cJSON_AddStringToObject(azureBlock, "addr", "0x1480 - 0x157F");
    cJSON_AddNumberToObject(azureBlock, "size", 256);
    cJSON_AddStringToObject(azureBlock, "type", "azure");

    fram_map_azure_t azure_cred;
    if (Fram_Read_Data(AZURE_FRAM_ADDR, (uint8_t *)&azure_cred, sizeof(azure_cred)) && azure_cred.magic == AZURE_CRED_MAGIC)
    {
        cJSON_AddBoolToObject(azureBlock, "valid", true);
        cJSON_AddStringToObject(azureBlock, "host", azure_cred.hostName);
        cJSON_AddStringToObject(azureBlock, "dev_id", azure_cred.deviceId);
    }
    else
    {
        cJSON_AddBoolToObject(azureBlock, "valid", false);
        cJSON_AddStringToObject(azureBlock, "host", "Empty / Not Configured");
        cJSON_AddStringToObject(azureBlock, "dev_id", "-");
    }
    cJSON_AddItemToArray(blocks, azureBlock);

    /* 3.1. Device Daily Runtimes block (0x1580 - 0x15A7) */
    cJSON *runtimeBlock = cJSON_CreateObject();
    cJSON_AddStringToObject(runtimeBlock, "name", "Device Daily Runtimes");
    cJSON_AddStringToObject(runtimeBlock, "addr", "0x1580 - 0x15A7");
    cJSON_AddNumberToObject(runtimeBlock, "size", 40);
    cJSON_AddStringToObject(runtimeBlock, "type", "runtime");
    cJSON_AddItemToArray(blocks, runtimeBlock);

    /* 3.2. VFD Daily & Last Cumulative Energy block (0x15A8 - 0x15B7) */
    cJSON *vfdEnergyBlock = cJSON_CreateObject();
    cJSON_AddStringToObject(vfdEnergyBlock, "name", "VFD Daily & Cumulative Energy");
    cJSON_AddStringToObject(vfdEnergyBlock, "addr", "0x15A8 - 0x15B7");
    cJSON_AddNumberToObject(vfdEnergyBlock, "size", 16);
    cJSON_AddStringToObject(vfdEnergyBlock, "type", "energy");
    cJSON_AddItemToArray(blocks, vfdEnergyBlock);

    /* 3.3. Backup Daily Runtimes & VFD Energy block (0x15B8 - 0x15E7) */
    cJSON *backupBlock = cJSON_CreateObject();
    cJSON_AddStringToObject(backupBlock, "name", "Backup Daily Runtimes & VFD Energy");
    cJSON_AddStringToObject(backupBlock, "addr", "0x15B8 - 0x15E7");
    cJSON_AddNumberToObject(backupBlock, "size", 48);
    cJSON_AddStringToObject(backupBlock, "type", "backup");
    cJSON_AddItemToArray(blocks, backupBlock);

    /* 4. System States & Modes block (0x1600 - 0x1615) */
    cJSON *sysBlock = cJSON_CreateObject();
    cJSON_AddStringToObject(sysBlock, "name", "System States, Modes & Logs Config");
    cJSON_AddStringToObject(sysBlock, "addr", "0x1600 - 0x1615");
    cJSON_AddNumberToObject(sysBlock, "size", 22);
    cJSON_AddStringToObject(sysBlock, "type", "system");
    cJSON_AddNumberToObject(sysBlock, "pond_mode", Sys_Info.pondMode);
    cJSON_AddNumberToObject(sysBlock, "vfd_enabled", Sys_Info.vfdEnabled);
    cJSON_AddNumberToObject(sysBlock, "feeder_mode", Sys_Info.feederMode);
    cJSON_AddNumberToObject(sysBlock, "feeder_active_id", Sys_Info.activeFeederId);
    cJSON_AddNumberToObject(sysBlock, "oxy_mode", Sys_Info.oxyMode);
    cJSON_AddNumberToObject(sysBlock, "oxy_active_id", Sys_Info.activeOxyId);
    cJSON_AddItemToArray(blocks, sysBlock);

    /* 5. Free Space block (0x1616 - dynamic end) */
    cJSON *freeBlock = cJSON_CreateObject();
    cJSON_AddStringToObject(freeBlock, "name", "Unallocated Free Memory");
    char freeAddrStr[32];
    snprintf(freeAddrStr, sizeof(freeAddrStr), "0x1616 - 0x%04X", (unsigned int)(total_capacity - 1));
    cJSON_AddStringToObject(freeBlock, "addr", freeAddrStr);
    cJSON_AddNumberToObject(freeBlock, "size", (total_capacity > 0x1616) ? (total_capacity - 0x1616) : 0);
    cJSON_AddStringToObject(freeBlock, "type", "free");
    cJSON_AddItemToArray(blocks, freeBlock);

    cJSON_AddItemToObject(root, "blocks", blocks);

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t ram_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_FAIL;

    // RAM Nội bộ (Internal SRAM)
    cJSON *internal = cJSON_CreateObject();
    if (internal)
    {
        size_t free_bytes = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t total_bytes = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
        size_t min_free = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        size_t used_bytes = (total_bytes > free_bytes) ? (total_bytes - free_bytes) : 0;

        cJSON_AddNumberToObject(internal, "total", total_bytes);
        cJSON_AddNumberToObject(internal, "free", free_bytes);
        cJSON_AddNumberToObject(internal, "used", used_bytes);
        cJSON_AddNumberToObject(internal, "min_free", min_free);
        cJSON_AddItemToObject(root, "internal", internal);
    }

    // RAM Ngoại vi (External PSRAM)
    cJSON *external = cJSON_CreateObject();
    if (external)
    {
        size_t free_bytes = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        size_t total_bytes = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
        size_t min_free = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
        size_t used_bytes = (total_bytes > free_bytes) ? (total_bytes - free_bytes) : 0;

        cJSON_AddNumberToObject(external, "total", total_bytes);
        cJSON_AddNumberToObject(external, "free", free_bytes);
        cJSON_AddNumberToObject(external, "used", used_bytes);
        cJSON_AddNumberToObject(external, "min_free", min_free);
        cJSON_AddItemToObject(root, "external", external);
    }

    cJSON_AddBoolToObject(root, "perf_enabled", Sys_Info.perfMonitorEnabled == 1);

    // Tải thông tin các Task đang hoạt động
    cJSON *tasks = cJSON_CreateArray();
    if (tasks)
    {
        if (Sys_Info.perfMonitorEnabled == 1)
        {
            UBaseType_t uxArraySize = uxTaskGetNumberOfTasks();
            TaskStatus_t *pxTaskStatusArray = malloc(uxArraySize * sizeof(TaskStatus_t));
            if (pxTaskStatusArray != NULL)
            {
                uxArraySize = uxTaskGetSystemState(pxTaskStatusArray, uxArraySize, NULL);
                for (UBaseType_t i = 0; i < uxArraySize; i++)
                {
                    cJSON *task = cJSON_CreateObject();
                    if (task)
                    {
                        cJSON_AddStringToObject(task, "name", pxTaskStatusArray[i].pcTaskName);
                        cJSON_AddNumberToObject(task, "priority", pxTaskStatusArray[i].uxCurrentPriority);
                        
                        #if ( configTASKLIST_INCLUDE_COREID == 1 )
                        cJSON_AddNumberToObject(task, "core", pxTaskStatusArray[i].xCoreID);
                        #else
                        cJSON_AddNumberToObject(task, "core", -1);
                        #endif

                        // Lấy dung lượng stack được phân bổ
                        uint32_t total_stack = 4096; // Fallback mặc định
                        const char *name = pxTaskStatusArray[i].pcTaskName;
                        if (strcmp(name, "Azure Task") == 0) total_stack = 4*4096;
                        else if (strcmp(name, "Azure transmit") == 0) total_stack = 2*4096;
                        else if (strcmp(name, "Azure process") == 0 || strcmp(name, "Azure process loop") == 0) total_stack = 8192;
                        else if (strcmp(name, "IO Task") == 0) total_stack = 8192;
                        else if (strcmp(name, "http server Task") == 0 || strcmp(name, "httpd") == 0) total_stack = 10240;
                        else if (strcmp(name, "Timer Task") == 0) total_stack = 4096;
                        else if (strcmp(name, "OTA Task") == 0) total_stack = 8192;
                        else if (strcmp(name, "Ext Flash Task") == 0) total_stack = 8192;
                        else if (strcmp(name, "main") == 0) total_stack = CONFIG_ESP_MAIN_TASK_STACK_SIZE;
                        else if (strcmp(name, "sys_evt") == 0) total_stack = CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE;
                        else if (strcmp(name, "tiT") == 0) total_stack = CONFIG_LWIP_TCPIP_TASK_STACK_SIZE;
                        else if (strcmp(name, "esp_timer") == 0) total_stack = CONFIG_ESP_TIMER_TASK_STACK_SIZE;
                        else if (strncmp(name, "IDLE", 4) == 0) total_stack = 1536;
                        else if (strncmp(name, "ipc", 3) == 0) total_stack = 2048;
                        else if (strcmp(name, "Tmr Svc") == 0) total_stack = 2048;

                        cJSON_AddNumberToObject(task, "stack_total", total_stack);
                        cJSON_AddNumberToObject(task, "stack_min_free", pxTaskStatusArray[i].usStackHighWaterMark);
                        
                        // Kiểm tra xem stack có nằm trên PSRAM không
                        bool is_psram = false;
                        if (pxTaskStatusArray[i].pxStackBase != NULL)
                        {
                            is_psram = esp_ptr_external_ram(pxTaskStatusArray[i].pxStackBase);
                        }
                        cJSON_AddBoolToObject(task, "is_psram", is_psram);

                        cJSON_AddItemToArray(tasks, task);
                    }
                }
                free(pxTaskStatusArray);
            }
        }
        cJSON_AddItemToObject(root, "tasks", tasks);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t system_logs_get_handler(httpd_req_t *req)
{
    // Thêm custom header để thông báo trạng thái bật/tắt Console cho trình duyệt
    httpd_resp_set_hdr(req, "X-Console-Enabled", Sys_Info.consoleMonitorEnabled == 1 ? "true" : "false");

    char query[64] = {0};
    uint32_t last_seq = 0;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK)
    {
        char param[32] = {0};
        if (httpd_query_key_value(query, "last_seq", param, sizeof(param)) == ESP_OK)
        {
            last_seq = (uint32_t)strtoul(param, NULL, 10);
        }
    }

    cJSON *root = cJSON_CreateArray();
    if (root == NULL) {
        return ESP_FAIL;
    }

    if (Sys_Info.consoleMonitorEnabled == 1)
    {
        uint32_t max_count = 100;
        WebLogLine_t *logs = (WebLogLine_t *)heap_caps_malloc(max_count * sizeof(WebLogLine_t), MALLOC_CAP_SPIRAM);
        if (logs != NULL)
        {
            uint32_t count = User_Log_Stream_Get(last_seq, logs, max_count);
            for (uint32_t i = 0; i < count; i++)
            {
                cJSON *item = cJSON_CreateObject();
                if (item != NULL)
                {
                    cJSON_AddNumberToObject(item, "seq", logs[i].seq);
                    cJSON_AddStringToObject(item, "text", logs[i].text);
                    cJSON_AddItemToArray(root, item);
                }
            }
            free(logs);
        }
    }

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);
    cJSON_Delete(root);

    return ESP_OK;
}

esp_err_t perf_config_post_handler(httpd_req_t *req)
{
    char content[128];
    size_t recv_size = MIN(req->content_len, sizeof(content) - 1);
    int ret = httpd_req_recv(req, content, recv_size);
    if (ret <= 0) return ESP_FAIL;
    content[ret] = '\0';
    cJSON *root = cJSON_Parse(content);
    if (root)
    {
        cJSON *enabled_item = cJSON_GetObjectItem(root, "enabled");
        if (enabled_item)
        {
            bool enabled = cJSON_IsTrue(enabled_item);
            Sys_Info.perfMonitorEnabled = enabled ? 1 : 0;
            Fram_Write_Data(FRAM_PERF_MONITOR_ENABLED_ADDR, &Sys_Info.perfMonitorEnabled, 1);
            ESP_LOGI("HTTP", "Performance monitor set to: %s", enabled ? "ON" : "OFF");
        }
        cJSON_Delete(root);
    }
    httpd_resp_set_type(req, "application/json");
    const char *resp = "{\"status\":\"ok\"}";
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

esp_err_t console_config_post_handler(httpd_req_t *req)
{
    char content[128];
    size_t recv_size = MIN(req->content_len, sizeof(content) - 1);
    int ret = httpd_req_recv(req, content, recv_size);
    if (ret <= 0) return ESP_FAIL;
    content[ret] = '\0';
    cJSON *root = cJSON_Parse(content);
    if (root)
    {
        cJSON *enabled_item = cJSON_GetObjectItem(root, "enabled");
        if (enabled_item)
        {
            bool enabled = cJSON_IsTrue(enabled_item);
            Sys_Info.consoleMonitorEnabled = enabled ? 1 : 0;
            Fram_Write_Data(FRAM_CONSOLE_MONITOR_ENABLED_ADDR, &Sys_Info.consoleMonitorEnabled, 1);
            ESP_LOGI("HTTP", "Console monitor set to: %s", enabled ? "ON" : "OFF");
        }
        cJSON_Delete(root);
    }
    httpd_resp_set_type(req, "application/json");
    const char *resp = "{\"status\":\"ok\"}";
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

static const httpd_uri_t hello = { .uri = "/hello", .method = HTTP_GET, .handler = hello_get_handler };
httpd_uri_t perf_config_api = { .uri = "/api/system/perf_config", .method = HTTP_POST, .handler = perf_config_post_handler };
httpd_uri_t console_config_api = { .uri = "/api/system/console_config", .method = HTTP_POST, .handler = console_config_post_handler };
httpd_uri_t control = { .uri = "/api/control", .method = HTTP_POST, .handler = device_control_post_handler };
httpd_uri_t uri_post_schedule = { .uri = "/api/schedule", .method = HTTP_POST, .handler = schedule_post_handler };
httpd_uri_t status = { .uri = "/api/status", .method = HTTP_GET, .handler = status_get_handler };
httpd_uri_t schedules_api = { .uri = "/api/schedules", .method = HTTP_GET, .handler = schedules_get_handler };
httpd_uri_t vfd_api = { .uri = "/api/vfd", .method = HTTP_GET, .handler = vfd_get_handler };
httpd_uri_t fram_map_api = { .uri = "/api/fram/map", .method = HTTP_GET, .handler = fram_map_get_handler };
httpd_uri_t ram_api = { .uri = "/api/system/ram", .method = HTTP_GET, .handler = ram_get_handler };
httpd_uri_t system_logs_api = { .uri = "/api/system/logs", .method = HTTP_GET, .handler = system_logs_get_handler };

esp_err_t http_404_error_handler(httpd_req_t *req, httpd_err_code_t err)
{
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not Found");
    return ESP_FAIL;
}

#include "lwip/apps/netbiosns.h"
#include "esp_mac.h"
static httpd_handle_t start_webserver(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    
    char mdns_host[32];
    snprintf(mdns_host, sizeof(mdns_host), "mebieco-%02x%02x%02x", mac[3], mac[4], mac[5]);
    
    char netbios_host[16];
    snprintf(netbios_host, sizeof(netbios_host), "MEBIECO-%02X%02X%02X", mac[3], mac[4], mac[5]);

    esp_err_t err = mdns_init();
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE)
    {
        mdns_hostname_set(mdns_host);
        mdns_instance_name_set("Mebieco Controller");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    }
    netbiosns_init();
    netbiosns_set_name(netbios_host);

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.max_uri_handlers = 32;
    config.stack_size = 10240; // 10 KB stack to prevent stack overflow
    config.uri_match_fn = httpd_uri_match_wildcard;
    
    if (httpd_start(&server, &config) == ESP_OK)
    {
        httpd_register_uri_handler(server, &hello);
        httpd_register_uri_handler(server, &control);
        httpd_register_uri_handler(server, &perf_config_api);
        httpd_register_uri_handler(server, &console_config_api);
        httpd_register_uri_handler(server, &uri_post_schedule);
        httpd_register_uri_handler(server, &status);
        httpd_register_uri_handler(server, &schedules_api);
        httpd_register_uri_handler(server, &vfd_api);
        httpd_register_uri_handler(server, &fram_map_api);
        httpd_register_uri_handler(server, &ram_api);
        httpd_register_uri_handler(server, &system_logs_api);
        web_portal_register_handlers(server);
        return server;
    }
    return NULL;
}


void User_Http_Server_Task(void)
{
    start_webserver();
    while(1) vTaskDelay(pdMS_TO_TICKS(100));
}
