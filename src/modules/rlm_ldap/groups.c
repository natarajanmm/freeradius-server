/*
 *   This program is is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or (at
 *   your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 * @file groups.c
 * @brief LDAP module group functions.
 *
 * @author Arran Cudbard-Bell (a.cudbardb@freeradius.org)
 *
 * @copyright 2013 Network RADIUS SAS (legal@networkradius.com)
 * @copyright 2013-2015 The FreeRADIUS Server Project.
 */
RCSID("$Id$")

USES_APPLE_DEPRECATED_API

#include <freeradius-devel/util/debug.h>
#include <ctype.h>

#define LOG_PREFIX "rlm_ldap groups"

#include "rlm_ldap.h"

static char const *null_attrs[] = { NULL };

/** Context to use when resolving group membership from the user object.
 *
 */
typedef struct {
	rlm_ldap_t const	*inst;					//!< Module instance.
	fr_value_box_t		*base_dn;				//!< The base DN to search for groups in.
	fr_ldap_thread_trunk_t	*ttrunk;				//!< Trunk on which to perform additional queries.
	fr_pair_list_t		groups;					//!< Temporary list to hold pairs.
	TALLOC_CTX		*list_ctx;				//!< In which to allocate pairs.
	char			*group_name[LDAP_MAX_CACHEABLE + 1];	//!< List of group names which need resolving.
	unsigned int		name_cnt;				//!< How many names need resolving.
	char			*group_dn[LDAP_MAX_CACHEABLE + 1];	//!< List of group DNs which need resolving.
	char			**dn;					//!< Current DN being resolved.
	char const		*attrs[2];				//!< For resolving name from DN.
	fr_ldap_query_t		*query;					//!< Current query performing group resolution.
} ldap_group_userobj_ctx_t;

/** Context to use when looking up group membership using group objects.
 *
 */
typedef struct {
	rlm_ldap_t const	*inst;					//!< Module instance.
	fr_value_box_t		*base_dn;				//!< The base DN to search for groups in.
	fr_ldap_thread_trunk_t	*ttrunk;				//!< Trunk on which to perform additional queries.
	char			filter[LDAP_MAX_FILTER_STR_LEN + 1];	//!< Filter used to search for groups.
	char const		*attrs[2];				//!< For retrieving the group name.
	fr_ldap_query_t		*query;					//!< Current query performing group lookup.
} ldap_group_groupobj_ctx_t;

/** Cancel a pending group lookup query
 *
 */
static void ldap_group_userobj_cancel(UNUSED request_t *request, UNUSED fr_signal_t action, void *uctx)
{
	ldap_group_userobj_ctx_t	*group_ctx = talloc_get_type_abort(uctx, ldap_group_userobj_ctx_t);

	/*
	 *	If the query is not in flight, just return.
	 */
	if (!group_ctx->query || !(group_ctx->query->treq)) return;

	fr_trunk_request_signal_cancel(group_ctx->query->treq);
}

/** Convert multiple group names into a DNs
 *
 * Given an array of group names, builds a filter matching all names, then retrieves all group objects
 * and stores the DN associated with each group object.
 *
 * @param[out] p_result		The result of trying to resolve a group name to a dn.
 * @param[out] priority		Unused
 * @param[in] request		Current request.
 * @param[in] uctx		Group lookup context.
 * @return One of the RLM_MODULE_* values.
 */
static unlang_action_t ldap_group_name2dn_start(rlm_rcode_t *p_result, UNUSED int *priority, request_t *request,
						void *uctx)
{
	ldap_group_userobj_ctx_t	*group_ctx = talloc_get_type_abort(uctx, ldap_group_userobj_ctx_t);
	rlm_ldap_t const		*inst = group_ctx->inst;
	char				**name = group_ctx->group_name;
	char				buffer[LDAP_MAX_GROUP_NAME_LEN + 1];
	char				*filter;

	if (!inst->groupobj_name_attr) {
		REDEBUG("Told to convert group names to DNs but missing 'group.name_attribute' directive");
		RETURN_MODULE_INVALID;
	}
	if (group_ctx->base_dn->type != FR_TYPE_STRING) {
		REDEBUG("Missing group base_dn");
		RETURN_MODULE_INVALID;
	}

	RDEBUG2("Converting group name(s) to group DN(s)");

	/*
	 *	It'll probably only save a few ms in network latency, but it means we can send a query
	 *	for the entire group list at once.
	 */
	filter = talloc_typed_asprintf(group_ctx, "%s%s%s",
				 inst->groupobj_filter ? "(&" : "",
				 inst->groupobj_filter ? inst->groupobj_filter : "",
				 group_ctx->group_name[0] && group_ctx->group_name[1] ? "(|" : "");
	while (*name) {
		fr_ldap_escape_func(request, buffer, sizeof(buffer), *name++, NULL);
		filter = talloc_asprintf_append_buffer(filter, "(%s=%s)", inst->groupobj_name_attr, buffer);

		group_ctx->name_cnt++;
	}
	filter = talloc_asprintf_append_buffer(filter, "%s%s",
					       inst->groupobj_filter ? ")" : "",
					       group_ctx->group_name[0] && group_ctx->group_name[1] ? ")" : "");

	return fr_ldap_trunk_search(p_result, group_ctx, &group_ctx->query, request, group_ctx->ttrunk,
				    group_ctx->base_dn->vb_strvalue, inst->groupobj_scope, filter,
				    null_attrs, NULL, NULL, true);
}

/** Process the results of looking up group DNs from names
 *
 * @param[out] p_result		The result of trying to resolve a group name to a dn.
 * @param[out] priority		Unused
 * @param[in] request		Current request.
 * @param[in] uctx		Group lookup context.
 * @return One of the RLM_MODULE_* values.
 */
static unlang_action_t ldap_group_name2dn_resume(rlm_rcode_t *p_result, UNUSED int *priority, request_t *request,
						void *uctx)
{
	ldap_group_userobj_ctx_t	*group_ctx = talloc_get_type_abort(uctx, ldap_group_userobj_ctx_t);
	fr_ldap_query_t			*query = talloc_get_type_abort(group_ctx->query, fr_ldap_query_t);
	rlm_ldap_t const		*inst = group_ctx->inst;
	rlm_rcode_t			rcode = RLM_MODULE_OK;
	unsigned int			entry_cnt;
	LDAPMessage			*entry;
	int				ldap_errno;
	char				*dn;
	fr_pair_t			*vp;

	switch (query->ret) {
	case LDAP_RESULT_SUCCESS:
		break;

	case LDAP_RESULT_NO_RESULT:
	case LDAP_RESULT_BAD_DN:
		RDEBUG2("Tried to resolve group name(s) to DNs but got no results");
		goto finish;

	default:
		rcode = RLM_MODULE_FAIL;
		goto finish;
	}

	entry_cnt = ldap_count_entries(query->ldap_conn->handle, query->result);
	if (entry_cnt > group_ctx->name_cnt) {
		REDEBUG("Number of DNs exceeds number of names, group and/or dn should be more restrictive");
		rcode = RLM_MODULE_INVALID;

		goto finish;
	}

	if (entry_cnt < group_ctx->name_cnt) {
		RWDEBUG("Got partial mapping of group names (%i) to DNs (%i), membership information may be incomplete",
			group_ctx->name_cnt, entry_cnt);
	}

	entry = ldap_first_entry(query->ldap_conn->handle, query->result);
	if (!entry) {
		ldap_get_option(query->ldap_conn->handle, LDAP_OPT_RESULT_CODE, &ldap_errno);
		REDEBUG("Failed retrieving entry: %s", ldap_err2string(ldap_errno));

		rcode = RLM_MODULE_FAIL;
		goto finish;
	}

	do {
		dn = ldap_get_dn(query->ldap_conn->handle, entry);
		if (!dn) {
			ldap_get_option(query->ldap_conn->handle, LDAP_OPT_RESULT_CODE, &ldap_errno);
			REDEBUG("Retrieving object DN from entry failed: %s", ldap_err2string(ldap_errno));

			rcode = RLM_MODULE_FAIL;
			goto finish;
		}
		fr_ldap_util_normalise_dn(dn, dn);

		RDEBUG2("Got group DN \"%s\"", dn);
		MEM(vp = fr_pair_afrom_da(group_ctx->list_ctx, inst->cache_da));
		fr_pair_value_bstrndup(vp, dn, strlen(dn), true);
		fr_pair_append(&group_ctx->groups, vp);
		ldap_memfree(dn);
	} while((entry = ldap_next_entry(query->ldap_conn->handle, entry)));

finish:
	/*
	 *	Remove pointer to group name to resolve so we don't
	 *	try to do it again
	 */
	*group_ctx->group_name = NULL;
	talloc_free(group_ctx->query);

	RETURN_MODULE_RCODE(rcode);
}

/** Initiate an LDAP search to turn a group DN into it's name
 *
 * Unlike the inverse conversion of a name to a DN, most LDAP directories don't allow filtering by DN,
 * so we need to search for each DN individually.
 *
 * @param[out] p_result		The result of trying to resolve a dn to a group name.
 * @param[in] priority		unused.
 * @param[in] request		Current request.
 * @param[in] uctx		The group resolution context.
 * @return One of the RLM_MODULE_* values.
 */
static unlang_action_t ldap_group_dn2name_start(rlm_rcode_t *p_result, UNUSED int *priority, request_t *request,
						void *uctx)
{
	ldap_group_userobj_ctx_t	*group_ctx = talloc_get_type_abort(uctx, ldap_group_userobj_ctx_t);
	rlm_ldap_t const		*inst = group_ctx->inst;

	if (!inst->groupobj_name_attr) {
		REDEBUG("Told to resolve group DN to name but missing 'group.name_attribute' directive");
		RETURN_MODULE_INVALID;
	}

	RDEBUG2("Resolving group DN \"%s\" to group name", *group_ctx->dn);

	return fr_ldap_trunk_search(p_result, group_ctx, &group_ctx->query, request, group_ctx->ttrunk, *group_ctx->dn,
				    LDAP_SCOPE_BASE, NULL, group_ctx->attrs, NULL, NULL, true);
}

/** Process the results of a group DN -> name lookup.
 *
 * The retrieved value is added as a value pair to the
 * temporary list in the group resolution context.
 *
 * @param[out] p_result		The result of trying to resolve a dn to a group name.
 * @param[in] priority		unused.
 * @param[in] request		Current request.
 * @param[in] uctx		The group resolution context.
 * @return One of the RLM_MODULE_* values.
 */
static unlang_action_t ldap_group_dn2name_resume(rlm_rcode_t *p_result, UNUSED int *priority, request_t *request,
						 void *uctx)
{
	ldap_group_userobj_ctx_t	*group_ctx = talloc_get_type_abort(uctx, ldap_group_userobj_ctx_t);
	fr_ldap_query_t			*query = talloc_get_type_abort(group_ctx->query, fr_ldap_query_t);
	rlm_ldap_t const		*inst = group_ctx->inst;
	LDAPMessage			*entry;
	struct berval			**values = NULL;
	int				ldap_errno;
	rlm_rcode_t			rcode = RLM_MODULE_OK;
	fr_pair_t			*vp;

	switch (query->ret) {
	case LDAP_RESULT_SUCCESS:
		break;

	case LDAP_RESULT_NO_RESULT:
	case LDAP_RESULT_BAD_DN:
		REDEBUG("Group DN \"%s\" did not resolve to an object", *group_ctx->dn);
		rcode = (inst->allow_dangling_group_refs ? RLM_MODULE_NOOP : RLM_MODULE_INVALID);
		goto finish;

	default:
		rcode = RLM_MODULE_FAIL;
		goto finish;
	}

	entry = ldap_first_entry(query->ldap_conn->handle, query->result);
	if (!entry) {
		ldap_get_option(query->ldap_conn->handle, LDAP_OPT_RESULT_CODE, &ldap_errno);
		REDEBUG("Failed retrieving entry: %s", ldap_err2string(ldap_errno));
		rcode = RLM_MODULE_INVALID;
		goto finish;
	}

	values = ldap_get_values_len(query->ldap_conn->handle, entry, inst->groupobj_name_attr);
	if (!values) {
		REDEBUG("No %s attributes found in object", inst->groupobj_name_attr);
		rcode = RLM_MODULE_INVALID;
		goto finish;
	}

	MEM(vp = fr_pair_afrom_da(group_ctx->list_ctx, inst->cache_da));
	fr_pair_value_bstrndup(vp, values[0]->bv_val, values[0]->bv_len, true);
	fr_pair_append(&group_ctx->groups, vp);
	RDEBUG2("Group DN \"%s\" resolves to name \"%pV\"", *group_ctx->dn, &vp->data);

finish:
	/*
	 *	Walk the pointer to the DN being resolved forward
	 *	ready for the next resolution.
	 */
	group_ctx->dn++;

	if (values) ldap_value_free_len(values);
	talloc_free(query);

	RETURN_MODULE_RCODE(rcode);
}

/** Convert a single group DN into a name
 *
 * Unlike the inverse conversion of a name to a DN, most LDAP directories don't allow filtering by DN,
 * so we need to search for each DN individually.
 *
 * @param[out] p_result		The result of trying to resolve a dn to a group name.
 * @param[in] inst		rlm_ldap configuration.
 * @param[in] request		Current request.
 * @param[in] ttrunk		to use.
 * @param[in] dn		to resolve.
 * @param[out] out		Where to write group name (must be freed with talloc_free).
 * @return One of the RLM_MODULE_* values.
 */
static unlang_action_t rlm_ldap_group_dn2name(rlm_rcode_t *p_result, rlm_ldap_t const *inst, request_t *request,
					      fr_ldap_thread_trunk_t *ttrunk, char const *dn, char **out)
{
	rlm_rcode_t	rcode = RLM_MODULE_OK;
	int		ldap_errno;

	struct berval	**values = NULL;
	char const	*attrs[] = { inst->groupobj_name_attr, NULL };
	LDAPMessage	*entry;
	fr_ldap_query_t	*query = NULL;

	*out = NULL;

	if (!inst->groupobj_name_attr) {
		REDEBUG("Told to resolve group DN to name but missing 'group.name_attribute' directive");

		RETURN_MODULE_INVALID;
	}

	RDEBUG2("Resolving group DN \"%s\" to group name", dn);

	if (fr_ldap_trunk_search(&rcode,
				 unlang_interpret_frame_talloc_ctx(request), &query, request, ttrunk, dn,
				 LDAP_SCOPE_BASE, NULL, attrs, NULL, NULL, false) < 0) {
		RETURN_MODULE_FAIL;
	}
	switch (rcode) {
	case RLM_MODULE_OK:
		break;

	case RLM_MODULE_NOTFOUND:
		REDEBUG("Group DN \"%s\" did not resolve to an object", dn);
		RETURN_MODULE_RCODE(inst->allow_dangling_group_refs ? RLM_MODULE_NOOP : RLM_MODULE_INVALID);

	default:
		RETURN_MODULE_FAIL;
	}

	entry = ldap_first_entry(query->ldap_conn->handle, query->result);
	if (!entry) {
		ldap_get_option(query->ldap_conn->handle, LDAP_OPT_RESULT_CODE, &ldap_errno);
		REDEBUG("Failed retrieving entry: %s", ldap_err2string(ldap_errno));

		rcode = RLM_MODULE_INVALID;
		goto finish;
	}

	values = ldap_get_values_len(query->ldap_conn->handle, entry, inst->groupobj_name_attr);
	if (!values) {
		REDEBUG("No %s attributes found in object", inst->groupobj_name_attr);

		rcode = RLM_MODULE_INVALID;

		goto finish;
	}

	*out = fr_ldap_berval_to_string(request, values[0]);
	RDEBUG2("Group DN \"%s\" resolves to name \"%s\"", dn, *out);

finish:
	if (values) ldap_value_free_len(values);

	RETURN_MODULE_RCODE(rcode);
}

/** Move user object group attributes to the control list
 *
 * @param p_result	The result of adding user object group attributes
 * @param request	Current request.
 * @param group_ctx	Context used to evaluate group attributes
 * @return RLM_MODULE_OK
 */
static unlang_action_t ldap_cacheable_userobj_store(rlm_rcode_t *p_result, request_t *request,
						    ldap_group_userobj_ctx_t *group_ctx)
{
	fr_pair_t		*vp;
	fr_pair_list_t		*list;

	list = tmpl_list_head(request, request_attr_control);
	fr_assert(list != NULL);

	RDEBUG2("Adding cacheable user object memberships");
	RINDENT();
	if (RDEBUG_ENABLED) {
		for (vp = fr_pair_list_head(&group_ctx->groups);
		     vp;
		     vp = fr_pair_list_next(&group_ctx->groups, vp)) {
			RDEBUG2("&control.%s += \"%pV\"", group_ctx->inst->cache_da->name, &vp->data);
		}
	}

	fr_pair_list_append(list, &group_ctx->groups);
	REXDENT();

	talloc_free(group_ctx);
	RETURN_MODULE_OK;
}

/** Initiate DN to name and name to DN group lookups
 *
 * Called repeatedly until there are no more lookups to perform
 * or an unresolved lookup causes the module to fail.
 *
 * @param p_result	The result of the previous expansion.
 * @param priority	unused.
 * @param request	Current request.
 * @param uctx		The group context being processed.
 * @return One of the RLM_MODULE_* values.
 */
static unlang_action_t ldap_cacheable_userobj_resolve(rlm_rcode_t *p_result, UNUSED int *priority,
						      request_t *request, void *uctx)
{
	ldap_group_userobj_ctx_t	*group_ctx = talloc_get_type_abort(uctx, ldap_group_userobj_ctx_t);

	/*
	 *	If we've previously failed to expand, fail the group section
	 */
	switch (*p_result) {
	case RLM_MODULE_FAIL:
	case RLM_MODULE_INVALID:
		talloc_free(group_ctx);
		return UNLANG_ACTION_CALCULATE_RESULT;
	default:
		break;
	}

	/*
	 *	Are there any DN to resolve to names?
	 *	These are resolved one at a time as most directories don't allow for
	 *	filters on the DN.
	 */
	if (*group_ctx->dn) {
		if (unlang_function_repeat_set(request, ldap_cacheable_userobj_resolve) < 0) RETURN_MODULE_FAIL;
		if (unlang_function_push(request, ldap_group_dn2name_start, ldap_group_dn2name_resume,
					 ldap_group_userobj_cancel, ~FR_SIGNAL_CANCEL,
					 UNLANG_SUB_FRAME, group_ctx) < 0) RETURN_MODULE_FAIL;
		return UNLANG_ACTION_PUSHED_CHILD;
	}

	/*
	 *	Are there any names to resolve to DN?
	 */
	if (*group_ctx->group_name) {
		if (unlang_function_repeat_set(request, ldap_cacheable_userobj_resolve) < 0) RETURN_MODULE_FAIL;
		if (unlang_function_push(request, ldap_group_name2dn_start, ldap_group_name2dn_resume,
					 ldap_group_userobj_cancel, ~FR_SIGNAL_CANCEL,
					 UNLANG_SUB_FRAME, group_ctx) < 0) RETURN_MODULE_FAIL;
		return UNLANG_ACTION_PUSHED_CHILD;
	}

	/*
	 *	Nothing left to resolve, move the resulting attributes to
	 *	the control list.
	 */
	return ldap_cacheable_userobj_store(p_result, request, group_ctx);
}

/** Convert group membership information into attributes
 *
 * This may just be able to parse attribute values in the user object
 * or it may need to yield to other LDAP searches depending on what was
 * returned and what is set to be cached.
 *
 * @param[out] p_result		The result of trying to resolve a dn to a group name.
 * @param[in] request		Current request.
 * @param[in] autz_ctx		LDAP authorization context being processed.
 * @param[in] attr		membership attribute to look for in the entry.
 * @return One of the RLM_MODULE_* values.
 */
unlang_action_t rlm_ldap_cacheable_userobj(rlm_rcode_t *p_result, request_t *request, ldap_autz_ctx_t *autz_ctx,
					   char const *attr)
{
	rlm_ldap_t const		*inst = autz_ctx->inst;
	LDAPMessage			*entry = autz_ctx->entry;
	fr_ldap_thread_trunk_t		*ttrunk = autz_ctx->ttrunk;
	ldap_group_userobj_ctx_t	*group_ctx;
	struct berval			**values;
	char				**name_p;
	char				**dn_p;
	fr_pair_t			*vp;
	int				is_dn, i, count;

	fr_assert(entry);
	fr_assert(attr);

	/*
	 *	Parse the membership information we got in the initial user query.
	 */
	values = ldap_get_values_len(fr_ldap_handle_thread_local(), entry, attr);
	if (!values) {
		RDEBUG2("No cacheable group memberships found in user object");

		RETURN_MODULE_OK;
	}
	count = ldap_count_values_len(values);

	/*
	 *	Set up context for managing group membership attribute resolution.
	 */
	MEM(group_ctx = talloc_zero(unlang_interpret_frame_talloc_ctx(request), ldap_group_userobj_ctx_t));
	group_ctx->inst = inst;
	group_ctx->ttrunk = ttrunk;
	group_ctx->base_dn = &autz_ctx->mod_env->group_base;
	group_ctx->list_ctx = tmpl_list_ctx(request, request_attr_control);
	fr_assert(group_ctx->list_ctx != NULL);

	/*
	 *	Set up pointers to entries in arrays of names / DNs to resolve.
	 */
	name_p = group_ctx->group_name;
	group_ctx->dn = dn_p = group_ctx->group_dn;

	/*
	 *	Temporary list to hold new group VPs, will be merged
	 *	once all group info has been gathered/resolved
	 *	successfully.
	 */
	fr_pair_list_init(&group_ctx->groups);

	for (i = 0; (i < LDAP_MAX_CACHEABLE) && (i < count); i++) {
		is_dn = fr_ldap_util_is_dn(values[i]->bv_val, values[i]->bv_len);

		if (inst->cacheable_group_dn) {
			/*
			 *	The easy case, we're caching DNs and we got a DN.
			 */
			if (is_dn) {
				MEM(vp = fr_pair_afrom_da(group_ctx->list_ctx, inst->cache_da));
				fr_pair_value_bstrndup(vp, values[i]->bv_val, values[i]->bv_len, true);
				fr_pair_append(&group_ctx->groups, vp);
			/*
			 *	We were told to cache DNs but we got a name, we now need to resolve
			 *	this to a DN. Store all the group names in an array so we can do one query.
			 */
			} else {
				*name_p++ = fr_ldap_berval_to_string(group_ctx, values[i]);
			}
		}

		if (inst->cacheable_group_name) {
			/*
			 *	The easy case, we're caching names and we got a name.
			 */
			if (!is_dn) {
				MEM(vp = fr_pair_afrom_da(group_ctx->list_ctx, inst->cache_da));
				fr_pair_value_bstrndup(vp, values[i]->bv_val, values[i]->bv_len, true);
				fr_pair_append(&group_ctx->groups, vp);
			/*
			 *	We were told to cache names but we got a DN, we now need to resolve
			 *	this to a name.  Store group DNs which need resolving to names.
			 */
			} else {
				*dn_p++ = fr_ldap_berval_to_string(group_ctx, values[i]);
			}
		}
	}

	ldap_value_free_len(values);

	/*
	 *	We either have group names which need converting to DNs or
	 *	DNs which need resolving to names.  Push a function which will
	 *	do the resolution.
	 */
	if ((name_p != group_ctx->group_name) || (dn_p != group_ctx->group_dn)) {
		group_ctx->attrs[0] = inst->groupobj_name_attr;
		if (unlang_function_push(request, ldap_cacheable_userobj_resolve, NULL, ldap_group_userobj_cancel,
					 ~FR_SIGNAL_CANCEL, UNLANG_SUB_FRAME, group_ctx) < 0) {
			talloc_free(group_ctx);
			RETURN_MODULE_FAIL;
		}
		return UNLANG_ACTION_PUSHED_CHILD;
	}

	/*
	 *	No additional queries needed, just process the context to
	 *	move any generated pairs into the correct list.
	 */
	return ldap_cacheable_userobj_store(p_result, request, group_ctx);
}

/** Initiate an LDAP search for group membership looking at the group objects
 *
 * @param[out] p_result		Result of submitting LDAP search
 * @param[out] priority		Unused.
 * @param[in] request		Current request.
 * @param[in] uctx		Group lookup context.
 * @return One of the RLM_MODULE_* values.
 */
static unlang_action_t ldap_cacheable_groupobj_start(rlm_rcode_t *p_result, UNUSED int *priority, request_t *request,
						     void *uctx)
{
	ldap_group_groupobj_ctx_t	*group_ctx = talloc_get_type_abort(uctx, ldap_group_groupobj_ctx_t);
	rlm_ldap_t const		*inst = group_ctx->inst;

	group_ctx->attrs[0] = inst->groupobj_name_attr;
	return fr_ldap_trunk_search(p_result, group_ctx, &group_ctx->query, request, group_ctx->ttrunk,
				    group_ctx->base_dn->vb_strvalue, inst->groupobj_scope,
				    group_ctx->filter, group_ctx->attrs, NULL, NULL, true);

}

/** Cancel a pending group object lookup.
 *
 */
static void ldap_group_groupobj_cancel(UNUSED request_t *request, UNUSED fr_signal_t action, void *uctx)
{
	ldap_group_groupobj_ctx_t	*group_ctx = talloc_get_type_abort(uctx, ldap_group_groupobj_ctx_t);

	/*
	 *	If the query is not in flight, just return
	 */
	if (!group_ctx->query || !group_ctx->query->treq) return;

	fr_trunk_request_signal_cancel(group_ctx->query->treq);
}

/** Process the results of a group object lookup.
 *
 * @param[out] p_result		Result of processing group lookup.
 * @param[out] priority		Unused.
 * @param[in] request		Current request.
 * @param[in] uctx		Group lookup context.
 * @return One of the RLM_MODULE_* values.
 */
static unlang_action_t ldap_cacheable_groupobj_resume(rlm_rcode_t *p_result, UNUSED int *priority, request_t *request,
						      void *uctx)
{
	ldap_group_groupobj_ctx_t	*group_ctx = talloc_get_type_abort(uctx, ldap_group_groupobj_ctx_t);
	rlm_ldap_t const		*inst = group_ctx->inst;
	fr_ldap_query_t			*query = group_ctx->query;
	rlm_rcode_t			rcode = RLM_MODULE_OK;
	LDAPMessage			*entry;
	int				ldap_errno;
	char				*dn;
	fr_pair_t			*vp;

	switch (query->ret) {
	case LDAP_SUCCESS:
		break;

	case LDAP_RESULT_NO_RESULT:
	case LDAP_RESULT_BAD_DN:
		RDEBUG2("No cacheable group memberships found in group objects");
		rcode = RLM_MODULE_NOTFOUND;
		goto finish;

	default:
		rcode = RLM_MODULE_FAIL;
		goto finish;
	}

	entry = ldap_first_entry(query->ldap_conn->handle, query->result);
	if (!entry) {
		ldap_get_option(query->ldap_conn->handle, LDAP_OPT_RESULT_CODE, &ldap_errno);
		REDEBUG("Failed retrieving entry: %s", ldap_err2string(ldap_errno));

		goto finish;
	}

	RDEBUG2("Adding cacheable group object memberships");
	do {
		if (inst->cacheable_group_dn) {
			dn = ldap_get_dn(query->ldap_conn->handle, entry);
			if (!dn) {
				ldap_get_option(query->ldap_conn->handle, LDAP_OPT_RESULT_CODE, &ldap_errno);
				REDEBUG("Retrieving object DN from entry failed: %s", ldap_err2string(ldap_errno));

				goto finish;
			}
			fr_ldap_util_normalise_dn(dn, dn);

			MEM(pair_append_control(&vp, inst->cache_da) == 0);
			fr_pair_value_strdup(vp, dn, false);

			RINDENT();
			RDEBUG2("&control.%pP", vp);
			REXDENT();
			ldap_memfree(dn);
		}

		if (inst->cacheable_group_name) {
			struct berval **values;

			values = ldap_get_values_len(query->ldap_conn->handle, entry, inst->groupobj_name_attr);
			if (!values) continue;

			MEM(pair_append_control(&vp, inst->cache_da) == 0);
			fr_pair_value_bstrndup(vp, values[0]->bv_val, values[0]->bv_len, true);

			RINDENT();
			RDEBUG2("&control.%pP", vp);
			REXDENT();

			ldap_value_free_len(values);
		}
	} while ((entry = ldap_next_entry(query->ldap_conn->handle, entry)));

finish:
	talloc_free(group_ctx);

	RETURN_MODULE_RCODE(rcode);
}

/** Convert group membership information into attributes
 *
 * @param[out] p_result		The result of trying to resolve a dn to a group name.
 * @param[in] request		Current request.
 * @param[in] autz_ctx		Authentication context being processed.
 * @return One of the RLM_MODULE_* values.
 */
unlang_action_t rlm_ldap_cacheable_groupobj(rlm_rcode_t *p_result, request_t *request, ldap_autz_ctx_t *autz_ctx)
{
	rlm_ldap_t const		*inst = autz_ctx->inst;
	ldap_group_groupobj_ctx_t	*group_ctx;
	char const			*filters[] = { inst->groupobj_filter, inst->groupobj_membership_filter };

	if (!inst->groupobj_membership_filter) {
		RDEBUG2("Skipping caching group objects as directive 'group.membership_filter' is not set");
		RETURN_MODULE_OK;
	}

	if (autz_ctx->mod_env->group_base.type != FR_TYPE_STRING) {
		REDEBUG("Missing group base_dn");
		RETURN_MODULE_INVALID;
	}

	MEM(group_ctx = talloc_zero(unlang_interpret_frame_talloc_ctx(request), ldap_group_groupobj_ctx_t));
	group_ctx->inst = inst;
	group_ctx->ttrunk = autz_ctx->ttrunk;
	group_ctx->base_dn = &autz_ctx->mod_env->group_base;

	if (fr_ldap_xlat_filter(request, filters, NUM_ELEMENTS(filters),
				group_ctx->filter, sizeof(group_ctx->filter)) < 0) {
		talloc_free(group_ctx);
		RETURN_MODULE_INVALID;
	}

	if (unlang_function_push(request, ldap_cacheable_groupobj_start, ldap_cacheable_groupobj_resume,
				 ldap_group_groupobj_cancel, ~FR_SIGNAL_CANCEL, UNLANG_SUB_FRAME, group_ctx) < 0) {
		talloc_free(group_ctx);
		RETURN_MODULE_FAIL;
	}

	return UNLANG_ACTION_PUSHED_CHILD;
}

/** Query the LDAP directory to check if a group object includes a user object as a member
 *
 * @param[out] p_result		Result of calling the module.
 * @param[in] inst		rlm_ldap configuration.
 * @param[in] request		Current request.
 * @param[in] ttrunk		to use.
 * @param[in] check		vp containing the group value (name or dn).
 */
unlang_action_t rlm_ldap_check_groupobj_dynamic(rlm_rcode_t *p_result, rlm_ldap_t const *inst, request_t *request,
						fr_ldap_thread_trunk_t *ttrunk, fr_pair_t const *check)
{
	rlm_rcode_t	rcode;
	fr_ldap_query_t	*query = NULL;

	char const	*base_dn;
	char		base_dn_buff[LDAP_MAX_DN_STR_LEN + 1];
	char 		filter[LDAP_MAX_FILTER_STR_LEN + 1];
	int		ret;

	fr_assert(inst->groupobj_base_dn);

	switch (check->op) {
	case T_OP_CMP_EQ:
	case T_OP_CMP_FALSE:
	case T_OP_CMP_TRUE:
	case T_OP_REG_EQ:
	case T_OP_REG_NE:
		break;

	default:
		REDEBUG("Operator \"%s\" not allowed for LDAP group comparisons",
			fr_table_str_by_value(fr_tokens_table, check->op, "<INVALID>"));
		return 1;
	}

	RDEBUG2("Checking for user in group objects");

	if (fr_ldap_util_is_dn(check->vp_strvalue, check->vp_length)) {
		char const *filters[] = { inst->groupobj_filter, inst->groupobj_membership_filter };

		RINDENT();
		ret = fr_ldap_xlat_filter(request,
					   filters, NUM_ELEMENTS(filters),
					   filter, sizeof(filter));
		REXDENT();

		if (ret < 0) RETURN_MODULE_INVALID;

		base_dn = check->vp_strvalue;
	} else {
		char name_filter[LDAP_MAX_FILTER_STR_LEN];
		char const *filters[] = { name_filter, inst->groupobj_filter, inst->groupobj_membership_filter };

		if (!inst->groupobj_name_attr) {
			REDEBUG("Told to search for group by name, but missing 'group.name_attribute' "
				"directive");

			RETURN_MODULE_INVALID;
		}

		snprintf(name_filter, sizeof(name_filter), "(%s=%s)", inst->groupobj_name_attr, check->vp_strvalue);
		RINDENT();
		ret = fr_ldap_xlat_filter(request,
					   filters, NUM_ELEMENTS(filters),
					   filter, sizeof(filter));
		REXDENT();
		if (ret < 0) RETURN_MODULE_INVALID;


		/*
		 *	rlm_ldap_find_user does this, too.  Oh well.
		 */
		RINDENT();
		ret = tmpl_expand(&base_dn, base_dn_buff, sizeof(base_dn_buff), request, inst->groupobj_base_dn,
				  fr_ldap_escape_func, NULL);
		REXDENT();
		if (ret < 0) {
			REDEBUG("Failed creating base_dn");

			RETURN_MODULE_INVALID;
		}
	}

	RINDENT();
	if (fr_ldap_trunk_search(&rcode,
				 unlang_interpret_frame_talloc_ctx(request), &query, request, ttrunk, base_dn,
				 inst->groupobj_scope, filter, NULL, NULL, NULL, false) < 0) {
		REXDENT();
		RETURN_MODULE_FAIL;
	}
	REXDENT();
	switch (rcode) {
	case RLM_MODULE_OK:
		RDEBUG2("User found in group object \"%s\"", base_dn);
		break;

	case RLM_MODULE_NOTFOUND:
		RETURN_MODULE_NOTFOUND;

	default:
		RETURN_MODULE_FAIL;
	}

	RETURN_MODULE_OK;
}

/** Query the LDAP directory to check if a user object is a member of a group
 *
 * @param[out] p_result		Result of calling the module.
 * @param[in] inst		rlm_ldap configuration.
 * @param[in] request		Current request.
 * @param[in] ttrunk		to use.
 * @param[in] dn		of user object.
 * @param[in] check		vp containing the group value (name or dn).
 */
unlang_action_t rlm_ldap_check_userobj_dynamic(rlm_rcode_t *p_result, rlm_ldap_t const *inst, request_t *request,
					       fr_ldap_thread_trunk_t *ttrunk,
					       char const *dn, fr_pair_t const *check)
{
	rlm_rcode_t	rcode = RLM_MODULE_NOTFOUND, ret;
	bool		name_is_dn = false, value_is_dn = false;
	fr_ldap_query_t	*query;

	LDAPMessage     *entry = NULL;
	struct berval	**values = NULL;

	char const	*attrs[] = { inst->userobj_membership_attr, NULL };
	int		i, count, ldap_errno;

	RDEBUG2("Checking user object's %s attributes", inst->userobj_membership_attr);
	RINDENT();
	if (fr_ldap_trunk_search(&rcode,
				 unlang_interpret_frame_talloc_ctx(request), &query, request, ttrunk, dn,
				 LDAP_SCOPE_BASE, NULL, attrs, NULL, NULL, false) < 0) {
		REXDENT();
		goto finish;
	}
	REXDENT();
	switch (rcode) {
	case RLM_MODULE_OK:
		break;

	case RLM_MODULE_NOTFOUND:
		RDEBUG2("Can't check membership attributes, user object not found");

		FALL_THROUGH;
	default:
		goto finish;
	}

	entry = ldap_first_entry(query->ldap_conn->handle, query->result);
	if (!entry) {
		ldap_get_option(query->ldap_conn->handle, LDAP_OPT_RESULT_CODE, &ldap_errno);
		REDEBUG("Failed retrieving entry: %s", ldap_err2string(ldap_errno));

		rcode = RLM_MODULE_FAIL;

		goto finish;
	}

	values = ldap_get_values_len(query->ldap_conn->handle, entry, inst->userobj_membership_attr);
	if (!values) {
		RDEBUG2("No group membership attribute(s) found in user object");

		goto finish;
	}

	/*
	 *	Loop over the list of groups the user is a member of,
	 *	looking for a match.
	 */
	name_is_dn = fr_ldap_util_is_dn(check->vp_strvalue, check->vp_length);
	count = ldap_count_values_len(values);
	for (i = 0; i < count; i++) {
		value_is_dn = fr_ldap_util_is_dn(values[i]->bv_val, values[i]->bv_len);

		RDEBUG2("Processing %s value \"%pV\" as a %s", inst->userobj_membership_attr,
			fr_box_strvalue_len(values[i]->bv_val, values[i]->bv_len),
			value_is_dn ? "DN" : "group name");

		/*
		 *	Both literal group names, do case sensitive comparison
		 */
		if (!name_is_dn && !value_is_dn) {
			if ((check->vp_length == values[i]->bv_len) &&
			    (memcmp(values[i]->bv_val, check->vp_strvalue, values[i]->bv_len) == 0)) {
				RDEBUG2("User found in group \"%s\". Comparison between membership: name, check: name",
				       check->vp_strvalue);
				rcode = RLM_MODULE_OK;

				goto finish;
			}

			continue;
		}

		/*
		 *	Both DNs, do case insensitive, binary safe comparison
		 */
		if (name_is_dn && value_is_dn) {
			if (check->vp_length == values[i]->bv_len) {
				int j;

				for (j = 0; j < (int)values[i]->bv_len; j++) {
					if (tolower(values[i]->bv_val[j]) != tolower(check->vp_strvalue[j])) break;
				}
				if (j == (int)values[i]->bv_len) {
					RDEBUG2("User found in group DN \"%s\". "
					       "Comparison between membership: dn, check: dn", check->vp_strvalue);
					rcode = RLM_MODULE_OK;

					goto finish;
				}
			}

			continue;
		}

		/*
		 *	If the value is not a DN, and the name we were given is a dn
		 *	convert the value to a DN and do a comparison.
		 */
		if (!value_is_dn && name_is_dn) {
			char *resolved;
			bool eq = false;

			RINDENT();
			rlm_ldap_group_dn2name(&ret, inst, request, ttrunk, check->vp_strvalue, &resolved);
			REXDENT();

			if (ret == RLM_MODULE_NOOP) continue;

			if (ret != RLM_MODULE_OK) {
				rcode = ret;
				goto finish;
			}

			if (((talloc_array_length(resolved) - 1) == values[i]->bv_len) &&
			    (memcmp(values[i]->bv_val, resolved, values[i]->bv_len) == 0)) eq = true;
			talloc_free(resolved);
			if (eq) {
				RDEBUG2("User found in group \"%pV\". Comparison between membership: name, check: name "
				       "(resolved from DN \"%s\")",
				       fr_box_strvalue_len(values[i]->bv_val, values[i]->bv_len), check->vp_strvalue);
				rcode = RLM_MODULE_OK;

				goto finish;
			}

			continue;
		}

		/*
		 *	We have a value which is a DN, and a check item which specifies the name of a group,
		 *	convert the value to a name so we can do a comparison.
		 */
		if (value_is_dn && !name_is_dn) {
			char *resolved;
			char *value;
			bool eq = false;

			value = fr_ldap_berval_to_string(request, values[i]);
			RINDENT();
			rlm_ldap_group_dn2name(&ret, inst, request, ttrunk, value, &resolved);
			REXDENT();
			talloc_free(value);

			if (ret == RLM_MODULE_NOOP) continue;

			if (ret != RLM_MODULE_OK) {
				rcode = ret;
				goto finish;
			}

			if (((talloc_array_length(resolved) - 1) == check->vp_length) &&
			    (memcmp(check->vp_strvalue, resolved, check->vp_length) == 0)) eq = true;
			talloc_free(resolved);
			if (eq) {
				RDEBUG2("User found in group \"%pV\". Comparison between membership: name "
				       "(resolved from DN \"%s\"), check: name", &check->data, value);
				rcode = RLM_MODULE_OK;

				goto finish;
			}

			continue;
		}
		fr_assert(0);
	}

finish:
	if (values) ldap_value_free_len(values);

	RETURN_MODULE_RCODE(rcode);
}

/** Check group membership attributes to see if a user is a member.
 *
 * @param[out] p_result		Result of calling the module.
 * @param[in] inst		rlm_ldap configuration.
 * @param[in] request		Current request.
 * @param[in] check		vp containing the group value (name or dn).
 */
unlang_action_t rlm_ldap_check_cached(rlm_rcode_t *p_result,
				      rlm_ldap_t const *inst, request_t *request, fr_pair_t const *check)
{
	fr_pair_t	*vp;
	int		ret;
	fr_dcursor_t	cursor;

	/*
	 *	We return RLM_MODULE_INVALID here as an indication
	 *	the caller should try a dynamic group lookup instead.
	 */
	vp =  fr_pair_dcursor_by_da_init(&cursor, &request->control_pairs, inst->cache_da);
	if (!vp) RETURN_MODULE_INVALID;

	for (vp = fr_dcursor_current(&cursor);
	     vp;
	     vp = fr_dcursor_next(&cursor)) {
		ret = fr_pair_cmp_op(T_OP_CMP_EQ, vp, check);
		if (ret == 1) {
			RDEBUG2("User found. Matched cached membership");
			RETURN_MODULE_OK;
		}

		if (ret < -1) RETURN_MODULE_FAIL;
	}

	RDEBUG2("Cached membership not found");

	RETURN_MODULE_NOTFOUND;
}
