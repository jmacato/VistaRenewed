/* Private CDP pipe transport. One worker owns all reads and writes. No COM or
 * HWND pointers cross into it. Requests and unsolicited messages have separate
 * ownership, so an outstanding navigation cannot swallow another reply/event.
 * Requires windows.h, stdio.h, string.h and mshtml_json.h. */
#ifndef TRITON_MSHTML_CDP_TRANSPORT_H
#define TRITON_MSHTML_CDP_TRANSPORT_H
#include <limits.h>
#define CDP_MESSAGE_LIMIT (16u * 1024u * 1024u)
#define CDP_QUEUE_LIMIT (16u * 1024u * 1024u)

typedef struct CdpMessage {
    struct CdpMessage *next;
    char session[80];
    unsigned id, tag;
    HRESULT status;
    size_t bytes;
    char json[];
} CdpMessage;

typedef struct CdpRequest {
    struct CdpRequest *next;
    LONG refs;
    HANDLE done;
    unsigned id, tag;
    DWORD started, timeout;
    BOOL sent, asynchronous;
    char session[80];
    char *wire, *reply;
    size_t wire_bytes, capacity;
    HRESULT result;
} CdpRequest;

typedef struct CdpTransport {
    LONG refs;
    CRITICAL_SECTION lock;
    HANDLE input, output, wake, stop, thread;
    HMODULE module_pin;
    CdpRequest *requests, **requests_tail;
    CdpMessage *messages, **messages_tail;
    size_t queued_bytes, message_bytes;
    unsigned next_id, request_count;
    HRESULT status;
    struct CdpSession *sessions;
} CdpTransport;

typedef struct CdpSession {
    struct CdpSession *next;
    char id[80];
} CdpSession;

static LONG cdp_live_workers;

static void cdp_message_free(CdpMessage *message)
{ HeapFree(GetProcessHeap(), 0, message); }

static void cdp_request_release(CdpRequest *request)
{
    if(!InterlockedDecrement(&request->refs)) {
        CloseHandle(request->done);
        HeapFree(GetProcessHeap(), 0, request->wire);
        HeapFree(GetProcessHeap(), 0, request->reply);
        HeapFree(GetProcessHeap(), 0, request);
    }
}

static void cdp_transport_release(CdpTransport *transport)
{
    if(!InterlockedDecrement(&transport->refs)) {
        CdpMessage *message;
        CdpSession *session;
        while((message = transport->messages)) {
            transport->messages = message->next;
            cdp_message_free(message);
        }
        while((session = transport->sessions)) {
            transport->sessions = session->next;
            HeapFree(GetProcessHeap(), 0, session);
        }
        if(transport->thread) CloseHandle(transport->thread);
        if(transport->input) CloseHandle(transport->input);
        if(transport->output) CloseHandle(transport->output);
        CloseHandle(transport->stop);
        CloseHandle(transport->wake);
        DeleteCriticalSection(&transport->lock);
        HeapFree(GetProcessHeap(), 0, transport);
    }
}

static BOOL cdp_json_id(const char *json, unsigned *id)
{
    const char *p = mj_member(json, "id");
    unsigned value = 0;
    if(!p || *p < '1' || *p > '9') return FALSE;
    do {
        if(value > (UINT_MAX - (unsigned)(*p - '0')) / 10) return FALSE;
        value = value * 10 + (unsigned)(*p++ - '0');
    } while(*p >= '0' && *p <= '9');
    if(*mj_space(p) != ',' && *mj_space(p) != '}') return FALSE;
    *id = value;
    return TRUE;
}

static BOOL cdp_session(const char *json, char *session)
{
    const char *p = mj_member(json, "sessionId");
    size_t length = 0;
    session[0] = 0;
    if(!p) return TRUE;
    if(*p++ != '"') return FALSE;
    while(*p && *p != '"') {
        if(mj_hex(*p) < 0 || length == 79) return FALSE;
        session[length++] = *p++;
    }
    session[length] = 0;
    return *p == '"' && length;
}

/* Caller holds lock. Overflow is an explicit transport failure, not silently
 * discarded navigation state. Runtime/DOM payloads never enter diagnostic logs. */
static HRESULT cdp_queue_message(CdpTransport *transport, const char *json,
                                 const char *session, unsigned id, unsigned tag, HRESULT status)
{
    size_t bytes = strlen(json) + 1;
    CdpMessage *message;
    if(bytes > CDP_QUEUE_LIMIT - transport->message_bytes)
        return HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW);
    message = HeapAlloc(GetProcessHeap(), 0, sizeof(*message) + bytes);
    if(!message) return E_OUTOFMEMORY;
    message->next = NULL;
    strcpy(message->session, session);
    message->id = id; message->tag = tag; message->status = status;
    message->bytes = bytes;
    memcpy(message->json, json, bytes);
    *transport->messages_tail = message;
    transport->messages_tail = &message->next;
    transport->message_bytes += bytes;
#ifdef TRITON_CDP_QUEUE_TRACE
    TRITON_CDP_QUEUE_TRACE(id);
#endif
    return S_OK;
}

static void cdp_finish_request(CdpTransport *transport, CdpRequest **link,
                               const char *json, HRESULT result)
{
    CdpRequest *request = *link;
    *link = request->next;
    if(!*link) transport->requests_tail = link;
    --transport->request_count;
    transport->queued_bytes -= request->wire_bytes + request->capacity;
    if(request->asynchronous) {
        HRESULT hr = cdp_queue_message(transport, json ? json : "{}", request->session,
                                      request->id, request->tag, result);
        if(FAILED(hr)) transport->status = hr;
    } else if(json) {
        size_t bytes = strlen(json) + 1;
        if(bytes > request->capacity) result = HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW);
        else memcpy(request->reply, json, bytes);
    }
    request->result = result;
    SetEvent(request->done);
    cdp_request_release(request); /* queue reference */
}

static void cdp_deliver(CdpTransport *transport, const char *json)
{
    const char *end = mj_skip(json, 0), *method;
    char session[80];
    unsigned id;
    CdpRequest **link;
    EnterCriticalSection(&transport->lock);
    if(!end || *mj_space(end) || !cdp_session(json, session)) {
        transport->status = E_FAIL;
    } else if(cdp_json_id(json, &id)) {
#ifdef TRITON_CDP_REPLY_TRACE
        TRITON_CDP_REPLY_TRACE(id);
#endif
        for(link = &transport->requests; *link; link = &(*link)->next)
            if((*link)->id == id) break;
        if(*link) {
            if(strcmp((*link)->session, session)) transport->status = E_FAIL;
            else cdp_finish_request(transport, link, json, mj_member(json, "error") ? E_FAIL : S_OK);
        } /* A timed-out request's late reply has no remaining consumer. */
    } else if(mj_member(json, "id")) {
        transport->status = E_FAIL;
    } else if((method = mj_member(json, "method")) &&
              (!strncmp(method, "\"Page.", 6) ||
               !strncmp(method, "\"Fetch.requestPaused\"", 21) ||
               !strncmp(method, "\"Runtime.bindingCalled\"", 23))) {
#ifdef TRITON_CDP_FETCH_TRACE
        if(!strncmp(method, "\"Fetch.", 7)) TRITON_CDP_FETCH_TRACE();
#endif
        CdpSession *watch;
        for(watch = transport->sessions; watch; watch = watch->next) if(!strcmp(watch->id, session)) break;
        if(watch) {
            transport->status = cdp_queue_message(transport, json, session, 0, 0, S_OK);
#ifdef TRITON_CDP_FETCH_QUEUED_TRACE
            if(!strncmp(method, "\"Fetch.", 7)) TRITON_CDP_FETCH_QUEUED_TRACE();
#endif
        }
#ifdef TRITON_CDP_FETCH_UNWATCHED_TRACE
        else if(!strncmp(method, "\"Fetch.", 7)) TRITON_CDP_FETCH_UNWATCHED_TRACE();
#endif
    }
    LeaveCriticalSection(&transport->lock);
}

static DWORD WINAPI cdp_worker(void *parameter)
{
    CdpTransport *transport = parameter;
    HMODULE pin = transport->module_pin;
    char block[16384], *message = NULL;
    size_t used = 0, capacity = 0;
    HRESULT result = S_OK;
    for(;;) {
        CdpRequest *send = NULL, **link;
        DWORD available, received, written, index;
        EnterCriticalSection(&transport->lock);
        result = transport->status;
        if(WaitForSingleObject(transport->stop, 0) == WAIT_OBJECT_0)
            result = HRESULT_FROM_WIN32(ERROR_OPERATION_ABORTED);
        if(SUCCEEDED(result)) {
            for(link = &transport->requests; *link;) {
                if(GetTickCount() - (*link)->started >= (*link)->timeout)
                    cdp_finish_request(transport, link, NULL, HRESULT_FROM_WIN32(ERROR_TIMEOUT));
                else {
                    if(!send && !(*link)->sent) send = *link;
                    link = &(*link)->next;
                }
            }
            if(send) { send->sent = TRUE; InterlockedIncrement(&send->refs); }
            result = transport->status;
        }
        LeaveCriticalSection(&transport->lock);
        if(FAILED(result)) { if(send) cdp_request_release(send); break; }
        if(send) {
#ifdef TRITON_CDP_SEND_TRACE
            TRITON_CDP_SEND_TRACE(send->id);
#endif
            BOOL ok = WriteFile(transport->output, send->wire, (DWORD)send->wire_bytes, &written, NULL);
            if(!ok || written != send->wire_bytes) result = HRESULT_FROM_WIN32(ERROR_BROKEN_PIPE);
            cdp_request_release(send);
            if(FAILED(result)) break;
        }
        if(!PeekNamedPipe(transport->input, NULL, 0, NULL, &available, NULL)) {
            result = HRESULT_FROM_WIN32(ERROR_BROKEN_PIPE); break;
        }
        if(available) {
            if(!ReadFile(transport->input, block, min(available, sizeof(block)), &received, NULL) || !received) {
                result = HRESULT_FROM_WIN32(ERROR_BROKEN_PIPE); break;
            }
            for(index = 0; index < received; ++index) {
                if(used + 1 >= capacity) {
                    size_t next = capacity ? capacity * 2 : 16384;
                    char *larger;
                    if(next > CDP_MESSAGE_LIMIT) { result = HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW); break; }
                    larger = message ? HeapReAlloc(GetProcessHeap(), 0, message, next) :
                                       HeapAlloc(GetProcessHeap(), 0, next);
                    if(!larger) { result = E_OUTOFMEMORY; break; }
                    message = larger; capacity = next;
                }
                message[used++] = block[index];
                if(!block[index]) { cdp_deliver(transport, message); used = 0; }
            }
            if(FAILED(result)) break;
        } else if(!send) {
            WaitForSingleObject(transport->wake, 10);
        }
    }
    HeapFree(GetProcessHeap(), 0, message);
    EnterCriticalSection(&transport->lock);
    transport->status = result;
    while(transport->requests) cdp_finish_request(transport, &transport->requests, NULL, result);
    LeaveCriticalSection(&transport->lock);
    cdp_transport_release(transport); /* worker reference */
    InterlockedDecrement(&cdp_live_workers);
    if(pin) FreeLibraryAndExitThread(pin, 0);
    return 0;
}

/* On success owns both handles; on failure the caller still owns them. */
static CdpTransport *cdp_start(HANDLE input, HANDLE output)
{
    CdpTransport *transport = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*transport));
    HMODULE pin = NULL;
    if(!transport) return NULL;
    if(!InitializeCriticalSectionAndSpinCount(&transport->lock, 0)) {
        HeapFree(GetProcessHeap(), 0, transport); return NULL;
    }
    transport->refs = 1;
    transport->requests_tail = &transport->requests;
    transport->messages_tail = &transport->messages;
    transport->stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    transport->wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    if(!transport->stop || !transport->wake ||
       !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            (LPCWSTR)(ULONG_PTR)cdp_worker, &pin)) goto failed;
    transport->module_pin = pin == GetModuleHandleW(NULL) ? NULL : pin;
    transport->input = input; transport->output = output;
    InterlockedIncrement(&transport->refs);
    InterlockedIncrement(&cdp_live_workers);
    transport->thread = CreateThread(NULL, 0, cdp_worker, transport, CREATE_SUSPENDED, NULL);
    if(!transport->thread) {
        InterlockedDecrement(&transport->refs);
        InterlockedDecrement(&cdp_live_workers);
        transport->input = transport->output = NULL;
        goto failed;
    }
    ResumeThread(transport->thread);
    return transport;
failed:
    if(transport->module_pin) FreeLibrary(transport->module_pin);
    cdp_transport_release(transport);
    return NULL;
}

static void cdp_stop(CdpTransport *transport)
{
    unsigned attempt;
    if(!transport) return;
    SetEvent(transport->stop);
    SetEvent(transport->wake);
    /* Vista API: interrupt a writer blocked because the browser stopped reading.
     * A timed-out worker retains its own memory and module pin, never a doc. */
    for(attempt = 0; attempt < 20; ++attempt) {
        CancelSynchronousIo(transport->thread);
        if(WaitForSingleObject(transport->thread, 100) == WAIT_OBJECT_0) break;
    }
    cdp_transport_release(transport);
}

static HRESULT cdp_submit(CdpTransport *transport, const char *method, const char *params,
                           const char *session, BOOL asynchronous, unsigned tag,
                           DWORD timeout, size_t capacity, CdpRequest **out)
{
    CdpRequest *request;
    int length;
    HRESULT hr;
    *out = NULL;
    if(!transport) return CO_E_OBJNOTCONNECTED;
    if(!method || !params || strlen(method) > 100 || strlen(params) > 8u * 1024u * 1024u ||
       (session && strlen(session) > 79) || !timeout || timeout > 600000 ||
       (!asynchronous && (!capacity || capacity > CDP_MESSAGE_LIMIT))) return E_INVALIDARG;
    request = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*request));
    if(!request) return E_OUTOFMEMORY;
    request->refs = 1;
    request->done = CreateEventW(NULL, TRUE, FALSE, NULL);
    request->capacity = capacity;
    request->asynchronous = asynchronous;
    request->tag = tag; request->timeout = timeout; request->started = GetTickCount();
    if(session) strcpy(request->session, session);
    if(!asynchronous) request->reply = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, capacity);
    if(!request->done || (!asynchronous && !request->reply)) { hr = E_OUTOFMEMORY; goto failed; }
    EnterCriticalSection(&transport->lock);
    hr = transport->status;
    if(FAILED(hr)) goto unlock;
    if(transport->request_count >= 128 || transport->next_id == UINT_MAX) {
        hr = HRESULT_FROM_WIN32(ERROR_BUSY); goto unlock;
    }
    request->id = ++transport->next_id;
    length = snprintf(NULL, 0, "{\"id\":%u,\"method\":\"%s\",\"params\":%s%s%s%s}",
        request->id, method, params, session ? ",\"sessionId\":\"" : "", session ? session : "", session ? "\"" : "");
    if(length < 0 || (size_t)length + 1 + capacity > CDP_QUEUE_LIMIT - transport->queued_bytes) {
        hr = HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW); goto unlock;
    }
    request->wire_bytes = (size_t)length + 1;
    request->wire = HeapAlloc(GetProcessHeap(), 0, request->wire_bytes);
    if(!request->wire) { hr = E_OUTOFMEMORY; goto unlock; }
    snprintf(request->wire, request->wire_bytes, "{\"id\":%u,\"method\":\"%s\",\"params\":%s%s%s%s}",
        request->id, method, params, session ? ",\"sessionId\":\"" : "", session ? session : "", session ? "\"" : "");
    InterlockedIncrement(&request->refs);
    *transport->requests_tail = request;
    transport->requests_tail = &request->next;
    transport->queued_bytes += request->wire_bytes + request->capacity;
    ++transport->request_count;
    *out = request;
    SetEvent(transport->wake);
unlock:
    LeaveCriticalSection(&transport->lock);
    if(SUCCEEDED(hr)) return hr;
failed:
    cdp_request_release(request);
    return hr;
}

static HRESULT cdp_call(CdpTransport *transport, const char *method, const char *params,
                        const char *session, char *reply, size_t capacity, DWORD timeout)
{
    CdpRequest *request;
    HRESULT hr;
    if(!reply || !capacity) return E_INVALIDARG;
    reply[0] = 0;
    hr = cdp_submit(transport, method, params, session, FALSE, 0, timeout, capacity, &request);
    if(FAILED(hr)) return hr;
    if(WaitForSingleObject(request->done, timeout + 100) != WAIT_OBJECT_0)
        hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    else {
        hr = request->result;
        if(request->reply) memcpy(reply, request->reply, capacity);
    }
    cdp_request_release(request);
    return hr;
}

static CdpMessage *cdp_take(CdpTransport *transport, const char *session)
{
    CdpMessage **link, *message = NULL;
    if(!transport) return NULL;
    EnterCriticalSection(&transport->lock);
    for(link = &transport->messages; *link; link = &(*link)->next) {
        if(!strcmp((*link)->session, session)) {
            message = *link;
            *link = message->next;
            if(!*link) transport->messages_tail = link;
            transport->message_bytes -= message->bytes;
#ifdef TRITON_CDP_TAKE_TRACE
            TRITON_CDP_TAKE_TRACE(message->id);
#endif
            break;
        }
    }
    LeaveCriticalSection(&transport->lock);
    return message;
}

static HRESULT cdp_watch(CdpTransport *transport, const char *id)
{
    CdpSession *session;
    if(!transport || !id || !*id || strlen(id) > 79) return E_INVALIDARG;
    EnterCriticalSection(&transport->lock);
    for(session = transport->sessions; session; session = session->next)
        if(!strcmp(session->id, id)) break;
    if(!session) {
        session = HeapAlloc(GetProcessHeap(), 0, sizeof(*session));
        if(session) {
            strcpy(session->id, id);
            session->next = transport->sessions;
            transport->sessions = session;
        }
    }
    LeaveCriticalSection(&transport->lock);
    return session ? S_OK : E_OUTOFMEMORY;
}

static void cdp_unwatch(CdpTransport *transport, const char *id)
{
    CdpSession **link, *session;
    CdpRequest **pending;
    CdpMessage *message;
    if(!transport || !id || !*id) return;
    EnterCriticalSection(&transport->lock);
    for(link = &transport->sessions; *link; link = &(*link)->next) {
        if(!strcmp((*link)->id, id)) {
            session = *link; *link = session->next;
            HeapFree(GetProcessHeap(), 0, session);
            break;
        }
    }
    for(pending = &transport->requests; *pending;) {
        if(!strcmp((*pending)->session, id))
            cdp_finish_request(transport, pending, NULL, HRESULT_FROM_WIN32(ERROR_OPERATION_ABORTED));
        else pending = &(*pending)->next;
    }
    while((message = cdp_take(transport, id))) cdp_message_free(message);
    LeaveCriticalSection(&transport->lock);
}

#endif
