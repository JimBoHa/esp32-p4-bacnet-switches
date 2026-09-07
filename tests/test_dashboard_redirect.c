#include "dashboard_redirect.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static httpd_config_t config;
static httpd_uri_t routes[2];
static int start_count, stop_count, route_count, fail_route;
static bool fail_start, fail_address;
static struct sockaddr_storage local_address;
static socklen_t local_length;
static int status_code, send_count, close_count;
static const char *fail_header;
static char header_names[8][64], header_values[8][128];
static size_t header_count;

esp_err_t httpd_start(httpd_handle_t *handle, const httpd_config_t *value)
{
    start_count++;
    config = *value;
    if (fail_start) { return ESP_FAIL; }
    *handle = &config;
    return ESP_OK;
}
esp_err_t httpd_stop(httpd_handle_t handle)
{
    assert(handle == &config);
    stop_count++;
    return ESP_OK;
}
esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *route)
{
    assert(handle == &config);
    route_count++;
    if (route_count == fail_route) { return ESP_FAIL; }
    assert(route_count <= 2);
    routes[route_count - 1] = *route;
    return ESP_OK;
}
esp_err_t httpd_resp_set_status(httpd_req_t *request, const char *status)
{
    (void)request;
    if (strcmp(status, "302 Found") == 0) { status_code = 302; }
    else if (strcmp(status, "400 Bad Request") == 0) { status_code = 400; }
    else if (strcmp(status, "405 Method Not Allowed") == 0) { status_code = 405; }
    else { assert(strcmp(status, "503 Service Unavailable") == 0); status_code = 503; }
    return ESP_OK;
}
esp_err_t httpd_resp_set_type(httpd_req_t *request, const char *type)
{
    (void)request;
    assert(strcmp(type, "text/plain; charset=utf-8") == 0);
    return ESP_OK;
}
esp_err_t httpd_resp_set_hdr(httpd_req_t *request, const char *name, const char *value)
{
    (void)request;
    if (fail_header != NULL && strcmp(name, fail_header) == 0) { return ESP_FAIL; }
    assert(header_count < 8 && strlen(name) < 64 && strlen(value) < 128);
    strcpy(header_names[header_count], name);
    strcpy(header_values[header_count++], value);
    return ESP_OK;
}
esp_err_t httpd_resp_send(httpd_req_t *request, const char *body, ssize_t length)
{
    (void)request;
    assert(body == NULL && length == 0);
    send_count++;
    return ESP_OK;
}
int httpd_req_to_sockfd(httpd_req_t *request) { (void)request; return 42; }
esp_err_t httpd_sess_trigger_close(httpd_handle_t handle, int socket)
{
    assert(handle == &config && socket == 42);
    close_count++;
    return ESP_OK;
}
bool httpd_uri_match_wildcard(const char *pattern, const char *uri, size_t length)
{
    (void)pattern; (void)uri; (void)length;
    return true; /* Matching itself belongs to ESP-IDF; assert selected callback. */
}
int dashboard_test_getsockname(int socket, struct sockaddr *address, socklen_t *length)
{
    assert(socket == 42 && *length >= local_length);
    if (fail_address) { return -1; }
    memcpy(address, &local_address, local_length);
    *length = local_length;
    return 0;
}

static const char *header(const char *name)
{
    for (size_t index = 0; index < header_count; ++index) {
        if (strcmp(name, header_names[index]) == 0) { return header_values[index]; }
    }
    return "";
}
static void reset_response(void)
{
    status_code = send_count = close_count = 0;
    header_count = 0;
    fail_header = NULL;
}
static void set_address(int family, const char *text)
{
    memset(&local_address, 0, sizeof(local_address));
    if (family == AF_INET) {
        struct sockaddr_in *ipv4 = (struct sockaddr_in *)&local_address;
        ipv4->sin_family = AF_INET;
        local_length = sizeof(*ipv4);
        assert(inet_pton(AF_INET, text, &ipv4->sin_addr) == 1);
    } else {
        struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)&local_address;
        ipv6->sin6_family = AF_INET6;
        local_length = sizeof(*ipv6);
        assert(inet_pton(AF_INET6, text, &ipv6->sin6_addr) == 1);
    }
}
static void assert_redirect(httpd_req_t *request, const char *expected)
{
    reset_response();
    assert(routes[0].handler(request) == ESP_OK);
    assert(status_code == 302 && send_count == 1 && close_count == 1);
    assert(strcmp(header("Location"), expected) == 0);
    assert(strcmp(header("Connection"), "close") == 0);
    assert(strcmp(header("Cache-Control"), "no-store") == 0);
    assert(strcmp(header("Referrer-Policy"), "no-referrer") == 0);
    assert(strcmp(header("X-Content-Type-Options"), "nosniff") == 0);
}

int main(void)
{
    assert(dashboard_redirect_start(0) == ESP_ERR_INVALID_ARG);
    assert(dashboard_redirect_start(80) == ESP_ERR_INVALID_ARG);
    assert(start_count == 0);
    fail_start = true;
    assert(dashboard_redirect_start(443) == ESP_FAIL);
    fail_start = false;
    for (int fail = 1; fail <= 2; ++fail) {
        route_count = 0; fail_route = fail;
        assert(dashboard_redirect_start(443) == ESP_FAIL);
        assert(stop_count == fail);
    }
    fail_route = route_count = 0;
    assert(dashboard_redirect_start(443) == ESP_OK);
    assert(route_count == 2 && routes[0].method == HTTP_GET && routes[1].method == HTTP_HEAD);
    assert(strcmp(routes[0].uri, "/*") == 0 && strcmp(routes[1].uri, "/*") == 0);
    assert(routes[0].handler == routes[1].handler);
    assert(config.server_port == 80 && config.ctrl_port == 32769);
    assert(config.max_open_sockets == 1 && config.backlog_conn == 2);
    assert(config.stack_size == 4096 && config.max_uri_handlers == 2 && config.max_resp_headers == 5);
    assert(config.recv_wait_timeout == 2 && config.send_wait_timeout == 2);
    assert(config.lru_purge_enable && !config.keep_alive_enable);
    assert(config.uri_match_fn == httpd_uri_match_wildcard);
    const int starts = start_count;
    assert(dashboard_redirect_start(443) == ESP_OK && start_count == starts);
    assert(dashboard_redirect_start(8443) == ESP_ERR_INVALID_STATE);

    httpd_req_t request = {.handle = &config, .method = HTTP_GET, .uri = "/"};
    set_address(AF_INET, "192.0.2.1");
    assert_redirect(&request, "https://192.0.2.1/diagnostics");
    request.uri = "//evil.example/@other?next=https://evil.example&token=synthetic";
    assert_redirect(&request, "https://192.0.2.1/diagnostics");
    request.method = HTTP_HEAD;
    assert_redirect(&request, "https://192.0.2.1/diagnostics");
    set_address(AF_INET6, "::ffff:192.0.2.2");
    assert_redirect(&request, "https://192.0.2.2/diagnostics");
    set_address(AF_INET6, "2001:db8::1234");
    assert_redirect(&request, "https://[2001:db8::1234]/diagnostics");
    dashboard_redirect_stop();
    route_count = 0;
    assert(dashboard_redirect_start(65535) == ESP_OK);
    assert_redirect(&request, "https://[2001:db8::1234]:65535/diagnostics");
    set_address(AF_INET, "203.0.113.255");
    assert_redirect(&request, "https://203.0.113.255:65535/diagnostics");

    reset_response();
    request.method = HTTP_POST;
    assert(routes[0].handler(&request) == ESP_FAIL);
    assert(status_code == 405 && send_count == 1 && close_count == 0 && header("Location")[0] == '\0');
    assert(strcmp(header("Connection"), "close") == 0);
    const size_t body_lengths[] = {1, 4096, SIZE_MAX};
    for (int method = HTTP_GET; method <= HTTP_HEAD; ++method) {
        request.method = (httpd_method_t)method;
        for (size_t index = 0; index < sizeof(body_lengths) / sizeof(body_lengths[0]); ++index) {
            request.content_len = body_lengths[index];
            for (int secure = 0; secure < 2; ++secure) {
                reset_response();
                const esp_err_t result = secure ? dashboard_root_redirect_handler(&request)
                                               : routes[0].handler(&request);
                /* No queued close: failure tells ESP-IDF to skip body draining. */
                assert(result == ESP_FAIL && status_code == 400 && send_count == 1 && close_count == 0);
                assert(header("Location")[0] == '\0' && strcmp(header("Connection"), "close") == 0);
            }
        }
    }
    reset_response(); fail_header = "Connection";
    assert(routes[0].handler(&request) == ESP_FAIL && send_count == 0 && close_count == 0);
    request.content_len = 0;
    request.method = HTTP_GET;
    for (int bad = 0; bad < 6; ++bad) {
        reset_response();
        set_address(AF_INET, "192.0.2.1");
        fail_address = bad == 0;
        if (bad == 1) { set_address(AF_INET, "0.0.0.0"); }
        if (bad == 2) { set_address(AF_INET6, "::"); }
        if (bad == 3) { set_address(AF_INET6, "fe80::1"); }
        if (bad == 4) { local_length = 1; }
        if (bad == 5) { set_address(AF_INET6, "::ffff:0.0.0.0"); }
        assert(routes[0].handler(&request) == ESP_OK);
        assert(status_code == 503 && close_count == 1 && send_count == 1 && header("Location")[0] == '\0');
    }
    fail_address = false;
    set_address(AF_INET, "192.0.2.1");
    reset_response(); fail_header = "Location";
    assert(routes[0].handler(&request) == ESP_FAIL && send_count == 0 && close_count == 1);

    for (int method = HTTP_GET; method <= HTTP_HEAD; ++method) {
        reset_response(); request.method = (httpd_method_t)method;
        assert(dashboard_root_redirect_handler(&request) == ESP_OK);
        assert(status_code == 302 && send_count == 1 && close_count == 0);
        assert(strcmp(header("Location"), "/diagnostics") == 0);
    }
    reset_response(); request.method = HTTP_PUT;
    assert(dashboard_root_redirect_handler(&request) == ESP_FAIL && status_code == 405 && send_count == 1);
    assert(strcmp(header("Connection"), "close") == 0);
    dashboard_redirect_stop();
    const int stops = stop_count;
    dashboard_redirect_stop();
    assert(stop_count == stops);
    puts("Dashboard redirect implementation, boundaries, and lifecycle passed");
    return 0;
}
