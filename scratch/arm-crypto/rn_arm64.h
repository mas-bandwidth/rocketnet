/* Tracer (scratch): the AEAD-level hook a fused ChaCha20-Poly1305 pass needs.
   The bundle's two implementation structs give one primitive each, so a pass
   that interleaves them needs its own entry; NULL means the composed path. */
#ifndef RN_ARM64_H
#define RN_ARM64_H

struct rn_aead_ietf_implementation
{
    /* the IETF AEAD's encrypt_detached: c (mlen bytes) and a 16-byte mac */
    int ( *seal )( unsigned char * c, unsigned char * mac, const unsigned char * m, unsigned long long mlen, const unsigned char * ad,
                   unsigned long long adlen, const unsigned char * npub, const unsigned char * k );
    /* decrypt_detached: 0 and the plaintext in m, or -1 and m zeroed; m may
       be NULL (verify only) and may equal c */
    int ( *open )( unsigned char * m, const unsigned char * c, unsigned long long clen, const unsigned char * mac, const unsigned char * ad,
                   unsigned long long adlen, const unsigned char * npub, const unsigned char * k );
};

#if defined( __aarch64__ ) && defined( __ARM_NEON ) && !defined( __ARM_BIG_ENDIAN )
extern struct rn_aead_ietf_implementation rn_aead_ietf_fused_implementation;
#endif

#endif
