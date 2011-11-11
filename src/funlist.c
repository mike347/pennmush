/**
 * \file funlist.c
 *
 * \brief List-handling functions for mushcode.
 *
 *
 */
#include "copyrite.h"

#include "config.h"
#define _GNU_SOURCE
#include <string.h>
#include <ctype.h>
#include "conf.h"
#include "case.h"
#include "externs.h"
#include "ansi.h"
#include "parse.h"
#include "function.h"
#include "mymalloc.h"
#include "mypcre.h"
#include "match.h"
#include "command.h"
#include "attrib.h"
#include "dbdefs.h"
#include "flags.h"
#include "mushdb.h"
#include "lock.h"
#include "sort.h"
#include "confmagic.h"


enum itemfun_op { IF_DELETE, IF_REPLACE, IF_INSERT };
static void do_itemfuns(char *buff, char **bp, char *str, char *num,
                        char *word, char *sep, enum itemfun_op flag);

extern const unsigned char *tables;

/** Convert list to array.
 * Chops up a list of words into an array of words. The list is
 * destructively modified. The array returned consists of
 * mush_strdup'd strings.
 * \param r pointer to array to store words.
 * \param max maximum number of words to split out.
 * \param list list of words as a string.
 * \param sep separator character between list items.
 * \param nullok are null elements allowed? if not, they'll be removed
 * \return number of words split out.
 */
int
list2arr_ansi(char *r[], int max, char *list, char sep, int nullok)
{
  char *p, *lp;
  int i;
  ansi_string *as;
  char *aptr;

  /* Since ansi_string is ridiculously slow, we only use it if the string
   * actually has markup. Unfortunately, freearr(), which is called only for
   * list2arr_ansi()'d stuff, requires we malloc each item. Sigh. */
  if (!has_markup(list)) {
    int ret = list2arr(r, max, list, sep, nullok);
    for (i = 0; i < ret; i++) {
      /* This is lame, but fortunately, assignment happens after we call
       * mush_strdup. A-hehehehe. */
      r[i] = GC_STRDUP(r[i]);
    }
    return ret;
  }

  as = parse_ansi_string(list);
  aptr = as->text;

  aptr = trim_space_sep(aptr, sep);

  lp = list;
  do {
    p = split_token(&aptr, sep);
  } while (!nullok && p && !*p);
  for (i = 0; p && (i < max); i++) {
    lp = list;
    safe_ansi_string(as, p - (as->text), strlen(p), list, &lp);
    *lp = '\0';
    r[i] = GC_STRDUP(list);
    do {
      p = split_token(&aptr, sep);
    } while (!nullok && p && !*p);
  }
  free_ansi_string(as);
  return i;
}

/** Convert list to array.
 * Chops up a list of words into an array of words. The list is
 * destructively modified.
 * \param r pointer to array to store words.
 * \param max maximum number of words to split out.
 * \param list list of words as a string.
 * \param sep separator character between list items.
 * \param nullok are null elements allowed? if not, they'll be removed
 * \return number of words split out.
 */
int
list2arr(char *r[], int max, char *list, char sep, int nullok)
{
  char *p;
  int i;
  char *aptr;
  size_t len;

  /* Quick-casing an empty list. */
  if (!*list) {
    return 0;
  }

  aptr = remove_markup(list, &len);

  memcpy(list, aptr, len);

  aptr = trim_space_sep(list, sep);

  do {
    p = split_token(&aptr, sep);
  } while (!nullok && p && !*p);
  for (i = 0; p && (i < max); i++) {
    r[i] = p;
    do {
      p = split_token(&aptr, sep);
    } while (!nullok && p && !*p);
  }
  return i;
}

/** Convert array to list.
 * Takes an array of words and concatenates them into a string,
 * using our safe string functions.
 * \param r pointer to array of words.
 * \param max maximum number of words to concatenate.
 * \param list string to fill with word list.
 * \param lp pointer into end of list.
 * \param sep string to use as separator between words.
 */
void
arr2list(char *r[], int max, char *list, char **lp, const char *sep)
{
  int i;
  int seplen = 0;

  if (!max)
    return;

  if (sep && *sep)
    seplen = strlen(sep);

  safe_str(r[0], list, lp);
  for (i = 1; i < max; i++) {
    safe_strl(sep, seplen, list, lp);
    safe_str(r[i], list, lp);
  }
  **lp = '\0';
}


/* ARGSUSED */
FUNCTION(fun_munge)
{
  /* This is a function which takes three arguments. The first is
   * an obj-attr pair referencing a u-function to be called. The
   * other two arguments are lists. The first list is passed to the
   * u-function.  The second list is then rearranged to match the
   * order of the first list as returned from the u-function.
   * This rearranged list is returned by MUNGE.
   * A fourth argument (separator) is optional.
   */
  char list1[BUFFER_LEN], *lp, rlist[BUFFER_LEN];
  char **ptrs1, **ptrs2, **results;
  char **ptrs3;
  int i, j, nptrs1, nptrs2, nresults;
  ufun_attrib ufun;
  char sep, isep[2] = { '\0', '\0' }, *osep, osepd[2] = {
  '\0', '\0'};
  int first;
  PE_REGS *pe_regs;

  if (!delim_check(buff, bp, nargs, args, 4, &sep))
    return;

  isep[0] = sep;
  if (nargs == 5)
    osep = args[4];
  else {
    osepd[0] = sep;
    osep = osepd;
  }

  /* find our object and attribute */
  if (!fetch_ufun_attrib(args[0], executor, &ufun, UFUN_DEFAULT))
    return;

  /* Copy the first list, since we need to pass it to two destructive
   * routines.
   */

  strcpy(list1, remove_markup(args[1], NULL));

  /* Break up the two lists into their respective elements. */

  ptrs1 = GC_MALLOC(MAX_SORTSIZE * sizeof(char *));
  ptrs2 = GC_MALLOC(MAX_SORTSIZE * sizeof(char *));

  /* ptrs3 is destructively modified, but it's a copy of ptrs2, so we
   * make it a straight copy of ptrs2 and freearr() on ptrs2. */
  ptrs3 = GC_MALLOC(MAX_SORTSIZE * sizeof(char *));

  if (!ptrs1 || !ptrs2)
    mush_panic("Unable to allocate memory in fun_munge");
  nptrs1 = list2arr_ansi(ptrs1, MAX_SORTSIZE, list1, sep, 1);
  nptrs2 = list2arr_ansi(ptrs2, MAX_SORTSIZE, args[2], sep, 1);
  memcpy(ptrs3, ptrs2, MAX_SORTSIZE * sizeof(char *));

  if (nptrs1 != nptrs2) {
    safe_str(T("#-1 LISTS MUST BE OF EQUAL SIZE"), buff, bp);
    return;
  }

  /* Call the user function */
  lp = args[1];
  pe_regs = pe_regs_create(PE_REGS_ARG, "fun_munge");
  pe_regs_setenv_nocopy(pe_regs, 0, lp);
  pe_regs_setenv_nocopy(pe_regs, 1, isep);
  call_ufun(&ufun, rlist, executor, enactor, pe_info, pe_regs);
  pe_regs_free(pe_regs);
  strcpy(rlist, remove_markup(rlist, NULL));

  /* Now that we have our result, put it back into array form. Search
   * through list1 until we find the element position, then copy the
   * corresponding element from list2.  Mark used elements with
   * NULL to handle duplicates
   */
  results = GC_MALLOC(MAX_SORTSIZE * sizeof(char *));
  if (!results)
    mush_panic("Unable to allocate memory in fun_munge");
  nresults = list2arr_ansi(results, MAX_SORTSIZE, rlist, sep, 1);

  first = 1;
  for (i = 0; i < nresults; i++) {
    for (j = 0; j < nptrs1; j++) {
      if (ptrs3[j] && !strcmp(results[i], ptrs1[j])) {
        if (first)
          first = 0;
        else
          safe_str(osep, buff, bp);
        safe_str(ptrs3[j], buff, bp);
        ptrs3[j] = NULL;
        break;
      }
    }
  }
}

/* ARGSUSED */
FUNCTION(fun_elements)
{
  /* Given a list and a list of numbers, return the corresponding
   * elements of the list. elements(ack bar eep foof yay,2 4) = bar foof
   * A separator for the first list is allowed.
   */
  int nwords, cur;
  char **ptrs;
  char *wordlist;
  char *s, *r, sep;
  char *osep, osepd[2] = { '\0', '\0' };

  if (!delim_check(buff, bp, nargs, args, 3, &sep))
    return;

  if (nargs == 4)
    osep = args[3];
  else {
    osepd[0] = sep;
    osep = osepd;
  }

  ptrs = GC_MALLOC(MAX_SORTSIZE * sizeof(char *));
  wordlist = alloc_buf();
  if (!ptrs || !wordlist)
    mush_panic("Unable to allocate memory in fun_elements");

  /* Turn the first list into an array. */
  strcpy(wordlist, args[0]);
  nwords = list2arr_ansi(ptrs, MAX_SORTSIZE, wordlist, sep, 1);

  s = trim_space_sep(args[1], ' ');

  /* Go through the second list, grabbing the numbers and finding the
   * corresponding elements.
   */
  r = split_token(&s, ' ');
  cur = atoi(r) - 1;
  if ((cur >= 0) && (cur < nwords) && ptrs[cur]) {
    safe_str(ptrs[cur], buff, bp);
  }
  while (s) {
    r = split_token(&s, ' ');
    cur = atoi(r) - 1;
    if ((cur >= 0) && (cur < nwords) && ptrs[cur]) {
      safe_str(osep, buff, bp);
      safe_str(ptrs[cur], buff, bp);
    }
  }
}

/* ARGSUSED */
FUNCTION(fun_matchall)
{
  /* Check each word individually, returning the word number of all
   * that match. If none match, return an empty string.
   */

  int wcount;
  char *r, *s, *b, sep;
  char *osep, osepd[2] = { '\0', '\0' };

  if (!delim_check(buff, bp, nargs, args, 3, &sep))
    return;

  if (nargs == 4)
    osep = args[3];
  else {
    osepd[0] = sep;
    osep = osepd;
  }

  wcount = 1;
  s = trim_space_sep(args[0], sep);
  b = *bp;
  do {
    r = split_token(&s, sep);
    if (quick_wild(args[1], r)) {
      if (*bp != b)
        safe_str(osep, buff, bp);
      safe_integer(wcount, buff, bp);
    }
    wcount++;
  } while (s);
}

/* ARGSUSED */
FUNCTION(fun_graball)
{
  /* Check each word individually, returning all that match.
   * If none match, return an empty string.  This is to grab()
   * what matchall() is to match().
   */

  char *r, *s, *b, sep;
  char *osep, osepd[2] = { '\0', '\0' };

  if (!delim_check(buff, bp, nargs, args, 3, &sep))
    return;

  if (nargs == 4)
    osep = args[3];
  else {
    osepd[0] = sep;
    osep = osepd;
  }

  s = trim_space_sep(args[0], sep);
  b = *bp;
  do {
    r = split_token(&s, sep);
    if (quick_wild(args[1], r)) {
      if (*bp != b)
        safe_str(osep, buff, bp);
      safe_str(r, buff, bp);
    }
  } while (s);
}



/* ARGSUSED */
FUNCTION(fun_fold)
{
  /* iteratively evaluates an attribute with a list of arguments and
   * optional base case. With no base case, the first list element is
   * passed as %0, and the second as %1. The attribute is then evaluated
   * with these args. The result is then used as %0, and the next arg as
   * %1. Repeat until no elements are left in the list. The base case
   * can provide a starting point.
   */

  ufun_attrib ufun;
  char *cp;
  PE_REGS *pe_regs;
  char sep;
  int funccount, per;
  char base[BUFFER_LEN];
  char result[BUFFER_LEN];

  if (!delim_check(buff, bp, nargs, args, 4, &sep))
    return;

  if (!fetch_ufun_attrib(args[0], executor, &ufun, UFUN_DEFAULT))
    return;

  cp = args[1];

  /* If we have three or more arguments, the third one is the base case */
  if (nargs >= 3) {
    strncpy(base, args[2], BUFFER_LEN);
  } else {
    strncpy(base, split_token(&cp, sep), BUFFER_LEN);
  }
  pe_regs = pe_regs_create(PE_REGS_ARG, "fun_fold");
  pe_regs_setenv_nocopy(pe_regs, 0, base);
  pe_regs_setenv_nocopy(pe_regs, 1, split_token(&cp, sep));

  call_ufun(&ufun, result, executor, enactor, pe_info, pe_regs);

  strncpy(base, result, BUFFER_LEN);

  funccount = pe_info->fun_invocations;

  /* handle the rest of the cases */
  while (cp && *cp) {
    pe_regs_setenv_nocopy(pe_regs, 1, split_token(&cp, sep));
    per = call_ufun(&ufun, result, executor, enactor, pe_info, pe_regs);
    if (per || (pe_info->fun_invocations >= FUNCTION_LIMIT &&
                pe_info->fun_invocations == funccount && !strcmp(base, result)))
      break;
    funccount = pe_info->fun_invocations;
    strncpy(base, result, BUFFER_LEN);
  }
  pe_regs_free(pe_regs);
  safe_str(base, buff, bp);
}

/* ARGSUSED */
FUNCTION(fun_itemize)
{
  /* Called in one of two ways:
   * itemize(<list>[,<delim>[,<conjunction>[,<punctuation>]]])
   * elist(<list>[,<conjunction> [,<delim> [,<output delim> [,<punctuation>]]]])
   * Either way, it takes the elements of list and:
   *  If there's just one, returns it.
   *  If there's two, returns <e1> <conjunction> <e2>
   *  If there's >2, returns <e1><punc> <e2><punc> ... <conjunction> <en>
   * Default <conjunction> is "and", default punctuation is ","
   */
  const char *outsep = " ";
  char sep = ' ';
  const char *lconj = "and";
  const char *punc = ",";
  int pos, n;
  char *words[MAX_SORTSIZE];

  if (strcmp(called_as, "ELIST") == 0) {
    /* elist ordering */
    if (!delim_check(buff, bp, nargs, args, 3, &sep))
      return;
    if (nargs > 1)
      lconj = args[1];
    if (nargs > 3)
      outsep = args[3];
    if (nargs > 4)
      punc = args[4];
  } else {
    /* itemize ordering */
    if (!delim_check(buff, bp, nargs, args, 2, &sep))
      return;
    if (nargs > 2)
      lconj = args[2];
    if (nargs > 3)
      punc = args[3];
  }
  n = list2arr_ansi(words, MAX_SORTSIZE, args[0], sep, 1) - 1;
  for (pos = 0; pos <= n; pos++) {
    safe_itemizer(pos + 1, pos == n, punc, lconj, outsep, buff, bp);
    safe_str(words[pos], buff, bp);
  }
}

/* ARGSUSED */
FUNCTION(fun_filter)
{
  /* take a user-def function and a list, and return only those elements
   * of the list for which the function evaluates to 1.
   */

  ufun_attrib ufun;
  char result[BUFFER_LEN];
  char *cp;
  PE_REGS *pe_regs;
  const char *cur;
  char sep;
  int first;
  int check_bool = 0;
  int funccount;
  char *osep, osepd[2] = { '\0', '\0' };

  if (!delim_check(buff, bp, nargs, args, 3, &sep))
    return;

  osepd[0] = sep;
  osep = (nargs >= 4) ? args[3] : osepd;

  if (strcmp(called_as, "FILTERBOOL") == 0)
    check_bool = 1;

  /* find our object and attribute */
  if (!fetch_ufun_attrib(args[0], executor, &ufun, UFUN_DEFAULT))
    return;

  /* Go through each argument */
  cp = trim_space_sep(args[1], sep);
  first = 1;
  funccount = pe_info->fun_invocations;
  pe_regs = pe_regs_create(PE_REGS_ARG, "fun_filter");
  while (cp && *cp) {
    cur = split_token(&cp, sep);
    pe_regs_setenv_nocopy(pe_regs, 0, cur);
    if (call_ufun(&ufun, result, executor, enactor, pe_info, pe_regs))
      break;
    if ((check_bool == 0)
        ? (*result == '1' && *(result + 1) == '\0')
        : parse_boolean(result)) {
      if (first)
        first = 0;
      else
        safe_str(osep, buff, bp);
      safe_str(cur, buff, bp);
    }
    /* Can't do *bp == oldbp like in all the others, because bp might not
     * move even when not full, if one of the list elements is null and
     * we have a null separator. */
    if (*bp == (buff + BUFFER_LEN - 1) && pe_info->fun_invocations == funccount)
      break;
    funccount = pe_info->fun_invocations;
  }
  pe_regs_free(pe_regs);
}

/* ARGSUSED */
FUNCTION(fun_shuffle)
{
  /* given a list of words, randomize the order of words.
   * We do this by taking each element, and swapping it with another
   * element with a greater array index (thus, words[0] can be swapped
   * with anything up to words[n], words[5] with anything between
   * itself and words[n], etc.
   * This is relatively fast - linear time - and reasonably random.
   * Will take an optional delimiter argument.
   */

  char *words[MAX_SORTSIZE];
  int n, i, j;
  char sep;
  char *osep, osepd[2] = { '\0', '\0' };

  if (!delim_check(buff, bp, nargs, args, 2, &sep))
    return;

  if (nargs == 3)
    osep = args[2];
  else {
    osepd[0] = sep;
    osep = osepd;
  }

  /* split the list up, or return if the list is empty */
  if (!*args[0])
    return;
  n = list2arr_ansi(words, MAX_SORTSIZE, args[0], sep, 1);

  /* shuffle it */
  for (i = 0; i < n; i++) {
    char *tmp;
    j = get_random32(i, n - 1);
    tmp = words[j];
    words[j] = words[i];
    words[i] = tmp;
  }

  arr2list(words, n, buff, bp, osep);
}


/* ARGSUSED */
FUNCTION(fun_sort)
{
  char *ptrs[MAX_SORTSIZE];
  int nptrs;
  SortType sort_type;
  char sep;
  char *osep, osepd[2] = { '\0', '\0' };

  if (!nargs || !*args[0])
    return;

  if (!delim_check(buff, bp, nargs, args, 3, &sep))
    return;

  if (nargs < 4) {
    osepd[0] = sep;
    osep = osepd;
  } else
    osep = args[3];

  nptrs = list2arr_ansi(ptrs, MAX_SORTSIZE, args[0], sep, 1);
  sort_type = get_list_type(args, nargs, 2, ptrs, nptrs);
  do_gensort(executor, ptrs, NULL, nptrs, sort_type);
  arr2list(ptrs, nptrs, buff, bp, osep);
}

/* ARGSUSED */
FUNCTION(fun_sortkey)
{
  char *ptrs[MAX_SORTSIZE];
  char *keys[MAX_SORTSIZE];
  int nptrs;
  SortType sort_type;
  PE_REGS *pe_regs;
  char sep;
  char *osep, osepd[2] = { '\0', '\0' };
  int i;
  char result[BUFFER_LEN];
  ufun_attrib ufun;

  /* sortkey(attr,list,sort_type,delim,osep) */

  if (!nargs || !*args[0] || !*args[1])
    return;

  if (!delim_check(buff, bp, nargs, args, 4, &sep))
    return;

  if (nargs < 5) {
    osepd[0] = sep;
    osep = osepd;
  } else
    osep = args[4];

  /* find our object and attribute */
  if (!fetch_ufun_attrib(args[0], executor, &ufun, UFUN_DEFAULT))
    return;

  nptrs = list2arr_ansi(ptrs, MAX_SORTSIZE, args[1], sep, 1);

  pe_regs = pe_regs_create(PE_REGS_ARG, "fun_sortkey");

  /* Now we make a list of keys */
  for (i = 0; i < nptrs; i++) {
    /* Build our %0 args */
    pe_regs_setenv_nocopy(pe_regs, 0, ptrs[i]);
    call_ufun(&ufun, result, executor, enactor, pe_info, pe_regs);
    keys[i] = GC_STRDUP(result);
  }
  pe_regs_free(pe_regs);

  sort_type = get_list_type(args, nargs, 3, keys, nptrs);
  do_gensort(executor, keys, ptrs, nptrs, sort_type);
  arr2list(ptrs, nptrs, buff, bp, osep);
}

/* ARGSUSED */
FUNCTION(fun_sortby)
{
  char *ptrs[MAX_SORTSIZE];
  char sep;
  int nptrs;
  char *osep, osepd[2] = { '\0', '\0' };
  ufun_attrib ufun;


  if (!nargs || !*args[0])
    return;

  if (!delim_check(buff, bp, nargs, args, 3, &sep))
    return;

  if (nargs == 4)
    osep = args[3];
  else {
    osepd[0] = sep;
    osep = osepd;
  }

  if (!fetch_ufun_attrib
      (args[0], executor, &ufun, UFUN_DEFAULT | UFUN_REQUIRE_ATTR))
    return;

  /* Split up the list, sort it, reconstruct it. */
  nptrs = list2arr_ansi(ptrs, MAX_SORTSIZE, args[1], sep, 1);
  if (nptrs > 1)                /* pointless to sort less than 2 elements */
    sane_qsort((void **) ptrs, 0, nptrs - 1, u_comp, executor, enactor, &ufun,
               pe_info);

  arr2list(ptrs, nptrs, buff, bp, osep);

}

#define OUTSEP() do { \
    if (found) { \
      safe_strl(osep, osepl, buff, bp); \
    } else { \
      found = 1; \
    } \
} while (0)

/* ARGSUSED */
FUNCTION(fun_setmanip)
{
  char sep;
  char **a1, **a2;
  int n1, n2, x1, x2, val;
  int found = 0;
  SortType sort_type = UNKNOWN_LIST;
  int osepl = 0;
  char *osep = NULL, osepd[2] = { '\0', '\0' };
  s_rec *sp1, *sp2;
  ListTypeInfo *lti;

  bool dolt = 0, dogt = 0, doeq = 0;
  if (strstr(called_as, "DIFF")) {
    dolt = 1;
  } else if (strstr(called_as, "INTER")) {
    doeq = 1;
  } else {
    /* Setunion. */
    dolt = dogt = doeq = 1;
  }

  /* if no lists, then no work */
  if (!*args[0] && !*args[1])
    return;

  if (!delim_check(buff, bp, nargs, args, 3, &sep))
    return;

  a1 = GC_MALLOC(MAX_SORTSIZE * sizeof(char *));
  a2 = GC_MALLOC(MAX_SORTSIZE * sizeof(char *));
  if (!a1 || !a2)
    mush_panic("Unable to allocate memory in fun_setmanip");

  /* make arrays out of the lists */
  n1 = list2arr_ansi(a1, MAX_SORTSIZE, args[0], sep, 1);
  n2 = list2arr_ansi(a2, MAX_SORTSIZE, args[1], sep, 1);

  if (nargs < 4) {
    sort_type = autodetect_2lists(a1, n1, a2, n2);
    osepd[0] = sep;
    osep = osepd;
    if (sep)
      osepl = 1;
  } else if (nargs == 4) {
    sort_type = get_list_type_noauto(args, nargs, 4);
    if (sort_type == UNKNOWN_LIST) {
      osep = args[3];
      osepl = arglens[3];
    } else {
      osepd[0] = sep;
      osep = osepd;
      if (sep)
        osepl = 1;
    }
  } else if (nargs == 5) {
    sort_type = get_list_type_noauto(args, nargs, 4);
    osep = args[4];
    osepl = arglens[4];
  }

  if (sort_type == UNKNOWN_LIST) {
    sort_type = autodetect_2lists(a1, n1, a2, n2);
  }

  /* sort each array */
  lti = get_list_type_info(sort_type);
  sp1 = slist_build(executor, a1, NULL, n1, lti);
  sp2 = slist_build(executor, a2, NULL, n2, lti);
  slist_qsort(sp1, n1, lti);
  n1 = slist_uniq(sp1, n1, lti);
  slist_qsort(sp2, n2, lti);
  n2 = slist_uniq(sp2, n2, lti);

  /* get the first value for the intersection, removing duplicates
   * We already know that duplicates have been removed from each list. */
  x1 = x2 = 0;
  while ((x1 < n1) && (x2 < n2)) {
    val = slist_comp(&sp1[x1], &sp2[x2], lti);
    if (val < 0) {
      if (dolt) {
        OUTSEP();
        safe_str(sp1[x1].val, buff, bp);
      }
      x1++;
    } else if (val > 0) {
      if (dogt) {
        OUTSEP();
        safe_str(sp2[x2].val, buff, bp);
      }
      x2++;
    } else {
      if (doeq) {
        OUTSEP();
        safe_str(sp1[x1].val, buff, bp);
      }
      x1++;
      x2++;
    }
  }
  if (dolt) {
    while (x1 < n1) {
      OUTSEP();
      safe_str(sp1[x1++].val, buff, bp);
    }
  }
  if (dogt) {
    while (x2 < n2) {
      OUTSEP();
      safe_str(sp2[x2++].val, buff, bp);
    }
  }
}

FUNCTION(fun_unique)
{
  char sep;
  char **ary;
  int n, i;
  int osepl = 0;
  char *osep = NULL, osepd[2] = { '\0', '\0' };
  SortType sort_type = ALPHANUM_LIST;
  ListTypeInfo *lti;
  s_rec *sp;

  /* if no lists, then no work */
  if (!*args[0])
    return;

  if (!delim_check(buff, bp, nargs, args, 3, &sep))
    return;

  ary = GC_MALLOC(MAX_SORTSIZE * sizeof(char *));

  if (!ary)
    mush_panic("Unable to allocate memory in fun_unique");

  /* make an array out of the list */
  n = list2arr_ansi(ary, MAX_SORTSIZE, args[0], sep, 1);

  if (nargs >= 2)
    sort_type = get_list_type(args, nargs, 2, ary, n);

  if (nargs < 4) {
    osepd[0] = sep;
    osep = osepd;
    if (sep)
      osepl = 1;
  } else if (nargs == 4) {
    osep = args[3];
    osepl = arglens[3];
  }

  lti = get_list_type_info(sort_type);
  sp = slist_build(executor, ary, NULL, n, lti);
  n = slist_uniq(sp, n, lti);

  for (i = 0; i < n; i++) {
    if (i > 0)
      safe_strl(osep, osepl, buff, bp);
    safe_str(sp[i].val, buff, bp);
  }
}

#define CACHE_SIZE 8  /**< Maximum size of the lnum cache */

/* ARGSUSED */
FUNCTION(fun_lnum)
{
  NVAL j;
  NVAL start;
  NVAL end;
  NVAL step = 1.0;
  int istart, iend, k, istep;
  char const *osep = " ";
  static NVAL cstart[CACHE_SIZE];
  static NVAL cend[CACHE_SIZE];
  static char csep[CACHE_SIZE][BUFFER_LEN];
  static char cresult[CACHE_SIZE][BUFFER_LEN];
  static int cstep[CACHE_SIZE];
  static int cpos;
  char *cp;

  if (!is_number(args[0])) {
    safe_str(T(e_num), buff, bp);
    return;
  }
  end = parse_number(args[0]);
  if (nargs > 1) {
    if (!is_number(args[1])) {
      safe_str(T(e_num), buff, bp);
      return;
    }
    if (nargs > 3 && is_number(args[3])) {
      step = parse_number(args[3]);
    }
    start = end;
    end = parse_number(args[1]);
    if ((start == 0) && (end == 0)) {
      safe_str("0", buff, bp);  /* Special case - lnum(0,0) -> 0 */
      return;
    }
  } else {
    if (end == 0.0)
      return;                   /* Special case - lnum(0) -> blank string */
    else if (end == 1.0) {
      safe_str("0", buff, bp);  /* Special case - lnum(1) -> 0 */
      return;
    }
    end--;
    if (end < 0.0) {
      safe_str(T("#-1 NUMBER OUT OF RANGE"), buff, bp);
      return;
    }
    start = 0.0;
  }
  if (nargs > 2) {
    osep = args[2];
  }
  for (k = 0; k < CACHE_SIZE; k++) {
    if (cstart[k] == start && cend[k] == end && !strcmp(csep[k], osep) &&
        cstep[k] == step) {
      safe_str(cresult[k], buff, bp);
      return;
    }
  }
  cpos = (cpos + 1) % CACHE_SIZE;
  cstart[cpos] = start;
  cend[cpos] = end;
  strcpy(csep[cpos], osep);
  cp = cresult[cpos];

  if (step == 0.0)
    step = 1.0;
  else if (step < 0.0)
    step = step * -1.0;

  istart = (int) start;
  iend = (int) end;
  istep = (int) step;
  if (istart == start && iend == end && istep == step) {
    safe_integer(istart, cresult[cpos], &cp);
    if (istart <= iend) {
      for (k = istart + istep; k <= iend; k += istep) {
        safe_str(osep, cresult[cpos], &cp);
        if (safe_integer(k, cresult[cpos], &cp))
          break;
      }
    } else {
      for (k = istart - istep; k >= iend; k -= istep) {
        safe_str(osep, cresult[cpos], &cp);
        if (safe_integer(k, cresult[cpos], &cp))
          break;
      }
    }
  } else {
    safe_number(start, cresult[cpos], &cp);
    if (start <= end) {
      for (j = start + step; j <= end; j += step) {
        safe_str(osep, cresult[cpos], &cp);
        if (safe_number(j, cresult[cpos], &cp))
          break;
      }
    } else {
      for (j = start - step; j >= end; j -= step) {
        safe_str(osep, cresult[cpos], &cp);
        if (safe_number(j, cresult[cpos], &cp))
          break;
      }
    }
  }
  *cp = '\0';

  safe_str(cresult[cpos], buff, bp);
}

/* ARGSUSED */
FUNCTION(fun_first)
{
  /* read first word from a string */

  char *p;
  char sep;

  if (!*args[0])
    return;

  if (!delim_check(buff, bp, nargs, args, 2, &sep))
    return;

  p = trim_space_sep(args[0], sep);
  safe_str(split_token(&p, sep), buff, bp);
}

/* ARGSUSED */
FUNCTION(fun_randword)
{
  char *s, *r;
  char sep;
  int word_count, word_index;

  if (!*args[0])
    return;

  if (!delim_check(buff, bp, nargs, args, 2, &sep))
    return;

  s = trim_space_sep(args[0], sep);
  word_count = do_wordcount(s, sep);

  if (word_count == 0)
    return;

  word_index = get_random32(0, word_count - 1);

  /* Go to the start of the token we're interested in. */
  while (word_index && s) {
    s = next_token(s, sep);
    word_index--;
  }

  if (!s || !*s)                /* ran off the end of the string */
    return;

  /* Chop off the end, and copy. No length checking needed. */
  r = s;
  if (s && *s)
    (void) split_token(&s, sep);
  safe_str(r, buff, bp);
}

/* ARGSUSED */
FUNCTION(fun_rest)
{
  char *p;
  char sep;

  if (!*args[0])
    return;

  if (!delim_check(buff, bp, nargs, args, 2, &sep))
    return;

  p = trim_space_sep(args[0], sep);
  (void) split_token(&p, sep);
  safe_str(p, buff, bp);
}

/* ARGSUSED */
FUNCTION(fun_last)
{
  /* read last word from a string */

  char *p, *r;
  char sep;

  if (!*args[0])
    return;

  if (!delim_check(buff, bp, nargs, args, 2, &sep))
    return;

  p = trim_space_sep(args[0], sep);
  if (!(r = strrchr(p, sep)))
    r = p;
  else
    r++;
  safe_str(r, buff, bp);
}

/* ARGSUSED */
FUNCTION(fun_grab)
{
  /* compares two strings with possible wildcards, returns the
   * word matched. Based on the 2.2 version of this function.
   */

  char *r, *s, sep;

  if (!delim_check(buff, bp, nargs, args, 3, &sep))
    return;

  /* Walk the wordstring, until we find the word we want. */
  s = trim_space_sep(args[0], sep);
  do {
    r = split_token(&s, sep);
    if (quick_wild(args[1], r)) {
      safe_str(r, buff, bp);
      return;
    }
  } while (s);
}

/* ARGSUSED */
FUNCTION(fun_namegraball)
{
  /* Given a list of dbrefs and a string, it matches the
   * name of the dbrefs against the string.
   * grabnameall(#1 #2 #3,god) -> #1
   */

  char *r, *s, sep;
  dbref victim;
  dbref absolute;
  int first = 1;

  if (!delim_check(buff, bp, nargs, args, 3, &sep))
    return;

  absolute = parse_objid(args[1]);
  if (!RealGoodObject(absolute))
    absolute = NOTHING;

  if (*args[1]) {
    s = trim_space_sep(args[0], sep);
    do {
      r = split_token(&s, sep);
      victim = parse_objid(r);
      if (!RealGoodObject(victim))
        continue;               /* Don't bother with garbage */
      if (!(string_match(Name(victim), args[1]) || (absolute == victim)))
        continue;
      if (!can_interact(victim, executor, INTERACT_MATCH, pe_info))
        continue;
      /* It matches, and is interact-able */
      if (!first)
        safe_chr(sep, buff, bp);
      safe_str(r, buff, bp);
      first = 0;
    } while (s);
  } else {
    /* Pull out all good objects (those that _have_ names) */
    s = trim_space_sep(args[0], sep);
    do {
      r = split_token(&s, sep);
      victim = parse_objid(r);
      if (!RealGoodObject(victim))
        continue;               /* Don't bother with garbage */
      if (!can_interact(victim, executor, INTERACT_MATCH, pe_info))
        continue;
      /* It's real, and is interact-able */
      if (!first)
        safe_chr(sep, buff, bp);
      safe_str(r, buff, bp);
      first = 0;
    } while (s);
  }
}

/* ARGSUSED */
FUNCTION(fun_namegrab)
{
  /* Given a list of dbrefs and a string, it matches the
   * name of the dbrefs against the string.
   */

  char *r, *s, sep;
  dbref victim;
  dbref absolute;
  char *exact_res, *res;

  exact_res = res = NULL;

  if (!delim_check(buff, bp, nargs, args, 3, &sep))
    return;

  absolute = parse_objid(args[1]);
  if (!RealGoodObject(absolute))
    absolute = NOTHING;

  /* Walk the wordstring, until we find the word we want. */
  s = trim_space_sep(args[0], sep);
  do {
    r = split_token(&s, sep);
    victim = parse_objid(r);
    if (!RealGoodObject(victim))
      continue;                 /* Don't bother with garbage */
    /* Dbref match has top priority */
    if ((absolute == victim)
        && can_interact(victim, executor, INTERACT_MATCH, pe_info)) {
      safe_str(r, buff, bp);
      return;
    }
    /* Exact match has second priority */
    if (!exact_res && !strcasecmp(Name(victim), args[1]) &&
        can_interact(victim, executor, INTERACT_MATCH, pe_info)) {
      exact_res = r;
    }
    /* Non-exact match. */
    if (!res && string_match(Name(victim), args[1]) &&
        can_interact(victim, executor, INTERACT_MATCH, pe_info)) {
      res = r;
    }
  } while (s);
  if (exact_res)
    safe_str(exact_res, buff, bp);
  else if (res)
    safe_str(res, buff, bp);
}

/* ARGSUSED */
FUNCTION(fun_match)
{
  /* compares two strings with possible wildcards, returns the
   * word position of the match. Based on the 2.0 version of this
   * function.
   */

  char *s, *r;
  char sep;
  int wcount = 1;

  if (!delim_check(buff, bp, nargs, args, 3, &sep))
    return;

  /* Walk the wordstring, until we find the word we want. */
  s = trim_space_sep(args[0], sep);
  do {
    r = split_token(&s, sep);
    if (quick_wild(args[1], r)) {
      safe_integer(wcount, buff, bp);
      return;
    }
    wcount++;
  } while (s);
  safe_chr('0', buff, bp);
}

/* ARGSUSED */
FUNCTION(fun_wordpos)
{
  int charpos, i;
  char *cp, *tp, *xp;
  char sep;

  if (!is_integer(args[1])) {
    safe_str(T(e_int), buff, bp);
    return;
  }
  charpos = parse_integer(args[1]);
  cp = args[0];
  if (!delim_check(buff, bp, nargs, args, 3, &sep))
    return;

  if ((charpos <= 0) || ((size_t) charpos > strlen(cp))) {
    safe_str("#-1", buff, bp);
    return;
  }
  tp = cp + charpos - 1;
  cp = trim_space_sep(cp, sep);
  xp = split_token(&cp, sep);
  for (i = 1; xp; i++) {
    if (tp < (xp + strlen(xp)))
      break;
    xp = split_token(&cp, sep);
  }
  safe_integer(i, buff, bp);
}

/* ARGSUSED */
FUNCTION(fun_extract)
{
  char sep = ' ';
  int start = 1, len = 1;
  char *s, *r;

  s = args[0];

  if (nargs > 1) {
    if (!is_integer(args[1])) {
      safe_str(T(e_ints), buff, bp);
      return;
    }
    start = parse_integer(args[1]);
  }
  if (nargs > 2) {
    if (!is_integer(args[2])) {
      safe_str(T(e_ints), buff, bp);
      return;
    }
    len = parse_integer(args[2]);
  }
  if ((nargs > 3) && (!delim_check(buff, bp, nargs, args, 4, &sep)))
    return;

  if ((start < 1) || (len < 1))
    return;

  /* Go to the start of the token we're interested in. */
  start--;
  s = trim_space_sep(s, sep);
  while (start && s) {
    s = next_token(s, sep);
    start--;
  }

  if (!s || !*s)                /* ran off the end of the string */
    return;

  /* Find the end of the string that we want. */
  r = s;
  len--;
  while (len && s) {
    s = next_token(s, sep);
    len--;
  }

  /* Chop off the end, and copy. No length checking needed. */
  if (s && *s)
    (void) split_token(&s, sep);
  safe_str(r, buff, bp);
}

/* ARGSUSED */
FUNCTION(fun_cat)
{
  int i;

  safe_strl(args[0], arglens[0], buff, bp);
  for (i = 1; i < nargs; i++) {
    safe_chr(' ', buff, bp);
    safe_strl(args[i], arglens[i], buff, bp);
  }
}

/* ARGSUSED */
FUNCTION(fun_remove)
{
  char sep;
  char **list, **rem;
  int list_counter, rem_counter, list_total, rem_total;
  int skip[MAX_SORTSIZE];
  int first = 1;

  /* zap word from string */

  if (!delim_check(buff, bp, nargs, args, 3, &sep))
    return;

  list = GC_MALLOC(MAX_SORTSIZE * sizeof(char *));
  rem = GC_MALLOC(MAX_SORTSIZE * sizeof(char *));

  list_total = list2arr_ansi(list, MAX_SORTSIZE, args[0], sep, 1);
  rem_total = list2arr_ansi(rem, MAX_SORTSIZE, args[1], sep, 1);

  memset(skip, 0, sizeof(int) * MAX_SORTSIZE);

  for (rem_counter = 0; rem_counter < rem_total; rem_counter++) {
    for (list_counter = 0; list_counter < list_total; list_counter++) {
      if (!skip[list_counter]
          && !ansi_strcmp(rem[rem_counter], list[list_counter])) {
        skip[list_counter] = 1;
        break;
      }
    }
  }

  for (list_counter = 0; list_counter < list_total; list_counter++) {
    if (skip[list_counter])
      continue;
    if (first)
      first = 0;
    else
      safe_chr(sep, buff, bp);
    safe_str(list[list_counter], buff, bp);
  }
}

/* ARGSUSED */
FUNCTION(fun_items)
{
  /* the equivalent of WORDS for an arbitrary separator */
  /* This differs from WORDS in its treatment of the space
   * separator.
   */

  char *s = args[0];
  char c = *args[1];
  int count = 1;

  if (c == '\0')
    c = ' ';

  while ((s = strchr(s, c))) {
    count++;
    s++;
  }

  safe_integer(count, buff, bp);
}

/* ARGSUSED */
FUNCTION(fun_element)
{
  /* the equivalent of MEMBER for an arbitrary separator */
  /* This differs from MEMBER in its use of quick_wild()
   * instead of strcmp().
   */

  char *s, *t;
  char c;
  int el;

  c = *args[2];

  if (c == '\0')
    c = ' ';
  if (strchr(args[1], c)) {
    safe_str(T("#-1 CAN ONLY TEST ONE ELEMENT"), buff, bp);
    return;
  }
  s = args[0];
  el = 1;

  do {
    t = s;
    s = seek_char(t, c);
    if (*s)
      *s++ = '\0';
    if (quick_wild(args[1], t)) {
      safe_integer(el, buff, bp);
      return;
    }
    el++;
  } while (*s);

  safe_chr('0', buff, bp);      /* no match */
}

/* ARGSUSED */
FUNCTION(fun_index)
{
  /* more or less the equivalent of EXTRACT for an arbitrary separator */
  /* This differs from EXTRACT in its handling of space separators. */

  int start, end;
  char c;
  char *s, *p;

  if (!is_integer(args[2]) || !is_integer(args[3])) {
    safe_str(T(e_ints), buff, bp);
    return;
  }
  s = args[0];
  c = *args[1];
  if (!c)
    c = ' ';

  start = parse_integer(args[2]);
  end = parse_integer(args[3]);

  if ((start < 1) || (end < 1) || (*s == '\0'))
    return;

  /* move s to the start of the item we want */
  while (--start) {
    if (!(s = strchr(s, c)))
      return;
    s++;
  }

  /* skip just spaces, not tabs or newlines, since people may MUSHcode things
   * like "%r%tPolgara %r%tDurnik %r%tJavelin"
   */
  while (*s == ' ')
    s++;
  if (!*s)
    return;

  /* now figure out where to end the string */
  p = s + 1;
  /* we may already be pointing to a separator */
  if (*s == c)
    end--;
  while (end--)
    if (!(p = strchr(p, c)))
      break;
    else
      p++;

  if (p)
    p--;
  else
    p = s + strlen(s);

  /* trim trailing spaces (just true spaces) */
  while ((p > s) && (p[-1] == ' '))
    p--;
  *p = '\0';

  safe_str(s, buff, bp);
}

/** Functions that operate on items - delete, replace, insert.
 * \param buff return buffer.
 * \param bp pointer to insertion point in buff.
 * \param str original string.
 * \param num string containing the element number to operate on.
 * \param word string to insert/delete/replace.
 * \param sep separator string.
 * \param flag operation to perform: IF_DELETE, IF_REPLACE, IF_INSERT
 */
static void
do_itemfuns(char *buff, char **bp, char *str, char *num, char *word,
            char *sep, enum itemfun_op flag)
{
  char c;
  int el, count, len = -1;
  char *sptr, *eptr;

  if (!is_integer(num)) {
    safe_str(T(e_int), buff, bp);
    return;
  }
  el = parse_integer(num);

  /* figure out the separator character */
  if (sep && *sep)
    c = *sep;
  else
    c = ' ';

  /* we can't remove anything before the first position */
  if ((el < 1 && flag != IF_INSERT) || el == 0) {
    safe_str(str, buff, bp);
    return;
  }
  if (el < 0) {
    sptr = str + strlen(str);
    eptr = sptr;
  } else {
    sptr = str;
    eptr = strchr(sptr, c);
  }
  count = 1;

  /* go to the correct item in the string */
  if (el < 0) {                 /* if using insert() with a negative insertion param */
    /* count keeps track of the number of words from the right
     * of the string.  When count equals the correct position, then
     * sptr will point to the count'th word from the right, or
     * a null string if the  word being added will be at the end of
     * the string.
     * eptr is just a helper.  */
    for (len = strlen(str); len >= 0 && count < abs(el); len--, eptr--) {
      if (*eptr == c)
        count++;
      if (count == abs(el)) {
        sptr = eptr + 1;
        break;
      }
    }
  } else {
    /* Loop invariant: if sptr and eptr are not NULL, eptr points to
     * the count'th instance of c in str, and sptr is the beginning of
     * the count'th item. */
    while (eptr && (count < el)) {
      sptr = eptr + 1;
      eptr = strchr(sptr, c);
      count++;
    }
  }

  if ((!eptr || len < 0) && (count < abs(el))) {
    /* we've run off the end of the string without finding anything */
    safe_str(str, buff, bp);
    return;
  }
  /* now find the end of that element */
  if ((el < 0 && *eptr) || (el > 0 && sptr != str))
    sptr[-1] = '\0';

  switch (flag) {
  case IF_DELETE:
    /* deletion */
    if (!eptr) {                /* last element in the string */
      if (el != 1)
        safe_str(str, buff, bp);
    } else if (sptr == str) {   /* first element in the string */
      eptr++;                   /* chop leading separator */
      safe_str(eptr, buff, bp);
    } else {
      safe_str(str, buff, bp);
      safe_str(eptr, buff, bp);
    }
    break;
  case IF_REPLACE:
    /* replacing */
    if (!eptr) {                /* last element in string */
      if (el != 1) {
        safe_str(str, buff, bp);
        safe_chr(c, buff, bp);
      }
      safe_str(word, buff, bp);
    } else if (sptr == str) {   /* first element in string */
      safe_str(word, buff, bp);
      safe_str(eptr, buff, bp);
    } else {
      safe_str(str, buff, bp);
      safe_chr(c, buff, bp);
      safe_str(word, buff, bp);
      safe_str(eptr, buff, bp);
    }
    break;
  case IF_INSERT:
    /* insertion */
    if (sptr == str) {          /* first element in string */
      safe_str(word, buff, bp);
      safe_chr(c, buff, bp);
      safe_str(str, buff, bp);
    } else {
      safe_str(str, buff, bp);
      safe_chr(c, buff, bp);
      safe_str(word, buff, bp);
      if (sptr && *sptr) {      /* Don't add an osep to the end of the list */
        safe_chr(c, buff, bp);
        safe_str(sptr, buff, bp);
      }
    }
    break;
  }
}


/* ARGSUSED */
FUNCTION(fun_ldelete)
{
  /* delete a word at given positions of a list */

  /* Given a list and a list of numbers, delete the corresponding
   * elements of the list. elements(ack bar eep foof yay,2 4) = bar foof
   * A separator for the first list is allowed.
   * This code modified slightly from 'elements'
   */
  int nwords, cur;
  char **ptrs;
  char *wordlist;
  int first = 0;
  char *s, *r, sep;
  char *osep, osepd[2] = { '\0', '\0' };

  if (!delim_check(buff, bp, nargs, args, 3, &sep))
    return;

  if (nargs == 4)
    osep = args[3];
  else {
    osepd[0] = sep;
    osep = osepd;
  }

  ptrs = GC_MALLOC(MAX_SORTSIZE * sizeof(char *));
  wordlist = alloc_buf();
  if (!ptrs || !wordlist)
    mush_panic("Unable to allocate memory in fun_elements");

  /* Turn the first list into an array. */
  strcpy(wordlist, args[0]);
  nwords = list2arr_ansi(ptrs, MAX_SORTSIZE, wordlist, sep, 1);

  s = trim_space_sep(args[1], ' ');

  /* Go through the second list, grabbing the numbers and finding the
   * corresponding elements.
   */
  do {
    r = split_token(&s, ' ');
    cur = atoi(r) - 1;
    if ((cur >= 0) && (cur < nwords)) {
      ptrs[cur] = NULL;
    }
  } while (s);
  for (cur = 0; cur < nwords; cur++) {
    if (ptrs[cur]) {
      if (first)
        safe_str(osep, buff, bp);
      else
        first = 1;
      safe_str(ptrs[cur], buff, bp);
    }
  }
}

/* ARGSUSED */
FUNCTION(fun_replace)
{
  /* replace a word at position X of a list */

  do_itemfuns(buff, bp, args[0], args[1], args[2], args[3], IF_REPLACE);
}

/* ARGSUSED */
FUNCTION(fun_insert)
{
  /* insert a word at position X of a list */

  do_itemfuns(buff, bp, args[0], args[1], args[2], args[3], IF_INSERT);
}

/* ARGSUSED */
FUNCTION(fun_member)
{
  char *s, *t;
  char sep;
  int el;

  if (!delim_check(buff, bp, nargs, args, 3, &sep))
    return;

  if (strchr(args[1], sep)) {
    safe_str(T("#-1 CAN ONLY TEST ONE ELEMENT"), buff, bp);
    return;
  }

  s = trim_space_sep(args[0], sep);
  el = 1;

  do {
    t = split_token(&s, sep);
    if (!strcmp(args[1], t)) {
      safe_integer(el, buff, bp);
      return;
    }
    el++;
  } while (s);

  safe_chr('0', buff, bp);      /* not found */
}

/* ARGSUSED */
FUNCTION(fun_before)
{
  const char *p, *q;
  ansi_string *as;
  size_t len;
  p = remove_markup(args[1], &len);

  if (!*p)
    p = " ";
  as = parse_ansi_string(args[0]);
  q = strstr(as->text, p);
  if (q) {
    safe_ansi_string(as, 0, q - as->text, buff, bp);
  } else {
    safe_strl(args[0], arglens[0], buff, bp);
  }
  free_ansi_string(as);
}

/* ARGSUSED */
FUNCTION(fun_after)
{
  ansi_string *as;
  char *p, *delim;
  size_t len, count;
  size_t start;

  if (!*args[1]) {
    args[1][0] = ' ';
    args[1][1] = '\0';
    arglens[1] = 1;
  }
  delim = remove_markup(args[1], &len);
  len--;
  as = parse_ansi_string(args[0]);

  p = strstr(as->text, delim);
  if (p) {
    start = p - as->text + len;
    count = as->len - start;
    safe_ansi_string(as, start, count, buff, bp);
  }
  free_ansi_string(as);
}

/* ARGSUSED */
FUNCTION(fun_revwords)
{
  char **words;
  int count;
  char sep;
  char *osep, osepd[2] = { '\0', '\0' };

  if (!delim_check(buff, bp, nargs, args, 2, &sep))
    return;

  if (nargs == 3)
    osep = args[2];
  else {
    osepd[0] = sep;
    osep = osepd;
  }

  words = GC_MALLOC(BUFFER_LEN * sizeof(char *));

  count = list2arr_ansi(words, BUFFER_LEN, args[0], sep, 1);
  if (count == 0)
    return;

  safe_str(words[--count], buff, bp);
  while (count) {
    safe_str(osep, buff, bp);
    safe_str(words[--count], buff, bp);
  }
}

/* ARGSUSED */
FUNCTION(fun_words)
{
  char sep;

  if (!delim_check(buff, bp, nargs, args, 2, &sep))
    return;
  safe_integer(do_wordcount(trim_space_sep(args[0], sep), sep), buff, bp);
}

/* ARGSUSED */
FUNCTION(fun_splice)
{
  /* like MERGE(), but does it for a word */
  char **orig;
  char **repl;
  char haystack[BUFFER_LEN];
  int ocount, rcount;
  int i;
  char sep;
  char osep[2];

  if (!delim_check(buff, bp, nargs, args, 4, &sep))
    return;

  osep[0] = sep;
  osep[1] = '\0';

  orig = GC_MALLOC(MAX_SORTSIZE * sizeof(char *));
  repl = GC_MALLOC(MAX_SORTSIZE * sizeof(char *));
  /* Turn them into lists */
  ocount = list2arr(orig, MAX_SORTSIZE, args[0], sep, 1);
  rcount = list2arr(repl, MAX_SORTSIZE, args[1], sep, 1);

  strncpy(haystack, remove_markup(args[2], NULL), BUFFER_LEN);
  if (!*haystack) {
    safe_str(T("#-1 NEED A WORD"), buff, bp);
    return;
  }
  if (do_wordcount(haystack, sep) != 1) {
    safe_str(T("#-1 TOO MANY WORDS"), buff, bp);
    return;
  }

  if (ocount != rcount) {
    safe_str(T("#-1 NUMBER OF WORDS MUST BE EQUAL"), buff, bp);
    return;
  }

  for (i = 0; i < ocount; i++) {
    if (!ansi_strcmp(orig[i], haystack)) {
      orig[i] = repl[i];
    }
  }

  arr2list(orig, ocount, buff, bp, osep);
}

/* ARGSUSED */
FUNCTION(fun_iter)
{
  /* Based on the TinyMUSH 2.0 code for this function. Please note that
   * arguments to this function are passed _unparsed_.
   */
  /* Actually, this code has changed so much that the above comment
   * isn't really true anymore. - Talek, 18 Oct 2000
   */
  char **ptrs = NULL;
  int nptrs, i;

  char sep;
  char *outsep, *list;
  char *tbuf2, *lp;
  char const *sp;
  int funccount, per;
  const char *replace[2];
  PE_REGS *pe_regs;

  if (nargs >= 3) {
    /* We have a delimiter. We've got to parse the third arg in place */
    char insep[BUFFER_LEN];
    char *isep = insep;
    const char *arg3 = args[2];
    if (process_expression(insep, &isep, &arg3, executor, caller, enactor,
                           eflags, PT_DEFAULT, pe_info))
      return;
    *isep = '\0';
    strcpy(args[2], insep);
  }
  if (!delim_check(buff, bp, nargs, args, 3, &sep))
    return;

  outsep = alloc_buf();
  list = alloc_buf();

  if (!outsep || !list) {
    mush_panic("Unable to allocate memory in fun_iter");
  }
  if (nargs < 4) {
    strcpy(outsep, " ");
  } else {
    const char *arg4 = args[3];
    char *osep = outsep;
    process_expression(outsep, &osep, &arg4, executor, caller, enactor,
                       eflags, PT_DEFAULT, pe_info);
    *osep = '\0';
  }
  lp = list;
  sp = args[0];
  per = process_expression(list, &lp, &sp, executor, caller, enactor,
                           eflags, PT_DEFAULT, pe_info);
  *lp = '\0';
  lp = trim_space_sep(list, sep);
  if (per || !*lp) {
    return;
  }

  /* Split lp up into an ansi-safe list */
  ptrs = GC_MALLOC(MAX_SORTSIZE * sizeof(char *));
  nptrs = list2arr_ansi(ptrs, MAX_SORTSIZE, lp, sep, 1);

  funccount = pe_info->fun_invocations;

  pe_regs = pe_regs_localize(pe_info, PE_REGS_ITER, "fun_iter");
  for (i = 0; i < nptrs; i++) {
    if (i > 0) {
      safe_str(outsep, buff, bp);
    }
    pe_regs_set(pe_regs, PE_REGS_ITER, "t0", ptrs[i]);
    pe_regs_set_int(pe_regs, PE_REGS_ITER, "n0", i + 1);
    replace[0] = ptrs[i];
    replace[1] = unparse_integer(i + 1);

    tbuf2 = replace_string2(standard_tokens, replace, args[1]);
    sp = tbuf2;
    if (process_expression(buff, bp, &sp, executor, caller, enactor,
                           eflags, PT_DEFAULT, pe_info)) {
      break;
    }
    if (*bp == (buff + BUFFER_LEN - 1) && pe_info->fun_invocations == funccount) {
      break;
    }
    funccount = pe_info->fun_invocations;
    if (pe_regs->flags & PE_REGS_IBREAK) {
      break;
    }
  }
  pe_regs_restore(pe_info, pe_regs);
  pe_regs_free(pe_regs);
}

/* ARGSUSED */
FUNCTION(fun_ibreak)
{
  int i = 1;
  PE_REGS *pe_regs;
  int maxlev = PE_Get_Ilev(pe_info);

  /* maxlev is 0-based. ibreak is 1-based. Blah */
  maxlev += 1;

  if (nargs && args[0] && *args[0]) {
    if (!is_strict_integer(args[0])) {
      safe_str(T(e_int), buff, bp);
      return;
    }
    i = parse_integer(args[0]);
  }

  if (i == 0)
    return;

  if (i < 0 || i > maxlev) {
    safe_str(T(e_range), buff, bp);
    return;
  }

  for (pe_regs = pe_info->regvals; i > 0 && pe_regs; pe_regs = pe_regs->prev) {
    if (pe_regs->flags & PE_REGS_ITER) {
      pe_regs->flags |= PE_REGS_IBREAK;
      i--;
    }
    if (pe_regs->flags & PE_REGS_NEWATTR) {
      break;
    }
  }
}

/* ARGSUSED */
FUNCTION(fun_ilev)
{
  safe_integer(PE_Get_Ilev(pe_info), buff, bp);
}

/* ARGSUSED */
FUNCTION(fun_itext)
{
  int i;
  int maxlev = PE_Get_Ilev(pe_info);

  if (!strcasecmp(args[0], "l")) {
    i = maxlev;
  } else {
    if (!is_strict_integer(args[0])) {
      safe_str(T(e_int), buff, bp);
      return;
    }
    i = parse_integer(args[0]);
  }

  if (i < 0 || i > maxlev) {
    safe_str(T(e_argrange), buff, bp);
    return;
  }

  safe_str(PE_Get_Itext(pe_info, i), buff, bp);
}

/* ARGSUSED */
FUNCTION(fun_inum)
{
  int i;
  int maxlev = PE_Get_Ilev(pe_info);

  if (!strcasecmp(args[0], "l")) {
    i = maxlev;
  } else {
    if (!is_strict_integer(args[0])) {
      safe_str(T(e_int), buff, bp);
      return;
    }
    i = parse_integer(args[0]);
  }

  if (i < 0 || i > maxlev) {
    safe_str(T(e_argrange), buff, bp);
    return;
  }

  safe_integer(PE_Get_Inum(pe_info, i), buff, bp);
}

/* ARGSUSED */
FUNCTION(fun_step)
{
  /* Like map, but passes up to 10 elements from the list at a time in %0-%9
   * If the attribute is not found, null is returned, NOT an error.
   * This function takes delimiters.
   */

  char *lp;
  char sep;
  int n;
  int step;
  char *osep, osepd[2] = { '\0', '\0' };
  PE_REGS *pe_regs;
  ufun_attrib ufun;
  char rbuff[BUFFER_LEN];
  char **ptrs = NULL;
  int nptrs, i;

  if (!is_integer(args[2])) {
    safe_str(T(e_int), buff, bp);
    return;
  }

  step = parse_integer(args[2]);

  if (step < 1 || step > 10) {
    safe_str(T("#-1 STEP OUT OF RANGE"), buff, bp);
    return;
  }

  if (!delim_check(buff, bp, nargs, args, 4, &sep))
    return;

  if (nargs == 5)
    osep = args[4];
  else {
    osepd[0] = sep;
    osep = osepd;
  }

  lp = trim_space_sep(args[1], sep);
  if (!*lp)
    return;

  /* find our object and attribute */
  if (!fetch_ufun_attrib(args[0], executor, &ufun, UFUN_DEFAULT))
    return;

  /* Split lp up into an ansi-safe list */
  ptrs = GC_MALLOC(MAX_SORTSIZE * sizeof(char *));
  nptrs = list2arr_ansi(ptrs, MAX_SORTSIZE, lp, sep, 1);

  /* Step through the list. */
  pe_regs = pe_regs_create(PE_REGS_ARG, "fun_step");
  for (i = 0; i < nptrs;) {
    for (n = 0; n < step; n++) {
      if (i < nptrs) {
        pe_regs_setenv_nocopy(pe_regs, n, ptrs[i++]);
      } else {
        pe_regs_setenv_nocopy(pe_regs, n, "");
      }
    }
    if (call_ufun(&ufun, rbuff, executor, enactor, pe_info, pe_regs)) {
      goto exitsequence;
    }
    if (i > step) {
      /* At least second loop */
      safe_str(osep, buff, bp);
    }
    safe_str(rbuff, buff, bp);
  }

exitsequence:
  pe_regs_free(pe_regs);
}

/* ARGSUSED */
FUNCTION(fun_map)
{
  /* Like iter(), but calls an attribute with list elements as %0 instead.
   * If the attribute is not found, null is returned, NOT an error.
   * This function takes delimiters.
   */

  ufun_attrib ufun;
  char *lp;
  PE_REGS *pe_regs;
  char sep;
  int funccount;
  char place[16];
  char *osep, osepd[2] = { '\0', '\0' };
  char rbuff[BUFFER_LEN];
  char **ptrs = NULL;
  int nptrs, i;

  if (!delim_check(buff, bp, nargs, args, 3, &sep))
    return;

  osepd[0] = sep;
  osep = (nargs >= 4) ? args[3] : osepd;

  lp = trim_space_sep(args[1], sep);
  if (!*lp)
    return;

  if (!fetch_ufun_attrib(args[0], executor, &ufun, UFUN_DEFAULT))
    return;

  strcpy(place, "1");

  ptrs = GC_MALLOC(MAX_SORTSIZE * sizeof(char *));
  nptrs = list2arr_ansi(ptrs, MAX_SORTSIZE, lp, sep, 1);

  /* Build our %0 args */
  pe_regs = pe_regs_create(PE_REGS_ARG, "fun_map");
  pe_regs_setenv_nocopy(pe_regs, 1, place);
  for (i = 0; i < nptrs; i++) {
    pe_regs_setenv_nocopy(pe_regs, 0, ptrs[i]);
    snprintf(place, 16, "%d", i + 1);

    funccount = pe_info->fun_invocations;

    if (call_ufun(&ufun, rbuff, executor, enactor, pe_info, pe_regs)) {
      break;
    }

    if (i > 0) {
      safe_str(osep, buff, bp);
    }
    safe_str(rbuff, buff, bp);
    if (*bp >= (buff + BUFFER_LEN - 1)
        && pe_info->fun_invocations == funccount) {
      break;
    }
  }
  pe_regs_free(pe_regs);
}


/* ARGSUSED */
FUNCTION(fun_mix)
{
  /* Like map(), but goes through lists, passing them as %0 and %1.. %9.
   * If the attribute is not found, null is returned, NOT an error.
   * This function takes delimiters.
   */

  ufun_attrib ufun;
  char rbuff[BUFFER_LEN];
  char *lp[10];
  char sep;
  PE_REGS *pe_regs;
  int funccount;
  int n, lists;
  char **ptrs[10] = { NULL };
  int nptrs[10], i, maxi;

  if (nargs > 3) {              /* Last arg must be the delimiter */
    n = nargs;
    lists = nargs - 2;
  } else {
    n = 4;
    lists = 2;
  }

  if (!delim_check(buff, bp, nargs, args, n, &sep))
    return;

  /* find our object and attribute */
  if (!fetch_ufun_attrib(args[0], executor, &ufun, UFUN_DEFAULT))
    return;

  maxi = 0;
  for (n = 0; n < lists; n++) {
    lp[n] = trim_space_sep(args[n + 1], sep);
    if (*lp[n]) {
      ptrs[n] = GC_MALLOC(MAX_SORTSIZE * sizeof(char *));
      nptrs[n] = list2arr_ansi(ptrs[n], MAX_SORTSIZE, lp[n], sep, 1);
    } else {
      ptrs[n] = NULL;
      nptrs[n] = 0;
    }
    if (nptrs[n] > maxi) {
      maxi = nptrs[n];
    }
  }

  pe_regs = pe_regs_create(PE_REGS_ARG, "fun_mix");
  for (i = 0; i < maxi; i++) {
    for (n = 0; n < lists; n++) {
      if (nptrs[n] > i) {
        pe_regs_setenv_nocopy(pe_regs, n, ptrs[n][i]);
      } else {
        pe_regs_setenv_nocopy(pe_regs, n, "");
      }
    }
    funccount = pe_info->fun_invocations;
    if (call_ufun(&ufun, rbuff, executor, enactor, pe_info, pe_regs)) {
      break;
    }
    if (i > 0) {
      safe_chr(sep, buff, bp);
    }
    safe_str(rbuff, buff, bp);
    if (*bp == (buff + BUFFER_LEN - 1)
        && pe_info->fun_invocations == funccount) {
      break;
    }
  }

  pe_regs_free(pe_regs);
}

/* ARGSUSED */
FUNCTION(fun_table)
{
  /* TABLE(list, field_width, line_length, delimiter, output sep)
   * Given a list, produce a table (a column'd list)
   * Optional parameters: field width, line length, delimiter, output sep
   * Number of columns = line length / (field width+1)
   */
  size_t line_length = 78;
  size_t field_width = 10;
  size_t col = 0;
  size_t offset, col_len;
  char sep, osep, *cp, *t;
  char aligntype = '<';
  char *fwidth;
  ansi_string *as;

  if (!delim_check(buff, bp, nargs, args, 5, &osep))
    return;
  if ((nargs == 5) && !*args[4])
    osep = '\0';

  if (!delim_check(buff, bp, nargs, args, 4, &sep))
    return;

  if (nargs > 2) {
    if (!is_integer(args[2])) {
      safe_str(T(e_ints), buff, bp);
      return;
    }
    line_length = parse_integer(args[2]);
    if (line_length < 2)
      line_length = 2;
  }
  if (nargs > 1) {
    fwidth = args[1];
    if ((*fwidth) == '<' || (*fwidth) == '>' || (*fwidth) == '-') {
      aligntype = *(fwidth++);
    }
    if (!is_integer(fwidth)) {
      safe_str(T(e_ints), buff, bp);
      return;
    }
    field_width = parse_integer(fwidth);
    if (field_width < 1)
      field_width = 1;
    if (field_width >= BUFFER_LEN)
      field_width = BUFFER_LEN - 1;
  }
  if (field_width >= line_length)
    field_width = line_length - 1;

  /* Split out each token, truncate/pad it to field_width, and pack
   * it onto the line. When the line would go over line_length,
   * send a return
   */

  as = parse_ansi_string(args[0]);

  cp = trim_space_sep(as->text, sep);
  if (!*cp) {
    free_ansi_string(as);
    return;
  }

  t = split_token(&cp, sep);
  offset = t - &as->text[0];
  col_len = strlen(t);
  if (col_len > field_width)
    col_len = field_width;
  switch (aligntype) {
  case '<':
    safe_ansi_string(as, offset, col_len, buff, bp);
    if (safe_fill(' ', field_width - col_len, buff, bp)) {
      free_ansi_string(as);
      return;
    }
    break;
  case '>':
    safe_fill(' ', field_width - col_len, buff, bp);
    if (safe_ansi_string(as, offset, col_len, buff, bp)) {
      free_ansi_string(as);
      return;
    }
    break;
  case '-':
    safe_fill(' ', (field_width - col_len) / 2, buff, bp);
    safe_ansi_string(as, offset, col_len, buff, bp);
    if (safe_fill(' ', (field_width - col_len + 1) / 2, buff, bp)) {
      free_ansi_string(as);
      return;
    }
    break;
  }
  col = field_width + (osep != '\0');
  while (cp) {
    col += field_width + (osep != '\0');
    if (col > line_length) {
      safe_chr('\n', buff, bp);
      col = field_width + ! !osep;
    } else {
      if (osep)
        safe_chr(osep, buff, bp);
    }
    t = split_token(&cp, sep);
    if (!t)
      break;
    offset = t - &as->text[0];
    col_len = strlen(t);
    if (col_len > field_width)
      col_len = field_width;
    switch (aligntype) {
    case '<':
      safe_ansi_string(as, offset, col_len, buff, bp);
      if (safe_fill(' ', field_width - col_len, buff, bp)) {
        free_ansi_string(as);
        return;
      }
      break;
    case '>':
      safe_fill(' ', field_width - col_len, buff, bp);
      if (safe_ansi_string(as, offset, col_len, buff, bp)) {
        free_ansi_string(as);
        return;
      }
      break;
    case '-':
      safe_fill(' ', (field_width - col_len) / 2, buff, bp);
      safe_ansi_string(as, offset, col_len, buff, bp);
      if (safe_fill(' ', (field_width - col_len + 1) / 2, buff, bp)) {
        free_ansi_string(as);
        return;
      }
      break;
    }
  }
  free_ansi_string(as);
}

/* In the following regexp functions, we use pcre_study to potentially
 * make pcre_exec faster. If pcre_study() can't help, it returns right
 * away, and if it can, the savings in the actual matching are usually
 * worth it.  Ideally, all compiled regexps and their study patterns
 * should be cached somewhere. Especially nice for patterns in the
 * master room. Just need to come up with a good caching algorithm to
 * use. Easiest would be a hashtable that's just cleared every
 * dbck_interval seconds. Except some benchmarking showed that compiling
 * patterns is faster than I thought it'd be, so this is low priority.
 */

/* string, regexp, replacement string. Acts like sed or perl's s///g,
 * with an ig version */
FUNCTION(fun_regreplace)
{
  pcre *re;
  pcre_extra *extra, *study = NULL;
  const char *errptr;
  int subpatterns;
  int offsets[99];
  int erroffset;
  int flags = 0, all = 0, match_offset = 0;
  PE_REGS *pe_regs = NULL;
  int i;
  const char *r, *obp;
  char *start;
  char tbuf[BUFFER_LEN], *tbp;
  char prebuf[BUFFER_LEN];
  char postbuf[BUFFER_LEN], *postp;
  ansi_string *orig = NULL;
  ansi_string *repl = NULL;
  int search;
  int prelen;
  size_t searchlen;
  int funccount;

  if (called_as[strlen(called_as) - 1] == 'I')
    flags = PCRE_CASELESS;

  if (string_prefix(called_as, "REGEDITALL"))
    all = 1;

  /* Build orig */
  postp = postbuf;
  r = args[0];
  if (process_expression(postbuf, &postp, &r, executor, caller, enactor, eflags,
                         PT_DEFAULT, pe_info))
    return;
  *postp = '\0';

  pe_regs = pe_regs_localize(pe_info, PE_REGS_REGEXP, "fun_regreplace");

  /* Ansi-less regedits */
  for (i = 1; i < nargs - 1; i += 2) {
    /* If this string has ANSI, switch to using ansi only */
    if (has_markup(postbuf))
      break;

    memcpy(prebuf, postbuf, BUFFER_LEN);
    prelen = strlen(prebuf);

    postp = postbuf;

    /* Get the needle */
    tbp = tbuf;
    r = args[i];
    if (process_expression(tbuf, &tbp, &r, executor, caller, enactor, eflags,
                           PT_DEFAULT, pe_info))
      goto exit_sequence;
    *tbp = '\0';

    if ((re = pcre_compile(remove_markup(tbuf, &searchlen),
                           flags, &errptr, &erroffset, tables)) == NULL) {
      /* Matching error. */
      safe_str(T("#-1 REGEXP ERROR: "), buff, bp);
      safe_str(errptr, buff, bp);
      goto exit_sequence;
    }
    if (searchlen)
      searchlen--;

    /* If we're doing a lot, study the regexp to make sure it's good */
    if (all) {
      study = pcre_study(re, 0, &errptr);
      if (errptr != NULL) {
        safe_str(T("#-1 REGEXP ERROR: "), buff, bp);
        safe_str(errptr, buff, bp);
        goto exit_sequence;
      }
    }

    if (study) {
      extra = study;
      set_match_limit(extra);
    } else {
      extra = default_match_limit();
    }

    /* Do all the searches and replaces we can */

    start = prebuf;
    subpatterns = pcre_exec(re, extra, prebuf, prelen, 0, 0, offsets, 99);

    /* Match wasn't found... we're done */
    if (subpatterns < 0) {
      safe_str(prebuf, postbuf, &postp);
      continue;
    }

    funccount = pe_info->fun_invocations;

    do {
      /* Copy up to the start of the matched area */
      char tmp = prebuf[offsets[0]];
      prebuf[offsets[0]] = '\0';
      safe_str(start, postbuf, &postp);
      prebuf[offsets[0]] = tmp;

      /* Now copy in the replacement, putting in captured sub-expressions */
      obp = args[i + 1];

      pe_regs_clear(pe_regs);
      pe_regs_set_rx_context(pe_regs, re, offsets, subpatterns, prebuf);

      if (process_expression(postbuf, &postp, &obp, executor, caller, enactor,
                             eflags | PE_DOLLAR, PT_DEFAULT, pe_info)) {
        goto exit_sequence;
      }
      if ((*bp == (buff + BUFFER_LEN - 1))
          && (pe_info->fun_invocations == funccount))
        break;

      funccount = pe_info->fun_invocations;

      start = prebuf + offsets[1];
      match_offset = offsets[1];
      /* Make sure we advance at least 1 char */
      if (offsets[0] == match_offset)
        match_offset++;
    } while (all && match_offset < prelen &&
             (subpatterns = pcre_exec(re, extra, prebuf, prelen,
                                      match_offset, 0, offsets, 99)) >= 0);

    safe_str(start, postbuf, &postp);
    *postp = '\0';

  }

  /* We get to this point if there is ansi in an 'orig' string */
  if (i < nargs - 1) {

    orig = parse_ansi_string(postbuf);

    /* For each search/replace pair, compare them against orig */
    for (; i < nargs - 1; i += 2) {

      /* Get the needle */
      tbp = tbuf;
      r = args[i];
      if (process_expression(tbuf, &tbp, &r, executor, caller, enactor, eflags,
                             PT_DEFAULT, pe_info))
        goto exit_sequence;

      *tbp = '\0';

      if ((re = pcre_compile(remove_markup(tbuf, &searchlen),
                             flags, &errptr, &erroffset, tables)) == NULL) {
        /* Matching error. */
        safe_str(T("#-1 REGEXP ERROR: "), buff, bp);
        safe_str(errptr, buff, bp);
        goto exit_sequence;
      }
      if (searchlen)
        searchlen--;

      /* If we're doing a lot, study the regexp to make sure it's good */
      if (all) {
        study = pcre_study(re, 0, &errptr);
        if (errptr != NULL) {
          safe_str(T("#-1 REGEXP ERROR: "), buff, bp);
          safe_str(errptr, buff, bp);
          goto exit_sequence;
        }
      }
      if (study) {
        extra = study;
        set_match_limit(extra);
      } else
        extra = default_match_limit();

      search = 0;
      /* Do all the searches and replaces we can */
      do {
        subpatterns =
          pcre_exec(re, extra, orig->text, orig->len, search, 0, offsets, 99);
        if (subpatterns >= 0) {
          /* We have a match */
          /* Process the replacement */
          r = args[i + 1];
          pe_regs_clear(pe_regs);
          pe_regs_set_rx_context_ansi(pe_regs, re, offsets, subpatterns, orig);
          tbp = tbuf;
          if (process_expression(tbuf, &tbp, &r, executor, caller, enactor,
                                 eflags | PE_DOLLAR, PT_DEFAULT, pe_info)) {
            goto exit_sequence;
          }
          *tbp = '\0';
          if (offsets[0] >= search) {
            repl = parse_ansi_string(tbuf);

            /* Do the replacement */
            ansi_string_replace(orig, offsets[0], offsets[1] - offsets[0],
                                repl);

            /* Advance search */
            if (search == offsets[1]) {
              search = offsets[0] + repl->len;
              search++;
            } else {
              search = offsets[0] + repl->len;
            }
            /* if (offsets[0] < 1) search++; */

            free_ansi_string(repl);
            if (search >= orig->len)
              break;
          } else {
            break;
          }
        }
      } while (subpatterns >= 0 && all);
    }
    safe_ansi_string(orig, 0, orig->len, buff, bp);
    free_ansi_string(orig);
    orig = NULL;
  } else {
    safe_str(postbuf, buff, bp);
  }

exit_sequence:
  if (orig) {
    free_ansi_string(orig);
  }
  pe_regs_restore(pe_info, pe_regs);
  pe_regs_free(pe_regs);
}

FUNCTION(fun_regmatch)
{
/* ---------------------------------------------------------------------------
 * fun_regmatch Return 0 or 1 depending on whether or not a regular
 * expression matches a string. If a third argument is specified, dump
 * the results of a regexp pattern match into a set of r()-registers.
 *
 * regmatch(string, pattern, list of registers)
 * Registers are by position (old way) or name:register (new way)
 *
 */
  int i, nqregs;
  char *qregs[NUMQ], *holder[NUMQ];
  pcre *re;
  pcre_extra *extra;
  const char *errptr;
  int erroffset;
  int offsets[99];
  int subpatterns;
  char lbuff[BUFFER_LEN], *lbp;
  int flags = 0;
  ansi_string *as;
  char *txt;
  char *needle;
  size_t len;

  if (strcmp(called_as, "REGMATCHI") == 0)
    flags = PCRE_CASELESS;

  needle = remove_markup(args[1], &len);

  as = parse_ansi_string(args[0]);
  txt = as->text;
  if (nargs == 2) {             /* Don't care about saving sub expressions */
    safe_boolean(quick_regexp_match(needle, txt, flags ? 0 : 1), buff, bp);
    free_ansi_string(as);
    return;
  }

  if ((re = pcre_compile(needle, flags, &errptr, &erroffset, tables)) == NULL) {
    /* Matching error. */
    safe_str(T("#-1 REGEXP ERROR: "), buff, bp);
    safe_str(errptr, buff, bp);
    free_ansi_string(as);
    return;
  }
  extra = default_match_limit();

  subpatterns = pcre_exec(re, extra, txt, arglens[0], 0, 0, offsets, 99);
  safe_integer(subpatterns >= 0, buff, bp);

  /* We need to parse the list of registers.  Anything that we don't parse
   * is assumed to be -1.  If we didn't match, or the match went wonky,
   * then set the register to empty.  Otherwise, fill the register with
   * the subexpression.
   */
  if (subpatterns == 0)
    subpatterns = 33;
  nqregs = list2arr(qregs, NUMQ, args[2], ' ', 1);

  /* Initialize every q-register used to '' */
  for (i = 0; i < nqregs; i++) {
    char *regname;
    holder[i] = GC_STRDUP(qregs[i]);
    if ((regname = strchr(holder[i], ':'))) {
      /* subexpr:register */
      *regname++ = '\0';
    } else {
      /* Get subexper by position in list */
      regname = holder[i];
    }

    if (ValidQregName(regname)) {
      PE_Setq(pe_info, regname, "");
    }
  }
  /* Now, only for those that have a pattern, copy text */
  for (i = 0; i < nqregs; i++) {
    char *regname;
    char *named_subpattern = NULL;
    int subpattern = 0;
    if ((regname = strchr(qregs[i], ':'))) {
      /* subexpr:register */
      *regname++ = '\0';
      if (is_strict_integer(qregs[i]))
        subpattern = parse_integer(qregs[i]);
      else
        named_subpattern = qregs[i];
    } else {
      /* Get subexper by position in list */
      subpattern = i;
      regname = qregs[i];
    }

    if (!ValidQregName(regname)) {
      if (regname[0] != '-' || regname[1]) {
        safe_str(T(e_badregname), buff, bp);
      }
      continue;
    }

    if (subpatterns < 0) {
      lbuff[0] = '\0';
    } else if (named_subpattern) {
      lbp = lbuff;
      ansi_pcre_copy_named_substring(re, as, offsets, subpatterns,
                                     named_subpattern, 1, lbuff, &lbp);
      *(lbp) = '\0';
    } else {
      lbp = lbuff;
      ansi_pcre_copy_substring(as, offsets, subpatterns, subpattern, 1,
                               lbuff, &lbp);
      *(lbp) = '\0';
    }
    PE_Setq(pe_info, regname, lbuff);
  }
  free_ansi_string(as);
}

/* Like grab, but with a regexp pattern. This same function handles
 *  regrab(), regraball(), and the case-insenstive versions. */
FUNCTION(fun_regrab)
{
  char *r, *s, *b, sep;
  size_t rlen;
  pcre *re;
  pcre_extra *study;
  const char *errptr;
  int erroffset;
  int offsets[99];
  int flags = 0, all = 0;
  char *osep, osepd[2] = { '\0', '\0' };
  char **ptrs;
  int nptrs, i;
  if (!delim_check(buff, bp, nargs, args, 3, &sep))
    return;

  if (nargs == 4)
    osep = args[3];
  else {
    osepd[0] = sep;
    osep = osepd;
  }

  s = trim_space_sep(args[0], sep);
  b = *bp;

  if (strrchr(called_as, 'I'))
    flags = PCRE_CASELESS;

  if (string_prefix(called_as, "REGRABALL"))
    all = 1;

  if ((re = pcre_compile(args[1], flags, &errptr, &erroffset, tables)) == NULL) {
    /* Matching error. */
    safe_str(T("#-1 REGEXP ERROR: "), buff, bp);
    safe_str(errptr, buff, bp);
    return;
  }

  study = pcre_study(re, 0, &errptr);
  if (errptr != NULL) {
    safe_str(T("#-1 REGEXP ERROR: "), buff, bp);
    safe_str(errptr, buff, bp);
    return;
  }
  if (study)
    set_match_limit(study);
  else
    study = default_match_limit();
  ptrs = GC_MALLOC(MAX_SORTSIZE * sizeof(char *));
  if (!ptrs)
    mush_panic("Unable to allocate memory in fun_regrab");
  nptrs = list2arr_ansi(ptrs, MAX_SORTSIZE, s, sep, 1);
  for (i = 0; i < nptrs; i++) {
    r = remove_markup(ptrs[i], &rlen);
    if (pcre_exec(re, study, r, rlen - 1, 0, 0, offsets, 99) >= 0) {
      if (all && *bp != b)
        safe_str(osep, buff, bp);
      safe_str(ptrs[i], buff, bp);
      if (!all)
        break;
    }
  }
}

FUNCTION(fun_isregexp)
{
  pcre *re;
  int flags = 0;
  const char *errptr;
  int erroffset;

  if (! !(re = pcre_compile(args[0], flags, &errptr, &erroffset, tables))) {
    safe_chr('1', buff, bp);
    return;
  }
  safe_chr('0', buff, bp);
}
