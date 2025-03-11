/*
 * areole benchmark - a scanner for the tool's own JSON.
 * SPDX-License-Identifier: MIT
 *
 * Deliberately not a JSON parser, and it does not claim to be one. It reads
 * exactly the shape this tool writes: a flat object, or an array of flat
 * objects, with string and number values and no nesting inside them.
 *
 * A real parser would be several hundred lines, would need a tree and
 * therefore an allocator, and would buy nothing: the only documents it will
 * ever read are ones this program wrote. Writing one anyway would be the kind
 * of thoroughness that costs a week and prevents no bug.
 *
 * ponytail: a scanner, not a parser. If areole ever needs to read JSON it did
 * not write, this gets replaced rather than extended, because extending a
 * scanner into a parser is how parsers acquire holes.
 */
#include "bench.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *bench_read_file(const char *path, long *len_out)
{
    FILE *f = fopen(path, "rb");
    long  len;
    char *buf;

    if (!f)
    {
        return 0;
    }
    if (fseek(f, 0, SEEK_END) != 0)
    {
        fclose(f);
        return 0;
    }
    len = ftell(f);
    if (len < 0)
    {
        fclose(f);
        return 0;
    }
    rewind(f);

    buf = (char *)malloc((size_t)len + 1);
    if (!buf)
    {
        fclose(f);
        return 0;
    }
    if (fread(buf, 1, (size_t)len, f) != (size_t)len)
    {
        free(buf);
        fclose(f);
        return 0;
    }
    buf[len] = 0;
    fclose(f);
    if (len_out)
    {
        *len_out = len;
    }
    return buf;
}

/* Finds the object whose "name" field equals the given value, and returns a
   pointer just past its opening brace. The search is for the field, then the
   enclosing brace is walked back to, which is simpler and more robust than
   tracking nesting forward. */
const char *bench_json_object_named(const char *text, const char *name)
{
    const char *p = text;
    size_t      nlen = strlen(name);

    while ((p = strstr(p, "\"name\"")) != 0)
    {
        const char *q = p + 6;
        const char *back;

        while (*q && *q != '"')
        {
            q++;
        }
        if (!*q)
        {
            return 0;
        }
        q++;
        if (strncmp(q, name, nlen) == 0 && q[nlen] == '"')
        {
            /* Walk back to the brace that opened this object. */
            back = p;
            while (back > text && *back != '{')
            {
                back--;
            }
            return (*back == '{') ? back : 0;
        }
        p = q;
    }
    return 0;
}

/* Reads a numeric field from within one object. Stops at the object's closing
   brace, so a field of the same name in a later object is not picked up. */
int bench_json_number(const char *obj, const char *key, double *out)
{
    char        pat[64];
    const char *p;
    const char *end;
    int         depth = 0;

    if (!obj)
    {
        return 0;
    }

    /* Find this object's end, so the search cannot run into the next one. */
    for (end = obj; *end; ++end)
    {
        if (*end == '{')
        {
            depth++;
        }
        else if (*end == '}')
        {
            depth--;
            if (depth == 0)
            {
                break;
            }
        }
    }

    sprintf(pat, "\"%s\"", key);
    p = strstr(obj, pat);
    if (!p || p > end)
    {
        return 0;
    }
    p += strlen(pat);
    while (*p && *p != ':')
    {
        p++;
    }
    if (!*p)
    {
        return 0;
    }
    p++;
    while (*p == ' ' || *p == '\t')
    {
        p++;
    }

    /* true and false appear in this format as booleans, and are read as one
       and zero rather than rejected, because that is what the caller wants. */
    if (strncmp(p, "true", 4) == 0)
    {
        *out = 1.0;
        return 1;
    }
    if (strncmp(p, "false", 5) == 0)
    {
        *out = 0.0;
        return 1;
    }
    if ((*p >= '0' && *p <= '9') || *p == '-' || *p == '+' || *p == '.')
    {
        *out = atof(p);
        return 1;
    }
    return 0;
}

int bench_json_string(const char *obj, const char *key, char *buf, size_t n)
{
    char        pat[64];
    const char *p;
    size_t      i = 0;

    if (!obj || n == 0)
    {
        return 0;
    }
    sprintf(pat, "\"%s\"", key);
    p = strstr(obj, pat);
    if (!p)
    {
        return 0;
    }
    p += strlen(pat);
    while (*p && *p != ':')
    {
        p++;
    }
    while (*p && *p != '"')
    {
        p++;
    }
    if (!*p)
    {
        return 0;
    }
    p++;
    while (*p && *p != '"' && i + 1 < n)
    {
        buf[i++] = *p++;
    }
    buf[i] = 0;
    return 1;
}
