/*
 * Copyright 2017 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

#include "s2n_test.h"

#include "testlib/s2n_testlib.h"

#include <sys/wait.h>
#include <unistd.h>
#include <stdint.h>
#include <fcntl.h>
#include <errno.h>

#include <s2n.h>

#include "tls/s2n_tls.h"
#include "tls/s2n_connection.h"
#include "tls/s2n_client_hello.h"
#include "tls/s2n_handshake.h"
#include "tls/s2n_tls_parameters.h"

#include "utils/s2n_safety.h"


#define ZERO_TO_THIRTY_ONE  0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, \
                            0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F

int main(int argc, char **argv)
{
    char *cert_chain;
    char *private_key;
    BEGIN_TEST();

    EXPECT_NOT_NULL(cert_chain = malloc(S2N_MAX_TEST_PEM_SIZE));
    EXPECT_NOT_NULL(private_key = malloc(S2N_MAX_TEST_PEM_SIZE));
    EXPECT_SUCCESS(setenv("S2N_ENABLE_CLIENT_MODE", "1", 0));
    EXPECT_SUCCESS(setenv("S2N_DONT_MLOCK", "1", 0));

    /* Minimal TLS 1.2 client hello. */
    {
        struct s2n_connection *server_conn;
        struct s2n_config *server_config;
        s2n_blocked_status server_blocked;
        int server_to_client[2];
        int client_to_server[2];
        uint8_t* sent_client_hello;
        uint8_t* expected_client_hello;

        uint8_t client_extensions[] = {
            /* Extension type TLS_EXTENSION_SERVER_NAME */
            0x00, 0x00,
            /* Extension size */
            0x00, 0x08,
            /* Server names len */
            0x00, 0x06,
            /* First server name type - host name */
            0x00,
            /* First server name len */
            0x00, 0x03,
            /* First server name, matches sent_server_name */
            's', 'v', 'r',
        };
        int client_extensions_len = sizeof(client_extensions);
        uint8_t client_hello_prefix[] = {
            /* Protocol version TLS 1.2 */
            0x03, 0x03,
            /* Client random */
            ZERO_TO_THIRTY_ONE,
            /* SessionID len - 32 bytes */
            0x20,
            /* Session ID */
            ZERO_TO_THIRTY_ONE,
            /* Cipher suites len */
            0x00, 0x02,
            /* Cipher suite - TLS_RSA_WITH_AES_128_CBC_SHA256 */
            0x00, 0x3C,
            /* Compression methods len */
            0x01,
            /* Compression method - none */
            0x00,
            /* Extensions len */
            (client_extensions_len >> 8) & 0xff, (client_extensions_len & 0xff),
        };
        int client_hello_prefix_len = sizeof(client_hello_prefix);
        int sent_client_hello_len = client_hello_prefix_len + client_extensions_len;
        uint8_t message_header[] = {
            /* Handshake message type CLIENT HELLO */
            0x01,
            /* Body len */
            (sent_client_hello_len >> 16) & 0xff, (sent_client_hello_len >> 8) & 0xff, (sent_client_hello_len & 0xff),
        };
        int message_len = sizeof(message_header) + sent_client_hello_len;
        uint8_t record_header[] = {
            /* Record type HANDSHAKE */
            0x16,
            /* Protocol version TLS 1.2 */
            0x03, 0x03,
            /* Message len */
            (message_len >> 8) & 0xff, (message_len & 0xff),
        };

        EXPECT_NOT_NULL(sent_client_hello = malloc(sent_client_hello_len));
        memcpy_check(sent_client_hello, client_hello_prefix, client_hello_prefix_len);
        memcpy_check(sent_client_hello + client_hello_prefix_len, client_extensions, client_extensions_len);

        /* Create nonblocking pipes */
        EXPECT_SUCCESS(pipe(server_to_client));
        EXPECT_SUCCESS(pipe(client_to_server));
        for (int i = 0; i < 2; i++) {
            EXPECT_NOT_EQUAL(fcntl(server_to_client[i], F_SETFL, fcntl(server_to_client[i], F_GETFL) | O_NONBLOCK), -1);
            EXPECT_NOT_EQUAL(fcntl(client_to_server[i], F_SETFL, fcntl(client_to_server[i], F_GETFL) | O_NONBLOCK), -1);
        }

        EXPECT_NOT_NULL(server_conn = s2n_connection_new(S2N_SERVER));
        server_conn->actual_protocol_version = S2N_TLS12;
        server_conn->server_protocol_version = S2N_TLS12;
        server_conn->client_protocol_version = S2N_TLS12;
        EXPECT_SUCCESS(s2n_connection_set_read_fd(server_conn, client_to_server[0]));
        EXPECT_SUCCESS(s2n_connection_set_write_fd(server_conn, server_to_client[1]));

        EXPECT_NOT_NULL(server_config = s2n_config_new());
        EXPECT_SUCCESS(s2n_read_test_pem(S2N_DEFAULT_TEST_CERT_CHAIN, cert_chain, S2N_MAX_TEST_PEM_SIZE));
        EXPECT_SUCCESS(s2n_read_test_pem(S2N_DEFAULT_TEST_PRIVATE_KEY, private_key, S2N_MAX_TEST_PEM_SIZE));
        EXPECT_SUCCESS(s2n_config_add_cert_chain_and_key(server_config, cert_chain, private_key));
        EXPECT_SUCCESS(s2n_connection_set_config(server_conn, server_config));

        /* Verify s2n_connection_get_client_hello returns null if ClientHello not yet processed */
        EXPECT_NULL(s2n_connection_get_client_hello(server_conn));

        /* Send the client hello message */
        EXPECT_EQUAL(write(client_to_server[1], record_header, sizeof(record_header)), sizeof(record_header));
        EXPECT_EQUAL(write(client_to_server[1], message_header, sizeof(message_header)), sizeof(message_header));
        EXPECT_EQUAL(write(client_to_server[1], sent_client_hello, sent_client_hello_len), sent_client_hello_len);

        /* Verify that the sent client hello message is accepted */
        s2n_negotiate(server_conn, &server_blocked);
        EXPECT_TRUE(s2n_conn_get_current_message_type(server_conn) > CLIENT_HELLO);
        EXPECT_EQUAL(server_conn->handshake.handshake_type, NEGOTIATED | FULL_HANDSHAKE);

        struct s2n_client_hello *client_hello = s2n_connection_get_client_hello(server_conn);

        /* Verify s2n_connection_get_client_hello returns handle to the s2n_client_hello on the connection */
        EXPECT_EQUAL(client_hello, &server_conn->client_hello);

        uint8_t* collected_client_hello = client_hello->raw_message.blob.data;
        uint16_t collected_client_hello_len = client_hello->raw_message.blob.size;

        /* Verify collected client hello message length */
        EXPECT_EQUAL(collected_client_hello_len, sent_client_hello_len);

        /* Verify the collected client hello has client random zero-ed out */
        uint8_t client_random_offset = S2N_TLS_PROTOCOL_VERSION_LEN;
        uint8_t expected_client_random[S2N_TLS_RANDOM_DATA_LEN] = { 0 };
        EXPECT_SUCCESS(memcmp(collected_client_hello + client_random_offset, expected_client_random, S2N_TLS_RANDOM_DATA_LEN));

        /* Verify the collected client hello matches what was sent except for the zero-ed client random */
        EXPECT_NOT_NULL(expected_client_hello = malloc(sent_client_hello_len));
        memcpy_check(expected_client_hello, sent_client_hello, sent_client_hello_len);
        memset_check(expected_client_hello + client_random_offset, 0, S2N_TLS_RANDOM_DATA_LEN);
        EXPECT_SUCCESS(memcmp(collected_client_hello, expected_client_hello, sent_client_hello_len));

        uint8_t* raw_ch_out;

        /* Verify get_client_hello_bytes retrieves the full message when its len <= max_len */
        EXPECT_TRUE(collected_client_hello_len < S2N_LARGE_RECORD_LENGTH);
        EXPECT_NOT_NULL(raw_ch_out = malloc(S2N_LARGE_RECORD_LENGTH));
        EXPECT_EQUAL(sent_client_hello_len, s2n_client_hello_get_raw_bytes(client_hello, raw_ch_out, S2N_LARGE_RECORD_LENGTH));
        EXPECT_SUCCESS(memcmp(raw_ch_out, expected_client_hello, sent_client_hello_len));
        free(raw_ch_out);
        raw_ch_out = NULL;

        /* Verify get_client_hello_bytes retrieves truncated message when its len > max_len */
        EXPECT_TRUE(collected_client_hello_len > 0);
        uint32_t max_len = collected_client_hello_len - 1;
        EXPECT_NOT_NULL(raw_ch_out = malloc(max_len));
        EXPECT_EQUAL(max_len, s2n_client_hello_get_raw_bytes(client_hello, raw_ch_out, max_len));
        EXPECT_SUCCESS(memcmp(raw_ch_out, expected_client_hello, max_len));
        free(raw_ch_out);
        raw_ch_out = NULL;

        uint8_t expected_cs[] = { 0x00, 0x3C };

        /* Verify collected cipher_suites size correct */
        EXPECT_EQUAL(client_hello->cipher_suites.size, sizeof(expected_cs));

        /* Verify collected cipher_suites correct */
        EXPECT_SUCCESS(memcmp(client_hello->cipher_suites.data, expected_cs, sizeof(expected_cs)));

        /* Verify get_cipher_suites correct */
        uint8_t* cs_out;

        /* Verify get_cipher_suites retrieves the full cipher_suites when its len <= max_len */
        EXPECT_TRUE(client_hello->cipher_suites.size < S2N_LARGE_RECORD_LENGTH);
        EXPECT_NOT_NULL(cs_out = malloc(S2N_LARGE_RECORD_LENGTH));
        EXPECT_EQUAL(sizeof(expected_cs), s2n_client_hello_get_cipher_suites(client_hello, cs_out, S2N_LARGE_RECORD_LENGTH));
        EXPECT_SUCCESS(memcmp(cs_out, client_hello->cipher_suites.data, sizeof(expected_cs)));
        free(cs_out);
        cs_out = NULL;

        /* Verify get_cipher_suites retrieves truncated message when cipher_suites len > max_len */
        max_len = sizeof(expected_cs) - 1;
        EXPECT_TRUE(max_len > 0);

        EXPECT_NOT_NULL(cs_out = malloc(max_len));
        EXPECT_EQUAL(max_len, s2n_client_hello_get_cipher_suites(client_hello, cs_out, max_len));
        EXPECT_SUCCESS(memcmp(cs_out, client_hello->cipher_suites.data, max_len));
        free(cs_out);
        cs_out = NULL;

        /* Verify collected extensions size correct */
        EXPECT_EQUAL(client_hello->extensions.size, client_extensions_len);

        /* Verify collected extensions correct */
        EXPECT_SUCCESS(memcmp(client_hello->extensions.data, client_extensions, client_extensions_len));

        /* Verify get_extensions correct */
        uint8_t* extensions_out;

        /* Verify get_extensions retrieves the full cipher_suites when its len <= max_len */
        EXPECT_TRUE(client_hello->extensions.size < S2N_LARGE_RECORD_LENGTH);
        EXPECT_NOT_NULL(extensions_out = malloc(S2N_LARGE_RECORD_LENGTH));
        EXPECT_EQUAL(client_extensions_len, s2n_client_hello_get_extensions(client_hello, extensions_out, S2N_LARGE_RECORD_LENGTH));
        EXPECT_SUCCESS(memcmp(extensions_out, client_extensions, client_extensions_len));
        free(extensions_out);
        extensions_out = NULL;

        /* Verify get_extensions retrieves truncated message when cipher_suites len > max_len */
        max_len = client_extensions_len - 1;
        EXPECT_TRUE(max_len > 0);

        EXPECT_NOT_NULL(extensions_out = malloc(max_len));
        EXPECT_EQUAL(max_len, s2n_client_hello_get_extensions(client_hello, extensions_out, max_len));
        EXPECT_SUCCESS(memcmp(extensions_out, client_hello->extensions.data, max_len));
        free(extensions_out);
        extensions_out = NULL;

        /* Not a real tls client but make sure we block on its close_notify */
        int shutdown_rc = s2n_shutdown(server_conn, &server_blocked);
        EXPECT_EQUAL(shutdown_rc, -1);
        EXPECT_EQUAL(errno, EAGAIN);
        EXPECT_EQUAL(server_conn->close_notify_queued, 1);

         /* Wipe connection */
        s2n_connection_wipe(server_conn);

        /* Verify connection_wipe resized the s2n_client_hello.raw_message stuffer */
        EXPECT_NOT_NULL(client_hello->raw_message.blob.data);
        EXPECT_EQUAL(client_hello->raw_message.blob.size, S2N_LARGE_RECORD_LENGTH);

        /* Verify connection_wipe cleared the s2n_client_hello.raw_message stuffer data */
        uint8_t zero_buffer[S2N_LARGE_RECORD_LENGTH] = { 0 };
        EXPECT_SUCCESS(memcmp(collected_client_hello, zero_buffer, S2N_LARGE_RECORD_LENGTH));

        /* Verify the s2n blobs referencing cipher_suites and extensions have cleared */
        EXPECT_EQUAL(client_hello->cipher_suites.size, 0);
        EXPECT_NULL(client_hello->cipher_suites.data);
        EXPECT_EQUAL(client_hello->extensions.size, 0);
        EXPECT_NULL(client_hello->extensions.data);


        /* Verify the connection is successfully reused after connection_wipe */

        /* Re-configure connection */
        server_conn->actual_protocol_version = S2N_TLS12;
        server_conn->server_protocol_version = S2N_TLS12;
        server_conn->client_protocol_version = S2N_TLS12;
        EXPECT_SUCCESS(s2n_connection_set_read_fd(server_conn, client_to_server[0]));
        EXPECT_SUCCESS(s2n_connection_set_write_fd(server_conn, server_to_client[1]));

        EXPECT_NOT_NULL(server_config = s2n_config_new());
        EXPECT_SUCCESS(s2n_read_test_pem(S2N_DEFAULT_TEST_CERT_CHAIN, cert_chain, S2N_MAX_TEST_PEM_SIZE));
        EXPECT_SUCCESS(s2n_read_test_pem(S2N_DEFAULT_TEST_PRIVATE_KEY, private_key, S2N_MAX_TEST_PEM_SIZE));
        EXPECT_SUCCESS(s2n_config_add_cert_chain_and_key(server_config, cert_chain, private_key));
        EXPECT_SUCCESS(s2n_connection_set_config(server_conn, server_config));

       /* Re-send the client hello message */
        EXPECT_EQUAL(write(client_to_server[1], record_header, sizeof(record_header)), sizeof(record_header));
        EXPECT_EQUAL(write(client_to_server[1], message_header, sizeof(message_header)), sizeof(message_header));
        EXPECT_EQUAL(write(client_to_server[1], sent_client_hello, sent_client_hello_len), sent_client_hello_len);

        /* Verify that the sent client hello message is accepted */
        s2n_negotiate(server_conn, &server_blocked);
        EXPECT_TRUE(s2n_conn_get_current_message_type(server_conn) > CLIENT_HELLO);
        EXPECT_EQUAL(server_conn->handshake.handshake_type, NEGOTIATED | FULL_HANDSHAKE);

        /* Verify the collected client hello on the reused connection matches the expected client hello */
        client_hello = s2n_connection_get_client_hello(server_conn);
        collected_client_hello = client_hello->raw_message.blob.data;
        EXPECT_SUCCESS(memcmp(collected_client_hello, expected_client_hello, sent_client_hello_len));

        /* Not a real tls client but make sure we block on its close_notify */
        shutdown_rc = s2n_shutdown(server_conn, &server_blocked);
        EXPECT_EQUAL(shutdown_rc, -1);
        EXPECT_EQUAL(errno, EAGAIN);
        EXPECT_EQUAL(server_conn->close_notify_queued, 1);

        EXPECT_SUCCESS(s2n_connection_free(server_conn));

        EXPECT_SUCCESS(s2n_config_free(server_config));
        for (int i = 0; i < 2; i++) {
            EXPECT_SUCCESS(close(server_to_client[i]));
            EXPECT_SUCCESS(close(client_to_server[i]));
        }

        free(expected_client_hello);
        free(sent_client_hello);
    }

    free(cert_chain);
    free(private_key);
    END_TEST();
    return 0;
}

