/* overlay_update - xbox_update.c: JSON fields, tag comparison, the answer. */
#include "xbox_update.h"

#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); } } while (0)

/* The shape of GitHub's answer: html_url and name also occur inside the
 * nested author and assets, and must not be taken for the release's own. */
static const char *k_release =
    "{\"url\":\"https://api.github.com/repos/o/r/releases/1\","
    "\"html_url\":\"https://github.com/o/r/releases/tag/v0.2.0\","
    "\"id\":1,\"author\":{\"login\":\"x\",\"html_url\":\"https://github.com/x\",\"name\":\"Wrong\"},"
    "\"tag_name\":\"v0.2.0\",\"name\":\"Second \\\"Light\\\"\",\"draft\":false,"
    "\"assets\":[{\"name\":\"asset.zip\",\"html_url\":\"https://example.com/a\",\"tag_name\":\"nope\"}],"
    "\"body\":\"## What changed\\r\\n\\r\\n* Faster **menus**\\r\\n- Fixed the crash \\u2013 caf\\u00e9 \\ud83d\\ude80\\r\\n"
    "Line three\\r\\n4\\n5\\n6\\n7\\n8\\n9 ninth\\n10 tenth\"}";

static void json(void)
{
    char o[64];
    CHECK(xbox_update_json_string(k_release, "tag_name", o, sizeof o) && !strcmp(o, "v0.2.0"));
    CHECK(xbox_update_json_string(k_release, "html_url", o, sizeof o)
          && !strcmp(o, "https://github.com/o/r/releases/tag/v0.2.0"));
    CHECK(xbox_update_json_string(k_release, "name", o, sizeof o) && !strcmp(o, "Second \"Light\""));
    CHECK(!xbox_update_json_string(k_release, "login", o, sizeof o));          /* nested only */
    CHECK(!xbox_update_json_string(k_release, "draft", o, sizeof o));          /* not a string */
    CHECK(!xbox_update_json_string(k_release, "tag_nam", o, sizeof o));        /* a prefix is not the key */
    CHECK(!xbox_update_json_string(k_release, "tag_name2", o, sizeof o));
    CHECK(!xbox_update_json_string("{\"body\":null}", "body", o, sizeof o));
    CHECK(xbox_update_json_string(" { \"a\" : \"b\\\\c\\/d\" } ", "a", o, sizeof o) && !strcmp(o, "b\\c/d"));
    CHECK(!xbox_update_json_string("not json", "a", o, sizeof o));
    CHECK(!xbox_update_json_string("{\"a\":\"unterminated", "b", o, sizeof o));
    CHECK(!xbox_update_json_string("", "a", o, sizeof o));
    CHECK(!xbox_update_json_string(NULL, "a", o, sizeof o));
    /* A value of a key that is a string inside an array of the top level is not a top-level key. */
    CHECK(!xbox_update_json_string("{\"l\":[\"tag_name\",\"v1\"]}", "tag_name", o, sizeof o));
    /* Truncation never leaves half a UTF-8 sequence: "é" is 2 bytes. */
    {
        char t[4];
        CHECK(xbox_update_json_string("{\"a\":\"ab\\u00e9\"}", "a", t, sizeof t) && !strcmp(t, "ab"));
        CHECK(xbox_update_json_string("{\"a\":\"abcdef\"}", "a", t, sizeof t) && !strcmp(t, "abc"));
    }
    /* Surrogate pair: U+1F680 is f0 9f 9a 80. */
    CHECK(xbox_update_json_string("{\"a\":\"\\ud83d\\ude80\"}", "a", o, sizeof o)
          && !strcmp(o, "\xf0\x9f\x9a\x80"));
}

static void versions(void)
{
    CHECK(xbox_update_compare("v0.2.0", "0.1.0") == 1);
    CHECK(xbox_update_compare("0.1.0", "0.1.0") == 0);
    CHECK(xbox_update_compare("v0.1.0", "0.1.0") == 0);
    CHECK(xbox_update_compare("v0.0.9", "0.1.0") == -1);
    CHECK(xbox_update_compare("v0.10.0", "0.9.0") == 1);          /* numbers, not text */
    CHECK(xbox_update_compare("v1.0", "0.9.9") == 1);
    CHECK(xbox_update_compare("v1", "1.0.0") == 0);               /* missing parts are 0 */
    CHECK(xbox_update_compare("v1.0.1", "1.0") == 1);
    CHECK(xbox_update_compare("v0.2.0-rc1", "0.1.0") == 1);       /* the suffix is ignored */
    CHECK(xbox_update_compare("V2.0.0", "1.9.9") == 1);
    CHECK(xbox_update_compare("nightly", "0.1.0") == -2);
    CHECK(xbox_update_compare("", "0.1.0") == -2);
    CHECK(xbox_update_compare("v0.2.0", "") == -2);
    CHECK(xbox_update_compare(NULL, "0.1.0") == -2);
    CHECK(xbox_update_compare("v1.2.3.4", "1.2.3.3") == 1);
}

static void answers(void)
{
    xbox_update_result r;

    xbox_update_evaluate(200, NULL, k_release, "0.1.0", &r);
    CHECK(r.kind == XU_NEWER);
    CHECK(!strcmp(r.tag, "v0.2.0"));
    CHECK(!strncmp(r.headline, "Version 0.2.0 is out", 20));
    CHECK(strstr(r.headline, "Second \"Light\"") != NULL);        /* a title that is not just the version */
    CHECK(!strcmp(r.url, "https://github.com/o/r/releases/tag/v0.2.0"));
    CHECK(r.note_count == XU_NOTE_LINES);                         /* the first 8 non-empty lines */
    CHECK(!strcmp(r.notes[0], "What changed"));                   /* heading marks gone, blank line skipped */
    CHECK(!strcmp(r.notes[1], "- Faster **menus**"));
    CHECK(!strncmp(r.notes[2], "- Fixed the crash - caf", 23));   /* en dash plain */
    CHECK(strstr(r.notes[2], "?") != NULL);                       /* the rocket: not in the font */
    CHECK(!strcmp(r.notes[3], "Line three"));
    CHECK(!strcmp(r.notes[7], "7"));

    /* The same, running it: the latest; a newer dev build too. */
    xbox_update_evaluate(200, NULL, k_release, "0.2.0", &r);
    CHECK(r.kind == XU_LATEST && !strcmp(r.headline, "You have the latest version (0.2.0)"));
    CHECK(r.note_count == 0 && !r.url[0]);
    xbox_update_evaluate(200, NULL, k_release, "0.3.0", &r);
    CHECK(r.kind == XU_LATEST);

    /* A release named after its own version adds nothing to the headline. */
    xbox_update_evaluate(200, NULL, "{\"tag_name\":\"v1.2.0\",\"name\":\"Takedawn v1.2.0\"}", "0.1.0", &r);
    CHECK(r.kind == XU_NEWER && !strcmp(r.headline, "Version 1.2.0 is out (you have 0.1.0)"));

    /* A release page that is not on github.com is not offered. */
    xbox_update_evaluate(200, NULL, "{\"tag_name\":\"v9.0.0\",\"html_url\":\"https://evil.example/x\"}", "0.1.0", &r);
    CHECK(r.kind == XU_NEWER && !r.url[0]);
    xbox_update_evaluate(200, NULL, "{\"tag_name\":\"v9.0.0\",\"html_url\":\"http://github.com/x\"}", "0.1.0", &r);
    CHECK(r.kind == XU_NEWER && !r.url[0]);

    /* No release published yet (the repository is private: 404). */
    xbox_update_evaluate(404, NULL, "{\"message\":\"Not Found\"}", "0.1.0", &r);
    CHECK(r.kind == XU_NONE && !strcmp(r.headline, "No release published yet"));

    /* Failures. */
    xbox_update_evaluate(0, "timed out", NULL, "0.1.0", &r);
    CHECK(r.kind == XU_ERROR && !strcmp(r.headline, "Could not check: timed out"));
    xbox_update_evaluate(0, "no connection", NULL, "0.1.0", &r);
    CHECK(!strcmp(r.headline, "Could not check: no connection"));
    xbox_update_evaluate(0, "", NULL, "0.1.0", &r);
    CHECK(r.kind == XU_ERROR && !strcmp(r.headline, "Could not check: no answer"));
    xbox_update_evaluate(403, NULL, "{}", "0.1.0", &r);
    CHECK(r.kind == XU_ERROR && strstr(r.headline, "limiting") != NULL);
    xbox_update_evaluate(500, NULL, NULL, "0.1.0", &r);
    CHECK(r.kind == XU_ERROR && strstr(r.headline, "HTTP 500") != NULL);
    xbox_update_evaluate(200, NULL, "<html>captive portal</html>", "0.1.0", &r);
    CHECK(r.kind == XU_ERROR && strstr(r.headline, "no release") != NULL);
    xbox_update_evaluate(200, NULL, "{\"tag_name\":\"nightly\"}", "0.1.0", &r);
    CHECK(r.kind == XU_ERROR && strstr(r.headline, "not a version") != NULL);

    /* A long note line is cut with "...". */
    {
        char big[300];
        snprintf(big, sizeof big, "{\"tag_name\":\"v2.0.0\",\"body\":\"%0120d\"}", 7);
        xbox_update_evaluate(200, NULL, big, "0.1.0", &r);
        CHECK(r.note_count == 1 && strlen(r.notes[0]) == XU_NOTE_WIDTH - 1
              && !strcmp(r.notes[0] + XU_NOTE_WIDTH - 4, "..."));
    }
}

static void urls(void)
{
    char u[200];
    CHECK(xbox_update_api_url("xenophobentx/takedawn", u, sizeof u)
          && !strcmp(u, "https://api.github.com/repos/xenophobentx/takedawn/releases/latest"));
    CHECK(xbox_update_api_url("SpeedBreakerProject/speedbreaker", u, sizeof u));
    CHECK(!xbox_update_api_url("", u, sizeof u));
    CHECK(!xbox_update_api_url("noslash", u, sizeof u));
    CHECK(!xbox_update_api_url("a/b/c", u, sizeof u));
    CHECK(!xbox_update_api_url("a/../b", u, sizeof u));
    CHECK(!xbox_update_api_url("/a", u, sizeof u));
    CHECK(!xbox_update_api_url("a b/c", u, sizeof u));
    CHECK(!xbox_update_api_url("a/b?x=1", u, sizeof u));
    CHECK(!xbox_update_api_url(NULL, u, sizeof u));
}

int main(void)
{
    json();
    versions();
    answers();
    urls();
    fprintf(stderr, "overlay_update: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
