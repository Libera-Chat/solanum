/*
 *  ircd-ratbox: A slightly useful ircd.
 *  m_challenge.c: Allows an IRC Operator to securely authenticate.
 *
 *  Copyright (C) 1990 Jarkko Oikarinen and University of Oulu, Co Center
 *  Copyright (C) 1996-2002 Hybrid Development Team
 *  Copyright (C) 2002-2005 ircd-ratbox development team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307
 *  USA
 */

#include "stdinc.h"

#ifdef HAVE_LIBCRYPTO
#include <openssl/hmac.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/md5.h>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/err.h>
#endif

#include "client.h"
#include "ircd.h"
#include "modules.h"
#include "numeric.h"
#include "send.h"
#include "s_conf.h"
#include "msg.h"
#include "parse.h"
#include "match.h"
#include "logger.h"
#include "s_user.h"
#include "cache.h"
#include "s_newconf.h"

#define CHALLENGE_WIDTH BUFSIZE - (NICKLEN + HOSTLEN + 12)
#define CHALLENGE_EXPIRES	180	/* 180 seconds should be more than long enough */
#define CHALLENGE_SECRET_LENGTH	128	/* how long our challenge secret should be */

#ifndef HAVE_LIBCRYPTO

static const char challenge_desc[] = "Does nothing as OpenSSL was not enabled.";

/* Maybe this should be an error or something?-davidt */
/* now it is	-larne	*/
static int challenge_load(void)
{
	sendto_realops_snomask(SNO_GENERAL, L_NETWIDE,
		"Challenge module not loaded because OpenSSL is not available.");
	ilog(L_MAIN, "Challenge module not loaded because OpenSSL is not available.");
	return -1;
}

DECLARE_MODULE_AV2(challenge, challenge_load, NULL, NULL, NULL, NULL, NULL, NULL, challenge_desc);
#else

static const char challenge_desc[] =
	"Provides the challenge-response facility used for becoming an IRC operator";

static void m_challenge(struct MsgBuf *, struct Client *, struct Client *, int, const char **);

/* We have openssl support, so include /CHALLENGE */
struct Message challenge_msgtab = {
	"CHALLENGE", 0, 0, 0, 0,
	{mg_unreg, {m_challenge, 2}, mg_ignore, mg_ignore, mg_ignore, {m_challenge, 2}}
};

mapi_clist_av1 challenge_clist[] = { &challenge_msgtab, NULL };

DECLARE_MODULE_AV2(challenge, NULL, NULL, challenge_clist, NULL, NULL, NULL, NULL, challenge_desc);

static bool generate_challenge(char **r_challenge, char **r_response, RSA * key);
static bool generate_challenge_gen(char const* token, int id, EVP_MD const* md, EVP_PKEY *pub, char **r_challenge, char **r_response);

static void
cleanup_challenge(struct Client *target_p)
{
	if(target_p->localClient == NULL)
		return;

	rb_free(target_p->localClient->challenge);
	rb_free(target_p->user->opername);
	target_p->localClient->challenge = NULL;
	target_p->user->opername = NULL;
	target_p->localClient->chal_time = 0;
}

/*
 * m_challenge - generate RSA challenge for wouldbe oper
 * parv[1] = operator to challenge for, or +response
 */
static void
m_challenge(struct MsgBuf *msgbuf_p, struct Client *client_p, struct Client *source_p, int parc, const char *parv[])
{
	struct oper_conf *oper_p;
	char *challenge = NULL; /* to placate gcc */
	char chal_line[CHALLENGE_WIDTH];
	size_t cnt;
	int len = 0;

        if (ConfigFileEntry.oper_secure_only && !IsSecureClient(source_p))
        {
                sendto_one_notice(source_p, ":You must be using a secure connection to /CHALLENGE on this server");
                if (ConfigFileEntry.failed_oper_notice)
                {
                        sendto_realops_snomask(SNO_GENERAL, L_NETWIDE,
					"Failed CHALLENGE attempt - missing secure connection by %s (%s@%s)",
					source_p->name, source_p->username, source_p->host);
                }
                return;
        }

	/* if theyre an oper, reprint oper motd and ignore */
	if(IsOper(source_p))
	{
		sendto_one(source_p, form_str(RPL_YOUREOPER), me.name, source_p->name);
		send_oper_motd(source_p);
		return;
	}

	if(*parv[1] == '+')
	{
		/* Ignore it if we aren't expecting this... -A1kmm */
		if(!source_p->localClient->challenge)
			return;

		if((rb_current_time() - source_p->localClient->chal_time) > CHALLENGE_EXPIRES)
		{
			sendto_one(source_p, form_str(ERR_PASSWDMISMATCH), me.name, source_p->name);
			ilog(L_FOPER, "EXPIRED CHALLENGE (%s) by (%s!%s@%s) (%s)",
			     source_p->user->opername, source_p->name,
			     source_p->username, source_p->host, source_p->sockhost);

			if(ConfigFileEntry.failed_oper_notice)
				sendto_realops_snomask(SNO_GENERAL, L_NETWIDE,
						     "Expired CHALLENGE attempt by %s (%s@%s)",
						     source_p->name, source_p->username,
						     source_p->host);
			cleanup_challenge(source_p);
			return;
		}

		parv[1]++;
		if(strcmp(source_p->localClient->challenge, parv[1]))
		{
			sendto_one(source_p, form_str(ERR_PASSWDMISMATCH), me.name, source_p->name);
			ilog(L_FOPER, "FAILED CHALLENGE (%s) by (%s!%s@%s) (%s)",
			     source_p->user->opername, source_p->name,
			     source_p->username, source_p->host, source_p->sockhost);

			if(ConfigFileEntry.failed_oper_notice)
				sendto_realops_snomask(SNO_GENERAL, L_NETWIDE,
						     "Failed CHALLENGE attempt by %s (%s@%s)",
						     source_p->name, source_p->username,
						     source_p->host);

			cleanup_challenge(source_p);
			return;
		}

		oper_p = find_oper_conf(source_p->username, source_p->orighost,
					source_p->sockhost,
					source_p->user->opername);

		if(oper_p == NULL)
		{
			sendto_one_numeric(source_p, ERR_NOOPERHOST, form_str(ERR_NOOPERHOST));
			ilog(L_FOPER, "FAILED OPER (%s) by (%s!%s@%s) (%s)",
			     source_p->user->opername, source_p->name,
			     source_p->username, source_p->host,
			     source_p->sockhost);

			if(ConfigFileEntry.failed_oper_notice)
				sendto_realops_snomask(SNO_GENERAL, L_NETWIDE,
						     "Failed CHALLENGE attempt - host mismatch by %s (%s@%s)",
						     source_p->name, source_p->username,
						     source_p->host);
			cleanup_challenge(source_p);
			return;
		}

		cleanup_challenge(source_p);

		oper_up(source_p, oper_p);

		ilog(L_OPERED, "OPER %s by %s!%s@%s (%s)",
		     source_p->user->opername, source_p->name,
		     source_p->username, source_p->host, source_p->sockhost);
		return;
	}

	cleanup_challenge(source_p);

	oper_p = find_oper_conf(source_p->username, source_p->orighost,
				source_p->sockhost, parv[1]);

	if(oper_p == NULL)
	{
		sendto_one_numeric(source_p, ERR_NOOPERHOST, form_str(ERR_NOOPERHOST));
		ilog(L_FOPER, "FAILED OPER (%s) by (%s!%s@%s) (%s)",
		     parv[1], source_p->name,
		     source_p->username, source_p->host, source_p->sockhost);

		if(ConfigFileEntry.failed_oper_notice)
			sendto_realops_snomask(SNO_GENERAL, L_NETWIDE,
					     "Failed CHALLENGE attempt - host mismatch by %s (%s@%s)",
					     source_p->name, source_p->username, source_p->host);
		return;
	}

	if(!oper_p->rsa_pubkey && !oper_p->x25519_pubkey)
	{
		sendto_one_notice(source_p, ":I'm sorry, PK authentication is not enabled for your oper{} block.");
		return;
	}

	if(IsOperConfNeedSSL(oper_p) && !IsSecureClient(source_p))
	{
		sendto_one_numeric(source_p, ERR_NOOPERHOST, form_str(ERR_NOOPERHOST));
		ilog(L_FOPER, "FAILED CHALLENGE (%s) by (%s!%s@%s) (%s) -- requires SSL/TLS",
		     parv[1], source_p->name, source_p->username, source_p->host,
		     source_p->sockhost);

		if(ConfigFileEntry.failed_oper_notice)
		{
			sendto_realops_snomask(SNO_GENERAL, L_NETWIDE,
					     "Failed CHALLENGE attempt - missing SSL/TLS by %s (%s@%s)",
					     source_p->name, source_p->username, source_p->host);
		}
		return;
	}

	if (oper_p->certfp != NULL)
	{
		if (source_p->certfp == NULL || rb_strcasecmp(source_p->certfp, oper_p->certfp))
		{
			sendto_one_numeric(source_p, ERR_NOOPERHOST, form_str(ERR_NOOPERHOST));
			ilog(L_FOPER, "FAILED OPER (%s) by (%s!%s@%s) (%s) -- client certificate fingerprint mismatch",
			     parv[1], source_p->name,
			     source_p->username, source_p->host, source_p->sockhost);

			if(ConfigFileEntry.failed_oper_notice)
			{
				sendto_realops_snomask(SNO_GENERAL, L_NETWIDE,
						     "Failed OPER attempt - client certificate fingerprint mismatch by %s (%s@%s)",
						     source_p->name, source_p->username, source_p->host);
			}
			return;
		}
	}

	bool challenge_generated
		= oper_p->x25519_pubkey
		? generate_challenge_gen(
			"solanum-challenge v1-x25519-sha256",
			EVP_PKEY_X25519,
			EVP_sha256(),
			oper_p->x25519_pubkey,
			&challenge, &(source_p->localClient->challenge))
		: generate_challenge(&challenge, &(source_p->localClient->challenge), oper_p->rsa_pubkey);
	if(challenge_generated)
	{
		char *chal = challenge;
		source_p->localClient->chal_time = rb_current_time();
		for(;;)
		{
			cnt = rb_strlcpy(chal_line, chal, CHALLENGE_WIDTH);
			sendto_one(source_p, form_str(RPL_RSACHALLENGE2), me.name, source_p->name, chal_line);
			if(cnt >= CHALLENGE_WIDTH)
				chal += CHALLENGE_WIDTH - 1;
			else
				break;

		}
		sendto_one(source_p, form_str(RPL_ENDOFRSACHALLENGE2),
			   me.name, source_p->name);
		rb_free(challenge);
		source_p->user->opername = rb_strdup(oper_p->name);
	}
	else
		sendto_one_notice(source_p, ":Failed to generate challenge.");
}

static void
report_openssl_errors(void)
{
	ERR_load_crypto_strings();
	for (int e, cnt = 0; (cnt < 100) && (e = ERR_get_error()); cnt++)
	{
		ilog(L_MAIN, "SSL error: %s", ERR_error_string(e, 0));
	}
}

static bool
generate_challenge_gen(char const* token, int id, EVP_MD const* md, EVP_PKEY *pub, char **r_challenge, char **r_response)
{
	EVP_PKEY *pkey = NULL;
	EVP_PKEY_CTX *ctx = NULL;
	unsigned char *epub = NULL;
	unsigned char *shared = NULL;
	size_t epub_len, shared_len;
	unsigned char const* d; // will point to static buffer - do not free
	unsigned int d_len;
	bool success = false;

	// Generate ephemeral key
	if (NULL == (ctx = EVP_PKEY_CTX_new_id(id, NULL))) goto done;
	if (1 != EVP_PKEY_keygen_init(ctx)) goto done;
	if (1 != EVP_PKEY_keygen(ctx, &pkey)) goto done;
	EVP_PKEY_CTX_free(ctx);

	// Setup shared secret derivation	
	if (NULL == (ctx = EVP_PKEY_CTX_new(pkey, NULL))) goto done;
	if (1 != EVP_PKEY_derive_init(ctx)) goto done;
	if (1 != EVP_PKEY_derive_set_peer(ctx, pub)) goto done;

	// Extract shared secret bytes
	if (1 != EVP_PKEY_derive(ctx, NULL, &shared_len)) goto done;
	if (NULL == (shared = rb_malloc(shared_len))) goto done;
	if (1 != EVP_PKEY_derive(ctx, shared, &shared_len)) goto done;

	// Extract public key bytes
	if (1 != EVP_PKEY_get_raw_public_key(pkey, NULL, &epub_len)) goto done;
	if (NULL == (epub = rb_malloc(epub_len))) goto done;
	if (1 != EVP_PKEY_get_raw_public_key(pkey, epub, &epub_len)) goto done;

	// Compute raw expected response
	if (NULL == (d = HMAC(md, shared, shared_len, token, strlen(token), NULL, &d_len))) goto done;

	// Success - report challenge and expected response in base64
	success = true;
	*r_challenge = rb_base64_encode(epub, epub_len);
	*r_response = rb_base64_encode(d, d_len);

done:
	if (!success) report_openssl_errors();

	rb_free(epub);
	rb_free(shared);
	EVP_PKEY_CTX_free(ctx);
	EVP_PKEY_free(pkey);
	return success;
}

static bool
generate_challenge(char **r_challenge, char **r_response, RSA * rsa)
{
	SHA_CTX ctx;
	unsigned char secret[CHALLENGE_SECRET_LENGTH], *tmp;
	unsigned long length;
	unsigned long e = 0;
	unsigned long cnt = 0;
	int ret;

	if(!rsa)
		return false;
	if(rb_get_random(secret, CHALLENGE_SECRET_LENGTH))
	{
		unsigned char md[SHA_DIGEST_LENGTH];
		SHA1_Init(&ctx);
		SHA1_Update(&ctx, (uint8_t *)secret, CHALLENGE_SECRET_LENGTH);
		SHA1_Final(md, &ctx);
		*r_response = rb_base64_encode(md, sizeof md);

		length = RSA_size(rsa);
		tmp = rb_malloc(length);
		ret = RSA_public_encrypt(CHALLENGE_SECRET_LENGTH, secret, tmp, rsa, RSA_PKCS1_OAEP_PADDING);

		if(ret >= 0)
		{
			*r_challenge = (char *)rb_base64_encode(tmp, ret);
			rb_free(tmp);
			return true;
		}

		rb_free(tmp);
		rb_free(*r_response);
		*r_response = NULL;
	}

	report_openssl_errors();
	return false;
}

#endif /* HAVE_LIBCRYPTO */
