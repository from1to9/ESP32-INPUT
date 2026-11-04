// SPDX-License-Identifier: Apache-2.0
#include "app_control_web.h"
#include <stdint.h>
#include <string.h>

extern const uint8_t app_web_html_start[] __asm__("_binary_controller_html_start");
extern const uint8_t app_web_html_end[] __asm__("_binary_controller_html_end");
extern const uint8_t app_web_css_start[] __asm__("_binary_controller_css_start");
extern const uint8_t app_web_css_end[] __asm__("_binary_controller_css_end");
extern const uint8_t app_web_js_start[] __asm__("_binary_controller_js_start");
extern const uint8_t app_web_js_end[] __asm__("_binary_controller_js_end");

esp_err_t app_control_web_serve(httpd_req_t *request, const char *allowed_host,
                                const char *browser_origin)
{
    char host[129] = {0}, origin[193] = {0};
    size_t host_length = httpd_req_get_hdr_value_len(request, "Host");
    size_t origin_length = httpd_req_get_hdr_value_len(request, "Origin");
    if (!allowed_host || !browser_origin || !host_length || host_length >= sizeof(host) ||
        origin_length >= sizeof(origin) ||
        httpd_req_get_hdr_value_str(request, "Host", host, sizeof(host)) != ESP_OK ||
        strcmp(host, allowed_host) ||
        (origin_length && (httpd_req_get_hdr_value_str(request, "Origin", origin, sizeof(origin)) != ESP_OK ||
                           strcmp(origin, browser_origin)))) {
        httpd_resp_set_status(request, "403 Forbidden");
        return httpd_resp_sendstr(request, "Forbidden");
    }
    const uint8_t *start = NULL, *end = NULL;
    const char *type = NULL;
    if (!strcmp(request->uri, "/")) {
        start = app_web_html_start; end = app_web_html_end; type = "text/html; charset=utf-8";
    } else if (!strcmp(request->uri, "/controller.css")) {
        start = app_web_css_start; end = app_web_css_end; type = "text/css; charset=utf-8";
    } else if (!strcmp(request->uri, "/controller.js")) {
        start = app_web_js_start; end = app_web_js_end; type = "text/javascript; charset=utf-8";
    } else {
        httpd_resp_set_status(request, "404 Not Found");
        return httpd_resp_sendstr(request, "Not found");
    }
    httpd_resp_set_type(request, type);
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(request, "Referrer-Policy", "no-referrer");
    httpd_resp_set_hdr(request, "Cross-Origin-Resource-Policy", "same-origin");
    httpd_resp_set_hdr(request, "Permissions-Policy", "camera=(), microphone=(), geolocation=()");
    httpd_resp_set_hdr(request, "Content-Security-Policy",
        "default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self'; "
        "base-uri 'none'; form-action 'self'; frame-ancestors 'none'; object-src 'none'");
    /* ESP-IDF's TEXT embedding appends one NUL; it is not part of the response. */
    return httpd_resp_send(request, (const char *)start, (ssize_t)(end - start - 1));
}
