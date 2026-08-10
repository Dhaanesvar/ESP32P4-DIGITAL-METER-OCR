#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"

#include "app_pushlog_camera.h"
#include "app_pushlog_web.h"

static const char *TAG = "pushlog_web";

static httpd_handle_t s_httpd = NULL;

static const char s_index_html[] =
"<!doctype html>\n"
"<html><head><meta charset=\"utf-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
"<title>Pushlog Camera</title>\n"
"<style>\n"
":root{--bg:#0f1217;--panel:#1d222b;--text:#e9edf2;--muted:#9ca7b7;--accent:#f04d3f;}\n"
"*{box-sizing:border-box;font-family:Verdana,sans-serif;}\n"
"body{margin:0;min-height:100vh;background:radial-gradient(circle at 20% 0%,#1b2230 0%,var(--bg) 50%);color:var(--text);display:grid;place-items:center;padding:20px;}\n"
".card{width:min(560px,100%);background:linear-gradient(180deg,#202735,var(--panel));border:1px solid #2f3746;border-radius:16px;padding:20px 20px 24px;box-shadow:0 20px 40px rgba(0,0,0,.35);}\n"
"h1{margin:0 0 10px;font-size:26px;letter-spacing:.3px;}\n"
"p{margin:0 0 16px;color:var(--muted);line-height:1.4;}\n"
".btn{width:100%;padding:14px 16px;border:0;border-radius:12px;background:var(--accent);color:white;font-size:18px;font-weight:700;cursor:pointer;}\n"
".btn:disabled{opacity:.6;cursor:not-allowed;}\n"
".status{margin-top:14px;min-height:22px;font-weight:600;}\n"
".ok{color:#73d13d;}.err{color:#ff7a7a;}.wait{color:#ffd166;}\n"
".hint{margin-top:10px;font-size:13px;color:var(--muted);}\n"
"</style></head>\n"
"<body><div class=\"card\">\n"
"<h1>Pushlog Camera</h1>\n"
"<p>Manual capture uploader. Press the button to snap the next frame and send it to Pushlog.</p>\n"
"<button id=\"snap\" class=\"btn\">Snap & Send to Pushlog</button>\n"
"<div id=\"status\" class=\"status\"></div>\n"
"<div class=\"hint\">Tip: On reset, firmware also triggers one immediate upload automatically.</div>\n"
"</div>\n"
"<script>\n"
"const b=document.getElementById('snap');const s=document.getElementById('status');\n"
"b.onclick=async()=>{b.disabled=true;s.className='status wait';s.textContent='Queued capture...';\n"
"try{const r=await fetch('/snap',{method:'POST'});const t=await r.text();\n"
"if(r.ok){s.className='status ok';s.textContent=t||'Capture request accepted';}\n"
"else{s.className='status err';s.textContent=t||'Capture request failed';}}\n"
"catch(e){s.className='status err';s.textContent='Network error';}\n"
"b.disabled=false;};\n"
"</script></body></html>\n";

static esp_err_t index_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, s_index_html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t snap_post_handler(httpd_req_t *req)
{
    esp_err_t ret = app_pushlog_camera_request_upload_now();
    if (ret != ESP_OK) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_sendstr(req, "Camera not ready yet");
    }

    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "Capture queued, upload in progress shortly");
}

esp_err_t app_pushlog_web_start(void)
{
    if (s_httpd != NULL) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port = 32768;

    esp_err_t ret = httpd_start(&s_httpd, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(ret));
        return ret;
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

    ret = httpd_register_uri_handler(s_httpd, &index_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "register index handler failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = httpd_register_uri_handler(s_httpd, &snap_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "register snap handler failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Web UI started: GET / , POST /snap");
    return ESP_OK;
}
