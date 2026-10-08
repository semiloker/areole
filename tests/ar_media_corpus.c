/*
 * Answers every query in tests/media_corpus.txt at a stated window.
 *
 *     ar_media_corpus <corpus.txt> <width> <height> <resolution-thousandths>
 *
 * Prints one line per query: the answer, a tab, then the query as written.
 * tools/compare_media.py runs this and puts a browser through the same list
 * with `matchMedia`, which is the only way to find out whether this engine
 * reads a stylesheet the way the stylesheet's author expected.
 *
 * There is no public entry point for "evaluate this query", and there should
 * not be: a media query is a thing a stylesheet contains, not a thing a
 * program asks about. So the query is wrapped in the smallest sheet that can
 * report an answer -- one guarded rule, and a width to read back -- which has
 * the useful property of testing the path a real stylesheet takes rather than
 * a back door only the corpus would use.
 */

#include "areole.h"

#include "ar_css.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ar_rule g_rules[16];
static ar_mq   g_queries[8];
static char    g_qtext[512];

static int answer(const char *query, ar_i32 w, ar_i32 h, ar_i32 res)
{
    ar_sheet   sheet;
    ar_media   m;
    ar_style   st;
    char       css[1024];
    ar_u32     tag = 0;
    ar_u32     id = 0;
    ar_classes klass;

    if (strlen(query) > sizeof css - 64)
    {
        return -1;
    }
    m.width = w;
    m.height = h;
    m.resolution = res;

    ar_sheet_init(&sheet, g_rules, 16);
    ar_sheet_set_queries(&sheet, g_queries, 8, g_qtext, sizeof g_qtext);
    ar_sheet_set_media(&sheet, &m);

    strcpy(css, "@media ");
    strcat(css, query);
    strcat(css, " { .probe { width: 7px; } }");
    ar_sheet_parse(&sheet, css);

    ar_selector_split(".probe", &tag, &klass, &id);
    ar_sheet_resolve(&sheet, tag, &klass, id, 0, &st);
    return st.v[AR_P_WIDTH] == 7;
}

int main(int argc, char **argv)
{
    FILE  *f;
    char   line[1024];
    ar_i32 w, h, res;

    if (argc < 5)
    {
        fprintf(stderr, "usage: %s <corpus.txt> <width> <height> <resolution>\n", argv[0]);
        return 2;
    }
    w = (ar_i32)atoi(argv[2]);
    h = (ar_i32)atoi(argv[3]);
    res = (ar_i32)atoi(argv[4]);

    f = fopen(argv[1], "r");
    if (!f)
    {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }
    printf("# areole %s at %ldx%ld, resolution %ld\n", ar_version(), (long)w, (long)h, (long)res);
    while (fgets(line, (int)sizeof line, f))
    {
        size_t n = strlen(line);

        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
        {
            line[--n] = 0;
        }
        if (n == 0 || line[0] == '#')
        {
            continue;
        }
        printf("%d\t%s\n", answer(line, w, h, res), line);
    }
    fclose(f);
    return 0;
}
