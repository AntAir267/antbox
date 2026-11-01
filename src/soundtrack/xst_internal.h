/*
 * xst_internal.h - what xst_library.c and xst_browse.c share: a folder's
 * entries. Not for a host.
 */
#ifndef XST_INTERNAL_H
#define XST_INTERNAL_H

typedef struct { char **v; int n, cap; } xst_names;

int  xst_names_add(xst_names *l, const char *s);
void xst_names_free(xst_names *l);

/* The playable files (files may be NULL: not wanted) and the folders of
 * `dir`, without hidden entries, each in natural order. 0: unreadable. */
int  xst_list_dir(const char *dir, xst_names *files, xst_names *dirs);
/* The same, for a scan: folders that are links (a junction on Windows) are
 * left out of `dirs`, and at most *budget entries are read (it counts down;
 * NULL: no bound). */
int  xst_list_dir_scan(const char *dir, xst_names *files, xst_names *dirs, int *budget);

#endif /* XST_INTERNAL_H */
