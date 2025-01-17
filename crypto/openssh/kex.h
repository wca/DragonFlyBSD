/* $OpenBSD: kex.h,v 1.64 2014/05/02 03:27:54 djm Exp $ */

/*
 * Copyright (c) 2000, 2001 Markus Friedl.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#ifndef KEX_H
#define KEX_H

#include <signal.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#ifdef OPENSSL_HAS_ECC
#include <openssl/ec.h>
#endif

#define KEX_COOKIE_LEN	16

#define	KEX_DH1			"diffie-hellman-group1-sha1"
#define	KEX_DH14		"diffie-hellman-group14-sha1"
#define	KEX_DHGEX_SHA1		"diffie-hellman-group-exchange-sha1"
#define	KEX_DHGEX_SHA256	"diffie-hellman-group-exchange-sha256"
#define	KEX_RESUME		"resume@appgate.com"
#define	KEX_ECDH_SHA2_NISTP256	"ecdh-sha2-nistp256"
#define	KEX_ECDH_SHA2_NISTP384	"ecdh-sha2-nistp384"
#define	KEX_ECDH_SHA2_NISTP521	"ecdh-sha2-nistp521"
#define	KEX_CURVE25519_SHA256	"curve25519-sha256@libssh.org"

#define COMP_NONE	0
#define COMP_ZLIB	1
#define COMP_DELAYED	2

enum kex_init_proposals {
	PROPOSAL_KEX_ALGS,
	PROPOSAL_SERVER_HOST_KEY_ALGS,
	PROPOSAL_ENC_ALGS_CTOS,
	PROPOSAL_ENC_ALGS_STOC,
	PROPOSAL_MAC_ALGS_CTOS,
	PROPOSAL_MAC_ALGS_STOC,
	PROPOSAL_COMP_ALGS_CTOS,
	PROPOSAL_COMP_ALGS_STOC,
	PROPOSAL_LANG_CTOS,
	PROPOSAL_LANG_STOC,
	PROPOSAL_MAX
};

enum kex_modes {
	MODE_IN,
	MODE_OUT,
	MODE_MAX
};

enum kex_exchange {
	KEX_DH_GRP1_SHA1,
	KEX_DH_GRP14_SHA1,
	KEX_DH_GEX_SHA1,
	KEX_DH_GEX_SHA256,
	KEX_ECDH_SHA2,
	KEX_C25519_SHA256,
	KEX_MAX
};

#define KEX_INIT_SENT	0x0001

typedef struct Kex Kex;
typedef struct Mac Mac;
typedef struct Comp Comp;
typedef struct Enc Enc;
typedef struct Newkeys Newkeys;

struct Enc {
	char	*name;
	const Cipher *cipher;
	int	enabled;
	u_int	key_len;
	u_int	iv_len;
	u_int	block_size;
	u_char	*key;
	u_char	*iv;
};
struct Mac {
	char	*name;
	int	enabled;
	u_int	mac_len;
	u_char	*key;
	u_int	key_len;
	int	type;
	int	etm;		/* Encrypt-then-MAC */
	struct ssh_hmac_ctx	*hmac_ctx;
	struct umac_ctx		*umac_ctx;
};
struct Comp {
	int	type;
	int	enabled;
	char	*name;
};
struct Newkeys {
	Enc	enc;
	Mac	mac;
	Comp	comp;
};
struct Kex {
	u_char	*session_id;
	u_int	session_id_len;
	Newkeys	*newkeys[MODE_MAX];
	u_int	we_need;
	u_int	dh_need;
	int	server;
	char	*name;
	int	hostkey_type;
	int	kex_type;
	int	roaming;
	Buffer	my;
	Buffer	peer;
	sig_atomic_t done;
	int	flags;
	int	hash_alg;
	int	ec_nid;
	char	*client_version_string;
	char	*server_version_string;
	int	(*verify_host_key)(Key *);
	Key	*(*load_host_public_key)(int);
	Key	*(*load_host_private_key)(int);
	int	(*host_key_index)(Key *);
	void    (*sign)(Key *, Key *, u_char **, u_int *, u_char *, u_int);
	void	(*kex[KEX_MAX])(Kex *);
};

int	 kex_names_valid(const char *);
char	*kex_alg_list(char);

void	 kex_prop2buf(Buffer *, char *proposal[PROPOSAL_MAX]);

Kex	*kex_setup(char *[PROPOSAL_MAX]);
void	 kex_finish(Kex *);

void	 kex_send_kexinit(Kex *);
void	 kex_input_kexinit(int, u_int32_t, void *);
void	 kex_derive_keys(Kex *, u_char *, u_int, const u_char *, u_int);
void	 kex_derive_keys_bn(Kex *, u_char *, u_int, const BIGNUM *);

Newkeys *kex_get_newkeys(int);

void	 kexdh_client(Kex *);
void	 kexdh_server(Kex *);
void	 kexgex_client(Kex *);
void	 kexgex_server(Kex *);
void	 kexecdh_client(Kex *);
void	 kexecdh_server(Kex *);
void	 kexc25519_client(Kex *);
void	 kexc25519_server(Kex *);

void
kex_dh_hash(char *, char *, char *, int, char *, int, u_char *, int,
    BIGNUM *, BIGNUM *, BIGNUM *, u_char **, u_int *);
void
kexgex_hash(int, char *, char *, char *, int, char *,
    int, u_char *, int, int, int, int, BIGNUM *, BIGNUM *, BIGNUM *,
    BIGNUM *, BIGNUM *, u_char **, u_int *);
#ifdef OPENSSL_HAS_ECC
void
kex_ecdh_hash(int, const EC_GROUP *, char *, char *, char *, int,
    char *, int, u_char *, int, const EC_POINT *, const EC_POINT *,
    const BIGNUM *, u_char **, u_int *);
#endif
void
kex_c25519_hash(int, char *, char *, char *, int,
    char *, int, u_char *, int, const u_char *, const u_char *,
    const u_char *, u_int, u_char **, u_int *);

#define CURVE25519_SIZE 32
void	kexc25519_keygen(u_char[CURVE25519_SIZE], u_char[CURVE25519_SIZE])
	__attribute__((__bounded__(__minbytes__, 1, CURVE25519_SIZE)))
	__attribute__((__bounded__(__minbytes__, 2, CURVE25519_SIZE)));
void kexc25519_shared_key(const u_char key[CURVE25519_SIZE],
    const u_char pub[CURVE25519_SIZE], Buffer *out)
	__attribute__((__bounded__(__minbytes__, 1, CURVE25519_SIZE)))
	__attribute__((__bounded__(__minbytes__, 2, CURVE25519_SIZE)));

void
derive_ssh1_session_id(BIGNUM *, BIGNUM *, u_int8_t[8], u_int8_t[16]);

#if defined(DEBUG_KEX) || defined(DEBUG_KEXDH) || defined(DEBUG_KEXECDH)
void	dump_digest(char *, u_char *, int);
#endif

#endif
