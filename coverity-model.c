/*
 * coverity-model.c — Coverity Scan Modeling File for Barrier
 *
 * Teaches Coverity how to handle OpenSSL and other opaque allocators
 * so it doesn't generate false-positive null-dereference or resource-leak
 * defects.
 *
 * DO NOT compile this into the project — upload via the Coverity Scan
 * dashboard under Project Settings → Modeling File.
 *
 * Valid primitives:
 *   __coverity_alloc__(p)                  — marks p as an allocated resource
 *   __coverity_free__(p)                   — marks p as freed/released
 *   __coverity_mark_as_uninitialized_buffer__(p)  — marks buffer uninitialized
 *   __coverity_writeall__(p)               — marks buffer as written
 *   __coverity_panic__()                   — marks a no-return path
 */

typedef void EVP_PKEY;
typedef void RSA;
typedef void BIGNUM;
typedef void X509;
typedef void BIO;

/* ── OpenSSL: EVP_PKEY ─────────────────────────────────────────────────── */

EVP_PKEY* EVP_PKEY_new(void)
{
    EVP_PKEY* p;
    int fail;
    if (fail) return (void*)0;
    __coverity_alloc__(p);
    return p;
}

void EVP_PKEY_free(EVP_PKEY* key)
{
    __coverity_free__(key);
}

/* ── OpenSSL: X509 ─────────────────────────────────────────────────────── */

X509* X509_new(void)
{
    X509* p;
    int fail;
    if (fail) return (void*)0;
    __coverity_alloc__(p);
    return p;
}

void X509_free(X509* cert)
{
    __coverity_free__(cert);
}

/* ── OpenSSL: RSA ──────────────────────────────────────────────────────── */

RSA* RSA_new(void)
{
    RSA* p;
    int fail;
    if (fail) return (void*)0;
    __coverity_alloc__(p);
    return p;
}

void RSA_free(RSA* rsa)
{
    __coverity_free__(rsa);
}

/* ── OpenSSL: BIGNUM ───────────────────────────────────────────────────── */

BIGNUM* BN_new(void)
{
    BIGNUM* p;
    int fail;
    if (fail) return (void*)0;
    __coverity_alloc__(p);
    return p;
}

void BN_free(BIGNUM* a)
{
    __coverity_free__(a);
}

/* ── OpenSSL: BIO ──────────────────────────────────────────────────────── */

BIO* BIO_new_file(const char* filename, const char* mode)
{
    BIO* p;
    int fail;
    if (fail) return (void*)0;
    __coverity_alloc__(p);
    return p;
}

void BIO_free(BIO* a)
{
    __coverity_free__(a);
}

void BIO_free_all(BIO* a)
{
    __coverity_free__(a);
}

/* ── POSIX: fopen ──────────────────────────────────────────────────────── */

void* fopen(const char* path, const char* mode)
{
    void* p;
    int fail;
    if (fail) return (void*)0;
    __coverity_alloc__(p);
    return p;
}

void fclose(void* stream)
{
    __coverity_free__(stream);
}
