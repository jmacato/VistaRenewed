/* Apartment-owned navigation state. CDP worker messages contain no COM/HWND
 * pointers. Command acknowledgements and renderer commits are distinct, and
 * only the current request cookie can complete a host-requested navigation. */
static BOOL nav_is(const char *value, const char *text)
{
    size_t length = strlen(text);
    return value && *value == '"' && !strncmp(value + 1, text, length) && value[length + 1] == '"';
}
static BOOL nav_id(const char *object, const char *key, char *out)
{
    const char *p = mj_member(object, key);
    unsigned used = 0;
    if(!p || *p++ != '"') return FALSE;
    while(*p && *p != '"') {
        if(mj_hex(*p) < 0 || used == 79) return FALSE;
        out[used++] = *p++;
    }
    out[used] = 0;
    return used && *p == '"';
}
static void document_renderer_navigation(ProbeDocument *document)
{
    ++document->navigation_serial;
    document->navigation_kind = 3;
    document->navigation_committed = FALSE;
    document->navigation_virtual_url = FALSE;
    document->host_navigate_notified = FALSE;
    document->host_load_phase = 0;
    document->navigation_loader[0] = 0;
    document->observed_same_document = FALSE;
    document->navigation_expects_same_document = FALSE;
    document->history_waiting_commit = FALSE;
    SysFreeString(document->renderer_title); document->renderer_title = NULL;
    document_set_ready_state(document, READYSTATE_LOADING);
    append_text(L"MSHTML_RENDERER_NAVIGATION_STARTED\r\n");
}

static void document_commit_observed(ProbeDocument *document)
{
    LONG state = READYSTATE_LOADING;
    if(!document->navigation_kind || document->navigation_wait_ack || !document->observed_url) return;
    if(document->history_waiting_commit) return;
    if(document->navigation_expects_same_document && !document->observed_same_document) return;
    if(!document->navigation_committed) {
        if(document->navigation_loader[0] && strcmp(document->navigation_loader, document->observed_loader)) return;
        if((document->navigation_kind == 2 || document->navigation_kind == 3) &&
           !document->observed_same_document && !strcmp(document->committed_loader, document->observed_loader)) return;
        if(!document->navigation_virtual_url && (!document->current_url || lstrcmpW(document->current_url, document->observed_url))) {
            if(FAILED(remember_url(document, document->observed_url))) return;
        }
        if(strcmp(document->committed_loader, document->observed_loader)) ++document->page_generation;
        strcpy(document->committed_loader, document->observed_loader);
        document->navigation_loader[0] = 0;
        document->navigation_committed = TRUE;
        /* Redirect Fetch pauses are over once Chromium reports the committed
         * top frame. Do not let caller headers bleed into a later page click. */
        document_request_clear(document);
        /* Queue outside this retained-message callback. This avoids a pipe
         * worker wakeup race observed when a new request was appended while
         * the acknowledgement that triggered it was still being drained. */
        document->history_snapshot_pending = TRUE;
        append_text(L"MSHTML_NAVIGATION_LOADER_COMMITTED\r\n");
        append_text(L"MSHTML_COMMITTED_URL=");
        append_text(document->current_url ? document->current_url : L"");
        append_text(L"\r\n");
    }
    if(document->observed_same_document) state = READYSTATE_COMPLETE;
    else if(!strcmp(document->lifecycle_loader, document->committed_loader)) state = document->lifecycle_state;
    if(state >= document->ready_state) document_set_ready_state(document, state);
    if(state == READYSTATE_COMPLETE && document->history_restart_state)
        history_apply_restart_state(document);
}

static void document_process_events(ProbeDocument *document)
{
    CdpMessage *message;
    if(document->processing_events || !document->page_session[0]) return;
    document_add_ref(document);
    document->processing_events = TRUE;
    while((message = cdp_take(document->runtime->cdp, document->page_session))) {
        const char *params = mj_member(message->json, "params");
        const char *method = mj_member(message->json, "method");
        char frame[80], loader[80];
        if(message->id) {
            if(message->id == document->popup_scan_request) {
                document_receive_popups(document, message);
            } else if(message->id == document->navigation_probe_request) {
                const char *tree = mj_member(mj_member(message->json, "result"), "frameTree");
                const char *entry = mj_member(tree, "frame");
                BSTR url = NULL;
                document->navigation_probe_request = 0;
                if(SUCCEEDED(message->status) && message->tag == document->navigation_serial &&
                   nav_id(entry, "id", frame) && !strcmp(frame, document->main_frame) &&
                   nav_id(entry, "loaderId", loader) && document->navigation_loader[0] &&
                   !strcmp(loader, document->navigation_loader) &&
                   SUCCEEDED(mj_bstr(mj_member(entry, "url"), &url))) {
                    strcpy(document->observed_loader, loader);
                    SysFreeString(document->observed_url); document->observed_url = url; url = NULL;
                    document->observed_same_document = FALSE;
                    document->history_waiting_commit = FALSE;
                    document_commit_observed(document);
                    append_text(L"MSHTML_NAVIGATION_FRAME_PROBE_RECOVERED\r\n");
                }
                SysFreeString(url);
                document->next_navigation_probe = GetTickCount() + 250;
            } else if(message->id == document->ready_probe_request) {
                const char *value = mj_member(mj_member(mj_member(message->json, "result"), "result"), "value");
                LONG state = 0;
                document->ready_probe_request = 0;
                if(SUCCEEDED(message->status) && message->tag == document->navigation_serial &&
                   document->navigation_committed) {
                    if(nav_is(value, "loading")) state = READYSTATE_LOADING;
                    else if(nav_is(value, "interactive")) state = READYSTATE_INTERACTIVE;
                    else if(nav_is(value, "complete")) state = READYSTATE_COMPLETE;
                    if(state) {
                        strcpy(document->lifecycle_loader, document->committed_loader);
                        document->lifecycle_state = state;
                        document_commit_observed(document);
                        if(state == READYSTATE_COMPLETE)
                            append_text(L"MSHTML_NAVIGATION_READY_PROBE_RECOVERED\r\n");
                    }
                }
                document->next_navigation_probe = GetTickCount() + 250;
            } else if(message->id == document->history_request) {
                history_receive_snapshot(document, message);
            } else if(message->id == document->title_request) {
                BSTR title = NULL;
                document->title_request = 0;
                if(SUCCEEDED(message->status) && message->tag == document->navigation_serial &&
                   SUCCEEDED(mj_bstr(mj_member(mj_member(mj_member(message->json, "result"), "result"), "value"), &title))) {
                    SysFreeString(document->renderer_title);
                    document->renderer_title = title;
                }
            } else if(message->id == document->navigation_request && message->tag == document->navigation_cookie) {
                const char *result = mj_member(message->json, "result");
                document->navigation_request = 0;
                document->navigation_wait_ack = FALSE;
                document->next_navigation_probe = GetTickCount() + 100;
                append_dword(L"MSHTML_ASYNC_NAVIGATION_RESULT=", message->status);
                if(FAILED(message->status)) {
                    /* Surface transport/protocol failure without claiming a new
                     * page committed. Native error-page policy is separate. */
                    document->navigation_security_error = TRUE;
                    document->host_navigate_notified = TRUE;
                    document->navigation_kind = 0;
                    document_set_ready_state(document, READYSTATE_COMPLETE);
                } else if(document->navigation_kind == 4) {
                    if(document->observed_url && !document->navigation_virtual_url)
                        remember_url(document, document->observed_url);
                    document->host_navigate_notified = TRUE;
                    document->navigation_kind = 0;
                    document_set_ready_state(document, READYSTATE_COMPLETE);
                    append_text(L"MSHTML_STOP_ACKNOWLEDGED\r\n");
                } else {
                    if(document->navigation_kind == 1) {
                        const char *download = mj_member(result, "isDownload");
                        if(download && !strncmp(download, "true", 4)) {
                            document->navigation_download = TRUE;
                            document->host_navigate_notified = TRUE;
                            document->navigation_kind = 0;
                            document_set_ready_state(document, READYSTATE_COMPLETE);
                            append_text(L"MSHTML_DOWNLOAD_NAVIGATION_SURFACED\r\n");
                        } else if(!nav_id(result, "loaderId", document->navigation_loader)) {
                            if(mj_member(result, "errorText")) {
                                document->navigation_security_error = TRUE;
                                document->host_navigate_notified = TRUE;
                                document->navigation_kind = 0;
                                document_set_ready_state(document, READYSTATE_COMPLETE);
                            } else {
                                /* CDP explicitly omits loaderId for same-document
                                 * navigation; it cannot be a new DOM generation. */
                                document->navigation_expects_same_document = TRUE;
                            }
                        }
                        append_text(L"MSHTML_PERSISTENT_NAVIGATION_ACKNOWLEDGED\r\n");
                    } else if(document->navigation_kind == 5) append_text(L"MSHTML_HISTORY_ACKNOWLEDGED\r\n");
                    else append_text(L"MSHTML_REFRESH_ACKNOWLEDGED\r\n");
                    document_commit_observed(document);
                }
            }
        } else if(nav_is(method, "Fetch.requestPaused")) {
            append_text(L"MSHTML_FETCH_REQUEST_PAUSED\r\n");
            document_continue_paused_request(document, params);
        } else if(nav_is(method, "Runtime.bindingCalled") &&
                  nav_is(mj_member(params, "name"), "__tritonHistoryTravel")) {
            history_renderer_travel_request(document, params);
        } else if(nav_is(method, "Runtime.bindingCalled") &&
                  nav_is(mj_member(params, "name"), "__tritonOpenWindow")) {
            document_open_new_window_event(document, params);
        } else if(nav_is(method, "Page.frameNavigated")) {
            const char *entry = mj_member(params, "frame");
            BSTR url = NULL;
            WCHAR drop[2];
            BOOL drop_for_test = GetEnvironmentVariableW(
                L"TRITON_MSHTML_DROP_NEXT_MAIN_FRAME_EVENT", drop, ARRAYSIZE(drop)) != 0;
            if(drop_for_test) {
                SetEnvironmentVariableW(L"TRITON_MSHTML_DROP_NEXT_MAIN_FRAME_EVENT", NULL);
                append_text(L"MSHTML_TEST_MAIN_FRAME_EVENT_DROPPED\r\n");
            }
            if(!drop_for_test && !mj_member(entry, "parentId") && nav_id(entry, "id", frame) &&
               nav_id(entry, "loaderId", loader) && SUCCEEDED(mj_bstr(mj_member(entry, "url"), &url))) {
                /* CDP's Frame separates the fragment from URL. The private
                 * window contract must return the full address, not just URL. */
                BSTR fragment = NULL;
                if(SUCCEEDED(mj_bstr(mj_member(entry, "urlFragment"), &fragment)) && fragment) {
                    UINT base = SysStringLen(url), extra = SysStringLen(fragment);
                    BSTR joined = SysAllocStringLen(NULL, base + extra);
                    if(joined) {
                        memcpy(joined, url, base * sizeof(WCHAR));
                        memcpy(joined + base, fragment, (extra + 1) * sizeof(WCHAR));
                        SysFreeString(url); url = joined;
                    }
                    SysFreeString(fragment);
                }
                if(!document->navigation_wait_ack && (!document->navigation_kind ||
                   (document->navigation_committed && strcmp(loader, document->committed_loader))))
                    document_renderer_navigation(document);
                strcpy(document->main_frame, frame);
                document->history_waiting_commit = FALSE;
                strcpy(document->observed_loader, loader);
                SysFreeString(document->observed_url); document->observed_url = url;
                document->observed_same_document = FALSE;
                if(nav_is(mj_member(params, "type"), "BackForwardCacheRestore")) {
                    strcpy(document->lifecycle_loader, loader);
                    document->lifecycle_state = READYSTATE_COMPLETE;
                }
                document_commit_observed(document);
            }
        } else if(nav_is(method, "Page.lifecycleEvent") && nav_id(params, "frameId", frame) &&
                  !strcmp(frame, document->main_frame) && nav_id(params, "loaderId", loader)) {
            LONG state = 0;
            if(nav_is(mj_member(params, "name"), "init")) state = READYSTATE_LOADING;
            else if(nav_is(mj_member(params, "name"), "DOMContentLoaded")) state = READYSTATE_INTERACTIVE;
            else if(nav_is(mj_member(params, "name"), "load")) state = READYSTATE_COMPLETE;
            if(state) {
                if(strcmp(document->lifecycle_loader, loader)) {
                    strcpy(document->lifecycle_loader, loader);
                    document->lifecycle_state = state;
                } else if(state > document->lifecycle_state) document->lifecycle_state = state;
                document_commit_observed(document);
            }
        } else if(nav_is(method, "Page.frameStartedLoading") && nav_id(params, "frameId", frame) &&
                  !strcmp(frame, document->main_frame) && !document->navigation_wait_ack &&
                  document->navigation_committed && document->ready_state == READYSTATE_COMPLETE) {
            document_renderer_navigation(document);
        } else if(nav_is(method, "Page.navigatedWithinDocument") && nav_id(params, "frameId", frame) &&
                  !strcmp(frame, document->main_frame)) {
            BSTR url = NULL;
            if(SUCCEEDED(mj_bstr(mj_member(params, "url"), &url))) {
                if(!document->navigation_wait_ack &&
                   (!document->navigation_kind || document->navigation_committed))
                    document_renderer_navigation(document);
                SysFreeString(document->observed_url); document->observed_url = url;
                document->observed_same_document = TRUE;
                document->history_waiting_commit = FALSE;
                document_commit_observed(document);
            }
        }
        cdp_message_free(message);
    }
    document->processing_events = FALSE;
    document_release(document);
}

static HRESULT document_post_navigation(ProbeDocument *document, const char *method,
    const char *params, unsigned kind, LPCWSTR url, BOOL virtual_url)
{
    CdpRequest *request;
    BSTR copy = url ? SysAllocString(url) : NULL;
    HRESULT hr;
    if(url && !copy) return E_OUTOFMEMORY;
    document_add_ref(document);
    document_process_events(document);
    if(kind == 1 && document->navigation_committed) {
        hr = history_capture_departure(document);
        if(FAILED(hr)) {
            SysFreeString(copy);
            document_release(document);
            return hr;
        }
    }
    hr = cdp_submit(document->runtime->cdp, method, params, document->page_session,
                    TRUE, document->navigation_cookie + 1, 120000, 0, &request);
    if(SUCCEEDED(hr)) {
        ++document->navigation_serial;
        ++document->navigation_cookie;
        document->navigation_request = request->id;
        document->navigation_kind = kind;
        document->navigation_security_error = FALSE;
        document->navigation_download = FALSE;
        document->history_request = 0; /* Superseded snapshot replies are ignored. */
        document->navigation_probe_request = document->ready_probe_request = 0;
        document->next_navigation_probe = GetTickCount() + 100;
        document->history_waiting_commit = kind == 5;
        document->navigation_wait_ack = TRUE;
        if(kind != 4) document->navigation_committed = FALSE;
        document->navigation_virtual_url = virtual_url;
        document->navigation_loader[0] = 0;
        document->observed_same_document = FALSE;
        document->navigation_expects_same_document = FALSE;
        if(kind != 4) {
            document->host_navigate_notified = FALSE;
            document->host_load_phase = 0;
            SysFreeString(document->renderer_title); document->renderer_title = NULL;
        }
        document->window_closed = FALSE;
        if(copy) {
            SysFreeString(document->current_url); document->current_url = copy; copy = NULL;
            SysFreeString(document->notified_title); document->notified_title = NULL;
            if(document->current_moniker) { IMoniker_Release(document->current_moniker); document->current_moniker = NULL; }
        }
        cdp_request_release(request);
        if(kind != 4) document_set_ready_state(document, READYSTATE_LOADING);
        append_text(L"MSHTML_NAVIGATION_COMMAND_QUEUED\r\n");
    }
    SysFreeString(copy);
    document_release(document);
    return hr;
}

static void document_poll_title(ProbeDocument *document)
{
    CdpRequest *request;
    if(!document->navigation_committed || document->title_request ||
       (document->next_title_poll &&
       (LONG)(GetTickCount() - document->next_title_poll) < 0)) return;
    document->next_title_poll = GetTickCount() + 1000;
    if(SUCCEEDED(cdp_submit(document->runtime->cdp, "Runtime.evaluate",
        "{\"expression\":\"document.title\",\"returnByValue\":true,\"timeout\":1000}",
        document->page_session, TRUE, document->navigation_serial, 2000, 0, &request))) {
        document->title_request = request->id;
        cdp_request_release(request);
    }
}

/* Page events are the primary commit/lifecycle source. Chromium has
 * occasionally acknowledged Page.navigate and completed the HTTP request
 * without delivering the corresponding retained frame event. Reconcile that
 * loss asynchronously: never block the host apartment and never accept a
 * frame whose checked loader differs from the current Page.navigate reply. */
static void document_poll_navigation_state(ProbeDocument *document)
{
    CdpRequest *request;
    HRESULT hr;
    if(document->history_snapshot_pending && !document->history_request) {
        history_request_snapshot(document);
        return;
    }
    if(!document->page_session[0] || document->navigation_wait_ack ||
       document->navigation_kind != 1 ||
       (document->next_navigation_probe &&
        (LONG)(GetTickCount() - document->next_navigation_probe) < 0)) return;
    if(!document->navigation_committed) {
        if(document->navigation_probe_request || !document->navigation_loader[0]) return;
        hr = cdp_submit(document->runtime->cdp, "Page.getFrameTree", "{}",
            document->page_session, TRUE, document->navigation_serial, 2000, 0, &request);
        if(SUCCEEDED(hr)) {
            document->navigation_probe_request = request->id;
            cdp_request_release(request);
        } else {
            document->next_navigation_probe = GetTickCount() + 250;
            append_dword(L"MSHTML_NAVIGATION_FRAME_PROBE_QUEUE_FAILURE=", hr);
        }
    } else if(document->ready_state != READYSTATE_COMPLETE) {
        if(document->ready_probe_request) return;
        hr = cdp_submit(document->runtime->cdp, "Runtime.evaluate",
            "{\"expression\":\"document.readyState\",\"returnByValue\":true,\"timeout\":1000}",
            document->page_session, TRUE, document->navigation_serial, 2000, 0, &request);
        if(SUCCEEDED(hr)) {
            document->ready_probe_request = request->id;
            cdp_request_release(request);
        } else {
            document->next_navigation_probe = GetTickCount() + 250;
            append_dword(L"MSHTML_NAVIGATION_READY_PROBE_QUEUE_FAILURE=", hr);
        }
    }
}
