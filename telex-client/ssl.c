#include <event2/dns.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <event2/util.h>
#include <event2/listener.h>
#include <event2/event.h>
#include <event2/bufferevent_ssl.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/rand.h>

#include "logger.h"
#include "ssl.h"
#include "tag/tag.h"
#include "util.h"

#ifdef PUBKEY_DATA_
#  include "pubkey_data_.h"
#endif
#ifdef ROOTPEM_DATA_
#  include "rootpem_data_.h"
#endif

#include <assert.h>
#include <stdint.h>

int ssl_log_errors(enum LogLevel level, const char *name)
{	
	int count = 0;
	int err;
	while ((err = ERR_get_error())) {		
		const char *msg = (const char*)ERR_reason_error_string(err);
		const char *lib = (const char*)ERR_lib_error_string(err);
		const char *func = (const char*)ERR_func_error_string(err);
		LogLog(level, name, "%s in %s %s\n", msg, lib, func);
		count++;
	}
	return count;
}

// Needed for peer_dh_tmp
typedef struct cert_pkey_st
    {
    X509 *x509;
    EVP_PKEY *privatekey;
    } CERT_PKEY;

#define SSL_PKEY_NUM 8
typedef struct sess_cert_st
    {
    STACK_OF(X509) *cert_chain; /* as received from peer (not for SSL2) */

    /* The 'peer_...' members are used only by clients. */
    int peer_cert_type;

    CERT_PKEY *peer_key; /* points to an element of peer_pkeys (never NULL!) */
    CERT_PKEY peer_pkeys[SSL_PKEY_NUM];
    /* Obviously we don't have the private keys of these,
     * so maybe we shouldn't even use the CERT_PKEY type here. */

#ifndef OPENSSL_NO_RSA
    RSA *peer_rsa_tmp; /* not used for SSL 2 */
#endif
#ifndef OPENSSL_NO_DH
    DH *peer_dh_tmp; /* not used for SSL 2 */
#endif
#ifndef OPENSSL_NO_ECDH
    EC_KEY *peer_ecdh_tmp;
#endif

    int references; /* actually always 1 at the moment */
    } SESS_CERT;


//modified from crypto/dh/dh_key.c (plz to make non-static)
int __generate_key(DH *dh)
{
    int ok=0;
    BN_CTX *ctx;
    BN_MONT_CTX *mont=NULL;
    BIGNUM *pub_key=NULL, *priv_key=NULL;
    

    ctx = BN_CTX_new();
    if (ctx == NULL) goto err;

    priv_key = dh->priv_key;    

    if (dh->pub_key == NULL) {
        pub_key = BN_new();
    } else {
        pub_key = dh->pub_key;
    }


    if (dh->flags & DH_FLAG_CACHE_MONT_P)
        {
        mont = BN_MONT_CTX_set_locked(&dh->method_mont_p,
                CRYPTO_LOCK_DH, dh->p, ctx);
        if (!mont)
            goto err;
        }

    
    {
        BIGNUM local_prk;
        BIGNUM *prk;

        if ((dh->flags & DH_FLAG_NO_EXP_CONSTTIME) == 0)
            {
            BN_init(&local_prk);
            prk = &local_prk;
            BN_with_flags(prk, priv_key, BN_FLG_CONSTTIME);
            }
        else
            prk = priv_key;

        if (!dh->meth->bn_mod_exp(dh, pub_key, dh->g, prk, dh->p, ctx, mont)) goto err;
    }

    dh->pub_key=pub_key;
    dh->priv_key=priv_key;
    ok=1;
err:
    if (ok != 1)
        DHerr(DH_F_GENERATE_KEY,ERR_R_BN_LIB);

    if ((pub_key != NULL)  && (dh->pub_key == NULL))  BN_free(pub_key);
    if ((priv_key != NULL) && (dh->priv_key == NULL)) BN_free(priv_key);
    BN_CTX_free(ctx);
    return(ok); 
}

// Replaces the generate_key function, called by DH_generate_key(dh_clnt)
// We fill the priv_key with 
int ssl_fake_DH_gen_key(DH *dh)
{
    // Grab the state pointer out of the DH g param
    struct telex_state *state; 
    char *str = BN_bn2hex((const BIGNUM*)dh->g);
    if (str) {
        LogTrace("ssl", "ssl_fake_DH_gen_key: %s", str);
        OPENSSL_free(str);
    }else {
        LogTrace("ssl", "ssl_fake_DH_gen_key null dh");
    }
    //BN_bn2bin(dh->g, (unsigned char *)&state);
    state = (struct telex_state*)BN_get_word(dh->g);
    LogTrace("ssl", "ssl_fake_DH_gen_key %p, state %p", dh, state);

    // Put the original back
    BN_free(dh->g); 
    dh->g = BN_dup((const BIGNUM *)state->hack_tmp_g);

    // Fill in our secret
    dh->priv_key = telex_ssl_get_dh_key(state->secret, NULL);

    return __generate_key(dh);
}

void ssl_info_callback(const SSL *ssl, int where, int ret)
{
    struct telex_state *state;
    if (!ssl) {
        return;
    }
    
    state = ssl->msg_callback_arg;
    LogTrace("ssl", "ssl_info_callback [%s]", state->name);

    if (where == SSL_CB_HANDSHAKE_START) { 

        //ssl->s3 has been cleared - store a pointer
        // to ssl parent obj in its server_random
        LogTrace("ssl", "ssl_info_callback (start) %p, arg %p", ssl, state);
        memcpy(ssl->s3->server_random, &state, sizeof(struct telex_state*));
        // Next callback is ssl_fake_pseudorandom (RAND_pseudo_bytes called by client_hello) 

    } else if (where == SSL_CB_CONNECT_LOOP) {
        LogTrace("ssl", "ssl_info_callback (loop)");
        if (ssl->session && ssl->session->sess_cert) {
            DH *dh_srvr = ssl->session->sess_cert->peer_dh_tmp;

            if (dh_srvr && !dh_srvr->priv_key && !state->hack_tmp_g) {
                // We have received a server key exchange (diffie hellman)
                // The dh_srvr parameters (p,g) will be copied to dh_clnt
                // We will hook the generate_key function, and put our state arg
                // as a BIGNUM in dh_srvr->g. 

                // Hook the function
                ((DH_METHOD*)dh_srvr->meth)->generate_key = ssl_fake_DH_gen_key;

                state->hack_tmp_g = BN_dup(dh_srvr->g); // Store the old g
                BN_free(dh_srvr->g);

                // Store our state
                dh_srvr->g = BN_new();
                BN_set_word(dh_srvr->g, (unsigned long)state);

                char *str = BN_bn2hex((const BIGNUM*)dh_srvr->g);
                LogTrace("ssl", "[%s] SET+++++ %p %p [%s]", state->name, dh_srvr->g, state, str);
                OPENSSL_free(str);
            }
        }
    }
}

int (*ssl_real_pseudorand_fn)(unsigned char *buf, int num);
// must be accessible to our function below (not sure we ever call this)

// for the client random (tag)
int ssl_fake_pseudorand(unsigned char *buf, int num)
{
    struct telex_state *state;  // this was stored in server_random
    struct ssl3_state_st *tmp_s3;
    unsigned char *tmp_client_random;

    if (num == 28) {
        // This is client_hello asking for a client_random (we think)
        tmp_client_random = (unsigned char*)(buf - 4);
        tmp_s3 = (struct ssl3_state_st*)
                ((char*)tmp_client_random - offsetof(struct ssl3_state_st, client_random));

        // Filled by ssl_info_callback
        memcpy(&state, tmp_s3->server_random, sizeof(struct telex_state*));

        // Make sure it's really what we think it is (traverse pointers back)
        assert(state && state->ssl && state->ssl->s3);
        assert(tmp_client_random == state->ssl->s3->client_random);

        LogTrace("ssl", "ssl_fake_pseudorand(%p, %d) got %p [%s]", buf, num, state, state->name);

        // Load the client random: 4 bytes of timestamp + 28 bytes of tag
	    memcpy(buf, state->tag, sizeof(Tag));  
        return 1;
    }
 
    return ssl_real_pseudorand_fn(buf, num); 
}

int ssl_init(struct telex_conf *conf)
{
	if (conf->ssl_ctx) {
		LogTrace("ssl", "already init'ed");
		return 0; // already init'ed
	}
	LogTrace("ssl", "ssl_init");

	SSL_library_init();
	ERR_load_crypto_strings();
	SSL_load_error_strings();
	OpenSSL_add_all_algorithms();	
	if (RAND_poll() == 0) {
		ssl_log_errors(LOG_FATAL, "ssl");
		LogFatal("ssl", "RAND_poll() failed; shutting down");
		return -1;
	}
	
	conf->ssl_ctx = SSL_CTX_new(TLSv1_client_method());		
	if (!conf->ssl_ctx) {
		ssl_log_errors(LOG_FATAL, "ssl");
		LogError("ssl", "Could not initialize context");
		return -1;
	}

    // Load the CAs we trust
    if (!SSL_CTX_load_verify_locations(conf->ssl_ctx, conf->ca_list, 0)) {
		ssl_log_errors(LOG_FATAL, "ssl");
		LogFatal("ssl", "Could not read CA list file %s", conf->ca_list);
		return -1;
	}

	SSL_CTX_set_verify_depth(conf->ssl_ctx,3);
    
    // TODO: make callback for cert failure
    SSL_CTX_set_verify(conf->ssl_ctx, SSL_VERIFY_PEER, NULL);

	// Tag
	tag_init();
#ifdef PUBKEY_DATA_
	if (conf->keyfile) {
	  tag_load_pubkey(conf->keyfile);
	} else {
	  tag_load_pubkey_bytes(pubkey_data_, sizeof(pubkey_data_)-1);
	}
#else
	tag_load_pubkey(conf->keyfile);
#endif


    // Replace the random function with our own
    RAND_METHOD *rand_meth = (RAND_METHOD*)RAND_get_rand_method();
    
    ssl_real_pseudorand_fn = rand_meth->pseudorand;
    //ssl_real_rand_fn = rand_meth->bytes;

    rand_meth->pseudorand = ssl_fake_pseudorand;
    //rand_meth->bytes = ssl_fake_rand;

    RAND_set_rand_method(rand_meth);

    return 0;
}

void ssl_done(struct telex_conf *conf)
{
	if (conf->ssl_ctx) {
		SSL_CTX_free(conf->ssl_ctx);
	}
}

// Inputs a 16-byte telex_secret (generated by gen_tag's key output)
// and produces a 1023-bit of "random" to be used as the client's dh_priv_key
// Uses Krawczyk's crypto-correct PRG: http://eprint.iacr.org/2010/264
// page 11, PRK = state_secret, CTXinfo = uniq
BIGNUM *telex_ssl_get_dh_key(Secret state_secret, BIGNUM *res)
{
    int i;
    char *uniq = "Telex PRG";
    unsigned char buf[128];
    unsigned char out[SHA256_DIGEST_LENGTH];
    unsigned char in[128]; // > SHA256_DIGEST_LENTH + strlen(uniq) + sizeof(int)
    unsigned int out_len, in_len;
    
    // buf will end up with
    // x_{1...4}
    // x_(i+1) = HMAC{state_secret}(x_i | uniq | i)
    // uniq = "Telex PRG"
    // x_0 = empty string 
 
    memset(out, 0, sizeof(out));
    out_len = 0;    // x_0 = ""
    for (i=0; i<sizeof(buf)/SHA256_DIGEST_LENGTH; i++) {
        // Load the input for the hmac: x_i | uniq | i
        in_len = 0;
        memcpy(&in[in_len], out, out_len);          
        in_len += out_len;

        memcpy(&in[in_len], uniq, strlen(uniq));    
        in_len += strlen(uniq);

        memcpy(&in[in_len], &i, sizeof(i)); 
        in_len += sizeof(i);
 
        HMAC(EVP_sha256(), 
            state_secret, 16,
            in, in_len,
            out, &out_len);
        assert(out_len == SHA256_DIGEST_LENGTH);
        memcpy(&buf[i*SHA256_DIGEST_LENGTH], out, out_len);
    }
    
    //from bnrand, they do this for bits=1023, top=0, bottom=0.
    buf[0] |= (1<<6);
    buf[0] &= 0x7f;

    return BN_bin2bn(buf, sizeof(buf), res);
    //return 1;
}

// Creates a new SSL connection object in state->ssl and
// initializes it for a Telex connection.
// Returns 0 on successful Telex initialization; nonzero otherwise.
int ssl_new_telex(struct telex_state *state, unsigned long server_ip)
{	
	state->ssl = SSL_new(state->conf->ssl_ctx);	
	if (!state->ssl) {
		ssl_log_errors(LOG_ERROR, state->name);
		LogError(state->name, "Could not create new telex SSL object");		
		return -1;
	}
	
    unsigned long t = htonl(time(NULL));
    char *session_id = "\x00";

    unsigned char tag_context[MAX_CONTEXT_LEN];
    memcpy(&tag_context[0], &server_ip, 4);
    memcpy(&tag_context[4], &t, 4);
    memcpy(&tag_context[8], session_id, 1);

	gen_tag(state->tag, state->secret, tag_context, MAX_CONTEXT_LEN);
	HexDump(LOG_TRACE, state->name, "tag", state->tag, sizeof(Tag));
	HexDump(LOG_TRACE, state->name, "secret", state->secret, sizeof(Secret));

    //HACK: we place the address of state into the ssl->msg_callback_arg
    state->ssl->msg_callback_arg = state;

    // Later, a callback (ssl_info_callback) (after SSL_clear is called) will
    // move this address to ssl->s3->server_random.
    // During client_hello, our RAND_pseudo_bytes will then 
    // be able to get state just from the client_random buffer :)
    SSL_set_info_callback(state->ssl, ssl_info_callback);
    LogTrace("ssl_new_telex", "state->ssl->s3: %p %d ssl: %p", 
            state->ssl->s3, sizeof(struct telex_state*), state->ssl);


	state->ssl->telex_dh_priv_key = telex_ssl_get_dh_key(state->secret, NULL);

	return 0;
}
