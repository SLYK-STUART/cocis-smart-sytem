#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

static inline const char *jl_find(const char *json, const char *key)
{
    char pat[40];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(json, pat);
    return p ? p + strlen(pat) : NULL;
}

static inline bool jl_get_int(const char *json, const char *key, int *out)
{
    const char *p = jl_find(json, key);
    if (!p) return false;
    *out = atoi(p);
    return true;
}

static inline bool jl_get_str(const char *json, const char *key, char *out, size_t n)
{
    const char *p = jl_find(json, key);
    if (!p || *p != '"') return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i < n - 1) out[i++] = *p++;
    out[i] = 0;
    return true;
}