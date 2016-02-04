/*
 * Two-cent Certification Authority: the C version
 * This utility is meant to replace easy-rsa in openvpn distributions.
 * It makes it easier to generate a root CA, server, or client certs.
 *
 * (c) nicolas314 -- MIT license
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#include <openssl/asn1.h>
#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/pem.h>
#include <openssl/pkcs12.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#define RSA_KEYSZ   2048
#define FIELD_SZ    128
#define SERIAL_SZ   16  /* in bytes */

#define MAX_SAN     8
#define BIG_FIELD   (MAX_SAN*(FIELD_SZ+1))

/* Use to shuffle key+cert around */
typedef struct _identity_ {
    EVP_PKEY * key ;
    X509    * cert ;
} identity ;

/* Config variables */
struct _certinfo_ {
    char o [FIELD_SZ+1];
    char ou[FIELD_SZ+1];
    char cn[FIELD_SZ+1];
    char c [FIELD_SZ+1];
    int  days ;
    char l [FIELD_SZ+1];
    char st[FIELD_SZ+1];
    char san[FIELD_SZ+1] ;

    enum {
        PROFILE_UNKNOWN=0,
        PROFILE_ROOT_CA,
        PROFILE_SUB_CA,
        PROFILE_SERVER,
        PROFILE_CLIENT,
        PROFILE_WWW,
    } profile ;
    char signing_ca[FIELD_SZ+1];
    int rsa_keysz ;
    char ec_name[FIELD_SZ+1] ;
} certinfo ;

/*
 * Set one extension in a given certificate
 */
static int set_extension(X509 * issuer, X509 * cert, int nid, char * value)
{
    X509_EXTENSION * ext ;
    X509V3_CTX ctx ;

    X509V3_set_ctx(&ctx, issuer, cert, NULL, NULL, 0);
    ext = X509V3_EXT_conf_nid(NULL, &ctx, nid, value);
    if (!ext)
        return -1;

    X509_add_ext(cert, ext, -1);
    X509_EXTENSION_free(ext);
    return 0 ;
}

/*
 * Set serial to a random 128-bit number
 */
static int set_serial128(X509 * cert)
{
    FILE * urandom;
    BIGNUM *        b_serial ;
    unsigned char   c_serial[SERIAL_SZ] ;

    /* Read random bits from /dev/urandom */
    urandom = fopen("/dev/urandom", "rb");
    fread(c_serial, SERIAL_SZ, 1, urandom);
    fclose(urandom);

    c_serial[0]=0x2c ;
    c_serial[1]=0xca ;

    b_serial = BN_bin2bn(c_serial, SERIAL_SZ, NULL);
    BN_to_ASN1_INTEGER(b_serial, X509_get_serialNumber(cert));
    BN_free(b_serial);
    return 0 ;
}

/*
 * Useful for showing progress on key generation
 */
static void progress(int p, int n, void *arg)
{
    char c='B';
    switch (p) {
        case 0: c='.'; break;
        case 1: c='+'; break;
        case 2: c='*'; break;
        default: c='\n'; break;
    }
    fputc(c, stderr);
}

/*
 * Load CA certificate and private key from current dir
 */
static int load_ca(char * ca_name, identity * ca)
{
    FILE * f ;
    RSA  * rsa ;
    char filename[FIELD_SZ+1] ;

    sprintf(filename, "%s.crt", ca_name);
    if ((f=fopen(filename, "r"))==NULL) {
        fprintf(stderr, "Cannot find: %s\n", filename);
        return -1 ; 
    }
    ca->cert = PEM_read_X509(f, NULL, NULL, NULL);
    fclose(f);

    sprintf(filename, "%s.key", ca_name);
    if ((f=fopen(filename, "r"))==NULL) {
        return -1 ; 
    }
    rsa = PEM_read_RSAPrivateKey(f, NULL, NULL, NULL);
    fclose(f);

    ca->key = EVP_PKEY_new();
    EVP_PKEY_assign_RSA(ca->key, rsa);

    if (!X509_check_private_key(ca->cert, ca->key)) {
        fprintf(stderr, "CA certificate and private key do not match\n");
        return -1 ;
    }
    return 0;
}

/*
 * Create identity
 */
int build_identity(void)
{
    EVP_PKEY * pkey ;
    RSA * rsa ;
    EC_KEY * ecc ;
    X509 * cert ;
    X509_NAME * name ;
    identity ca ;
    char filename[FIELD_SZ+5];
    FILE * pem ;

    /* Check before overwriting */
    sprintf(filename, "%s.crt", certinfo.cn);
    if (access(filename, F_OK)!=-1) {
        fprintf(stderr, "identity named %s already exists in this directory. Exiting now\n", filename);
        return -1 ;
    }
    sprintf(filename, "%s.key", certinfo.cn);
    if (access(filename, F_OK)!=-1) {
        fprintf(stderr, "identity named %s already exists in this directory. Exiting now\n", filename);
        return -1 ;
    }

    switch (certinfo.profile) {
        case PROFILE_ROOT_CA:
        strcpy(certinfo.ou, "Root");
        break;

        case PROFILE_SUB_CA:
        strcpy(certinfo.ou, "Sub");
        break;

        case PROFILE_SERVER:
        strcpy(certinfo.ou, "Server");
        break;
        
        case PROFILE_CLIENT:
        strcpy(certinfo.ou, "Client");
        break;

        case PROFILE_WWW:
        strcpy(certinfo.ou, "Server");
        break;

        default:
        fprintf(stderr, "Unknown profile: aborting\n");
        return -1 ;
    }

    if (certinfo.ec_name[0] && certinfo.profile!=PROFILE_CLIENT) {
        fprintf(stderr, "ECC keys are only supported for clients\n");
        return -1 ;
    }

    if (certinfo.profile != PROFILE_ROOT_CA) {
        /* Need to load signing CA */
        if (load_ca(certinfo.signing_ca, &ca)!=0) {
            fprintf(stderr, "Cannot find CA key or certificate\n");
            return -1 ;
        }
        /* Organization is the same as root */
        X509_NAME_get_text_by_NID(X509_get_subject_name(ca.cert),
                                  NID_organizationName,
                                  certinfo.o,
                                  FIELD_SZ);
    }

    /* Generate key pair */
    if (certinfo.ec_name[0]) {
        printf("Generating EC key [%s]\n", certinfo.ec_name);
        ecc = EC_KEY_new_by_curve_name(OBJ_txt2nid(certinfo.ec_name));
        if (!ecc) {
            fprintf(stderr, "Unknown curve: [%s]\n", certinfo.ec_name);
            return -1 ;
        }
        EC_KEY_set_asn1_flag(ecc, OPENSSL_EC_NAMED_CURVE);
        EC_KEY_generate_key(ecc);
        pkey = EVP_PKEY_new();
        EVP_PKEY_assign_EC_KEY(pkey, ecc);
    } else {
        printf("Generating RSA-%d key\n", certinfo.rsa_keysz);
        pkey = EVP_PKEY_new();
        rsa = RSA_generate_key(certinfo.rsa_keysz, RSA_F4, progress, 0);
        EVP_PKEY_assign_RSA(pkey, rsa);
    }

    /* Assign all certificate fields */
    cert = X509_new();
    X509_set_version(cert, 2);
    set_serial128(cert);
    X509_gmtime_adj(X509_get_notBefore(cert), 0);
    X509_gmtime_adj(X509_get_notAfter(cert), certinfo.days * 24*60*60);
    X509_set_pubkey(cert, pkey);

    name = X509_get_subject_name(cert);
    if (certinfo.c[0]) {
        X509_NAME_add_entry_by_txt(name, "C", MBSTRING_ASC, (unsigned char*)certinfo.c, -1, -1, 0);
    }
    X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC, (unsigned char*)certinfo.o, -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (unsigned char*)certinfo.cn, -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "OU", MBSTRING_ASC, (unsigned char*)certinfo.ou, -1, -1, 0);
    if (certinfo.l[0]) {
        X509_NAME_add_entry_by_txt(name, "L", MBSTRING_ASC, (unsigned char *)certinfo.l, -1, -1, 0);
    }
    if (certinfo.st[0]) {
        X509_NAME_add_entry_by_txt(name, "ST", MBSTRING_ASC, (unsigned char *)certinfo.st, -1, -1, 0);
    }

    /* Set extensions according to profile */
    switch (certinfo.profile) {
        case PROFILE_ROOT_CA:
        /* CA profiles can issue certs and sign CRLS */
        set_extension(cert, cert, NID_basic_constraints, "critical,CA:TRUE");
        set_extension(cert, cert, NID_key_usage, "critical,keyCertSign,cRLSign");
        set_extension(cert, cert, NID_subject_key_identifier, "hash");
        set_extension(cert, cert, NID_authority_key_identifier, "keyid:always");
        break ;

        case PROFILE_SUB_CA:
        /* CA profiles can issue certs and sign CRLS */
        set_extension(ca.cert, cert, NID_basic_constraints, "critical,CA:TRUE");
        set_extension(ca.cert, cert, NID_key_usage, "critical,keyCertSign,cRLSign");
        set_extension(ca.cert, cert, NID_subject_key_identifier, "hash");
        set_extension(ca.cert, cert, NID_authority_key_identifier, "keyid:always");
        break;

        case PROFILE_CLIENT:
        if (certinfo.san[0]) {
            set_extension(ca.cert, cert, NID_subject_alt_name, certinfo.san);
        }
        set_extension(ca.cert, cert, NID_basic_constraints, "CA:FALSE");
        set_extension(ca.cert, cert, NID_anyExtendedKeyUsage, "clientAuth");
        set_extension(ca.cert, cert, NID_key_usage, "digitalSignature");
        set_extension(ca.cert, cert, NID_subject_key_identifier, "hash");
        set_extension(ca.cert, cert, NID_authority_key_identifier, "issuer:always,keyid:always");
        break ;

        case PROFILE_SERVER:
        if (certinfo.san[0]) {
            set_extension(ca.cert, cert, NID_subject_alt_name, certinfo.san);
        }
        set_extension(ca.cert, cert, NID_basic_constraints, "CA:FALSE");
        set_extension(ca.cert, cert, NID_netscape_cert_type, "server");
        set_extension(ca.cert, cert, NID_anyExtendedKeyUsage, "serverAuth");
        set_extension(ca.cert, cert, NID_key_usage, "digitalSignature,keyEncipherment");
        set_extension(ca.cert, cert, NID_subject_key_identifier, "hash");
        set_extension(ca.cert, cert, NID_authority_key_identifier, "issuer:always,keyid:always");
        break ;

        case PROFILE_WWW:
        if (certinfo.san[0]) {
            set_extension(ca.cert, cert, NID_subject_alt_name, certinfo.san);
        }
        set_extension(ca.cert, cert, NID_basic_constraints, "CA:FALSE");
        set_extension(ca.cert, cert, NID_netscape_cert_type, "server");
        set_extension(ca.cert, cert, NID_anyExtendedKeyUsage, "serverAuth,clientAuth");
        set_extension(ca.cert, cert, NID_key_usage, "digitalSignature,keyEncipherment");
        set_extension(ca.cert, cert, NID_subject_key_identifier, "hash");
        set_extension(ca.cert, cert, NID_authority_key_identifier, "issuer:always,keyid:always");
        break;

        case PROFILE_UNKNOWN:
        default:
        break ;
    }
    /* Set issuer */
    if (certinfo.profile==PROFILE_ROOT_CA) {
        /* Self-signed */
        X509_set_issuer_name(cert, name);
        X509_sign(cert, pkey, EVP_sha256());
    } else {
        /* Signed by parent CA */
        X509_set_issuer_name(cert, X509_get_subject_name(ca.cert));
        X509_sign(cert, ca.key, EVP_sha256());
    }

    printf("Saving results to %s.[crt|key]\n", certinfo.cn);
    pem = fopen(filename, "wb");
    PEM_write_PrivateKey(pem, pkey, NULL, NULL, 0, NULL, NULL);
    fclose(pem);
    sprintf(filename, "%s.crt", certinfo.cn);
    pem = fopen(filename, "wb");
    PEM_write_X509(pem, cert);
    fclose(pem);
    X509_free(cert);
    EVP_PKEY_free(pkey);

    if (certinfo.profile!=PROFILE_ROOT_CA) {
        X509_free(ca.cert);
        EVP_PKEY_free(ca.key);
    }
    printf("done\n");

    return 0;
}

static X509_CRL * load_crl(char * ca_name)
{
    FILE * fp ;
    BIO  * in ;
    X509_CRL * crl ;
    char filename[FIELD_SZ+5];

    sprintf(filename, "%s.crl", ca_name);
    in = BIO_new(BIO_s_file());
    if ((fp=fopen(filename, "rb"))==NULL) {
        BIO_free(in);
        return NULL ;
    }
    BIO_set_fp(in, fp, BIO_NOCLOSE);
    crl = PEM_read_bio_X509_CRL(in, NULL, NULL, NULL);
    fclose(fp);
    BIO_free(in);
    return crl ;
}

/*
 * openssl crl -in ca.crl -text
 */
void show_crl(char * ca_name)
{
    X509_CRL * crl ;
    X509_REVOKED * rev ;
    int i, total ;
    STACK_OF(X509_REVOKED) * rev_list ;
    BIO * out ;

    if ((crl = load_crl(ca_name))==NULL) {
        printf("No CRL found\n");
        return ;
    }
    rev_list = X509_CRL_get_REVOKED(crl);
    total = sk_X509_REVOKED_num(rev_list);

    out = BIO_new(BIO_s_file());
    out = BIO_new_fp(stdout, BIO_NOCLOSE);

    BIO_printf(out, "-- Revoked certificates found in CRL\n");
    for (i=0 ; i<total ; i++) {
        rev=sk_X509_REVOKED_value(rev_list, i);
        BIO_printf(out, "serial: ");
        i2a_ASN1_INTEGER(out, rev->serialNumber);
        BIO_printf(out, "\n  date: ");
        ASN1_TIME_print(out, rev->revocationDate);
        BIO_printf(out, "\n\n");
    }
    X509_CRL_free(crl);
    BIO_free_all(out);
    return ;
}

/*
 * Revoke one certificate at a time
 * No check performed to see if certificate already revoked.
 */
void revoke_cert(char * ca_name, char * name)
{
    char filename[FIELD_SZ+5];
    FILE * f ;
    X509_CRL * crl ;
    X509 * cert ;
    ASN1_INTEGER * r_serial ;
    ASN1_INTEGER * crlnum ;
    X509_REVOKED * rev ;
    ASN1_TIME * tm ;
    identity ca ;
    BIO * out ;
    BIGNUM * b_crlnum ;

    /* Find requested certificate by name */
    sprintf(filename, "%s.crt", name);
    if ((f=fopen(filename, "r"))==NULL) {
        fprintf(stderr, "Cannot find: %s\n", filename);
        return ; 
    }
    cert = PEM_read_X509(f, NULL, NULL, NULL);
    fclose(f);
    /* Get certificate serial number */
    r_serial = X509_get_serialNumber(cert);

    /* Find out if if was already revoked */

    /* Make a revoked object with that serial */
    rev = X509_REVOKED_new();
    X509_REVOKED_set_serialNumber(rev, r_serial);
    X509_free(cert);
    /* Set reason to unspecified */
    rev->reason = ASN1_ENUMERATED_get(CRL_REASON_UNSPECIFIED);

    /* Load or create new CRL */
    if ((crl = load_crl(ca_name))==NULL) {
        crl = X509_CRL_new();
        X509_CRL_set_version(crl, 1);
        /* Set CRL number */
        crlnum = ASN1_INTEGER_new();
        ASN1_INTEGER_set(crlnum, 1);
        X509_CRL_add1_ext_i2d(crl, NID_crl_number, crlnum, 0, 0);
        ASN1_INTEGER_free(crlnum);
    } else {
        crlnum = X509_CRL_get_ext_d2i(crl, NID_crl_number, 0, 0);
        b_crlnum = ASN1_INTEGER_to_BN(crlnum, NULL);
        BN_add_word(b_crlnum, 1);
        BN_to_ASN1_INTEGER(b_crlnum, crlnum);
        BN_free(b_crlnum);
        X509_CRL_add1_ext_i2d(crl, NID_crl_number, crlnum, 0, X509V3_ADD_REPLACE_EXISTING);
        ASN1_INTEGER_free(crlnum);
    }

    /* What time is it? */
    tm = ASN1_TIME_new();
    X509_gmtime_adj(tm, 0);
    X509_REVOKED_set_revocationDate(rev, tm);
    X509_CRL_set_lastUpdate(crl, tm);

    /* Set CRL next update to a year from now */
    X509_gmtime_adj(tm, 365*24*60*60);
    X509_CRL_set_nextUpdate(crl, tm);
    ASN1_TIME_free(tm);

    /* Add revoked to CRL */
    X509_CRL_add0_revoked(crl, rev);    
    X509_CRL_sort(crl);

    /* Load root key to sign CRL */
    if (load_ca(ca_name, &ca)!=0) {
        fprintf(stderr, "Cannot find CA key/crt\n");
        return ;
    }
    X509_CRL_set_issuer_name(crl, X509_get_subject_name(ca.cert));
    X509_free(ca.cert);

    /* Sign CRL */
    X509_CRL_sign(crl, ca.key, EVP_sha256());
    EVP_PKEY_free(ca.key);

    /* Dump CRL */
    sprintf(filename, "%s.crl", ca_name);
    if ((f = fopen(filename, "wb"))==NULL) {
        fprintf(stderr, "Cannot write %s: aborting\n", filename);
        X509_CRL_free(crl);
        return ;
    }
    out = BIO_new(BIO_s_file());
    BIO_set_fp(out, f, BIO_NOCLOSE);
    PEM_write_bio_X509_CRL(out, crl);
    BIO_free_all(out);
    fclose(f);
    X509_CRL_free(crl);
    return ;
}

int generate_dhparam(int dh_bits)
{
    DH * dh ;
    char filename[FIELD_SZ+1];
    FILE * out;

    sprintf(filename, "dh%d.pem", dh_bits);
    if ((out=fopen(filename, "wb"))==NULL) {
        fprintf(stderr, "Cannot create %s: aborting\n", filename);
        return -1;
    }
    dh = DH_new();
    printf("Generating DH parameters (%d bits) -- this can take long\n", dh_bits);
    DH_generate_parameters_ex(dh, dh_bits, DH_GENERATOR_2, 0);
    PEM_write_DHparams(out, dh);
    fclose(out);
    printf("done\n");
    return 0;
}

void usage(void)
{
    printf(
        "\n"
        "\tUse:\n"
        "\t2cca root   [DN] [days=xx]         # Create a root CA\n"
        "\t2cca sub    [DN] [days=xx] [ca=xx] # Create a sub CA\n"
        "\t2cca server [DN] [days=xx] [ca=xx] # Create a server\n"
        "\t2cca client [DN] [days=xx] [ca=xx] # Create a client\n"
        "\t2cca www    [DN] [days=xx] [ca=xx] [dns=x] [dns=x]\n"
        "\n"
        "Where DN is given as key=val pairs. Supported fields:\n"
        "\n"
        "\tO     Organization, only for root (default: Home)\n"
        "\tCN    Common Name (default: root|server|client\n"
        "\tC     2-letter country code like US, FR, UK (optional)\n"
        "\tST    a state name (optional)\n"
        "\tL     a locality or city name (optional)\n"
        "\temail an email address\n"
        "\n"
        "\tdays specifies certificate duration in days\n"
        "\n"
        "Key generation:\n"
        "\tEither RSA with keysize set by rsa=xx\n"
        "\tOr elliptic-curve with curve name set by ec=xx\n"
        "\tDefault is RSA-2048, i.e. rsa=2048\n"
        "\tSigning CA is specified with ca=CN (default: root)\n"
        "\n"
        "CRL management\n"
        "\t2cca crl [ca=xx]            # Show CRL for CA xx\n"
        "\t2cca revoke NAME [ca=xx]    # Revoke single cert by name\n"
        "\n"
        "\t2cca dh [numbits]           # Generate DH parameters\n"
        "\n"
        "Web server certificates\n"
        "\tGenerate web server certificates using 'wwww'\n"
        "\tSpecify DNS names using dns=x dns=y on the command-line\n"
        "\n"
    );
}

int parse_cmd_line(int argc, char ** argv)
{
    char key[FIELD_SZ+1] ;
    char val[FIELD_SZ+1] ;
    char tmp[FIELD_SZ+1] ;
    int  ns=0 ;
    char san[BIG_FIELD+1];
    int  i ;

    memset(san, 0, BIG_FIELD+1);
    for (i=2 ; i<argc ; i++) { 
        if (sscanf(argv[i], "%[^=]=%s", key, val)==2) {
            if (!strcmp(key, "rsa")) {
                certinfo.rsa_keysz = atoi(val);
            } else if (!strcmp(key, "ec")) {
                strcpy(certinfo.ec_name, val);
            } else if (!strcmp(key, "O")) {
                strcpy(certinfo.o, val);
            } else if (!strcmp(key, "C")) {
                strcpy(certinfo.c, val);
            } else if (!strcmp(key, "ST")) {
                strcpy(certinfo.st, val);
            } else if (!strcmp(key, "CN")) {
                strcpy(certinfo.cn, val);
            } else if (!strcmp(key, "L")) {
                strcpy(certinfo.l, val);
            } else if (!strcmp(key, "email")) {
                sprintf(tmp, "email:%s", val);
                if (ns==0) {
                    strcpy(san, tmp);
                } else {
                    strcat(san, ",\n");
                    strcat(san, tmp);
                }
                ns++;
            } else if (!strcmp(key, "dns")) {
                sprintf(tmp, "DNS:%s", val);
                if (ns==0) {
                    strcpy(san, tmp);
                } else {
                    strcat(san, ",");
                    strcat(san, tmp);
                }
                ns++;
            } else if (!strcmp(key, "days")) {
                certinfo.days = atoi(val);
            } else if (!strcmp(key, "ca")) {
                strcpy(certinfo.signing_ca, val);
            } else {
                fprintf(stderr, "Unsupported field: [%s]\n", key);
                return -1 ;
            }
        }
    }
    if (ns>0) {
        strcpy(certinfo.san, san);
        printf("SAN[%s]\n", certinfo.san);
    }
    return 0 ;
}

int main(int argc, char * argv[])
{
    int dh_bits=2048;

	if (argc<2) {
        usage();
		return 1 ;
	}

    OpenSSL_add_all_algorithms();

    /* Initialize DN fields to default values */
    memset(&certinfo, 0, sizeof(certinfo));
    certinfo.rsa_keysz = RSA_KEYSZ ;
    strcpy(certinfo.o, "Home");
    certinfo.days = 3650 ;
    strcpy(certinfo.signing_ca, "root");

    if ((argc>2) && (parse_cmd_line(argc, argv)!=0)) {
        return -1 ;
    }

    if (certinfo.cn[0]==0) {
        strcpy(certinfo.cn, argv[1]);
    }

    if (!strcmp(argv[1], "root")) {
        certinfo.profile = PROFILE_ROOT_CA ;
        build_identity();
    } else if (!strcmp(argv[1], "sub")) {
        certinfo.profile = PROFILE_SUB_CA ;
        build_identity();
    } else if (!strcmp(argv[1], "server")) {
        certinfo.profile = PROFILE_SERVER ;
        build_identity() ;
    } else if (!strcmp(argv[1], "client")) {
        certinfo.profile = PROFILE_CLIENT ;
        build_identity() ;
    } else if (!strcmp(argv[1], "www")) {
        certinfo.profile = PROFILE_WWW ;
        build_identity() ;
    } else if (!strcmp(argv[1], "crl")) {
        show_crl(certinfo.signing_ca);
    } else if (!strcmp(argv[1], "revoke")) {
        if (argc>2) {
            revoke_cert(certinfo.signing_ca, argv[2]);
        } else {
            fprintf(stderr, "Missing certificate name for revocation\n");
        }
    } else if (!strcmp(argv[1], "dh")) {
        if (argc>2) {
            dh_bits=atoi(argv[2]);
        }
        generate_dhparam(dh_bits);
    }
	return 0 ;
}

