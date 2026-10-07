/* The compatibility report as a GitHub issue-form link. Pure C. */
#ifndef COMPAT_H
#define COMPAT_H
typedef struct { const char *id, *value; } form_field;
int url_encode(const char *src, char *dst, int n);               /* RFC 3986, returns the length */
/* APP_REPO/issues/new?template=<tpl>&title=<title>&<id>=<value>...: the ids are
 * the issue form's field ids, so the form opens prefilled. With tpl NULL the
 * link opens a blank issue: then "body" is the id GitHub (web and the mobile
 * app) fills. -1 when out does not fit. */
int issue_form_url(const char *tpl, const char *title, const form_field *f, int nf, char *out, int n);
#endif
