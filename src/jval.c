/* Minimal JSON value parser (arrays, objects, strings, ints, bools, null). */
#include "pf.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>

typedef struct { const char *p; char *err; size_t el; } Jp;

static void jerr(Jp *j, const char *msg)
{
    if (j->err[0] == '\0')
        snprintf(j->err, j->el, "%s", msg);
}

static void jskip(Jp *j) { while (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r') j->p++; }

static Jv *jval(Jp *j);

static char *jstring_raw(Jp *j)
{
    if (*j->p != '"') { jerr(j, "expected string"); return NULL; }
    j->p++;
    size_t cap = 16, n = 0;
    char *s = (char *)malloc(cap);
    if (!s) pf_fatal("out of memory");
    while (*j->p && *j->p != '"') {
        unsigned char c = (unsigned char)*j->p++;
        if (c == '\\') {
            char e = *j->p++;
            switch (e) {
            case '"':  c = '"';  break;
            case '\\': c = '\\'; break;
            case '/':  c = '/';  break;
            case 'b':  c = '\b'; break;
            case 'f':  c = '\f'; break;
            case 'n':  c = '\n'; break;
            case 'r':  c = '\r'; break;
            case 't':  c = '\t'; break;
            case 'u': {
                unsigned v = 0;
                for (int k = 0; k < 4; k++) {
                    char h = *j->p++;
                    v <<= 4;
                    if (h >= '0' && h <= '9') v |= (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f') v |= (unsigned)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') v |= (unsigned)(h - 'A' + 10);
                    else { jerr(j, "bad \\u escape"); free(s); return NULL; }
                }
                c = (v < 0x80) ? (unsigned char)v : '?';
                break;
            }
            default: jerr(j, "bad escape"); free(s); return NULL;
            }
        }
        if (n + 1 >= cap) { cap *= 2; s = (char *)realloc(s, cap); if (!s) pf_fatal("out of memory"); }
        s[n++] = (char)c;
    }
    if (*j->p != '"') { jerr(j, "unterminated string"); free(s); return NULL; }
    j->p++;
    s[n] = '\0';
    return s;
}

static Jv *jval(Jp *j)
{
    jskip(j);
    Jv *v = (Jv *)calloc(1, sizeof(Jv));
    if (!v) pf_fatal("out of memory");
    char c = *j->p;
    if (c == '{') {
        v->t = JV_OBJ;
        j->p++;
        jskip(j);
        int cap = 0;
        if (*j->p == '}') { j->p++; return v; }
        for (;;) {
            jskip(j);
            char *k = jstring_raw(j);
            if (!k) { free(v); return NULL; }
            jskip(j);
            if (*j->p != ':') { jerr(j, "expected ':'"); free(k); free(v); return NULL; }
            j->p++;
            Jv *val = jval(j);
            if (!val) { free(k); free(v); return NULL; }
            if (v->on == cap) { cap = cap ? cap * 2 : 4;
                v->keys = (char **)realloc(v->keys, sizeof(char *) * cap);
                v->vals = (Jv **)realloc(v->vals, sizeof(Jv *) * cap);
                if (!v->keys || !v->vals) pf_fatal("out of memory"); }
            v->keys[v->on] = k;
            v->vals[v->on] = val;
            v->on++;
            jskip(j);
            if (*j->p == ',') { j->p++; continue; }
            if (*j->p == '}') { j->p++; break; }
            jerr(j, "expected ',' or '}'");
            return v; /* partial; error is set */
        }
    } else if (c == '[') {
        v->t = JV_ARR;
        j->p++;
        jskip(j);
        int cap = 0;
        if (*j->p == ']') { j->p++; return v; }
        for (;;) {
            Jv *el = jval(j);
            if (!el) { free(v); return NULL; }
            if (v->an == cap) { cap = cap ? cap * 2 : 4;
                v->arr = (Jv **)realloc(v->arr, sizeof(Jv *) * cap);
                if (!v->arr) pf_fatal("out of memory"); }
            v->arr[v->an++] = el;
            jskip(j);
            if (*j->p == ',') { j->p++; continue; }
            if (*j->p == ']') { j->p++; break; }
            jerr(j, "expected ',' or ']'");
            return v;
        }
    } else if (c == '"') {
        v->t = JV_STR;
        v->str = jstring_raw(j);
        if (!v->str) { free(v); return NULL; }
    } else if (c == 't' && strncmp(j->p, "true", 4) == 0) {
        v->t = JV_BOOL; v->b = 1; j->p += 4;
    } else if (c == 'f' && strncmp(j->p, "false", 5) == 0) {
        v->t = JV_BOOL; v->b = 0; j->p += 5;
    } else if (c == 'n' && strncmp(j->p, "null", 4) == 0) {
        v->t = JV_NULL; j->p += 4;
    } else if (c == '-' || (c >= '0' && c <= '9')) {
        char *end = NULL;
        errno = 0;
        long long n = strtoll(j->p, &end, 10);
        if (end == j->p) { jerr(j, "bad number"); free(v); return NULL; }
        if (*end == '.' || *end == 'e' || *end == 'E') { jerr(j, "non-integer number"); free(v); return NULL; }
        v->t = JV_NUM; v->num = n; j->p = end;
    } else {
        jerr(j, "unexpected token");
        free(v);
        return NULL;
    }
    return v;
}

Jv *json_parse(const char *text, char *err, size_t el)
{
    Jp j = { text, err, el };
    err[0] = '\0';
    Jv *v = jval(&j);
    if (!v) return NULL;
    jskip(&j);
    if (*j.p != '\0') { jerr(&j, "trailing data"); return v; } /* caller may still reject */
    return v;
}

Jv *jget(const Jv *o, const char *k)
{
    if (!o || o->t != JV_OBJ) return NULL;
    for (int i = 0; i < o->on; i++)
        if (strcmp(o->keys[i], k) == 0) return o->vals[i];
    return NULL;
}

void json_free(Jv *v)
{
    if (!v) return;
    if (v->t == JV_STR) free(v->str);
    if (v->t == JV_ARR) {
        for (int i = 0; i < v->an; i++) json_free(v->arr[i]);
        free(v->arr);
    }
    if (v->t == JV_OBJ) {
        for (int i = 0; i < v->on; i++) { free(v->keys[i]); json_free(v->vals[i]); }
        free(v->keys); free(v->vals);
    }
    free(v);
}
