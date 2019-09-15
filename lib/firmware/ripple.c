/*
 * This file is part of the KeepKey project.
 *
 * Copyright (C) 2019 ShapeShift
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "keepkey/firmware/ripple.h"

#include "trezor/crypto/base58.h"
#include "trezor/crypto/secp256k1.h"

#include <assert.h>

typedef enum {
    FT_INT16 = 1,
    FT_INT32 = 2,
    FT_AMOUNT = 6,
    FT_VL = 7,
    FT_ACCOUNT = 8,
} FieldType;

typedef struct _FieldMapping {
    FieldType type;
    int key;
} FieldMapping;

static const FieldMapping FM_account =            { .type = FT_ACCOUNT, .key = 1 };
static const FieldMapping FM_amount  =            { .type = FT_AMOUNT,  .key = 1 };
static const FieldMapping FM_destination =        { .type = FT_ACCOUNT, .key = 3 };
static const FieldMapping FM_fee =                { .type = FT_AMOUNT,  .key  = 8 };
static const FieldMapping FM_sequence =           { .type = FT_INT32,   .key = 4 };
static const FieldMapping FM_type =               { .type = FT_INT16,   .key = 2 };
static const FieldMapping FM_signingPubKey =      { .type = FT_VL,      .key = 3 };
static const FieldMapping FM_flags =              { .type = FT_INT32,   .key = 2 };
static const FieldMapping FM_txnSignature =       { .type = FT_VL,      .key = 4 };
static const FieldMapping FM_lastLedgerSequence = { .type = FT_INT32,   .key = 27 };
static const FieldMapping FM_destinationTag =     { .type = FT_INT32,   .key = 14 };


// https://developers.ripple.com/base58-encodings.html
static const char *ripple_b58digits = "rpshnaf39wBUDNEGHJKLM4PQRST7VWXYZ2bcdeCg65jkm8oFqi1tuvAxyz";

bool ripple_getAddress(const HDNode *node, char address[MAX_ADDR_SIZE])
{
    uint8_t buff[64];
    memset(buff, 0, sizeof(buff));

    Hasher hasher;
    hasher_Init(&hasher, HASHER_SHA2_RIPEMD);
    hasher_Update(&hasher, node->public_key, 33);
    hasher_Final(&hasher, buff + 1);

    if (!base58_encode_check(buff, 21, HASHER_SHA2D,
                             address, MAX_ADDR_SIZE, ripple_b58digits))
        return false;

    return true;
}

void ripple_formatAmount(char *buf, size_t len, uint64_t amount)
{
    bignum256 val;
    bn_read_uint64(amount, &val);
    bn_format(&val, NULL, " XRP", RIPPLE_DECIMALS, 0, false, buf, len);
}

static bool append_u8(uint8_t **buf, const uint8_t *end, uint8_t val)
{
    if (*buf + 1 > end) {
        return false;
    }

    **buf = val;
    *buf += 1;
    return true;
}

bool ripple_serializeType(uint8_t **buf, const uint8_t *end, const FieldMapping *m)
{
    if (m->key <= 0xf) {
        return append_u8(buf, end, m->type << 4 | m->key);
    } else {
        bool ok = true;
        ok = ok && append_u8(buf, end, m->type << 4);
        ok = ok && append_u8(buf, end, m->key);
        return ok;
    }
}

bool ripple_serializeInt16(uint8_t **buf, const uint8_t *end,
                           const FieldMapping *m, int16_t val)
{
    assert(m->type == FT_INT16 && "wrong type?");

    bool ok = true;
    ok = ok && ripple_serializeType(buf, end, m);
    ok = ok && append_u8(buf, end, (val >> 8) & 0xff);
    ok = ok && append_u8(buf, end, val & 0xff);
    return ok;
}

bool ripple_serializeInt32(uint8_t **buf, const uint8_t *end,
                           const FieldMapping *m, int32_t val)
{
    assert(m->type == FT_INT32 && "wrong type?");
    bool ok = true;

    ok = ok && ripple_serializeType(buf, end, m);
    ok = ok && append_u8(buf, end, (val >> 24) & 0xff);
    ok = ok && append_u8(buf, end, (val >> 16) & 0xff);
    ok = ok && append_u8(buf, end, (val >>  8) & 0xff);
    ok = ok && append_u8(buf, end, val & 0xff);
    return ok;
}

bool ripple_serializeAmount(uint8_t **buf, const uint8_t *end,
                            const FieldMapping *m, int64_t amount)
{
    bool ok = true;
    ok = ok && ripple_serializeType(buf, end, m);

    assert(amount >= 0 && "amounts cannot be negative");
    assert(amount <= 100000000000 && "larger amounts not supported");
    uint8_t msb = (amount >> (7 * 8)) & 0xff;
    msb &= 0x7f; // Clear first bit, indicating XRP
    msb |= 0x40; // Clear second bit, indicating value is positive

    ok = ok && append_u8(buf, end, msb);
    ok = ok && append_u8(buf, end, (amount >> (6 * 8)) & 0xff);
    ok = ok && append_u8(buf, end, (amount >> (5 * 8)) & 0xff);
    ok = ok && append_u8(buf, end, (amount >> (4 * 8)) & 0xff);
    ok = ok && append_u8(buf, end, (amount >> (3 * 8)) & 0xff);
    ok = ok && append_u8(buf, end, (amount >> (2 * 8)) & 0xff);
    ok = ok && append_u8(buf, end, (amount >> (1 * 8)) & 0xff);
    ok = ok && append_u8(buf, end, amount & 0xff);
    return ok;
}

bool ripple_serializeVarint(uint8_t **buf, const uint8_t *end, int val)
{
    if (val < 0)
        return false;

    if (val < 192) {
        return append_u8(buf, end, val);
    }

    if (val <= 12480) {
        val -= 193;
        bool ok = true;
        ok = ok && append_u8(buf, end, 193 + (val >> 8));
        ok = ok && append_u8(buf, end, val & 0xff);
        return ok;
    }

    if (val < 918744) {
        assert(*buf + 3 < end && "buffer not long enough");
        val -= 12481;
        bool ok = true;
        ok = ok && append_u8(buf, end, 241 + (val >> 16));
        ok = ok && append_u8(buf, end, (val >> 8) & 0xff);
        ok = ok && append_u8(buf, end, val & 0xff);
        return ok;
    }

    assert(false && "value too large");
    return false;
}

bool ripple_serializeBytes(uint8_t **buf, const uint8_t *end,
                           const uint8_t *bytes, size_t count)
{
    bool ok = true;
    ok = ok && ripple_serializeVarint(buf, end, count);

    if (!ok || *buf + count >= end) {
        assert(false && "buffer not long enough");
        return false;
    }

    memcpy(*buf, bytes, count);
    *buf += count;
    return ok;
}

bool ripple_serializeAddress(uint8_t **buf, const uint8_t *end,
                             const FieldMapping *m, const char *address)
{
    uint8_t addr_raw[MAX_ADDR_RAW_SIZE];
    uint32_t addr_raw_len = base58_decode_check(address, HASHER_SHA2D,
                                                addr_raw, MAX_ADDR_RAW_SIZE,
                                                ripple_b58digits);
    if (addr_raw_len != 20) {
        // FIXME: something is broken in base58_decode_check
        //return false;

        *buf += 20;
        return true;
    }

    return ripple_serializeBytes(buf, end, addr_raw + 1, addr_raw_len - 1);
}

bool ripple_serializeVL(uint8_t **buf, const uint8_t *end, const FieldMapping *m,
                        const uint8_t *bytes, size_t count)
{
    bool ok = true;
    ok = ok && ripple_serializeType(buf, end, m);
    ok = ok && ripple_serializeBytes(buf, end, bytes, count);
    return ok;
}

bool ripple_serialize(uint8_t **buf, const uint8_t *end, const RippleSignTx *tx,
                      const char *source_address,
                      const uint8_t *pubkey, const uint8_t *sig)
{
    bool ok = true;
    ok = ok && ripple_serializeInt16(buf, end, &FM_type, /*Payment*/0);
    if (tx->has_flags)
        ok = ok && ripple_serializeInt32(buf, end, &FM_flags, tx->flags);
    if (tx->has_sequence)
        ok = ok && ripple_serializeInt32(buf, end, &FM_sequence, tx->sequence);
    if (tx->payment.has_destination_tag)
        ok = ok && ripple_serializeInt32(buf, end, &FM_destinationTag, tx->payment.destination_tag);
    if (tx->has_last_ledger_sequence)
        ok = ok && ripple_serializeInt32(buf, end, &FM_lastLedgerSequence, tx->last_ledger_sequence);
    if (tx->payment.has_amount)
        ok = ok && ripple_serializeAmount(buf, end, &FM_amount, tx->payment.amount);
    if (tx->has_fee)
        ok = ok && ripple_serializeAmount(buf, end, &FM_amount, tx->fee);
    if (pubkey)
        ok = ok && ripple_serializeVL(buf, end, &FM_signingPubKey, pubkey, 33);
    if (sig)
        ok = ok && ripple_serializeVL(buf, end, &FM_txnSignature, sig, 64);
    if (source_address)
        ok = ok && ripple_serializeAddress(buf, end, &FM_account, source_address);
    if (tx->payment.has_destination)
        ok = ok && ripple_serializeAddress(buf, end, &FM_destination, tx->payment.destination);
    return ok;
}

void ripple_signTx(const HDNode *node, RippleSignTx *tx,
                   RippleSignedTx *resp) {
    const curve_info *curve = get_curve_by_name("secp256k1");
    if (!curve) return;

    // Set canonical flag, since trezor-crypto ECDSA implementation returns
    // fully-canonical signatures, thereby enforcing it in the transaction
    // using the designated flag.
    // See: https://github.com/trezor/trezor-crypto/blob/3e8974ff8871263a70b7fbb9a27a1da5b0d810f7/ecdsa.c#L791
    if (!tx->has_flags) {
        tx->flags = 0;
        tx->has_flags = true;
    }
    tx->flags |= RIPPLE_FLAG_FULLY_CANONICAL;

    memset(resp->serialized_tx.bytes, 0, sizeof(resp->serialized_tx.bytes));

    // 'STX'
    memcpy(resp->serialized_tx.bytes, "\x53\x54\x58\x00", 4);

    char source_address[MAX_ADDR_SIZE];
    if (!ripple_getAddress(node, source_address))
        return;

    uint8_t *buf = resp->serialized_tx.bytes + 4;
    size_t len = sizeof(resp->serialized_tx.bytes) - 4;
    if (!ripple_serialize(&buf, buf + len, tx, source_address, node->public_key, NULL))
        return;

    // Ripple uses the first half of SHA512
    uint8_t hash[64];
    sha512_Raw(resp->serialized_tx.bytes, buf - resp->serialized_tx.bytes, hash);

    uint8_t sig[64];
    if (ecdsa_sign_digest(&secp256k1, node->private_key, hash, sig, NULL, NULL) != 0) {
        // Failure
    }

    resp->signature.size = ecdsa_sig_to_der(sig, resp->signature.bytes);
    resp->has_signature = true;

    memset(resp->serialized_tx.bytes, 0, sizeof(resp->serialized_tx.bytes));

    buf = resp->serialized_tx.bytes;
    len = sizeof(resp->serialized_tx);
    if (!ripple_serialize(&buf, buf + len, tx, source_address, node->public_key, resp->signature.bytes))
        return;

    resp->has_serialized_tx = true;
    resp->serialized_tx.size = buf - resp->serialized_tx.bytes;
}
