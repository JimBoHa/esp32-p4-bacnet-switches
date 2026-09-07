#ifndef REDIRECT_TEST_HTTP_SERVER_H
#define REDIRECT_TEST_HTTP_SERVER_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
typedef void *httpd_handle_t;
typedef enum { HTTP_GET, HTTP_HEAD, HTTP_POST, HTTP_PUT } httpd_method_t;
typedef struct {
    httpd_handle_t handle;
    httpd_method_t method;
    const char *uri;
    size_t content_len;
} httpd_req_t;
typedef struct {
    const char *uri;
    httpd_method_t method;
    esp_err_t (*handler)(httpd_req_t *);
} httpd_uri_t;
typedef struct {
    uint16_t server_port, ctrl_port, max_uri_handlers, max_resp_headers;
    uint16_t max_open_sockets, backlog_conn;
    bool lru_purge_enable, keep_alive_enable;
    unsigned stack_size, recv_wait_timeout, send_wait_timeout;
    bool (*uri_match_fn)(const char *, const char *, size_t);
} httpd_config_t;
#define HTTPD_DEFAULT_CONFIG() { .ctrl_port = 32768 }
esp_err_t httpd_start(httpd_handle_t *, const httpd_config_t *);
esp_err_t httpd_stop(httpd_handle_t);
esp_err_t httpd_register_uri_handler(httpd_handle_t, const httpd_uri_t *);
esp_err_t httpd_resp_set_status(httpd_req_t *, const char *);
esp_err_t httpd_resp_set_type(httpd_req_t *, const char *);
esp_err_t httpd_resp_set_hdr(httpd_req_t *, const char *, const char *);
esp_err_t httpd_resp_send(httpd_req_t *, const char *, ssize_t);
int httpd_req_to_sockfd(httpd_req_t *);
esp_err_t httpd_sess_trigger_close(httpd_handle_t, int);
bool httpd_uri_match_wildcard(const char *, const char *, size_t);
#endif
