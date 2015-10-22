/**************************************************************************
 **
 ** sngrep - SIP Messages flow viewer
 **
 ** Copyright (C) 2013,2014 Ivan Alonso (Kaian)
 ** Copyright (C) 2013,2014 Irontec SL. All rights reserved.
 **
 ** This program is free software: you can redistribute it and/or modify
 ** it under the terms of the GNU General Public License as published by
 ** the Free Software Foundation, either version 3 of the License, or
 ** (at your option) any later version.
 **
 ** This program is distributed in the hope that it will be useful,
 ** but WITHOUT ANY WARRANTY; without even the implied warranty of
 ** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 ** GNU General Public License for more details.
 **
 ** You should have received a copy of the GNU General Public License
 ** along with this program.  If not, see <http://www.gnu.org/licenses/>.
 **
 ****************************************************************************/
/**
 * @file capture_tls.c
 * @author Ivan Alonso [aka Kaian] <kaian@irontec.com>
 *
 * @brief Functions to manage SIP TLS transport for messages
 *
 * This file contains the functions and structures to manage the SIP messages
 * that use TLS as transport.
 *
 */

#include <unistd.h>
#include "capture.h"
#include "capture_gnutls.h"
#include "option.h"
#include "util.h"
#include "sip.h"

struct SSLConnection *connections;

struct CipherSuite TLS_RSA_WITH_AES_128_CBC_SHA =
{ 0x00, 0x2F };
struct CipherSuite TLS_RSA_WITH_AES_256_CBC_SHA =
{ 0x00, 0x35 };

int
P_hash(const char *digest, unsigned char *dest, int dlen, unsigned char *secret, int sslen,
       unsigned char *seed, int slen)
{
    unsigned char hmac[48];
    unsigned int hlen;
    gcry_md_hd_t md;
    unsigned int tmpslen;
    unsigned char tmpseed[slen];
    unsigned char *out = dest;
    int pending = dlen;
    int algo = gcry_md_map_name(digest);
    int algolen = gcry_md_get_algo_dlen(algo);

    // Copy initial seed
    memcpy(tmpseed, seed, slen);
    tmpslen = slen;

    // Calculate enough data to fill destination
    while (pending > 0) {
        gcry_md_open(&md, algo, GCRY_MD_FLAG_HMAC);
        gcry_md_setkey(md, secret, sslen);
        gcry_md_write(md, tmpseed, tmpslen);
        memcpy(tmpseed, gcry_md_read(md, algo), algolen);
        tmpslen = algolen;
        gcry_md_close(md);

        gcry_md_open(&md, algo, GCRY_MD_FLAG_HMAC);
        gcry_md_setkey(md, secret, sslen);
        gcry_md_write(md, tmpseed, tmpslen);
        gcry_md_write(md, seed, slen);
        memcpy(hmac, gcry_md_read(md, algo), algolen);
        hlen = algolen;

        hlen = (hlen > pending) ? pending : hlen;
        memcpy(out, hmac, hlen);
        out += hlen;
        pending -= hlen;
    }

    return hlen;
}

int
PRF(unsigned char *dest, int dlen, unsigned char *pre_master_secret, int plen, unsigned char *label,
    unsigned char *seed, int slen)
{
    int i;

    // Split the secret by half to generate MD5 and SHA secret parts
    int hplen = plen / 2 + plen % 2;
    unsigned char md5_secret[hplen];
    unsigned char sha_secret[hplen];
    memcpy(md5_secret, pre_master_secret, hplen);
    memcpy(sha_secret, pre_master_secret + plen / 2, plen / 2);

    // This vars will store the values of P_MD5 and P_SHA-1
    unsigned char h_md5[dlen];
    unsigned char h_sha[dlen];

    // Concatenate given seed to the label to get the final seed
    int llen = strlen((const char*) label);
    unsigned char fseed[slen + llen];
    memcpy(fseed, label, llen);
    memcpy(fseed + llen, seed, slen);

    // Get enough MD5 and SHA1 data to fill output len
    P_hash("MD5", h_md5, dlen, pre_master_secret, hplen, fseed, slen + llen);
    P_hash("SHA1", h_sha, dlen, pre_master_secret + hplen, hplen, fseed, slen + llen);

    // Final output will be MD5 and SHA1 X-ORed
    for (i = 0; i < dlen; i++)
        dest[i] = h_md5[i] ^ h_sha[i];

    return dlen;
}

struct SSLConnection *
tls_connection_create(struct in_addr caddr, u_short cport, struct in_addr saddr, u_short sport) {
    struct SSLConnection *conn = NULL;
    gnutls_datum_t keycontent = { NULL, 0 };
    FILE *keyfp;
    gnutls_x509_privkey_t spkey;
    size_t br;

    // Allocate memory for this connection
    conn = sng_malloc(sizeof(struct SSLConnection));

    memcpy(&conn->client_addr, &caddr, sizeof(struct in_addr));
    memcpy(&conn->server_addr, &saddr, sizeof(struct in_addr));
    memcpy(&conn->client_port, &cport, sizeof(u_short));
    memcpy(&conn->server_port, &sport, sizeof(u_short));

    SSL_library_init();
    OpenSSL_add_all_algorithms();

    if (!(conn->ssl_ctx = SSL_CTX_new(SSLv23_server_method())))
        return NULL;

    if (!(conn->ssl = SSL_new(conn->ssl_ctx)))
        return NULL;

    if (!(keyfp = fopen(capture_get_keyfile(), "rb")))
        return NULL;
    fseek(keyfp, 0, SEEK_END);
    keycontent.size = ftell(keyfp);
    fseek(keyfp, 0, SEEK_SET);
    keycontent.data = sng_malloc(keycontent.size);
    br = fread(keycontent.data, 1, keycontent.size, keyfp);
    fclose(keyfp);

    gnutls_x509_privkey_init(&spkey);
    gnutls_x509_privkey_import(spkey, &keycontent, GNUTLS_X509_FMT_PEM);
    sng_free(keycontent.data);
    gnutls_privkey_init(&conn->server_private_key);
    gnutls_privkey_import_x509(conn->server_private_key, spkey, 0);

    // Add this connection to the list
    conn->next = connections;
    connections = conn;

    return conn;
}

void
tls_connection_destroy(struct SSLConnection *conn)
{
    struct SSLConnection *c;

    // Remove connection from connections list
    if (conn == connections) {
        connections = conn->next;
    } else {
        for (c = connections; c; c = c->next) {
            if (c->next == conn) {
                c->next = conn->next;
                break;
            }
        }
    }

    // Deallocate connection memory
    SSL_CTX_free(conn->ssl_ctx);
    SSL_free(conn->ssl);
    sng_free(conn);
}

/**
 * FIXME Replace this with a tls_load_key function and use it
 * in tls_connection_create.
 *
 * Most probably we only need one context and key for all connections
 */
int
tls_check_keyfile(const char *keyfile)
{
    gnutls_x509_privkey_t key;
    gnutls_datum_t keycontent = { NULL, 0 };
    FILE *keyfp;
    size_t br;

    SSL_library_init();
    OpenSSL_add_all_algorithms();

    if (access(capture_get_keyfile(), R_OK) != 0)
        return 0;

    if (!(keyfp = fopen(capture_get_keyfile(), "rb")))
        return 0;

    fseek(keyfp, 0, SEEK_END);
    keycontent.size = ftell(keyfp);
    fseek(keyfp, 0, SEEK_SET);
    keycontent.data = sng_malloc(keycontent.size);
    br = fread(keycontent.data, 1, keycontent.size, keyfp);
    fclose(keyfp);

    gnutls_x509_privkey_init(&key);
    if (gnutls_x509_privkey_import(key, &keycontent, GNUTLS_X509_FMT_PEM) < 0)
        return 0;
    sng_free(keycontent.data);

    return 1;
}

int
tls_connection_dir(struct SSLConnection *conn, struct in_addr addr, u_short port)
{
    if (conn->client_addr.s_addr == addr.s_addr && conn->client_port == port)
        return 0;
    if (conn->server_addr.s_addr == addr.s_addr && conn->server_port == port)
        return 1;
    return -1;
}

struct SSLConnection*
tls_connection_find(struct in_addr addr, u_short port) {
    struct SSLConnection *conn;

    for (conn = connections; conn; conn = conn->next) {
        if (tls_connection_dir(conn, addr, port) != -1) {
            return conn;
        }
    }
    return NULL;
}

int
tls_process_segment(capture_packet_t *packet, struct tcphdr *tcp)
{
    struct SSLConnection *conn;
    const u_char *payload = capture_packet_get_payload(packet);
    uint32_t size_payload = capture_packet_get_payload_len(packet);
    uint8 *out;
    uint32_t outl = packet->payload_len;
    out = sng_malloc(outl);
    struct in_addr ip_src, ip_dst;
    u_short sport = packet->sport;
    u_short dport = packet->dport;

    // Convert addresses
    inet_pton(AF_INET, packet->ip_src, &ip_src);
    inet_pton(AF_INET, packet->ip_dst, &ip_dst);

    // Try to find a session for this ip
    if ((conn = tls_connection_find(ip_src, sport))) {
        // Update last connection direction
        conn->direction = tls_connection_dir(conn, ip_src, sport);

        // Check current connection state
        switch (conn->state) {
            case TCP_STATE_SYN:
                // First SYN received, this package must be SYN/ACK
                if (tcp->th_flags & TH_SYN & ~TH_ACK)
                    conn->state = TCP_STATE_SYN_ACK;
                break;
            case TCP_STATE_SYN_ACK:
                // We expect an ACK packet here
                if (tcp->th_flags & ~TH_SYN & TH_ACK)
                    conn->state = TCP_STATE_ESTABLISHED;
                break;
            case TCP_STATE_ACK:
            case TCP_STATE_ESTABLISHED:
                // Process data segment!
                if (tls_process_record(conn, payload, size_payload, &out, &outl) == 0) {
                    if ((int32_t) outl > 0) {
                        capture_packet_set_payload(packet, out, outl);
                        capture_packet_set_type(packet, CAPTURE_PACKET_SIP_TLS);
                        return 0;
                    }
                }
                break;
            case TCP_STATE_FIN:
            case TCP_STATE_CLOSED:
                // We can delete this connection
                tls_connection_destroy(conn);
                break;
        }
    } else {
        if (tcp->th_flags & TH_SYN & ~TH_ACK) {
            // New connection, store it status and leave
            tls_connection_create(ip_src, sport, ip_dst, dport);
        }
    }

    sng_free(out);
    return 0;
}

int
tls_process_record(struct SSLConnection *conn, const uint8 *payload, const int len, uint8 **out,
                   uint32_t *outl)
{
    struct TLSPlaintext *record;
    int record_len;
    const opaque *fragment;

    // No record data here!
    if (len == 0)
        return 0;

    // Get Record data
    record = (struct TLSPlaintext *) payload;
    record_len = sizeof(struct TLSPlaintext) + UINT16_INT(record->length);

    // Process record fragment
    if (UINT16_INT(record->length) > 0) {
        // TLSPlaintext fragment pointer
        fragment = (opaque *) payload + sizeof(struct TLSPlaintext);

        switch (record->type) {
            case handshake:
                // Hanshake Record, Try to get MasterSecret data
                if (tls_process_record_handshake(conn, fragment) != 0)
                    return 1;
                break;
            case change_cipher_spec:
                // From now on, this connection will be encrypted using MasterSecret
                conn->encrypted = 1;
                break;
            case application_data:
                if (conn->encrypted) {
                    // Decrypt application data using MasterSecret
                    tls_process_record_data(conn, fragment, UINT16_INT(record->length), out, outl);
                }
                break;
            default:
                break;
        }
    }

    // MultiRecord packet
    if (len > record_len)
        return tls_process_record(conn, payload + record_len, len - record_len, out, outl);

    return 0;
}

int
tls_process_record_handshake(struct SSLConnection *conn, const opaque *fragment)
{
    struct Handshake *handshake;
    struct ClientHello *clienthello;
    struct ServerHello *serverhello;
    struct ClientKeyExchange *clientkeyex;
    const opaque *body;

    // Get Handshake data
    handshake = (struct Handshake *) fragment;

    if (UINT24_INT(handshake->length) > 0) {
        // Hanshake body pointer
        body = fragment + sizeof(struct Handshake);

        switch (handshake->type) {
            case hello_request:
                break;
            case client_hello:
                // Store client random
                clienthello = (struct ClientHello *) body;
                memcpy(&conn->client_random, &clienthello->random, sizeof(struct Random));
                // Check we have a TLS handshake
                if (!(clienthello->client_version.major == 0x03
                      && clienthello->client_version.minor == 0x01)) {
                    tls_connection_destroy(conn);
                    return 1;
                }
                break;
            case server_hello:
                // Store server random
                serverhello = (struct ServerHello *) body;
                memcpy(&conn->server_random, &serverhello->random, sizeof(struct Random));
                // Get the selected cipher
                memcpy(&conn->cipher_suite,
                       body + sizeof(struct ServerHello) + serverhello->session_id_length,
                       sizeof(uint16));
                // Check if we have a handled cipher
                if (tls_connection_load_cipher(conn) != 0) {
                    tls_connection_destroy(conn);
                    return 1;
                }
                break;
            case certificate:
            case certificate_request:
            case server_hello_done:
            case certificate_verify:
                break;
            case client_key_exchange:
                // Decrypt PreMasterKey
                clientkeyex = (struct ClientKeyExchange *) body;

                gnutls_datum_t exkeys, pms;
                exkeys.size = UINT16_INT(clientkeyex->length);
                exkeys.data = (unsigned char *)&clientkeyex->exchange_keys;
                gnutls_privkey_decrypt_data(conn->server_private_key, 0, &exkeys, &pms);
                memcpy(&conn->pre_master_secret, pms.data, pms.size);

                // Get MasterSecret
                unsigned char *seed = sng_malloc(sizeof(struct Random) * 2);
                memcpy(seed, &conn->client_random, sizeof(struct Random));
                memcpy(seed + sizeof(struct Random), &conn->server_random, sizeof(struct Random));
                PRF((unsigned char *) &conn->master_secret, sizeof(struct MasterSecret),
                    (unsigned char *) &conn->pre_master_secret, sizeof(struct PreMasterSecret),
                    (unsigned char *) "master secret", seed, sizeof(struct Random) * 2);

                memcpy(seed, &conn->server_random, sizeof(struct Random));
                memcpy(seed + sizeof(struct Random), &conn->client_random, sizeof(struct Random));

                // Generate MACs, Write Keys and IVs
                PRF((unsigned char *) &conn->key_material, sizeof(struct tls_data),
                    (unsigned char *) &conn->master_secret, sizeof(struct MasterSecret),
                    (unsigned char *) "key expansion", seed, sizeof(struct Random) * 2);

                // Done with the seed
                sng_free(seed);

                // Create Client decoder
                gcry_cipher_open(&conn->client_cipher_ctx, conn->ciph, GCRY_CIPHER_MODE_CBC, 0);
                gcry_cipher_setkey(conn->client_cipher_ctx,
                                   conn->key_material.client_write_key,
                                   gcry_cipher_get_algo_keylen(conn->ciph));
                gcry_cipher_setiv(conn->client_cipher_ctx,
                                  conn->key_material.client_write_IV,
                                  gcry_cipher_get_algo_blklen(conn->ciph));

                // Create Server decoder
                gcry_cipher_open(&conn->server_cipher_ctx, conn->ciph, GCRY_CIPHER_MODE_CBC, 0);
                gcry_cipher_setkey(conn->server_cipher_ctx,
                                   conn->key_material.server_write_key,
                                   gcry_cipher_get_algo_keylen(conn->ciph));
                gcry_cipher_setiv(conn->server_cipher_ctx,
                                  conn->key_material.server_write_IV,
                                  gcry_cipher_get_algo_blklen(conn->ciph));

                break;
            case finished:
                break;
            default:
                if (conn->encrypted) {
                    // Encrypted Hanshake Message
                    unsigned char *decoded = sng_malloc(48);
                    uint32_t decodedlen;
                    tls_process_record_data(conn, fragment, 48, &decoded, &decodedlen);
                    sng_free(decoded);
                }
                break;
        }
    }

    return 0;
}

int
tls_process_record_data(struct SSLConnection *conn, const opaque *fragment, const int len,
                        uint8 **out, uint32_t *outl)
{
    gcry_cipher_hd_t *evp;
    unsigned char pad;
    unsigned char *decoded;
    size_t dlen = len;

    if (conn->direction == 0) {
        evp = &conn->client_cipher_ctx;
    } else {
        evp = &conn->server_cipher_ctx;
    }

    decoded = sng_malloc(len);
    gcry_cipher_decrypt(*evp, decoded, dlen, (unsigned char *) fragment, len);

    // Get padding counter and remove from data
    pad = decoded[len - 1];
    dlen = (len - (pad + 1) - /* Trailing MAC */20);

    if ((int32_t)dlen > 0 && dlen <= *outl) {
        memcpy(*out, decoded, dlen);
        *outl = dlen;
    }

    // Clenaup decoded memory
    sng_free(decoded);
    return *outl;
}

int
tls_connection_load_cipher(struct SSLConnection *conn)
{
    if (conn->cipher_suite.cs1 != 0x00)
        return 1;

    if (conn->cipher_suite.cs2 == TLS_RSA_WITH_AES_256_CBC_SHA.cs2) {
        conn->ciph = gcry_cipher_map_name("AES256");
    } else if (conn->cipher_suite.cs2 == TLS_RSA_WITH_AES_128_CBC_SHA.cs2) {
        conn->ciph = gcry_cipher_map_name("AES");
    } else {
        return 1;
    }
    return 0;
}