/*
 * Copyright (c) 2001-2003 Sendmail, Inc. and its suppliers.
 *      All rights reserved.
 *
 * By using this file, you agree to the terms and conditions set
 * forth in the LICENSE file which can be found at the top level of
 * the sendmail distribution.
 */

#include <sm/gen.h>
SM_RCSID("@(#)$Sendmail: ldap.c,v 1.44.2.5 2003/12/23 21:21:56 gshapiro Exp $")

#if LDAPMAP
# include <sys/types.h>
# include <errno.h>
# include <setjmp.h>
# include <stdlib.h>
# include <unistd.h>

# include <sm/bitops.h>
# include <sm/clock.h>
# include <sm/conf.h>
# include <sm/debug.h>
# include <sm/errstring.h>
# include <sm/ldap.h>
# include <sm/string.h>
#  ifdef EX_OK
#   undef EX_OK			/* for SVr4.2 SMP */
#  endif /* EX_OK */
# include <sm/sysexits.h>

SM_DEBUG_T SmLDAPTrace = SM_DEBUG_INITIALIZER("sm_trace_ldap",
	"@(#)$Debug: sm_trace_ldap - trace LDAP operations $");

static void	ldaptimeout __P((int));

/*
**  SM_LDAP_CLEAR -- set default values for SM_LDAP_STRUCT
**
**	Parameters:
**		lmap -- pointer to SM_LDAP_STRUCT to clear
**
**	Returns:
**		None.
**
*/

void
sm_ldap_clear(lmap)
	SM_LDAP_STRUCT *lmap;
{
	if (lmap == NULL)
		return;

	lmap->ldap_target = NULL;
	lmap->ldap_port = LDAP_PORT;
#if _FFR_LDAP_URI
	lmap->ldap_uri = false;
#endif /* _FFR_LDAP_URI */
#  if _FFR_LDAP_SETVERSION
	lmap->ldap_version = 0;
#  endif /* _FFR_LDAP_SETVERSION */
	lmap->ldap_deref = LDAP_DEREF_NEVER;
	lmap->ldap_timelimit = LDAP_NO_LIMIT;
	lmap->ldap_sizelimit = LDAP_NO_LIMIT;
# ifdef LDAP_REFERRALS
	lmap->ldap_options = LDAP_OPT_REFERRALS;
# else /* LDAP_REFERRALS */
	lmap->ldap_options = 0;
# endif /* LDAP_REFERRALS */
	lmap->ldap_attrsep = '\0';
	lmap->ldap_binddn = NULL;
	lmap->ldap_secret = NULL;
	lmap->ldap_method = LDAP_AUTH_SIMPLE;
	lmap->ldap_base = NULL;
	lmap->ldap_scope = LDAP_SCOPE_SUBTREE;
	lmap->ldap_attrsonly = LDAPMAP_FALSE;
	lmap->ldap_timeout.tv_sec = 0;
	lmap->ldap_timeout.tv_usec = 0;
	lmap->ldap_ld = NULL;
	lmap->ldap_filter = NULL;
	lmap->ldap_attr[0] = NULL;
#if _FFR_LDAP_RECURSION
	lmap->ldap_attr_type[0] = SM_LDAP_ATTR_NONE;
	lmap->ldap_attr_needobjclass[0] = NULL;
#endif /* _FFR_LDAP_RECURSION */
	lmap->ldap_res = NULL;
	lmap->ldap_next = NULL;
	lmap->ldap_pid = 0;
}

/*
**  SM_LDAP_START -- actually connect to an LDAP server
**
**	Parameters:
**		name -- name of map for debug output.
**		lmap -- the LDAP map being opened.
**
**	Returns:
**		true if connection is successful, false otherwise.
**
**	Side Effects:
**		Populates lmap->ldap_ld.
*/

static jmp_buf	LDAPTimeout;

#define SM_LDAP_SETTIMEOUT(to)						\
do									\
{									\
	if (to != 0)							\
	{								\
		if (setjmp(LDAPTimeout) != 0)				\
		{							\
			errno = ETIMEDOUT;				\
			return false;					\
		}							\
		ev = sm_setevent(to, ldaptimeout, 0);			\
	}								\
} while (0)

#define SM_LDAP_CLEARTIMEOUT()						\
do									\
{									\
	if (ev != NULL)							\
		sm_clrevent(ev);					\
} while (0)

bool
sm_ldap_start(name, lmap)
	char *name;
	SM_LDAP_STRUCT *lmap;
{
	int bind_result;
	int save_errno;
	SM_EVENT *ev = NULL;
	LDAP *ld;

	if (sm_debug_active(&SmLDAPTrace, 2))
		sm_dprintf("ldapmap_start(%s)\n", name == NULL ? "" : name);

	if (sm_debug_active(&SmLDAPTrace, 9))
		sm_dprintf("ldapmap_start(%s, %d)\n",
			   lmap->ldap_target == NULL ? "localhost" : lmap->ldap_target,
			   lmap->ldap_port);

# if USE_LDAP_INIT
#  if _FFR_LDAP_URI
	if (lmap->ldap_uri)
		errno = ldap_initialize(&ld, lmap->ldap_target);
	else
#  endif /* _FFR_LDAP_URI */
		ld = ldap_init(lmap->ldap_target, lmap->ldap_port);
	save_errno = errno;
# else /* USE_LDAP_INIT */
	/*
	**  If using ldap_open(), the actual connection to the server
	**  happens now so we need the timeout here.  For ldap_init(),
	**  the connection happens at bind time.
	*/

	SM_LDAP_SETTIMEOUT(lmap->ldap_timeout.tv_sec);
	ld = ldap_open(lmap->ldap_target, lmap->ldap_port);
	save_errno = errno;

	/* clear the event if it has not sprung */
	SM_LDAP_CLEARTIMEOUT();
# endif /* USE_LDAP_INIT */

	errno = save_errno;
	if (ld == NULL)
		return false;

	sm_ldap_setopts(ld, lmap);

# if USE_LDAP_INIT
	/*
	**  If using ldap_init(), the actual connection to the server
	**  happens at ldap_bind_s() so we need the timeout here.
	*/

	SM_LDAP_SETTIMEOUT(lmap->ldap_timeout.tv_sec);
# endif /* USE_LDAP_INIT */

# ifdef LDAP_AUTH_KRBV4
	if (lmap->ldap_method == LDAP_AUTH_KRBV4 &&
	    lmap->ldap_secret != NULL)
	{
		/*
		**  Need to put ticket in environment here instead of
		**  during parseargs as there may be different tickets
		**  for different LDAP connections.
		*/

		(void) putenv(lmap->ldap_secret);
	}
# endif /* LDAP_AUTH_KRBV4 */

	bind_result = ldap_bind_s(ld, lmap->ldap_binddn,
				  lmap->ldap_secret, lmap->ldap_method);

# if USE_LDAP_INIT
	/* clear the event if it has not sprung */
	SM_LDAP_CLEARTIMEOUT();
# endif /* USE_LDAP_INIT */

	if (bind_result != LDAP_SUCCESS)
	{
		errno = bind_result + E_LDAPBASE;
		return false;
	}

	/* Save PID to make sure only this PID closes the LDAP connection */
	lmap->ldap_pid = getpid();
	lmap->ldap_ld = ld;
	return true;
}

/* ARGSUSED */
static void
ldaptimeout(unused)
	int unused;
{
	/*
	**  NOTE: THIS CAN BE CALLED FROM A SIGNAL HANDLER.  DO NOT ADD
	**	ANYTHING TO THIS ROUTINE UNLESS YOU KNOW WHAT YOU ARE
	**	DOING.
	*/

	errno = ETIMEDOUT;
	longjmp(LDAPTimeout, 1);
}

/*
**  SM_LDAP_SEARCH -- iniate LDAP search
**
**	Initiate an LDAP search, return the msgid.
**	The calling function must collect the results.
**
**	Parameters:
**		lmap -- LDAP map information
**		key -- key to substitute in LDAP filter
**
**	Returns:
**		-1 on failure, msgid on success
**
*/

int
sm_ldap_search(lmap, key)
	SM_LDAP_STRUCT *lmap;
	char *key;
{
	int msgid;
	char *fp, *p, *q;
	char filter[LDAPMAP_MAX_FILTER + 1];

	/* substitute key into filter, perhaps multiple times */
	memset(filter, '\0', sizeof filter);
	fp = filter;
	p = lmap->ldap_filter;
	while ((q = strchr(p, '%')) != NULL)
	{
		if (q[1] == 's')
		{
			(void) sm_snprintf(fp, SPACELEFT(filter, fp),
					   "%.*s%s", (int) (q - p), p, key);
			fp += strlen(fp);
			p = q + 2;
		}
		else if (q[1] == '0')
		{
			char *k = key;

			(void) sm_snprintf(fp, SPACELEFT(filter, fp),
					   "%.*s", (int) (q - p), p);
			fp += strlen(fp);
			p = q + 2;

			/* Properly escape LDAP special characters */
			while (SPACELEFT(filter, fp) > 0 &&
			       *k != '\0')
			{
				if (*k == '*' || *k == '(' ||
				    *k == ')' || *k == '\\')
				{
					(void) sm_strlcat(fp,
						       (*k == '*' ? "\\2A" :
							(*k == '(' ? "\\28" :
							 (*k == ')' ? "\\29" :
							  (*k == '\\' ? "\\5C" :
							   "\00")))),
						SPACELEFT(filter, fp));
					fp += strlen(fp);
					k++;
				}
				else
					*fp++ = *k++;
			}
		}
		else
		{
			(void) sm_snprintf(fp, SPACELEFT(filter, fp),
				"%.*s", (int) (q - p + 1), p);
			p = q + (q[1] == '%' ? 2 : 1);
			fp += strlen(fp);
		}
	}
	(void) sm_strlcpy(fp, p, SPACELEFT(filter, fp));
	if (sm_debug_active(&SmLDAPTrace, 20))
		sm_dprintf("ldap search filter=%s\n", filter);

	lmap->ldap_res = NULL;
	msgid = ldap_search(lmap->ldap_ld, lmap->ldap_base,
			    lmap->ldap_scope, filter,
			    (lmap->ldap_attr[0] == NULL ? NULL :
			     lmap->ldap_attr),
			    lmap->ldap_attrsonly);
	return msgid;
}

# if _FFR_LDAP_RECURSION
/*
**  SM_LDAP_HAS_OBJECTCLASS -- determine if an LDAP entry is part of a
**			       particular objectClass
**
**	Parameters:
**		lmap -- pointer to SM_LDAP_STRUCT in use
**		entry -- current LDAP entry struct
**		ocvalue -- particular objectclass in question.
**			   may be of form (fee|foo|fum) meaning
**			   any entry can be part of either fee,
**			   foo or fum objectclass
**
**	Returns:
**		true if item has that objectClass
*/

static bool
sm_ldap_has_objectclass(lmap, entry, ocvalue)
	SM_LDAP_STRUCT *lmap;
	LDAPMessage *entry;
	char *ocvalue;
{
	char **vals = NULL;
	int i;

	if (ocvalue == NULL)
		return false;

	vals = ldap_get_values(lmap->ldap_ld, entry, "objectClass");
	if (vals == NULL)
		return false;

	for (i = 0; vals[i] != NULL; i++)
	{
		char *p;
		char *q;

		p = q = ocvalue;
		while (*p != '\0')
		{
			while (*p != '\0' && *p != '|')
				p++;

			if ((p - q) == strlen(vals[i]) &&
			    sm_strncasecmp(vals[i], q, p - q) == 0)
			{
				ldap_value_free(vals);
				return true;
			}

			while (*p == '|')
				p++;
			q = p;
		}
	}

	ldap_value_free(vals);
	return false;
}

/*
**  SM_LDAP_RESULTS -- return results from an LDAP lookup in result
**
**	Parameters:
**		lmap -- pointer to SM_LDAP_STRUCT in use
**		msgid -- msgid returned by sm_ldap_search()
**		flags -- flags for the lookup
**		delim -- delimiter for result concatenation
**		rpool -- memory pool for storage
**		result -- return string
**		recurse -- recursion list
**
**	Returns:
**		status (sysexit)
*/

# define SM_LDAP_ERROR_CLEANUP()				\
{								\
	if (lmap->ldap_res != NULL)				\
	{							\
		ldap_msgfree(lmap->ldap_res);			\
		lmap->ldap_res = NULL;				\
	}							\
	(void) ldap_abandon(lmap->ldap_ld, msgid);		\
}

static SM_LDAP_RECURSE_ENTRY *
sm_ldap_add_recurse(top, item, type, rpool)
	SM_LDAP_RECURSE_LIST **top;
	char *item;
	int type;
	SM_RPOOL_T *rpool;
{
	int n;
	int m;
	int p;
	int insertat;
	int moveb;
	int oldsizeb;
	int rc;
	SM_LDAP_RECURSE_ENTRY *newe;
	SM_LDAP_RECURSE_ENTRY **olddata;

	/*
	**  This code will maintain a list of
	**  SM_LDAP_RECURSE_ENTRY structures
	**  in ascending order.
	*/

	if (*top == NULL)
	{
		/* Allocate an initial SM_LDAP_RECURSE_LIST struct */
		*top = sm_rpool_malloc_x(rpool, sizeof **top);
		(*top)->lr_cnt = 0;
		(*top)->lr_size = 0;
		(*top)->lr_data = NULL;
	}

	if ((*top)->lr_cnt >= (*top)->lr_size)
	{
		/* Grow the list of SM_LDAP_RECURSE_ENTRY ptrs */
		olddata = (*top)->lr_data;
		if ((*top)->lr_size == 0)
		{
			oldsizeb = 0;
			(*top)->lr_size = 256;
		}
		else
		{
			oldsizeb = (*top)->lr_size * sizeof *((*top)->lr_data);
			(*top)->lr_size *= 2;
		}
		(*top)->lr_data = sm_rpool_malloc_x(rpool,
						    (*top)->lr_size * sizeof *((*top)->lr_data));
		if (oldsizeb > 0)
			memcpy((*top)->lr_data, olddata, oldsizeb);
	}

	/*
	**  Binary search/insert item:type into list.
	**  Return current entry pointer if already exists.
	*/

	n = 0;
	m = (*top)->lr_cnt - 1;
	if (m < 0)
		insertat = 0;
	else
		insertat = -1;

	while (insertat == -1)
	{
		p = (m + n) / 2;

		rc = sm_strcasecmp(item, (*top)->lr_data[p]->lr_search);
		if (rc == 0)
			rc = type - (*top)->lr_data[p]->lr_type;

		if (rc < 0)
			m = p - 1;
		else if (rc > 0)
			n = p + 1;
		else
			return (*top)->lr_data[p];

		if (m == -1)
			insertat = 0;
		else if (n >= (*top)->lr_cnt)
			insertat = (*top)->lr_cnt;
		else if (m < n)
			insertat = m + 1;
	}

	/*
	** Not found in list, make room
	** at insert point and add it.
	*/

	newe = sm_rpool_malloc_x(rpool, sizeof *newe);
	if (newe != NULL)
	{
		moveb = ((*top)->lr_cnt - insertat) * sizeof *((*top)->lr_data);
		if (moveb > 0)
			memmove(&((*top)->lr_data[insertat + 1]),
				&((*top)->lr_data[insertat]),
				moveb);

		newe->lr_search = sm_rpool_strdup_x(rpool, item);
		newe->lr_type = type;
		newe->lr_done = false;

		((*top)->lr_data)[insertat] = newe;
		(*top)->lr_cnt++;
	}
	return newe;
}

int
sm_ldap_results(lmap, msgid, flags, delim, rpool, result,
		resultln, resultsz, recurse)
	SM_LDAP_STRUCT *lmap;
	int msgid;
	int flags;
	int delim;
	SM_RPOOL_T *rpool;
	char **result;
	int *resultln;
	int *resultsz;
	SM_LDAP_RECURSE_LIST *recurse;
{
	bool toplevel;
	int i;
	int statp;
	int vsize;
	int ret;
	int save_errno;
	char *p;
	SM_LDAP_RECURSE_ENTRY *rl;

	/* Are we the top top level of the search? */
	toplevel = (recurse == NULL);

	/* Get results */
	statp = EX_NOTFOUND;
	while ((ret = ldap_result(lmap->ldap_ld, msgid, 0,
				  (lmap->ldap_timeout.tv_sec == 0 ? NULL :
				   &(lmap->ldap_timeout)),
				  &(lmap->ldap_res))) == LDAP_RES_SEARCH_ENTRY)
	{
		LDAPMessage *entry;

		/* If we don't want multiple values and we have one, break */
		if ((char) delim == '\0' && *result != NULL)
			break;

		/* Cycle through all entries */
		for (entry = ldap_first_entry(lmap->ldap_ld, lmap->ldap_res);
		     entry != NULL;
		     entry = ldap_next_entry(lmap->ldap_ld, lmap->ldap_res))
		{
			BerElement *ber;
			char *attr;
			char **vals = NULL;
			char *dn;

			/*
			**  If matching only and found an entry,
			**  no need to spin through attributes
			*/

			if (bitset(SM_LDAP_MATCHONLY, flags))
			{
				statp = EX_OK;
				continue;
			}

			/* record completed DN's to prevent loops */
			dn = ldap_get_dn(lmap->ldap_ld, entry);
			if (dn == NULL)
			{
				save_errno = sm_ldap_geterrno(lmap->ldap_ld);
				save_errno += E_LDAPBASE;
				SM_LDAP_ERROR_CLEANUP();
				errno = save_errno;
				return EX_TEMPFAIL;
			}

			rl = sm_ldap_add_recurse(&recurse, dn,
						 SM_LDAP_ATTR_DN,
						 rpool);

			if (rl == NULL)
			{
				ldap_memfree(dn);
				SM_LDAP_ERROR_CLEANUP();
				errno = ENOMEM;
				return EX_OSERR;
			}
			else if (rl->lr_done)
			{
				/* already on list, skip it */
				ldap_memfree(dn);
				continue;
			}
			ldap_memfree(dn);

# if !defined(LDAP_VERSION_MAX) && !defined(LDAP_OPT_SIZELIMIT)
			/*
			**  Reset value to prevent lingering
			**  LDAP_DECODING_ERROR due to
			**  OpenLDAP 1.X's hack (see below)
			*/

			lmap->ldap_ld->ld_errno = LDAP_SUCCESS;
# endif /* !defined(LDAP_VERSION_MAX) !defined(LDAP_OPT_SIZELIMIT) */

			for (attr = ldap_first_attribute(lmap->ldap_ld, entry,
							 &ber);
			     attr != NULL;
			     attr = ldap_next_attribute(lmap->ldap_ld, entry,
							ber))
			{
				char *tmp, *vp_tmp;
				int type;
				char *needobjclass = NULL;

				type = SM_LDAP_ATTR_NONE;
				for (i = 0; lmap->ldap_attr[i] != NULL; i++)
				{
					if (sm_strcasecmp(lmap->ldap_attr[i],
							  attr) == 0)
					{
						type = lmap->ldap_attr_type[i];
						needobjclass = lmap->ldap_attr_needobjclass[i];
						break;
					}
				}

				if (bitset(SM_LDAP_USE_ALLATTR, flags) &&
				    type == SM_LDAP_ATTR_NONE)
				{
					/* URL lookups specify attrs to use */
					type = SM_LDAP_ATTR_NORMAL;
					needobjclass = NULL;
				}

				if (type == SM_LDAP_ATTR_NONE)
				{
					/* attribute not requested */
					ldap_memfree(attr);
					SM_LDAP_ERROR_CLEANUP();
					errno = EFAULT;
					return EX_SOFTWARE;
				}

				/*
				**  For recursion on a particular attribute,
				**  we may need to see if this entry is
				**  part of a particular objectclass.
				**  Also, ignore objectClass attribute.
				**  Otherwise we just ignore this attribute.
				*/

				if (type == SM_LDAP_ATTR_OBJCLASS ||
				    (needobjclass != NULL &&
				     !sm_ldap_has_objectclass(lmap, entry,
							      needobjclass)))
				{
					ldap_memfree(attr);
					continue;
				}

				if (lmap->ldap_attrsonly == LDAPMAP_FALSE)
				{
					vals = ldap_get_values(lmap->ldap_ld,
							       entry,
							       attr);
					if (vals == NULL)
					{
						save_errno = sm_ldap_geterrno(lmap->ldap_ld);
						if (save_errno == LDAP_SUCCESS)
						{
							ldap_memfree(attr);
							continue;
						}

						/* Must be an error */
						save_errno += E_LDAPBASE;
						ldap_memfree(attr);
						SM_LDAP_ERROR_CLEANUP();
						errno = save_errno;
						return EX_TEMPFAIL;
					}
				}

				statp = EX_OK;

# if !defined(LDAP_VERSION_MAX) && !defined(LDAP_OPT_SIZELIMIT)
				/*
				**  Reset value to prevent lingering
				**  LDAP_DECODING_ERROR due to
				**  OpenLDAP 1.X's hack (see below)
				*/

				lmap->ldap_ld->ld_errno = LDAP_SUCCESS;
# endif /* !defined(LDAP_VERSION_MAX) !defined(LDAP_OPT_SIZELIMIT) */

				/*
				**  If matching only,
				**  no need to spin through entries
				*/

				if (bitset(SM_LDAP_MATCHONLY, flags))
				{
					if (lmap->ldap_attrsonly == LDAPMAP_FALSE)
						ldap_value_free(vals);
					ldap_memfree(attr);
					continue;
				}

				/*
				**  If we don't want multiple values,
				**  return first found.
				*/

				if ((char) delim == '\0')
				{
					if (*result != NULL)
					{
						/* already have a value */
						break;
					}

					if (bitset(SM_LDAP_SINGLEMATCH,
						   flags) &&
					    *result != NULL)
					{
						/* only wanted one match */
						SM_LDAP_ERROR_CLEANUP();
						errno = ENOENT;
						return EX_NOTFOUND;
					}

					if (lmap->ldap_attrsonly == LDAPMAP_TRUE)
					{
						*result = sm_rpool_strdup_x(rpool,
									    attr);
						ldap_memfree(attr);
						break;
					}

					if (vals[0] == NULL)
					{
						ldap_value_free(vals);
						ldap_memfree(attr);
						continue;
					}

					vsize = strlen(vals[0]) + 1;
					if (lmap->ldap_attrsep != '\0')
						vsize += strlen(attr) + 1;
					*result = sm_rpool_malloc_x(rpool,
								    vsize);
					if (lmap->ldap_attrsep != '\0')
						sm_snprintf(*result, vsize,
							    "%s%c%s",
							    attr,
							    lmap->ldap_attrsep,
							    vals[0]);
					else
						sm_strlcpy(*result, vals[0],
							   vsize);
					ldap_value_free(vals);
					ldap_memfree(attr);
					break;
				}

				/* attributes only */
				if (lmap->ldap_attrsonly == LDAPMAP_TRUE)
				{
					if (*result == NULL)
						*result = sm_rpool_strdup_x(rpool,
									    attr);
					else
					{
						if (bitset(SM_LDAP_SINGLEMATCH,
							   flags) &&
						    *result != NULL)
						{
							/* only wanted one match */
							SM_LDAP_ERROR_CLEANUP();
							errno = ENOENT;
							return EX_NOTFOUND;
						}

						vsize = strlen(*result) +
							strlen(attr) + 2;
						tmp = sm_rpool_malloc_x(rpool,
									vsize);
						(void) sm_snprintf(tmp,
							vsize, "%s%c%s",
							*result, (char) delim,
							attr);
						*result = tmp;
					}
					ldap_memfree(attr);
					continue;
				}

				/*
				**  If there is more than one, munge then
				**  into a map_coldelim separated string.
				**  If we are recursing we may have an entry
				**  with no 'normal' values to put in the
				**  string.
				**  This is not an error.
				*/

				if (type == SM_LDAP_ATTR_NORMAL &&
				    bitset(SM_LDAP_SINGLEMATCH, flags) &&
				    *result != NULL)
				{
					/* only wanted one match */
					SM_LDAP_ERROR_CLEANUP();
					errno = ENOENT;
					return EX_NOTFOUND;
				}

				vsize = 0;
				for (i = 0; vals[i] != NULL; i++)
				{
					if (type == SM_LDAP_ATTR_DN ||
					    type == SM_LDAP_ATTR_FILTER ||
					    type == SM_LDAP_ATTR_URL)
					{
						/* add to recursion */
						if (sm_ldap_add_recurse(&recurse,
									vals[i],
									type,
									rpool) == NULL)
						{
							SM_LDAP_ERROR_CLEANUP();
							errno = ENOMEM;
							return EX_OSERR;
						}
						continue;
					}

					vsize += strlen(vals[i]) + 1;
					if (lmap->ldap_attrsep != '\0')
						vsize += strlen(attr) + 1;
				}

				/*
				**  Create/Append to string any normal
				**  attribute values.  Otherwise, just free
				**  memory and move on to the next
				**  attribute in this entry.
				*/

				if (type == SM_LDAP_ATTR_NORMAL && vsize > 0)
				{
					char *pe;

					/* Grow result string if needed */
					if ((*resultln + vsize) >= *resultsz)
					{
						while ((*resultln + vsize) >= *resultsz)
						{
							if (*resultsz == 0)
								*resultsz = 1024;
							else
								*resultsz *= 2;
						}

						vp_tmp = sm_rpool_malloc_x(rpool, *resultsz);
						*vp_tmp = '\0';

						if (*result != NULL)
							sm_strlcpy(vp_tmp,
								   *result,
								   *resultsz);
						*result = vp_tmp;
					}

					p = *result + *resultln;
					pe = *result + *resultsz;

					for (i = 0; vals[i] != NULL; i++)
					{
						if (*resultln > 0 &&
						    p < pe)
							*p++ = (char) delim;

						if (lmap->ldap_attrsep != '\0')
						{
							p += sm_strlcpy(p, attr,
									pe - p);
							if (p < pe)
								*p++ = lmap->ldap_attrsep;
						}

						p += sm_strlcpy(p, vals[i],
								pe - p);
						*resultln = p - (*result);
						if (p >= pe)
						{
							/* Internal error: buffer too small for LDAP values */
							SM_LDAP_ERROR_CLEANUP();
							errno = ENOMEM;
							return EX_OSERR;
						}
					}
				}

				ldap_value_free(vals);
				ldap_memfree(attr);
			}
			save_errno = sm_ldap_geterrno(lmap->ldap_ld);

			/*
			**  We check save_errno != LDAP_DECODING_ERROR since
			**  OpenLDAP 1.X has a very ugly *undocumented*
			**  hack of returning this error code from
			**  ldap_next_attribute() if the library freed the
			**  ber attribute.  See:
			**  http://www.openldap.org/lists/openldap-devel/9901/msg00064.html
			*/

			if (save_errno != LDAP_SUCCESS &&
			    save_errno != LDAP_DECODING_ERROR)
			{
				/* Must be an error */
				save_errno += E_LDAPBASE;
				SM_LDAP_ERROR_CLEANUP();
				errno = save_errno;
				return EX_TEMPFAIL;
			}

			/* mark this DN as done */
			rl->lr_done = true;

			/* We don't want multiple values and we have one */
			if ((char) delim == '\0' && *result != NULL)
				break;
		}
		save_errno = sm_ldap_geterrno(lmap->ldap_ld);
		if (save_errno != LDAP_SUCCESS &&
		    save_errno != LDAP_DECODING_ERROR)
		{
			/* Must be an error */
			save_errno += E_LDAPBASE;
			SM_LDAP_ERROR_CLEANUP();
			errno = save_errno;
			return EX_TEMPFAIL;
		}
		ldap_msgfree(lmap->ldap_res);
		lmap->ldap_res = NULL;
	}

	if (ret == 0)
		save_errno = ETIMEDOUT;
	else
		save_errno = sm_ldap_geterrno(lmap->ldap_ld);
	if (save_errno != LDAP_SUCCESS)
	{
		statp = EX_TEMPFAIL;
		if (ret != 0)
		{
			switch (save_errno)
			{
#ifdef LDAP_SERVER_DOWN
			  case LDAP_SERVER_DOWN:
#endif /* LDAP_SERVER_DOWN */
			  case LDAP_TIMEOUT:
			  case LDAP_UNAVAILABLE:

				/*
				**  server disappeared,
				**  try reopen on next search
				*/

				statp = EX_RESTART;
				break;
			}
			save_errno += E_LDAPBASE;
		}
		SM_LDAP_ERROR_CLEANUP();
		errno = save_errno;
		return statp;
	}

	if (lmap->ldap_res != NULL)
	{
		ldap_msgfree(lmap->ldap_res);
		lmap->ldap_res = NULL;
	}

	if (toplevel)
	{
		int rlidx;

		/*
		**  Spin through the built-up recurse list at the top
		**  of the recursion.  Since new items are added at the
		**  end of the shared list, we actually only ever get
		**  one level of recursion before things pop back to the
		**  top.  Any items added to the list during that recursion
		**  will be expanded by the top level.
		*/

		for (rlidx = 0; recurse != NULL && rlidx < recurse->lr_cnt; rlidx++)
		{
			int newflags;
			int sid;
			int status;

			rl = recurse->lr_data[rlidx];

			newflags = flags;
			if (rl->lr_done)
			{
				/* already expanded */
				continue;
			}

			if (rl->lr_type == SM_LDAP_ATTR_DN)
			{
				/* do DN search */
				sid = ldap_search(lmap->ldap_ld,
						  rl->lr_search,
						  lmap->ldap_scope,
						  "(objectClass=*)",
						  (lmap->ldap_attr[0] == NULL ?
						   NULL : lmap->ldap_attr),
						  lmap->ldap_attrsonly);
			}
			else if (rl->lr_type == SM_LDAP_ATTR_FILTER)
			{
				/* do new search */
				sid = ldap_search(lmap->ldap_ld,
						  lmap->ldap_base,
						  lmap->ldap_scope,
						  rl->lr_search,
						  (lmap->ldap_attr[0] == NULL ?
						   NULL : lmap->ldap_attr),
						  lmap->ldap_attrsonly);
			}
			else if (rl->lr_type == SM_LDAP_ATTR_URL)
			{
				/* do new URL search */
				sid = ldap_url_search(lmap->ldap_ld,
						      rl->lr_search,
						      lmap->ldap_attrsonly);
				newflags |= SM_LDAP_USE_ALLATTR;
			}
			else
			{
				/* unknown or illegal attribute type */
				errno = EFAULT;
				return EX_SOFTWARE;
			}

			/* Collect results */
			if (sid == -1)
			{
				save_errno = sm_ldap_geterrno(lmap->ldap_ld);
				statp = EX_TEMPFAIL;
				switch (save_errno)
				{
#ifdef LDAP_SERVER_DOWN
				  case LDAP_SERVER_DOWN:
#endif /* LDAP_SERVER_DOWN */
				  case LDAP_TIMEOUT:
				  case LDAP_UNAVAILABLE:

					/*
					**  server disappeared,
					**  try reopen on next search
					*/

					statp = EX_RESTART;
					break;
				}
				errno = save_errno + E_LDAPBASE;
				return statp;
			}

			status = sm_ldap_results(lmap, sid, newflags, delim,
						 rpool, result, resultln,
						 resultsz, recurse);
			save_errno = errno;
			if (status != EX_OK && status != EX_NOTFOUND)
			{
				errno = save_errno;
				return status;
			}

			/* Mark as done */
			rl->lr_done = true;

			/* Reset rlidx as new items may have been added */
			rlidx = -1;
		}
	}
	return statp;
}
#endif /* _FFR_LDAP_RECURSION */

/*
**  SM_LDAP_CLOSE -- close LDAP connection
**
**	Parameters:
**		lmap -- LDAP map information
**
**	Returns:
**		None.
**
*/

void
sm_ldap_close(lmap)
	SM_LDAP_STRUCT *lmap;
{
	if (lmap->ldap_ld == NULL)
		return;

	if (lmap->ldap_pid == getpid())
		ldap_unbind(lmap->ldap_ld);
	lmap->ldap_ld = NULL;
	lmap->ldap_pid = 0;
}

/*
**  SM_LDAP_SETOPTS -- set LDAP options
**
**	Parameters:
**		ld -- LDAP session handle
**		lmap -- LDAP map information
**
**	Returns:
**		None.
**
*/

void
sm_ldap_setopts(ld, lmap)
	LDAP *ld;
	SM_LDAP_STRUCT *lmap;
{
# if USE_LDAP_SET_OPTION
#  if _FFR_LDAP_SETVERSION
	if (lmap->ldap_version != 0)
	{
		ldap_set_option(ld, LDAP_OPT_PROTOCOL_VERSION,
				&lmap->ldap_version);
	}
#  endif /* _FFR_LDAP_SETVERSION */
	ldap_set_option(ld, LDAP_OPT_DEREF, &lmap->ldap_deref);
	if (bitset(LDAP_OPT_REFERRALS, lmap->ldap_options))
		ldap_set_option(ld, LDAP_OPT_REFERRALS, LDAP_OPT_ON);
	else
		ldap_set_option(ld, LDAP_OPT_REFERRALS, LDAP_OPT_OFF);
	ldap_set_option(ld, LDAP_OPT_SIZELIMIT, &lmap->ldap_sizelimit);
	ldap_set_option(ld, LDAP_OPT_TIMELIMIT, &lmap->ldap_timelimit);
#  ifdef LDAP_OPT_RESTART
	ldap_set_option(ld, LDAP_OPT_RESTART, LDAP_OPT_ON);
#  endif /* LDAP_OPT_RESTART */
# else /* USE_LDAP_SET_OPTION */
	/* From here on in we can use ldap internal timelimits */
	ld->ld_deref = lmap->ldap_deref;
	ld->ld_options = lmap->ldap_options;
	ld->ld_sizelimit = lmap->ldap_sizelimit;
	ld->ld_timelimit = lmap->ldap_timelimit;
# endif /* USE_LDAP_SET_OPTION */
}

/*
**  SM_LDAP_GETERRNO -- get ldap errno value
**
**	Parameters:
**		ld -- LDAP session handle
**
**	Returns:
**		LDAP errno.
**
*/

int
sm_ldap_geterrno(ld)
	LDAP *ld;
{
	int err = LDAP_SUCCESS;

# if defined(LDAP_VERSION_MAX) && LDAP_VERSION_MAX >= 3
	(void) ldap_get_option(ld, LDAP_OPT_ERROR_NUMBER, &err);
# else /* defined(LDAP_VERSION_MAX) && LDAP_VERSION_MAX >= 3 */
#  ifdef LDAP_OPT_SIZELIMIT
	err = ldap_get_lderrno(ld, NULL, NULL);
#  else /* LDAP_OPT_SIZELIMIT */
	err = ld->ld_errno;

	/*
	**  Reset value to prevent lingering LDAP_DECODING_ERROR due to
	**  OpenLDAP 1.X's hack (see above)
	*/

	ld->ld_errno = LDAP_SUCCESS;
#  endif /* LDAP_OPT_SIZELIMIT */
# endif /* defined(LDAP_VERSION_MAX) && LDAP_VERSION_MAX >= 3 */
	return err;
}
# endif /* LDAPMAP */
