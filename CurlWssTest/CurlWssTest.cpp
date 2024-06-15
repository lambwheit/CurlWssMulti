#define _CRT_SECURE_NO_WARNINGS
#include <sstream>
#include <stdio.h>
#include <string.h>
#include <string>
#include <uv.h>
#include <curl/curl.h>
#include <windows.h>

int check_curl_ws(curl_version_info_data* curl_version_info_data)
{
    int wss = 0;
    while (*curl_version_info_data->protocols)
    {
        if (strcmp(*curl_version_info_data->protocols, "wss") == 0)
        {
            wss = 1;
            break;
        }
        curl_version_info_data->protocols++;
    }
    return wss;
}
void send_data(CURL* curl, const char* data)
{
    size_t sent;
    CURLcode result = curl_ws_send(curl, (const void*)data, strlen(data), &sent, 0, CURLWS_TEXT);
    printf("result %d, sent %d\n", result, sent);
}
int recv_any(CURL* curl) {
    size_t rlen;
    const struct curl_ws_frame* meta;
    char buffer[512]; // Buffer size increased to accommodate larger data fragments
    CURLcode result;
    std::string out_data;
    std::stringstream data_stream;
    do {
        result = curl_ws_recv(curl, buffer, sizeof(buffer), &rlen, &meta);
        if (result != CURLE_OK) {
            if (result != CURLE_AGAIN)
                printf("Error occurred in recv_any. Code: %d\n", result);
            //CURLE_AGAIN = no data
            return 0;
            //return result;
        }
        if (meta->flags & CURLWS_TEXT) {
            data_stream.write(buffer, rlen);
        }
        else if (meta->flags & CURLWS_CLOSE) {
            printf("Connection closed\n");
            exit(0);  // Consider changing this to handle the closure without exiting.
        }
        else {
            printf("Other WebSocket flags or data\n");
        }
    } while (meta->bytesleft > 0); // Continue reading while there are bytes left
    out_data = data_stream.str();
    printf("Received data: %s\n", out_data.c_str());
    return 0;
}
void websocket_close(CURL* curl) {
    size_t sent;
    (void)curl_ws_send(curl, "", 0, &sent, 0, CURLWS_CLOSE);
}


void handle_websocket(CURL* curl) {
    do {
        int result = recv_any(curl);
        //Sleep(200);
        //send_hearbeat(curl);
    } while (true);
}
static void websocket_timer_callback(uv_timer_t* handle) {
    CURL* curl = static_cast<CURL*>(handle->data);
    if (curl) {
        recv_any(curl);  // Receive any data available
    }
}


uv_loop_t* loop;
CURLM* multi_handle;
uv_timer_t timeout;

typedef struct curl_context_s {
    uv_poll_t poll_handle;
    curl_socket_t sockfd;
} curl_context_t;
curl_context_t* create_curl_context(curl_socket_t sockfd)
{
    curl_context_t* context;

    context = (curl_context_t*)malloc(sizeof(*context));

    context->sockfd = sockfd;

    uv_poll_init_socket(loop, &context->poll_handle, sockfd);
    context->poll_handle.data = context;

    return context;
}
void curl_close_cb(uv_handle_t* handle)
{
    curl_context_t* context = (curl_context_t*)handle->data;
    free(context);
}
void destroy_curl_context(curl_context_t* context)
{
    uv_close((uv_handle_t*)&context->poll_handle, curl_close_cb);
}
void check_multi_info(void)
{
    char* done_url;
    CURLMsg* message;
    int pending;
    CURL* easy_handle;
    FILE* file;
    std::string out_data;
    std::stringstream data_stream;
    uv_timer_t* ws_timer;
    while ((message = curl_multi_info_read(multi_handle, &pending))) {
        switch (message->msg) {
        case CURLMSG_DONE:
            /* Do not use message data after calling curl_multi_remove_handle() and
               curl_easy_cleanup(). As per curl_multi_info_read() docs:
               "WARNING: The data the returned pointer points to does not survive
               calling curl_multi_cleanup, curl_multi_remove_handle or
               curl_easy_cleanup." */
            easy_handle = message->easy_handle;
            ws_timer = (uv_timer_t*)malloc(sizeof(uv_timer_t));
            uv_timer_init(loop, ws_timer);
            ws_timer->data = easy_handle;  // Store CURL handle in timer data for callback use
            uv_timer_start(ws_timer, websocket_timer_callback, 0, 1);  // Set timer for 1 second recurring
            break;
        default:
            fprintf(stderr, "CURLMSG default, curl multi info not done\n");
            break;
        }
    }
}
void curl_perform(uv_poll_t* req, int status, int events)
{
    int running_handles;
    int flags = 0;
    curl_context_t* context;
    if (events & UV_READABLE)
        flags |= CURL_CSELECT_IN;
    if (events & UV_WRITABLE)
        flags |= CURL_CSELECT_OUT;
    context = (curl_context_t*)req->data;
    curl_multi_socket_action(multi_handle, context->sockfd, flags, &running_handles);
    check_multi_info();
}
int handle_socket(CURL* easy, curl_socket_t s, int action, void* userp, void* socketp)
{
    curl_context_t* curl_context;
    int events = 0;
    switch (action) {
    case CURL_POLL_IN:
    case CURL_POLL_OUT:
    case CURL_POLL_INOUT:
        curl_context = socketp ?
            (curl_context_t*)socketp : create_curl_context(s);
        curl_multi_assign(multi_handle, s, (void*)curl_context);
        if (action != CURL_POLL_IN)
            events |= UV_WRITABLE;
        if (action != CURL_POLL_OUT)
            events |= UV_READABLE;
        uv_poll_start(&curl_context->poll_handle, events, curl_perform);
        break;
    case CURL_POLL_REMOVE:
        if (socketp) {
            uv_poll_stop(&((curl_context_t*)socketp)->poll_handle);
            destroy_curl_context((curl_context_t*)socketp);
            curl_multi_assign(multi_handle, s, NULL);
        }
        break;
    default:
        abort();
    }
    return 0;
}
void on_timeout(uv_timer_t* req)
{
    int running_handles;
    curl_multi_socket_action(multi_handle, CURL_SOCKET_TIMEOUT, 0,
        &running_handles);
    check_multi_info();
}
int start_timeout(CURLM* multi, long timeout_ms, void* userp)
{
    if (timeout_ms < 0) {
        uv_timer_stop(&timeout);
    }
    else {
        if (timeout_ms == 0)
            timeout_ms = 1; /* 0 means call socket_action asap */
        uv_timer_start(&timeout, on_timeout, timeout_ms, 0);
    }
    return 0;
}


int main(void) {
    if (!check_curl_ws(curl_version_info(CURLVERSION_NOW)))
    {
        (void)fprintf(stderr, "libcurl doesn't support wss\n");
        return 1;
    }
    loop = uv_default_loop();
    curl_global_init(CURL_GLOBAL_ALL);
    uv_timer_init(loop, &timeout);
    multi_handle = curl_multi_init(); // init curl multi handle
    curl_multi_setopt(multi_handle, CURLMOPT_SOCKETFUNCTION, handle_socket);
    curl_multi_setopt(multi_handle, CURLMOPT_TIMERFUNCTION, start_timeout);
    const char* wss_urls[] = {
    };
    // add Downloads in multiUV example
    for (auto& url : wss_urls)
    {
        CURL* curl_easy_handle = curl_easy_init();
        if (curl_easy_handle)
        {
            curl_easy_setopt(curl_easy_handle, CURLOPT_URL, url);
            curl_easy_setopt(curl_easy_handle, CURLOPT_CONNECT_ONLY, 2L);
            curl_multi_add_handle(multi_handle, curl_easy_handle);
        }
    }
    uv_run(loop, UV_RUN_DEFAULT);
    curl_multi_cleanup(multi_handle);

    return 0;
}

