#include <sstream>

#include <auth/davixauth.hpp>
#include <status/davixstatusrequest.hpp>


#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/pkcs12.h>
#include <openssl/x509v3.h>
#include <openssl/rand.h>
#include <openssl/opensslv.h>
#include <libneon/src/ne_openssl_internal.h>

// openSSL security layer implem

namespace Davix {

const std::string openssl_scope = "Davix::OpenSSL";


void opensslErrorMapper(const std::string & msg, DavixError** err){
    unsigned long e = 0;
    char buff_err[255] ={0};
    e= ERR_get_error();
    if(e == 0){
        DavixError::setupError(err, openssl_scope, StatusCode::UnknowError, "No Error reported by OpenSSL");
        return;
    }

    StatusCode::Code c;
    switch(ERR_GET_REASON(e)){
        case PEM_R_BAD_DECRYPT:
        case PEM_R_BAD_PASSWORD_READ:
        case PEM_R_PROBLEMS_GETTING_PASSWORD:
            c= StatusCode::CredDecryptionError;
        default:
            c = StatusCode::SSLError;
    }
    std::ostringstream ss;
    ERR_error_string_n(e, buff_err, 255);
    ss << msg << " : " << buff_err;
    DavixError::setupError(err, openssl_scope, c, ss.str());
}

static int SSL_pem_passwd_cb(char *buffer, int size, int rwflag, void *userdata){
    if(userdata != NULL){
        std::copy(buffer, buffer + size, static_cast<char*>(userdata));
        buffer[size - 1] = '\0';
        return(strlen(buffer));
    }
    return 0;
}


ne_ssl_client_cert *SSL_X509_Pem_Read(const std::string & pkeyfile_str, const std::string & credfile_str,
                                      const std::string & password_str, DavixError** err){
    FILE *fp;
    BIO* in;
    X509 *cert,*ca;
    STACK_OF(X509)* chain = NULL;
    EVP_PKEY *pkey;
    ne_ssl_client_cert *cc=NULL;
    int len, errcode;
    const char * pkeyfile = pkeyfile_str.c_str(), *credfile = credfile_str.c_str(), *password = password_str.c_str();
    unsigned char* name;


    if( pkeyfile ==NULL || credfile ==NULL || ((in = BIO_new(BIO_s_file())) == NULL)){
        DavixError::setupError(err, openssl_scope, StatusCode::UnknowError, "init error");
        return NULL;
    }

    // load cred
    if (BIO_read_filename(in, credfile) <= 0){
        opensslErrorMapper(std::string("impossible to open ").append(credfile), err);
        ERR_clear_error();
        return NULL;

    }
    if ( (cert = PEM_read_bio_X509(in, NULL, SSL_pem_passwd_cb, (void*) password)) == NULL){
        opensslErrorMapper(std::string(" parse PEM credential failed ").append(credfile), err);
        ERR_clear_error();
        BIO_free(in);
        return NULL;
    }

    // load chain
    chain = sk_X509_new_null();
    while ((ca = PEM_read_bio_X509(in,NULL, SSL_pem_passwd_cb, (void*) password))
                != NULL){
        sk_X509_push(chain, ca);
    }
            /* When the while loop ends, it's usually just EOF. */
    BIO_free(in);
    errcode = ERR_peek_last_error();
    if (ERR_GET_LIB(errcode) == ERR_LIB_PEM && ERR_GET_REASON(errcode) == PEM_R_NO_START_LINE){
        ERR_clear_error();
    }else{
        opensslErrorMapper(" parse PEM credential chain failed ", err);
        ERR_clear_error();
        X509_free(cert);
        return NULL;
    }

    // load pkey
    fp = fopen(pkeyfile, "rb");
    if (fp == NULL){
        std::ostringstream ss;
        ss << "Impossible to open " << pkeyfile << " : " << strerror(errno);
        DavixError::setupError(err, openssl_scope, StatusCode::CredentialNotFound, ss.str());
        errno = 0;
        X509_free(cert);
        return NULL;
    }
    if ( (pkey = PEM_read_PrivateKey(fp, NULL, SSL_pem_passwd_cb,  (void*) password)) == NULL){
        opensslErrorMapper(std::string("parse PEM private key failed ").append(pkeyfile), err);
        ERR_clear_error();
        X509_free(cert);
        return NULL;
    }
    fclose(fp);

    // load ca chain


    name = X509_alias_get0(cert, &len);
    cc = (ne_ssl_client_cert*) calloc(sizeof(ne_ssl_client_cert),1);
    cc->pkey = pkey;
    cc->decrypted = 1;
    if (name && len > 0)
        cc->friendly_name = strndup((char *)name, len);
    populate_cert(&cc->cert, cert);
    cc->cert.chain = chain;
    return cc;

}


}
