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

#pragma once

#include <stdint.h>
#include <s2n.h>

#include "stuffer/s2n_stuffer.h"

struct s2n_client_hello {
    struct s2n_stuffer raw_message;

    /*
     * the 'data' pointers in the below blobs
     * point to data in the raw_message stuffer
     */
    struct s2n_blob cipher_suites;
    struct s2n_blob extensions;

    uint8_t compression_methods;
};

int s2n_client_hello_free(struct s2n_client_hello *client_hello);

extern struct s2n_client_hello *s2n_connection_get_client_hello(struct s2n_connection *conn);
extern uint32_t s2n_client_hello_get_raw_bytes(struct s2n_client_hello *ch, uint8_t *out, uint32_t max_length);
extern uint32_t s2n_client_hello_get_cipher_suites(struct s2n_client_hello *ch, uint8_t *out, uint32_t max_length);
extern uint32_t s2n_client_hello_get_extensions(struct s2n_client_hello *ch, uint8_t *out, uint32_t max_length);
