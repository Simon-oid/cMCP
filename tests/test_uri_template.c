/* RFC 6570 URI-template matching for resources/templates/list routing:
 * simple {var} and reserved {+var} expansion, the subset cMCP supports. */
#include <stdio.h>
#include <string.h>

#include "test.h"
#include "../src/uri_template.h"

static void test_valid_templates(void) {
    TEST_ASSERT(cmcp_uri_template_valid("file:///{path}"));
    TEST_ASSERT(cmcp_uri_template_valid("db://{table}/rows/{id}"));
    TEST_ASSERT(cmcp_uri_template_valid("docs://{+path}"));
    TEST_ASSERT(cmcp_uri_template_valid("plain://no/variables"));
}

static void test_invalid_templates_are_refused(void) {
    TEST_ASSERT(!cmcp_uri_template_valid(NULL));
    TEST_ASSERT(!cmcp_uri_template_valid(""));
    TEST_ASSERT(!cmcp_uri_template_valid("x://{unclosed"));
    TEST_ASSERT(!cmcp_uri_template_valid("x://closed}"));
    TEST_ASSERT(!cmcp_uri_template_valid("x://{}"));
    TEST_ASSERT(!cmcp_uri_template_valid("x://{a{b}}"));
    TEST_ASSERT(!cmcp_uri_template_valid("x://{bad name}"));
    TEST_ASSERT(!cmcp_uri_template_valid("x://{a}{b}")); /* ambiguous split */
    TEST_ASSERT(
        !cmcp_uri_template_valid("x://{?query}")); /* unsupported operator */
    TEST_ASSERT(!cmcp_uri_template_valid("x://{#frag}"));
    TEST_ASSERT(!cmcp_uri_template_valid("x://{a,b}")); /* lists */
    TEST_ASSERT(!cmcp_uri_template_valid("x://{a:3}")); /* prefix modifier */
}

static void test_simple_variables_match_one_segment(void) {
    TEST_ASSERT(cmcp_uri_template_match("db://{table}/rows/{id}",
                                        "db://users/rows/42"));
    TEST_ASSERT(
        !cmcp_uri_template_match("db://{table}/rows/{id}", "db://users/rows/"));
    TEST_ASSERT(
        !cmcp_uri_template_match("db://{table}/rows/{id}", "db://a/b/rows/42"));
    TEST_ASSERT(!cmcp_uri_template_match("db://{table}/rows/{id}",
                                         "db://users/cols/42"));
    TEST_ASSERT(!cmcp_uri_template_match("file:///{name}", "file:///a/b"));
    TEST_ASSERT(!cmcp_uri_template_match("file:///{name}", "file:///a?x=1"));
}

static void test_reserved_variables_cross_slashes(void) {
    TEST_ASSERT(cmcp_uri_template_match("docs://{+path}", "docs://a/b/c.md"));
    TEST_ASSERT(
        cmcp_uri_template_match("docs://{+path}.md", "docs://a/b/c.md"));
    TEST_ASSERT(
        !cmcp_uri_template_match("docs://{+path}.md", "docs://a/b/c.txt"));
    TEST_ASSERT(!cmcp_uri_template_match("docs://{+path}", "docs://"));
}

static void test_literals_and_edges(void) {
    TEST_ASSERT(cmcp_uri_template_match("plain://no/variables",
                                        "plain://no/variables"));
    TEST_ASSERT(!cmcp_uri_template_match("plain://no/variables",
                                         "plain://no/variable"));
    TEST_ASSERT(!cmcp_uri_template_match("db://{t}", NULL));
    TEST_ASSERT(!cmcp_uri_template_match("x://{a}{b}",
                                         "x://ab")); /* invalid never matches */
}

/* Backtracking must stay bounded on a hostile URI. */
static void test_long_uri_terminates(void) {
    char uri[4096];
    memcpy(uri, "docs://", 7);
    memset(uri + 7, 'a', sizeof uri - 8);
    uri[sizeof uri - 1] = '\0';
    TEST_ASSERT(!cmcp_uri_template_match("docs://{+a}b{+c}b{+d}b{+e}b",
                                         uri)); /* over the cap */

    /* Under the cap, every "b" is a candidate split for every variable:
       without memoisation this is ~2000^4 tries. */
    char hostile[2001];
    memcpy(hostile, "docs://", 7);
    memset(hostile + 7, 'b', sizeof hostile - 8);
    hostile[sizeof hostile - 1] = '\0';
    TEST_ASSERT(
        !cmcp_uri_template_match("docs://{+a}b{+c}b{+d}b{+e}bX", hostile));
}

int main(void) {
    fprintf(stderr, "test_uri_template:\n");
    TEST_RUN(test_valid_templates);
    TEST_RUN(test_invalid_templates_are_refused);
    TEST_RUN(test_simple_variables_match_one_segment);
    TEST_RUN(test_reserved_variables_cross_slashes);
    TEST_RUN(test_literals_and_edges);
    TEST_RUN(test_long_uri_terminates);
    TEST_DONE();
}
