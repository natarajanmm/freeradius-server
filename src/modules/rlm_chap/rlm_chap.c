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
 * @file rlm_chap.c
 * @brief Process chap authentication requests.
 *
 * @copyright 2001,2006 The FreeRADIUS server project
 */
RCSID("$Id$")

#define LOG_PREFIX mctx->inst->name

#include <freeradius-devel/server/base.h>
#include <freeradius-devel/server/password.h>
#include <freeradius-devel/server/module_rlm.h>
#include <freeradius-devel/radius/radius.h>
#include <freeradius-devel/unlang/xlat_func.h>

typedef struct {
	fr_dict_enum_value_t		*auth_type;
} rlm_chap_t;

static fr_dict_t const *dict_freeradius;
static fr_dict_t const *dict_radius;

extern fr_dict_autoload_t rlm_chap_dict[];
fr_dict_autoload_t rlm_chap_dict[] = {
	{ .out = &dict_freeradius, .proto = "freeradius" },
	{ .out = &dict_radius, .proto = "radius" },
	{ NULL }
};

static fr_dict_attr_t const *attr_auth_type;
static fr_dict_attr_t const *attr_cleartext_password;

static fr_dict_attr_t const *attr_chap_password;
static fr_dict_attr_t const *attr_chap_challenge;
static fr_dict_attr_t const *attr_user_name;

extern fr_dict_attr_autoload_t rlm_chap_dict_attr[];
fr_dict_attr_autoload_t rlm_chap_dict_attr[] = {
	{ .out = &attr_auth_type, .name = "Auth-Type", .type = FR_TYPE_UINT32, .dict = &dict_freeradius },
	{ .out = &attr_cleartext_password, .name = "Password.Cleartext", .type = FR_TYPE_STRING, .dict = &dict_freeradius },

	{ .out = &attr_chap_password, .name = "Chap-Password", .type = FR_TYPE_OCTETS, .dict = &dict_radius },
	{ .out = &attr_chap_challenge, .name = "Chap-Challenge", .type = FR_TYPE_OCTETS, .dict = &dict_radius },
	{ .out = &attr_user_name, .name = "User-Name", .type = FR_TYPE_STRING, .dict = &dict_radius },
	{ NULL }
};

static xlat_arg_parser_t const xlat_func_chap_password_args[] = {
	{ .required = true, .single = true, .type = FR_TYPE_STRING },
	XLAT_ARG_PARSER_TERMINATOR
};

/** Produce a CHAP-Password hash value
 *
 * Example:
@verbatim
"%(chap_password:<password>)" == 0x<id><md5_hash>
@endverbatim
 *
 * @ingroup xlat_functions
 */
static xlat_action_t xlat_func_chap_password(TALLOC_CTX *ctx, fr_dcursor_t *out,
				 	     UNUSED xlat_ctx_t const *xctx,
					     request_t *request, fr_value_box_list_t *in)
{
	uint8_t		chap_password[1 + RADIUS_CHAP_CHALLENGE_LENGTH];
	fr_value_box_t	*vb;
	fr_pair_t	*challenge;
	uint8_t	const	*vector;
	fr_value_box_t	*in_head = fr_value_box_list_head(in);

	/*
	 *	Use Chap-Challenge pair if present,
	 *	Request Authenticator otherwise.
	 */
	challenge = fr_pair_find_by_da(&request->request_pairs, NULL, attr_chap_challenge);
	if (challenge && (challenge->vp_length == RADIUS_AUTH_VECTOR_LENGTH)) {
		vector = challenge->vp_octets;
	} else {
		vector = request->packet->vector;
	}
	fr_radius_encode_chap_password(chap_password, (uint8_t)(fr_rand() & 0xff), vector,
				       in_head->vb_strvalue, in_head->vb_length);

	MEM(vb = fr_value_box_alloc_null(ctx));
	fr_value_box_memdup(vb, vb, NULL, chap_password, sizeof(chap_password), false);
	fr_dcursor_append(out, vb);

	return XLAT_ACTION_DONE;
}

static unlang_action_t CC_HINT(nonnull) mod_authorize(rlm_rcode_t *p_result, module_ctx_t const *mctx, request_t *request)
{
	fr_pair_t		*vp;
	rlm_chap_t const	*inst = talloc_get_type_abort_const(mctx->inst->data, rlm_chap_t);

	if (fr_pair_find_by_da(&request->control_pairs, NULL, attr_auth_type) != NULL) {
		RDEBUG3("Auth-Type is already set.  Not setting 'Auth-Type := %s'", mctx->inst->name);
		RETURN_MODULE_NOOP;
	}

	/*
	 *	This case means the warnings below won't be printed
	 *	unless there's a CHAP-Password in the request.
	 */
	if (!fr_pair_find_by_da(&request->request_pairs, NULL, attr_chap_password)) {
		RETURN_MODULE_NOOP;
	}

	/*
	 *	Create the CHAP-Challenge if it wasn't already in the packet.
	 *
	 *	This is so that the rest of the code does not need to
	 *	understand CHAP.
	 */
	vp = fr_pair_find_by_da(&request->request_pairs, NULL, attr_chap_challenge);
	if (!vp) {
		RDEBUG2("Creating &%s from request authenticator", attr_chap_challenge->name);

		MEM(vp = fr_pair_afrom_da(request->request_ctx, attr_chap_challenge));
		fr_pair_value_memdup(vp, request->packet->vector, sizeof(request->packet->vector), true);
		fr_pair_append(&request->request_pairs, vp);
	}

	if (!inst->auth_type) {
		WARN("No 'authenticate %s {...}' section or 'Auth-Type = %s' set.  Cannot setup CHAP authentication",
		     mctx->inst->name, mctx->inst->name);
		RETURN_MODULE_NOOP;
	}

	if (!module_rlm_section_type_set(request, attr_auth_type, inst->auth_type)) {
		RETURN_MODULE_NOOP;
	}

	RETURN_MODULE_OK;
}

/*
 *	Find the named user in this modules database.  Create the set
 *	of attribute-value pairs to check and reply with for this user
 *	from the database. The authentication code only needs to check
 *	the password, the rest is done here.
 */
static unlang_action_t CC_HINT(nonnull) mod_authenticate(rlm_rcode_t *p_result, UNUSED module_ctx_t const *mctx, request_t *request)
{
	fr_pair_t		*known_good;
	fr_pair_t		*chap, *username;
	uint8_t			pass_str[1 + RADIUS_CHAP_CHALLENGE_LENGTH];

	int			ret;

	fr_dict_attr_t const	*allowed_passwords[] = { attr_cleartext_password };
	bool			ephemeral;

	fr_pair_t		*challenge;
	uint8_t	const		*vector;

	username = fr_pair_find_by_da(&request->request_pairs, NULL, attr_user_name);
	if (!username) {
		REDEBUG("&User-Name attribute is required for authentication");
		RETURN_MODULE_INVALID;
	}

	chap = fr_pair_find_by_da(&request->request_pairs, NULL, attr_chap_password);
	if (!chap) {
		REDEBUG("You set '&control.Auth-Type = CHAP' for a request that "
			"does not contain a CHAP-Password attribute!");
		RETURN_MODULE_INVALID;
	}

	if (chap->vp_length == 0) {
		REDEBUG("&request.CHAP-Password is empty");
		RETURN_MODULE_INVALID;
	}

	if (chap->vp_length != RADIUS_CHAP_CHALLENGE_LENGTH + 1) {
		REDEBUG("&request.CHAP-Password has invalid length");
		RETURN_MODULE_INVALID;
	}

	/*
	 *	Retrieve the normalised version of
	 *	the known_good password, without
	 *	mangling the current password attributes
	 *	in the request.
	 */
	known_good = password_find(&ephemeral, request, request,
				   allowed_passwords, NUM_ELEMENTS(allowed_passwords),
				   false);
	if (!known_good) {
		REDEBUG("No \"known good\" password found for user");
		RETURN_MODULE_FAIL;
	}

	/*
	 *	Output is id + password hash
	 */

	/*
	 *	Use Chap-Challenge pair if present,
	 *	Request Authenticator otherwise.
	 */
	challenge = fr_pair_find_by_da(&request->request_pairs, NULL, attr_chap_challenge);
	if (challenge && (challenge->vp_length == RADIUS_AUTH_VECTOR_LENGTH)) {
		vector = challenge->vp_octets;
	} else {
		vector = request->packet->vector;
	}
	fr_radius_encode_chap_password(pass_str, chap->vp_octets[0], vector,
				       known_good->vp_strvalue, known_good->vp_length);

	/*
	 *	The password_find function already emits
	 *	a log message about the password attribute contents
	 *	so we don't need to duplicate it here.
	 */
	if (RDEBUG_ENABLED3) {
		uint8_t	const	*p;
		size_t		length;
		fr_pair_t	*vp;

		vp = fr_pair_find_by_da(&request->request_pairs, NULL, attr_chap_challenge);
		if (vp) {
			RDEBUG2("Using challenge from &request.CHAP-Challenge");
			p = vp->vp_octets;
			length = vp->vp_length;
		} else {
			RDEBUG2("Using challenge from authenticator field");
			p = request->packet->vector;
			length = sizeof(request->packet->vector);
		}

		RINDENT();
		RDEBUG3("CHAP challenge : %pH", fr_box_octets(p, length));
		RDEBUG3("Client sent    : %pH", fr_box_octets(chap->vp_octets + 1, RADIUS_CHAP_CHALLENGE_LENGTH));
		RDEBUG3("We calculated  : %pH", fr_box_octets(pass_str + 1, RADIUS_CHAP_CHALLENGE_LENGTH));
		REXDENT();
	}

	/*
	 *	Skip the id field at the beginning of the
	 *	password and chap response.
	 */
	ret = fr_digest_cmp(pass_str + 1, chap->vp_octets + 1, RADIUS_CHAP_CHALLENGE_LENGTH);
	if (ephemeral) TALLOC_FREE(known_good);
	if (ret != 0) {
		REDEBUG("Password comparison failed: password is incorrect");

		RETURN_MODULE_REJECT;
	}

	RDEBUG2("CHAP user \"%pV\" authenticated successfully", &username->data);

	RETURN_MODULE_OK;
}

/*
 *	Create instance for our module. Allocate space for
 *	instance structure and read configuration parameters
 */
static int mod_instantiate(module_inst_ctx_t const *mctx)
{
	rlm_chap_t		*inst = talloc_get_type_abort(mctx->inst->data, rlm_chap_t);

	inst->auth_type = fr_dict_enum_by_name(attr_auth_type, mctx->inst->name, -1);
	if (!inst->auth_type) {
		WARN("Failed to find 'authenticate %s {...}' section.  CHAP authentication will likely not work",
		     mctx->inst->name);
	}

	return 0;
}

static int mod_load(void)
{
	xlat_t	*xlat;

	if (unlikely((xlat = xlat_func_register(NULL, "chap_password", xlat_func_chap_password, FR_TYPE_OCTETS)) == NULL)) return -1;
	xlat_func_args_set(xlat, xlat_func_chap_password_args);

	return 0;
}

static void mod_unload(void)
{
	xlat_func_unregister("chap_password");
}

/*
 *	The module name should be the only globally exported symbol.
 *	That is, everything else should be 'static'.
 *
 *	If the module needs to temporarily modify it's instantiation
 *	data, the type should be changed to MODULE_TYPE_THREAD_UNSAFE.
 *	The server will then take care of ensuring that the module
 *	is single-threaded.
 */
extern module_rlm_t rlm_chap;
module_rlm_t rlm_chap = {
	.common = {
		.magic		= MODULE_MAGIC_INIT,
		.name		= "chap",
		.inst_size	= sizeof(rlm_chap_t),
		.onload		= mod_load,
		.unload		= mod_unload,
		.instantiate	= mod_instantiate
	},
	.dict		= &dict_radius,
	.method_names = (module_method_name_t[]){
		{ .name1 = "recv", 		.name2 = "access-request",	.method = mod_authorize    },
		{ .name1 = "authenticate",	.name2 = CF_IDENT_ANY,		.method = mod_authenticate    },
		MODULE_NAME_TERMINATOR
	}
};
