/*
 * Calculate IKEv2 prf and keying material, for libreswan
 *
 * Copyright (C) 2007 Michael C. Richardson <mcr@xelerance.com>
 * Copyright (C) 2010 Paul Wouters <paul@xelerance.com>
 * Copyright (C) 2013 D. Hugh Redelmeier <hugh@mimosa.com>
 * Copyright (C) 2015,2017 Andrew Cagney
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <http://www.fsf.org/copyleft/gpl.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * This code was developed with the support of Redhat corporation.
 *
 */

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <signal.h>

#include <libreswan.h>

#include "sysdep.h"
#include "constants.h"
#include "defs.h"
#include "packet.h"
#include "demux.h"
#include "crypto.h"
#include "rnd.h"
#include "state.h"
#include "pluto_crypt.h"
#include "lswlog.h"
#include "log.h"
#include "timer.h"
#include "ike_alg.h"
#include "id.h"
#include "keys.h"
#include "ikev2_prf.h"
#include "crypt_prf.h"
#include "crypt_dh.h"
#include "crypt_symkey.h"

/*
 * IKEv2 - RFC4306 2.14 SKEYSEED - calculation.
 */

/* MUST BE THREAD-SAFE */
static void calc_skeyseed_v2(struct pcr_skeyid_q *skq,
			     PK11SymKey *shared,
			     const size_t key_size,
			     const size_t salt_size,
			     PK11SymKey **SK_d_out,
			     PK11SymKey **SK_ai_out,
			     PK11SymKey **SK_ar_out,
			     PK11SymKey **SK_ei_out,
			     PK11SymKey **SK_er_out,
			     PK11SymKey **SK_pi_out,
			     PK11SymKey **SK_pr_out,
			     chunk_t *initiator_salt_out,
			     chunk_t *responder_salt_out,
			     chunk_t *chunk_SK_pi_out,
			     chunk_t *chunk_SK_pr_out)
{
	DBG(DBG_CRYPT, DBG_log("NSS: Started key computation"));

	PK11SymKey
		*skeyseed_k,
		*SK_d_k,
		*SK_ai_k,
		*SK_ar_k,
		*SK_ei_k,
		*SK_er_k,
		*SK_pi_k,
		*SK_pr_k;
	chunk_t initiator_salt;
	chunk_t responder_salt;
	chunk_t chunk_SK_pi;
	chunk_t chunk_SK_pr;

	/* this doesn't take any memory, it's just moving pointers around */
	chunk_t ni;
	chunk_t nr;
	chunk_t spii;
	chunk_t spir;
	setchunk_from_wire(ni, skq, &skq->ni);
	setchunk_from_wire(nr, skq, &skq->nr);
	setchunk_from_wire(spii, skq, &skq->icookie);
	setchunk_from_wire(spir, skq, &skq->rcookie);

	passert(skq->prf != NULL);
	DBG(DBG_CONTROLMORE,
	    DBG_log("calculating skeyseed using prf=%s integ=%s cipherkey-size=%zu salt-size=%zu",
		    skq->prf->common.name,
		    (skq->integ ? skq->integ->common.name : "n/a"),
		    key_size, salt_size));

	const struct prf_desc *prf = skq->prf;

	const struct encrypt_desc *encrypter = skq->encrypter;
	passert(encrypter != NULL);

	if (skq->skey_d_old == NULL) {
	/* generate SKEYSEED from key=(Ni|Nr), hash of shared */
		skeyseed_k = ikev2_ike_sa_skeyseed(skq->prf, ni, nr, shared);
	}  else {
		skeyseed_k = ikev2_ike_sa_rekey_skeyseed(skq->old_prf,
					skq->skey_d_old,
					shared, ni, nr);
		release_symkey(__func__, "parent-SK_d", &skq->skey_d_old);
	}

	passert(skeyseed_k != NULL);

	/* now we have to generate the keys for everything */

	/* need to know how many bits to generate */
	/* SK_d needs PRF hasher key bytes */
	/* SK_p needs PRF hasher*2 key bytes */
	/* SK_e needs key_size*2 key bytes */
	/* ..._salt needs salt_size*2 bytes */
	/* SK_a needs integ's key size*2 bytes */

	int skd_bytes = prf->prf_key_size;
	int skp_bytes = prf->prf_key_size;
	int integ_size = skq->integ ? skq->integ->integ_keymat_size : 0;
	size_t total_keysize = skd_bytes + 2*skp_bytes + 2*key_size + 2*salt_size + 2*integ_size;
	PK11SymKey *finalkey = ikev2_ike_sa_keymat(skq->prf, skeyseed_k,
						   ni, nr, spii, spir,
						   total_keysize);
	release_symkey(__func__, "skeyseed_k", &skeyseed_k);

	size_t next_byte = 0;

	SK_d_k = key_from_symkey_bytes(finalkey, next_byte, skd_bytes);
	next_byte += skd_bytes;

	SK_ai_k = key_from_symkey_bytes(finalkey, next_byte, integ_size);
	next_byte += integ_size;

	SK_ar_k = key_from_symkey_bytes(finalkey, next_byte, integ_size);
	next_byte += integ_size;

	/* The encryption key and salt are extracted together. */
	SK_ei_k = symkey_from_symkey_bytes("SK_ei_k", DBG_CRYPT,
					   &encrypter->common,
					   next_byte, key_size,
					   finalkey);
	next_byte += key_size;
	PK11SymKey *initiator_salt_key = key_from_symkey_bytes(finalkey, next_byte,
							       salt_size);
	initiator_salt = chunk_from_symkey("initiator salt", DBG_CRYPT,
					   initiator_salt_key);
	release_symkey(__func__, "initiator-salt-key", &initiator_salt_key);

	next_byte += salt_size;

	/* The encryption key and salt are extracted together. */
	SK_er_k = symkey_from_symkey_bytes("SK_er_k", DBG_CRYPT,
					   &encrypter->common,
					   next_byte, key_size,
					   finalkey);
	next_byte += key_size;
	PK11SymKey *responder_salt_key = key_from_symkey_bytes(finalkey, next_byte,
							       salt_size);
	responder_salt = chunk_from_symkey("responder salt", DBG_CRYPT,
					   responder_salt_key);
	release_symkey(__func__, "responder-salt-key", &responder_salt_key);
	next_byte += salt_size;

	SK_pi_k = key_from_symkey_bytes(finalkey, next_byte, skp_bytes);
	/* store copy of SK_pi_k for later use in authnull */
	chunk_SK_pi = chunk_from_symkey("chunk_SK_pi", DBG_CRYPT, SK_pi_k);
	next_byte += skp_bytes;

	SK_pr_k = key_from_symkey_bytes(finalkey, next_byte, skp_bytes);
	/* store copy of SK_pr_k for later use in authnull */
	chunk_SK_pr = chunk_from_symkey("chunk_SK_pr", DBG_CRYPT, SK_pr_k);
	next_byte += skp_bytes;	/* next_byte not subsequently used */

	DBG(DBG_CRYPT,
	    DBG_log("NSS ikev2: finished computing individual keys for IKEv2 SA"));
	release_symkey(__func__, "finalkey", &finalkey);

	passert(*SK_d_out == NULL);
	*SK_d_out = SK_d_k;
	passert(*SK_ai_out == NULL);
	*SK_ai_out = SK_ai_k;
	passert(*SK_ar_out == NULL);
	*SK_ar_out = SK_ar_k;
	passert(*SK_ei_out == NULL);
	*SK_ei_out = SK_ei_k;
	passert(*SK_er_out == NULL);
	*SK_er_out = SK_er_k;
	passert(*SK_pi_out == NULL);
	*SK_pi_out = SK_pi_k;
	passert(*SK_pr_out == NULL);
	*SK_pr_out = SK_pr_k;

	*initiator_salt_out = initiator_salt;
	*responder_salt_out = responder_salt;
	*chunk_SK_pi_out = chunk_SK_pi;
	*chunk_SK_pr_out = chunk_SK_pr;

	DBG(DBG_CRYPT,
	    DBG_log("calc_skeyseed_v2 pointers: shared-key@%p, SK_d-key@%p, SK_ai-key@%p, SK_ar-key@%p, SK_ei-key@%p, SK_er-key@%p, SK_pi-key@%p, SK_pr-key@%p",
		    shared, SK_d_k, SK_ai_k, SK_ar_k, SK_ei_k, SK_er_k, SK_pi_k, SK_pr_k);
	    DBG_dump_chunk("calc_skeyseed_v2 initiator salt", initiator_salt);
	    DBG_dump_chunk("calc_skeyseed_v2 responder salt", responder_salt);
	    DBG_dump_chunk("calc_skeyseed_v2 SK_pi", chunk_SK_pi);
	    DBG_dump_chunk("calc_skeyseed_v2 SK_pr", chunk_SK_pr));
}

/* NOTE: if NSS refuses to calculate DH, skr->shared == NULL */
/* MUST BE THREAD-SAFE */
void calc_dh_v2(struct pluto_crypto_req *r)
{
	struct pcr_skeycalc_v2_r *const skr = &r->pcr_d.dhv2;

	/* copy the request, since the reply will re-use the memory of the r->pcr_d.dhq */
	struct pcr_skeyid_q dhq;
	memcpy(&dhq, &r->pcr_d.dhq, sizeof(r->pcr_d.dhq));

	/* clear out the reply (including pointers) */
	static const struct pcr_skeycalc_v2_r zero_pcr_skeycalc_v2_r;
	*skr = zero_pcr_skeycalc_v2_r;
	INIT_WIRE_ARENA(*skr);

	const struct oakley_group_desc *group = dhq.oakley_group;
	passert(group != NULL);

	SECKEYPrivateKey *ltsecret = dhq.secret;
	SECKEYPublicKey *pubk = dhq.pubk;

	/* now calculate the (g^x)(g^y) --- need gi on responder, gr on initiator */

	chunk_t g;
	setchunk_from_wire(g, &dhq, dhq.role == ORIGINAL_RESPONDER ? &dhq.gi : &dhq.gr);

	DBG(DBG_CRYPT, DBG_dump_chunk("peer's g: ", g));

	skr->shared = calc_dh_shared(g, ltsecret, group, pubk);

	if (skr->shared != NULL) {
	/* okay, so now all the shared key material */
	calc_skeyseed_v2(&dhq,  /* input */
		skr->shared,   /* input */
		dhq.key_size,  /* input */
		dhq.salt_size, /* input */

		&skr->skeyid_d,        /* output */
		&skr->skeyid_ai,       /* output */
		&skr->skeyid_ar,       /* output */
		&skr->skeyid_ei,       /* output */
		&skr->skeyid_er,       /* output */
		&skr->skeyid_pi,       /* output */
		&skr->skeyid_pr,       /* output */
		&skr->skey_initiator_salt, /* output */
		&skr->skey_responder_salt, /* output */
		&skr->skey_chunk_SK_pi, /* output */
		&skr->skey_chunk_SK_pr); /* output */
	}
}

static PK11SymKey *ikev2_prfplus(const struct prf_desc *prf_desc,
				 PK11SymKey *key, PK11SymKey *seed,
				 size_t required_keymat)
{
	uint8_t count = 1;

	/* T1(prfplus) = prf(KEY, SEED|1) */
	PK11SymKey *prfplus;
	{
		struct crypt_prf *prf = crypt_prf_init_symkey("prf+0", DBG_CRYPT,
							      prf_desc, "key", key);
		crypt_prf_update_symkey("seed", prf, seed);
		crypt_prf_update_byte("1++", prf, count++);
		prfplus = crypt_prf_final_symkey(&prf);
	}

	/* make a copy to keep things easy */
	PK11SymKey *old_t = reference_symkey(__func__, "old_t[1]", prfplus);
	while (sizeof_symkey(prfplus) < required_keymat) {
		/* Tn = prf(KEY, Tn-1|SEED|n) */
		struct crypt_prf *prf = crypt_prf_init_symkey("prf+N", DBG_CRYPT,
							      prf_desc, "key", key);
		crypt_prf_update_symkey("old_t", prf, old_t);
		crypt_prf_update_symkey("seed", prf, seed);
		crypt_prf_update_byte("N++", prf, count++);
		PK11SymKey *new_t = crypt_prf_final_symkey(&prf);
		append_symkey_symkey(&prfplus, new_t);
		release_symkey(__func__, "old_t[N]", &old_t);
		old_t = new_t;
	}
	release_symkey(__func__, "old_t[final]", &old_t);
	return prfplus;
}

/*
 * SKEYSEED = prf(Ni | Nr, g^ir)
 */
PK11SymKey *ikev2_ike_sa_skeyseed(const struct prf_desc *prf_desc,
				  const chunk_t Ni, const chunk_t Nr,
				  PK11SymKey *dh_secret)
{
	/* key = Ni|Nr */
	chunk_t key = concat_chunk_chunk("key = Ni|Nr", Ni, Nr);
	struct crypt_prf *prf = crypt_prf_init_chunk("ike sa SKEYSEED",
						     DBG_CRYPT,
						     prf_desc,
						     "Ni|Nr", key);
	freeanychunk(key);
	if (prf == NULL) {
		libreswan_log("failed to create IKEv2 PRF for computing SKEYSEED = prf(Ni | Nr, g^ir)");
		return NULL;
	}
	/* seed = g^ir */
	crypt_prf_update_symkey("g^ir", prf, dh_secret);
	/* generate */
	return crypt_prf_final_symkey(&prf);
}

/*
 * SKEYSEED = prf(SK_d (old), g^ir (new) | Ni | Nr)
 */
PK11SymKey *ikev2_ike_sa_rekey_skeyseed(const struct prf_desc *prf_desc,
					PK11SymKey *SK_d_old,
					PK11SymKey *new_dh_secret,
					const chunk_t Ni, const chunk_t Nr)
{
	/* key = SK_d (old) */
	struct crypt_prf *prf = crypt_prf_init_symkey("ike sa rekey skeyseed",
						      DBG_CRYPT, prf_desc,
						      "SK_d (old)", SK_d_old);
	if (prf == NULL) {
		libreswan_log("failed to create IKEv2 PRF for computing SKEYSEED = prf(SK_d (old), g^ir (new) | Ni | Nr)");
		return NULL;
	}

	/* seed: g^ir (new) | Ni | Nr) */
	crypt_prf_update_symkey("g^ir (new)", prf, new_dh_secret);
	crypt_prf_update_chunk("Ni", prf, Ni);
	crypt_prf_update_chunk("Nr", prf, Nr);
	/* generate */
	return crypt_prf_final_symkey(&prf);
}

/*
 * Compute: prf+ (SKEYSEED, Ni | Nr | SPIi | SPIr)
 */
PK11SymKey *ikev2_ike_sa_keymat(const struct prf_desc *prf_desc,
				PK11SymKey *skeyseed,
				const chunk_t Ni, const chunk_t Nr,
				const chunk_t SPIi, const chunk_t SPIr,
				size_t required_bytes)
{
	PK11SymKey *data = symkey_from_chunk("data", DBG_CRYPT, NULL, Ni);
	append_symkey_chunk(&data, Nr);
	append_symkey_chunk(&data, SPIi);
	append_symkey_chunk(&data, SPIr);
	PK11SymKey *prfplus = ikev2_prfplus(prf_desc,
					    skeyseed, data,
					    required_bytes);
	release_symkey(__func__, "data", &data);
	return prfplus;
}

/*
 * Compute: prf+(SK_d, [ g^ir (new) | ] Ni | Nr)
 */
PK11SymKey *ikev2_child_sa_keymat(const struct prf_desc *prf_desc,
				  PK11SymKey *SK_d,
				  PK11SymKey *new_dh_secret,
				  const chunk_t Ni, const chunk_t Nr,
				  size_t required_bytes)
{
	PK11SymKey *data;
	if (new_dh_secret == NULL) {
		data = symkey_from_chunk("data", DBG_CRYPT, NULL, Ni);
		append_symkey_chunk(&data, Nr);
	} else {
		data = concat_symkey_chunk(new_dh_secret, Ni);
		append_symkey_chunk(&data, Nr);
	}
	PK11SymKey *prfplus = ikev2_prfplus(prf_desc,
					    SK_d, data,
					    required_bytes);
	release_symkey(__func__, "data", &data);
	return prfplus;
}
