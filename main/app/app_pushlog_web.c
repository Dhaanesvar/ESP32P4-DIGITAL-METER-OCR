#include <string.h>
#include <stdio.h>

#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_pushlog_camera.h"
#include "app_pushlog_web.h"

static const char *TAG = "pushlog_web";
static uint8_t *s_frame_buf = NULL;
static const size_t s_frame_buf_size = 220 * 1024;

static httpd_handle_t s_ctrl_httpd = NULL;
static httpd_handle_t s_stream_httpd = NULL;

static const char s_index_html[] =
"<!doctype html>\n"
"<html><head><meta charset=\"utf-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
"<title>Pushlog Live Snap</title>\n"
"<style>\n"
":root{--bg:#f6f7fb;--card:#ffffff;--ink:#111827;--muted:#5b6473;--line:#dfe4ee;--accent:#0f766e;--accent2:#164e63;--ok:#15803d;--bad:#b91c1c;}\n"
"*{box-sizing:border-box}body{margin:0;background:radial-gradient(1100px 500px at 5% -10%,#d9f3ff 0%,transparent 60%),radial-gradient(1000px 500px at 100% 0%,#d8ffe7 0%,transparent 60%),var(--bg);font-family:'Trebuchet MS',Tahoma,sans-serif;color:var(--ink)}\n"
".wrap{max-width:980px;margin:24px auto;padding:0 14px}.head{display:flex;justify-content:space-between;align-items:end;gap:10px}.title{font-size:30px;font-weight:800;letter-spacing:.2px}.sub{color:var(--muted);margin-top:6px}\n"
".grid{display:grid;grid-template-columns:2fr 1fr;gap:14px;margin-top:14px}.card{background:var(--card);border:1px solid var(--line);border-radius:16px;box-shadow:0 16px 32px rgba(15,23,42,.08);padding:12px}\n"
".frame{position:relative;width:100%}.live{width:100%;aspect-ratio:16/9;border-radius:12px;border:1px solid #cfd6e6;background:#0b1020;object-fit:cover}.roi{position:absolute;left:32%;top:36%;width:36%;height:26%;border:3px solid #00d26a;border-radius:10px;box-shadow:0 0 0 2px rgba(0,0,0,.25) inset;pointer-events:none}.label{font-size:13px;color:var(--muted);margin:8px 2px 0}\n"
".btns{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-top:10px}.btn{border:0;border-radius:12px;padding:13px 12px;font-size:15px;font-weight:800;cursor:pointer}.primary{background:linear-gradient(135deg,var(--accent),var(--accent2));color:#fff}.secondary{background:#eef2ff;color:#1e293b;border:1px solid #d8e0fb}\n"
".btn:disabled{opacity:.65;cursor:not-allowed}.status{margin-top:10px;min-height:22px;font-weight:700}.ok{color:var(--ok)}.err{color:var(--bad)}.wait{color:#a16207}\n"
".snap{width:100%;aspect-ratio:4/3;border-radius:12px;border:1px solid #cfd6e6;background:#f1f5f9;object-fit:cover}.hint{margin-top:10px;font-size:13px;color:var(--muted);line-height:1.4}\n"
"@media (max-width:900px){.grid{grid-template-columns:1fr}.title{font-size:24px}}\n"
"</style></head>\n"
"<body><div class=\"wrap\">\n"
"<div class=\"head\"><div><div class=\"title\">Pushlog Camera Control</div><div class=\"sub\">Live stream + one-tap capture upload</div></div></div>\n"
"<div class=\"grid\">\n"
"<section class=\"card\">\n"
"<div class=\"frame\"><img id=\"live\" class=\"live\" src=\"\" alt=\"Live stream\"><div class=\"roi\"></div></div>\n"
"<div class=\"label\">Live stream endpoint: :81/stream</div>\n"
"</section>\n"
"<aside class=\"card\">\n"
"<div class=\"frame\"><img id=\"snapPreview\" class=\"snap\" src=\"\" alt=\"Latest captured frame\"><div class=\"roi\"></div></div>\n"
"<div class=\"label\">Latest captured frame preview</div>\n"
"<div id=\"ocrWord\" class=\"label\">OCR word: --</div>\n"
"<div id=\"meter\" class=\"label\">Meter reading: --</div>\n"
"<div class=\"btns\"><button id=\"expDown\" class=\"btn secondary\">Darker</button><button id=\"expUp\" class=\"btn secondary\">Brighter</button></div>\n"
"<div class=\"btns\"><button id=\"expAuto\" class=\"btn secondary\">Auto Exposure</button><button id=\"expManual\" class=\"btn secondary\">Manual Exposure</button></div>\n"
"<div class=\"btns\"><button id=\"zoomOut\" class=\"btn secondary\">Zoom -</button><button id=\"zoomIn\" class=\"btn secondary\">Zoom +</button></div>\n"
"<div class=\"btns\"><button id=\"afOn\" class=\"btn secondary\">Autofocus ON</button><button id=\"afOff\" class=\"btn secondary\">Autofocus OFF</button></div>\n"
"<div class=\"btns\"><button id=\"snap\" class=\"btn primary\">Snap & Send</button><button id=\"refresh\" class=\"btn secondary\">Refresh Live</button></div>\n"
"<div id=\"status\" class=\"status\"></div>\n"
"<div class=\"hint\">Use the on-device knob for zoom/focus. This page sends POST /snap and shows /jpg preview.</div>\n"
"</aside>\n"
"</div></div>\n"
"<script>\n"
"const snapBtn=document.getElementById('snap');const refreshBtn=document.getElementById('refresh');const statusEl=document.getElementById('status');\n"
"const zoomInBtn=document.getElementById('zoomIn');const zoomOutBtn=document.getElementById('zoomOut');\n"
"const expDownBtn=document.getElementById('expDown');const expUpBtn=document.getElementById('expUp');\n"
"const expAutoBtn=document.getElementById('expAuto');const expManualBtn=document.getElementById('expManual');\n"
"const afOnBtn=document.getElementById('afOn');const afOffBtn=document.getElementById('afOff');\n"
"const liveEl=document.getElementById('live');const snapEl=document.getElementById('snapPreview');\n"
"const ocrWordEl=document.getElementById('ocrWord');\n"
"const meterEl=document.getElementById('meter');\n"
"const host=location.hostname;\n"
"let liveFallbackTried=false;\n"
"function setStatus(kind,msg){statusEl.className='status '+kind;statusEl.textContent=msg||'';}\n"
"function setLive(){liveFallbackTried=false;liveEl.src='/stream?ts='+Date.now();}\n"
"function setSnapPreview(){snapEl.src='/jpg?ts='+Date.now();}\n"
"async function postCmd(path){const r=await fetch(path,{method:'POST'});const t=await r.text();if(!r.ok)throw new Error(t||'command failed');return t;}\n"
"async function refreshMeter(){try{const r=await fetch('/ocr');if(!r.ok)return;const j=await r.json();if(j.ok){ocrWordEl.textContent='OCR word: '+(j.word||'--');meterEl.textContent='Meter reading: '+(j.reading||'--')+' (score '+Number(j.score||0).toFixed(2)+')';}else{ocrWordEl.textContent='OCR word: --';meterEl.textContent='Meter reading: --';}}catch(e){}}\n"
"liveEl.onerror=()=>{if(!liveFallbackTried){liveFallbackTried=true;liveEl.src='http://'+host+':81/stream?ts='+Date.now();setStatus('wait','Trying fallback stream...');return;}setStatus('err','Live stream unavailable right now');};\n"
"setLive();setSnapPreview();refreshMeter();setInterval(refreshMeter,2000);\n"
"zoomInBtn.onclick=async()=>{try{const t=await postCmd('/zoom?dir=in');setStatus('ok',t||'Zoom updated');setTimeout(setSnapPreview,300);}catch(e){setStatus('err',e.message||'Zoom failed');}};\n"
"zoomOutBtn.onclick=async()=>{try{const t=await postCmd('/zoom?dir=out');setStatus('ok',t||'Zoom updated');setTimeout(setSnapPreview,300);}catch(e){setStatus('err',e.message||'Zoom failed');}};\n"
"expDownBtn.onclick=async()=>{try{const t=await postCmd('/exposure?dir=down');setStatus('ok',t||'Exposure updated');setTimeout(setSnapPreview,300);}catch(e){setStatus('err',e.message||'Exposure failed');}};\n"
"expUpBtn.onclick=async()=>{try{const t=await postCmd('/exposure?dir=up');setStatus('ok',t||'Exposure updated');setTimeout(setSnapPreview,300);}catch(e){setStatus('err',e.message||'Exposure failed');}};\n"
"expAutoBtn.onclick=async()=>{try{const t=await postCmd('/exposure_auto?enable=1');setStatus('ok',t||'Auto exposure enabled');setTimeout(setSnapPreview,300);}catch(e){setStatus('err',e.message||'Auto exposure failed');}};\n"
"expManualBtn.onclick=async()=>{try{const t=await postCmd('/exposure_auto?enable=0');setStatus('ok',t||'Manual exposure enabled');setTimeout(setSnapPreview,300);}catch(e){setStatus('err',e.message||'Manual exposure failed');}};\n"
"afOnBtn.onclick=async()=>{try{const t=await postCmd('/autofocus?enable=1');setStatus('ok',t||'Autofocus enabled');}catch(e){setStatus('err',e.message||'Autofocus failed');}};\n"
"afOffBtn.onclick=async()=>{try{const t=await postCmd('/autofocus?enable=0');setStatus('ok',t||'Autofocus disabled');}catch(e){setStatus('err',e.message||'Autofocus failed');}};\n"
"refreshBtn.onclick=()=>{setStatus('wait','Refreshing live stream...');setLive();setTimeout(()=>{if(statusEl.className.indexOf('wait')>=0)setStatus('', '');},800);};\n"
"snapBtn.onclick=async()=>{snapBtn.disabled=true;setStatus('wait','Capturing and extracting...');\n"
"try{const r=await fetch('/snap',{method:'POST'});const j=await r.json();\n"
"if(r.ok&&j.ok){ocrWordEl.textContent='OCR word: '+(j.word||'--');if(j.reading){meterEl.textContent='Meter reading: '+j.reading+' (score '+Number(j.score||0).toFixed(2)+')';}setStatus('ok',j.message||'Capture queued');setTimeout(setSnapPreview,700);setTimeout(setSnapPreview,1500);}\n"
"else{setStatus('err',(j&&j.message)||'Capture request failed');}}\n"
"catch(e){setStatus('err','Network error while sending snap');}\n"
"snapBtn.disabled=false;};\n"
"</script></body></html>\n";

static esp_err_t ocr_get_handler(httpd_req_t *req)
{
    char word[32] = {0};
    char reading[32] = {0};
    float score = 0.0f;
    uint32_t seq = 0;
    bool ok = app_pushlog_camera_get_last_ocr_snapshot(word,
                                                        sizeof(word),
                                                        reading,
                                                        sizeof(reading),
                                                        &score,
                                                        &seq);

    char json[192] = {0};
    if (ok) {
        snprintf(json,
                 sizeof(json),
                 "{\"ok\":true,\"word\":\"%s\",\"reading\":\"%s\",\"score\":%.2f,\"seq\":%lu}",
                 word,
                 reading,
                 score,
                 (unsigned long)seq);
    } else {
        snprintf(json, sizeof(json), "{\"ok\":false}");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t zoom_post_handler(httpd_req_t *req)
{
    char query[64] = {0};
    esp_err_t ret = httpd_req_get_url_query_str(req, query, sizeof(query));
    if (ret != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "missing query");
    }

    char dir[8] = {0};
    if (httpd_query_key_value(query, "dir", dir, sizeof(dir)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "missing dir");
    }

    int32_t min = 0, max = 0, step = 1, cur = 0;
    if (!app_pushlog_camera_get_zoom_range(&min, &max, &step, &cur)) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "zoom not supported");
    }

    int32_t delta = (step > 0) ? step : 1;
    int32_t next = cur;
    if (strcmp(dir, "in") == 0) {
        next = cur + delta;
    } else if (strcmp(dir, "out") == 0) {
        next = cur - delta;
    } else {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "dir must be in/out");
    }

    if (next < min) {
        next = min;
    }
    if (next > max) {
        next = max;
    }

    ret = app_pushlog_camera_set_zoom(next);
    if (ret != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "zoom set failed");
    }

    char resp[64] = {0};
    snprintf(resp, sizeof(resp), "zoom=%ld", (long)next);
    return httpd_resp_sendstr(req, resp);
}

static esp_err_t autofocus_post_handler(httpd_req_t *req)
{
    char query[64] = {0};
    esp_err_t ret = httpd_req_get_url_query_str(req, query, sizeof(query));
    if (ret != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "missing query");
    }

    char enable[8] = {0};
    if (httpd_query_key_value(query, "enable", enable, sizeof(enable)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "missing enable");
    }

    bool af = (strcmp(enable, "1") == 0 || strcasecmp(enable, "true") == 0 || strcasecmp(enable, "on") == 0);
    ret = app_pushlog_camera_set_autofocus(af);
    if (ret != ESP_OK) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "autofocus not supported");
    }

    return httpd_resp_sendstr(req, af ? "autofocus=on" : "autofocus=off");
}

static esp_err_t exposure_post_handler(httpd_req_t *req)
{
    char query[64] = {0};
    esp_err_t ret = httpd_req_get_url_query_str(req, query, sizeof(query));
    if (ret != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "missing query");
    }

    char dir[8] = {0};
    if (httpd_query_key_value(query, "dir", dir, sizeof(dir)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "missing dir");
    }

    int32_t min = 0, max = 0, step = 1, cur = 0;
    if (!app_pushlog_camera_get_exposure_range(&min, &max, &step, &cur)) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "exposure not supported");
    }

    int32_t delta = (step > 0) ? step : 1;
    int32_t next = cur;
    if (strcmp(dir, "up") == 0) {
        next = cur + delta;
    } else if (strcmp(dir, "down") == 0) {
        next = cur - delta;
    } else {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "dir must be up/down");
    }

    if (next < min) {
        next = min;
    }
    if (next > max) {
        next = max;
    }

    ret = app_pushlog_camera_set_exposure(next);
    if (ret != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "exposure set failed");
    }

    char resp[80] = {0};
    snprintf(resp, sizeof(resp), "exposure=%ld", (long)next);
    return httpd_resp_sendstr(req, resp);
}

static esp_err_t exposure_auto_post_handler(httpd_req_t *req)
{
    char query[64] = {0};
    esp_err_t ret = httpd_req_get_url_query_str(req, query, sizeof(query));
    if (ret != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "missing query");
    }

    char enable[8] = {0};
    if (httpd_query_key_value(query, "enable", enable, sizeof(enable)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "missing enable");
    }

    bool ae = (strcmp(enable, "1") == 0 || strcasecmp(enable, "true") == 0 || strcasecmp(enable, "on") == 0);
    ret = app_pushlog_camera_set_auto_exposure(ae);
    if (ret != ESP_OK) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "auto exposure not supported");
    }

    return httpd_resp_sendstr(req, ae ? "auto_exposure=on" : "auto_exposure=off");
}

static esp_err_t index_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, s_index_html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t snap_post_handler(httpd_req_t *req)
{
    char before_word[32] = {0};
    char before_reading[32] = {0};
    float before_score = 0.0f;
    uint32_t before_seq = 0;
    (void)app_pushlog_camera_get_last_ocr_snapshot(before_word,
                                                   sizeof(before_word),
                                                   before_reading,
                                                   sizeof(before_reading),
                                                   &before_score,
                                                   &before_seq);

    esp_err_t ret = app_pushlog_camera_request_upload_now();
    if (ret != ESP_OK) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"message\":\"Camera not ready yet\"}");
    }

    char word[32] = {0};
    char reading[32] = {0};
    float score = 0.0f;
    uint32_t seq = before_seq;
    bool got_ocr = false;
    bool fresh = false;

    for (int i = 0; i < 30; ++i) {
        got_ocr = app_pushlog_camera_get_last_ocr_snapshot(word,
                                                           sizeof(word),
                                                           reading,
                                                           sizeof(reading),
                                                           &score,
                                                           &seq);
        if (got_ocr && seq > before_seq) {
            fresh = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (!got_ocr) {
        got_ocr = app_pushlog_camera_get_last_ocr_snapshot(word,
                                                            sizeof(word),
                                                            reading,
                                                            sizeof(reading),
                                                            &score,
                                                            &seq);
    }

    char json[256] = {0};
    if (got_ocr) {
        snprintf(json,
                 sizeof(json),
                 "{\"ok\":true,\"word\":\"%s\",\"reading\":\"%s\",\"score\":%.2f,\"fresh\":%s,\"message\":\"Captured%s\"}",
                 word,
                 reading,
                 score,
                 fresh ? "true" : "false",
                 fresh ? " and extracted" : ", using latest extracted reading");
    } else {
        snprintf(json,
                 sizeof(json),
                 "{\"ok\":true,\"fresh\":false,\"message\":\"Captured, OCR not available yet\"}");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t jpg_get_handler(httpd_req_t *req)
{
    if (!s_frame_buf) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "frame buffer not ready");
    }

    size_t out_size = 0;
    esp_err_t ret = app_pushlog_camera_get_web_jpeg(s_frame_buf, s_frame_buf_size, &out_size);
    if (ret != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "frame not ready");
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    return httpd_resp_send(req, (const char *)s_frame_buf, out_size);
}

static esp_err_t stream_get_handler(httpd_req_t *req)
{
    static const char *STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=frame";
    static const char *STREAM_BOUNDARY = "\r\n--frame\r\n";
    static const char *STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

    if (!s_frame_buf) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "frame buffer not ready");
    }

    esp_err_t ret = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    if (ret != ESP_OK) {
        return ret;
    }
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");

    char part_buf[80];
    while (1) {
        size_t out_size = 0;
        ret = app_pushlog_camera_get_web_jpeg(s_frame_buf, s_frame_buf_size, &out_size);
        if (ret != ESP_OK || out_size == 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        int header_len = snprintf(part_buf, sizeof(part_buf), STREAM_PART, (unsigned int)out_size);
        if (header_len <= 0) {
            return ESP_FAIL;
        }

        if (httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY)) != ESP_OK ||
            httpd_resp_send_chunk(req, part_buf, header_len) != ESP_OK ||
            httpd_resp_send_chunk(req, (const char *)s_frame_buf, out_size) != ESP_OK) {
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }

    return ESP_OK;
}

esp_err_t app_pushlog_web_start(void)
{
    if (s_ctrl_httpd != NULL && s_stream_httpd != NULL) {
        return ESP_OK;
    }

    httpd_config_t ctrl_config = HTTPD_DEFAULT_CONFIG();
    ctrl_config.server_port = 80;
    ctrl_config.ctrl_port = 32768;

    httpd_config_t stream_config = HTTPD_DEFAULT_CONFIG();
    stream_config.server_port = 81;
    stream_config.ctrl_port = 32769;
    stream_config.lru_purge_enable = true;

    esp_err_t ret = httpd_start(&s_ctrl_httpd, &ctrl_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "control httpd_start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = httpd_start(&s_stream_httpd, &stream_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "stream httpd_start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_frame_buf = heap_caps_malloc(s_frame_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_frame_buf) {
        ESP_LOGE(TAG, "failed to allocate frame buffer for web stream");
        return ESP_ERR_NO_MEM;
    }

    httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_get_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t snap_uri = {
        .uri = "/snap",
        .method = HTTP_POST,
        .handler = snap_post_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t jpg_uri = {
        .uri = "/jpg",
        .method = HTTP_GET,
        .handler = jpg_get_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t ocr_uri = {
        .uri = "/ocr",
        .method = HTTP_GET,
        .handler = ocr_get_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t zoom_uri = {
        .uri = "/zoom",
        .method = HTTP_POST,
        .handler = zoom_post_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t autofocus_uri = {
        .uri = "/autofocus",
        .method = HTTP_POST,
        .handler = autofocus_post_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t exposure_uri = {
        .uri = "/exposure",
        .method = HTTP_POST,
        .handler = exposure_post_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t exposure_auto_uri = {
        .uri = "/exposure_auto",
        .method = HTTP_POST,
        .handler = exposure_auto_post_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t stream_uri = {
        .uri = "/stream",
        .method = HTTP_GET,
        .handler = stream_get_handler,
        .user_ctx = NULL,
    };

    ret = httpd_register_uri_handler(s_ctrl_httpd, &index_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "register index handler failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = httpd_register_uri_handler(s_ctrl_httpd, &snap_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "register snap handler failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = httpd_register_uri_handler(s_ctrl_httpd, &jpg_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "register jpg handler failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = httpd_register_uri_handler(s_ctrl_httpd, &ocr_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "register ocr handler failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = httpd_register_uri_handler(s_ctrl_httpd, &stream_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "register control stream handler failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = httpd_register_uri_handler(s_ctrl_httpd, &zoom_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "register zoom handler failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = httpd_register_uri_handler(s_ctrl_httpd, &autofocus_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "register autofocus handler failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = httpd_register_uri_handler(s_ctrl_httpd, &exposure_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "register exposure handler failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = httpd_register_uri_handler(s_ctrl_httpd, &exposure_auto_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "register exposure_auto handler failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = httpd_register_uri_handler(s_stream_httpd, &stream_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "register stream handler failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = httpd_register_uri_handler(s_stream_httpd, &jpg_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "register stream jpg handler failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Web UI started: control on :80 (/,/snap,/jpg,/ocr,/zoom,/autofocus,/exposure,/exposure_auto,/stream), stream on :81 (/stream,/jpg)");
    return ESP_OK;
}
