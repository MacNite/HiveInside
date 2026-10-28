/* Host stand-in for the PSA Crypto calls audio.c makes: HMAC-SHA256 only. */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef int32_t psa_status_t;
typedef uint32_t psa_key_id_t;
typedef uint32_t psa_algorithm_t;
typedef struct { uint32_t usage, alg, type; size_t bits; } psa_key_attributes_t;

#define PSA_SUCCESS ((psa_status_t)0)
#define PSA_ERROR_NOT_SUPPORTED ((psa_status_t)-134)
#define PSA_ERROR_BUFFER_TOO_SMALL ((psa_status_t)-138)
#define PSA_ERROR_INVALID_HANDLE ((psa_status_t)-136)
#define PSA_KEY_ATTRIBUTES_INIT ((psa_key_attributes_t){ 0 })
#define PSA_KEY_USAGE_SIGN_MESSAGE 0x400u
#define PSA_ALG_SHA_256 0x02000009u
#define PSA_ALG_HMAC(hash) (0x03800000u | ((hash) & 0xffu))
#define PSA_KEY_TYPE_HMAC 0x1100u

psa_status_t psa_crypto_init(void);
void psa_set_key_usage_flags(psa_key_attributes_t *a, uint32_t usage);
void psa_set_key_algorithm(psa_key_attributes_t *a, psa_algorithm_t alg);
void psa_set_key_type(psa_key_attributes_t *a, uint32_t type);
void psa_set_key_bits(psa_key_attributes_t *a, size_t bits);
void psa_reset_key_attributes(psa_key_attributes_t *a);
psa_status_t psa_import_key(const psa_key_attributes_t *a, const uint8_t *data,
			    size_t len, psa_key_id_t *key);
psa_status_t psa_mac_compute(psa_key_id_t key, psa_algorithm_t alg,
			     const uint8_t *input, size_t input_len, uint8_t *mac,
			     size_t mac_size, size_t *mac_len);
psa_status_t psa_destroy_key(psa_key_id_t key);

/* Test helper: HMAC-SHA256 of msg under key (the shim's own implementation). */
void shim_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *msg,
		      size_t msg_len, uint8_t out[32]);
