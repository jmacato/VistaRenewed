#define _WIN32_WINNT 0x0600
#include <windows.h>
#include <ole2.h>
#include <stdio.h>
#include <string.h>
#include "mshtml_json.h"
#include "mshtml_cdp_transport.h"
#define CHECK(x) do { if(!(x)) { printf("FAIL line=%d: %s\n", __LINE__, #x); return 1; } } while(0)

typedef struct FakeBrowser { HANDLE input, output, release_slow; BOOL failed; } FakeBrowser;
static BOOL fake_read(FakeBrowser *browser, char *request, size_t capacity)
{
    DWORD count;
    size_t used = 0;
    while(used + 1 < capacity) {
        if(!ReadFile(browser->input, request + used, 1, &count, NULL) || !count) return FALSE;
        if(!request[used++]) return TRUE;
    }
    browser->failed = TRUE;
    return FALSE;
}
static BOOL fake_write(FakeBrowser *browser, const char *message)
{
    DWORD written;
    size_t bytes = strlen(message) + 1;
    if(!WriteFile(browser->output, message, (DWORD)bytes, &written, NULL) || written != bytes) {
        browser->failed = TRUE; return FALSE;
    }
    return TRUE;
}
static DWORD WINAPI fake_browser(void *parameter)
{
    FakeBrowser *browser = parameter;
    char request[4096], reply[4096];
    unsigned slow, fast, id;
    if(!fake_read(browser, request, sizeof(request)) || !cdp_json_id(request, &slow) ||
       !fake_read(browser, request, sizeof(request)) || !cdp_json_id(request, &fast)) return 1;
    fake_write(browser, "{\"method\":\"Page.frameNavigated\",\"sessionId\":\"AA\",\"params\":{\"marker\":\"event-A\"}}");
    fake_write(browser, "{\"method\":\"Page.frameNavigated\",\"sessionId\":\"BB\",\"params\":{\"marker\":\"event-B\"}}");
    snprintf(reply, sizeof(reply), "{\"id\":%u,\"sessionId\":\"BB\",\"result\":{\"marker\":\"fast\"}}", fast);
    fake_write(browser, reply);
    WaitForSingleObject(browser->release_slow, 5000);
    snprintf(reply, sizeof(reply), "{\"id\":%u,\"sessionId\":\"AA\",\"result\":{\"marker\":\"slow\"}}", slow);
    fake_write(browser, reply);
    while(fake_read(browser, request, sizeof(request))) {
        if(!cdp_json_id(request, &id)) { browser->failed = TRUE; break; }
        if(strstr(request, "test.disconnect")) break;
        if(strstr(request, "test.noReply")) continue;
        if(strstr(request, "test.error"))
            snprintf(reply, sizeof(reply), "{\"id\":%u,\"error\":{\"code\":-1,\"message\":\"injected\"}}", id);
        else snprintf(reply, sizeof(reply), "{\"id\":%u,\"result\":{\"marker\":\"normal-response\"}}", id);
        if(!fake_write(browser, reply)) break;
    }
    CloseHandle(browser->output); browser->output = NULL;
    return 0;
}

int main(void)
{
    HANDLE input, output, server;
    CdpTransport *transport;
    CdpRequest *slow, *fast;
    CdpMessage *message;
    FakeBrowser browser = {0};
    char reply[4096];
    DWORD before;
    HRESULT hr;
    unsigned attempt;
    CHECK(CreatePipe(&browser.input, &output, NULL, 0));
    CHECK(CreatePipe(&input, &browser.output, NULL, 0));
    transport = cdp_start(input, output);
    CHECK(transport);
    CHECK(cdp_watch(transport, "AA") == S_OK && cdp_watch(transport, "BB") == S_OK);
    browser.release_slow = CreateEventW(NULL, TRUE, FALSE, NULL); CHECK(browser.release_slow);
    server = CreateThread(NULL, 0, fake_browser, &browser, 0, NULL);
    CHECK(server);
    CHECK(cdp_submit(transport, "test.slow", "{}", "AA", TRUE, 42, 2000, 0, &slow) == S_OK);
    CHECK(cdp_submit(transport, "test.fast", "{}", "BB", FALSE, 0, 2000, sizeof(reply), &fast) == S_OK);
    before = GetTickCount();
    CHECK(WaitForSingleObject(fast->done, 1000) == WAIT_OBJECT_0);
    CHECK(GetTickCount() - before < 1000 && fast->result == S_OK && strstr(fast->reply, "fast"));
    CHECK(WaitForSingleObject(slow->done, 0) == WAIT_TIMEOUT);
    SetEvent(browser.release_slow);
    message = cdp_take(transport, "BB");
    CHECK(message && !message->id && strstr(message->json, "event-B"));
    cdp_message_free(message);
    CHECK(!cdp_take(transport, "BB"));
    message = cdp_take(transport, "AA");
    CHECK(message && !message->id && strstr(message->json, "event-A"));
    cdp_message_free(message);
    CHECK(WaitForSingleObject(slow->done, 1000) == WAIT_OBJECT_0);
    message = cdp_take(transport, "AA");
    CHECK(message && message->id == slow->id && message->tag == 42 && message->status == S_OK && strstr(message->json, "slow"));
    cdp_message_free(message);
    cdp_request_release(slow); cdp_request_release(fast);
    cdp_unwatch(transport, "AA"); cdp_unwatch(transport, "BB");
    puts("CDP OUT-OF-ORDER REPLIES AND SESSION EVENT ISOLATION PASSED");
    before = GetTickCount();
    hr = cdp_call(transport, "test.noReply", "{}", NULL, reply, sizeof(reply), 80);
    CHECK(hr == HRESULT_FROM_WIN32(ERROR_TIMEOUT) && GetTickCount() - before < 500);
    CHECK(cdp_call(transport, "test.ping", "{}", NULL, reply, sizeof(reply), 1000) == S_OK);
    CHECK(strstr(reply, "normal-response"));
    CHECK(cdp_call(transport, "test.ping", "{}", NULL, reply, 8, 1000) == HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW));
    CHECK(cdp_call(transport, "test.error", "{}", NULL, reply, sizeof(reply), 1000) == E_FAIL);
    CHECK(strstr(reply, "injected"));
    CHECK(cdp_submit(transport, "test.noReply", "{}", NULL, TRUE, 93, 80, 0, &slow) == S_OK);
    CHECK(WaitForSingleObject(slow->done, 500) == WAIT_OBJECT_0);
    message = cdp_take(transport, "");
    CHECK(message && message->tag == 93 && message->status == HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    cdp_message_free(message); cdp_request_release(slow);
    puts("CDP TIMEOUT, ASYNC FAILURE, RESPONSE OVERFLOW AND ERROR PROPAGATION PASSED");
    CHECK(cdp_call(transport, "test.disconnect", "{}", NULL, reply, sizeof(reply), 1000) == HRESULT_FROM_WIN32(ERROR_BROKEN_PIPE));
    cdp_stop(transport);
    CHECK(WaitForSingleObject(server, 1000) == WAIT_OBJECT_0 && !browser.failed);
    CloseHandle(server); CloseHandle(browser.input); CloseHandle(browser.release_slow);
    for(attempt = 0; attempt < 20 && cdp_live_workers; ++attempt) Sleep(10);
    CHECK(cdp_live_workers == 0);

    /* A browser that never reads must not leave a write stuck during shutdown. */
    CHECK(CreatePipe(&browser.input, &output, NULL, 0));
    CHECK(CreatePipe(&input, &browser.output, NULL, 0));
    transport = cdp_start(input, output); CHECK(transport);
    {
        char *large = HeapAlloc(GetProcessHeap(), 0, 1024 * 1024);
        CHECK(large);
        memset(large, 'x', 1024 * 1024 - 1); large[0] = '"';
        large[1024 * 1024 - 2] = '"'; large[1024 * 1024 - 1] = 0;
        CHECK(cdp_submit(transport, "test.blockedWrite", large, NULL, FALSE, 0, 10000, 128, &slow) == S_OK);
        HeapFree(GetProcessHeap(), 0, large);
    }
    Sleep(100);
    before = GetTickCount(); cdp_stop(transport);
    CHECK(GetTickCount() - before < 2500 && cdp_live_workers == 0);
    CHECK(WaitForSingleObject(slow->done, 0) == WAIT_OBJECT_0 && FAILED(slow->result));
    cdp_request_release(slow);
    CloseHandle(browser.input); CloseHandle(browser.output);
    puts("CDP BLOCKED-WRITER CANCELLATION AND OWNERSHIP PASSED");
    puts("MSHTML CDP TRANSPORT TEST PASSED");
    return 0;
}
