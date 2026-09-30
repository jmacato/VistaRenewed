/* Bounded JSON primitives for the private CDP transport. No page text is code. */
#ifndef TRITON_MSHTML_JSON_H
#define TRITON_MSHTML_JSON_H

static inline const char *mj_space(const char *p)
{
    while(*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    return p;
}

static inline int mj_hex(char c)
{
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static inline const char *mj_string_end(const char *p)
{
    unsigned i;
    if(*p++ != '"') return NULL;
    while(*p && *p != '"') {
        if((unsigned char)*p < 32) return NULL;
        if(*p++ == '\\') {
            if(!*p) return NULL;
            if(*p == 'u') {
                ++p;
                for(i = 0; i < 4; ++i) if(mj_hex(*p++) < 0) return NULL;
            } else {
                if(!strchr("\"\\/bfnrt", *p)) return NULL;
                ++p;
            }
        }
    }
    return *p == '"' ? p + 1 : NULL;
}

static inline const char *mj_skip(const char *p, unsigned depth)
{
    char closing;
    if(!p || depth > 32) return NULL;
    p = mj_space(p);
    if(*p == '"') return mj_string_end(p);
    if(*p == '{' || *p == '[') {
        closing = *p++ == '{' ? '}' : ']';
        p = mj_space(p);
        if(*p == closing) return p + 1;
        for(;;) {
            if(closing == '}') {
                p = mj_string_end(p);
                if(!p || *(p = mj_space(p)) != ':') return NULL;
                ++p;
            }
            p = mj_skip(p, depth + 1);
            if(!p) return NULL;
            p = mj_space(p);
            if(*p == closing) return p + 1;
            if(*p++ != ',') return NULL;
            p = mj_space(p);
        }
    }
    if(!strncmp(p, "true", 4)) return p + 4;
    if(!strncmp(p, "false", 5)) return p + 5;
    if(!strncmp(p, "null", 4)) return p + 4;
    if(*p == '-') ++p;
    if(*p == '0') ++p;
    else {
        if(*p < '1' || *p > '9') return NULL;
        while(*p >= '0' && *p <= '9') ++p;
    }
    if(*p == '.') {
        ++p;
        if(*p < '0' || *p > '9') return NULL;
        while(*p >= '0' && *p <= '9') ++p;
    }
    if(*p == 'e' || *p == 'E') {
        ++p;
        if(*p == '+' || *p == '-') ++p;
        if(*p < '0' || *p > '9') return NULL;
        while(*p >= '0' && *p <= '9') ++p;
    }
    return p;
}

/* Protocol keys are ASCII. Reject duplicate requested keys and malformed
 * containers; never match fields embedded in strings or descendant objects. */
static inline const char *mj_member(const char *object, const char *key)
{
    const char *p, *end, *found = NULL, *name;
    size_t size = strlen(key);
    if(!object || *mj_space(object) != '{' || !mj_skip(object, 0)) return NULL;
    p = mj_space(mj_space(object) + 1);
    while(*p != '}') {
        name = p;
        end = mj_string_end(p);
        if(!end) return NULL;
        p = mj_space(mj_space(end) + 1);
        if((size_t)(end - name) == size + 2 && !memcmp(name + 1, key, size)) {
            if(found) return NULL;
            found = p;
        }
        p = mj_space(mj_skip(p, 0));
        if(*p == ',') p = mj_space(p + 1);
    }
    return found;
}

static inline HRESULT mj_bstr(const char *json, BSTR *out)
{
    const char *p, *end, *start;
    BSTR buffer;
    UINT used = 0;
    int n;
    if(!out) return E_POINTER;
    *out = NULL;
    if(!json || !(end = mj_string_end(json))) return E_FAIL;
    buffer = SysAllocStringLen(NULL, (UINT)(end - json));
    if(!buffer) return E_OUTOFMEMORY;
    p = json + 1;
    while(p < end - 1) {
        if(*p != '\\') {
            start = p;
            while(p < end - 1 && *p != '\\') ++p;
            n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, start, (int)(p - start),
                                   buffer + used, (int)(end - json) - (int)used);
            if(!n) { SysFreeString(buffer); return E_FAIL; }
            used += n;
        } else {
            WCHAR value;
            ++p;
            switch(*p++) {
            case 'u':
                value = (WCHAR)((mj_hex(p[0]) << 12) | (mj_hex(p[1]) << 8) |
                                (mj_hex(p[2]) << 4) | mj_hex(p[3]));
                p += 4; break;
            case 'b': value = '\b'; break;
            case 'f': value = '\f'; break;
            case 'n': value = '\n'; break;
            case 'r': value = '\r'; break;
            case 't': value = '\t'; break;
            default: value = (unsigned char)p[-1]; break;
            }
            buffer[used++] = value;
        }
    }
    *out = SysAllocStringLen(buffer, used);
    SysFreeString(buffer);
    return *out ? S_OK : E_OUTOFMEMORY;
}

static inline char *mj_quote(BSTR value)
{
    static const char hex[] = "0123456789abcdef";
    UINT i, length = SysStringLen(value);
    char *out, *p;
    if(length > 1024 * 1024) return NULL;
    out = HeapAlloc(GetProcessHeap(), 0, (size_t)length * 6 + 3);
    if(!out) return NULL;
    p = out; *p++ = '"';
    for(i = 0; i < length; ++i) {
        unsigned v = value[i];
        *p++ = '\\'; *p++ = 'u';
        *p++ = hex[v >> 12]; *p++ = hex[(v >> 8) & 15];
        *p++ = hex[(v >> 4) & 15]; *p++ = hex[v & 15];
    }
    *p++ = '"'; *p = 0;
    return out;
}
#endif
