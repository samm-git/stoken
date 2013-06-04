/*
 * securid.c - SecurID token handling
 *
 * Copyright 2012 Kevin Cernekee <cernekee@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "config.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include TOMCRYPT_HDR
#include "securid.h"

static void aes128_ecb_encrypt(const uint8_t *key, const uint8_t *in,
	uint8_t *out)
{
	symmetric_key skey;
	uint8_t tmp[AES_BLOCK_SIZE];

	/* these shouldn't allocate memory or fail */
	if (rijndael_setup(key, AES_KEY_SIZE, 0, &skey) != CRYPT_OK ||
	    rijndael_ecb_encrypt(in, tmp, &skey) != CRYPT_OK)
		abort();
	rijndael_done(&skey);

	/* in case "in" and "out" point to the same buffer */
	memcpy(out, tmp, AES_BLOCK_SIZE);
}

static void aes128_ecb_decrypt(const uint8_t *key, const uint8_t *in,
	uint8_t *out)
{
	symmetric_key skey;
	uint8_t tmp[AES_BLOCK_SIZE];

	if (rijndael_setup(key, AES_KEY_SIZE, 0, &skey) != CRYPT_OK ||
	    rijndael_ecb_decrypt(in, tmp, &skey) != CRYPT_OK)
		abort();
	rijndael_done(&skey);

	memcpy(out, tmp, AES_BLOCK_SIZE);
}

static int securid_rand(void *out, int len)
{
	if (rng_get_bytes(out, len, NULL) != len)
		return ERR_GENERAL;
	return ERR_NONE;
}

static void encrypt_then_xor(const uint8_t *key, uint8_t *work, uint8_t *enc)
{
	int i;

	aes128_ecb_encrypt(key, work, enc);
	for (i = 0; i < AES_BLOCK_SIZE; i++)
		work[i] ^= enc[i];
}

static void securid_mac(const uint8_t *in, int in_len, uint8_t *out)
{
	int i, odd = 0;
	const int incr = AES_KEY_SIZE;
	uint8_t work[incr], enc[incr], pad[incr], zero[incr], lastblk[incr], *p;

	memset(zero, 0, incr);
	memset(pad, 0, incr);
	memset(lastblk, 0, incr);
	memset(work, 0xff, incr);

	/* padding */
	p = &pad[incr - 1];
	for (i = in_len * 8; i > 0; i >>= 8)
		*(p--) = (uint8_t)i;

	/* handle the bulk of the input data here */
	for (; in_len > incr; in_len -= incr, in += incr, odd = !odd)
		encrypt_then_xor(in, work, enc);

	/* final 0-16 bytes of input data */
	memcpy(lastblk, in, in_len);
	encrypt_then_xor(lastblk, work, enc);

	/* hash an extra block of zeroes, for certain input lengths */
	if (odd)
		encrypt_then_xor(zero, work, enc);

	/* always hash the padding */
	encrypt_then_xor(pad, work, enc);

	/* run hash over current hash value, then return */
	memcpy(out, work, incr);
	encrypt_then_xor(work, out, enc);
}

static uint16_t securid_shortmac(const uint8_t *in, int in_len)
{
	uint8_t hash[AES_BLOCK_SIZE];

	securid_mac(in, in_len, hash);
	return (hash[0] << 7) | (hash[1] >> 1);
}

static void numinput_to_bits(const char *in, uint8_t *out, unsigned int n_bits)
{
	int bitpos = 13;

	memset(out, 0, (n_bits + 7) / 8);
	for (; n_bits; n_bits -= TOKEN_BITS_PER_CHAR, in++) {
		uint16_t decoded = (*in - '0') & 0x07;
		decoded <<= bitpos;
		out[0] |= decoded >> 8;
		out[1] |= decoded & 0xff;

		bitpos -= TOKEN_BITS_PER_CHAR;
		if (bitpos < 0) {
			bitpos += 8;
			out++;
		}
	}
}

static void bits_to_numoutput(const uint8_t *in, char *out, unsigned int n_bits)
{
	int bitpos = 13;

	for (; n_bits; n_bits -= TOKEN_BITS_PER_CHAR, out++) {
		uint16_t binary = (in[0] << 8) | in[1];
		*out = ((binary >> bitpos) & 0x07) + '0';

		bitpos -= TOKEN_BITS_PER_CHAR;
		if (bitpos < 0) {
			bitpos += 8;
			in++;
		}
	}
	*out = 0;
}

static uint32_t get_bits(const uint8_t *in, unsigned int start, int n_bits)
{
	uint32_t out = 0;

	in += start / 8;
	start %= 8;

	for (; n_bits > 0; n_bits--) {
		out <<= 1;
		if ((*in << start) & 0x80)
			out |= 0x01;
		start++;
		if (start == 8) {
			start = 0;
			in++;
		}
	}
	return out;
}

static void set_bits(uint8_t *out, unsigned int start, int n_bits, uint32_t val)
{
	out += start / 8;
	start %= 8;
	val <<= (32 - n_bits);

	for (; n_bits > 0; n_bits--) {
		if (val & BIT(31))
			*out |= BIT(7 - start);
		else
			*out &= ~BIT(7 - start);
		val <<= 1;
		start++;
		if (start == 8) {
			start = 0;
			out++;
		}
	}
}

int securid_decode_token(const char *in, struct securid_token *t)
{
	uint8_t d[MAX_TOKEN_BITS / 8 + 2];
	int len = strlen(in);
	uint16_t token_mac, computed_mac;

	if (len < MIN_TOKEN_CHARS || len > MAX_TOKEN_CHARS)
		return ERR_BAD_LEN;

	/* we can handle v1 or v2 tokens */
	if (in[0] != '1' && in[0] != '2')
		return ERR_TOKEN_VERSION;

	memset(t, 0, sizeof(*t));

	/* the last 5 digits provide a checksum for the rest of the string */
	numinput_to_bits(&in[len - CHECKSUM_CHARS], d, 15);
	token_mac = get_bits(d, 0, 15);
	computed_mac = securid_shortmac(in, len - CHECKSUM_CHARS);

	if (token_mac != computed_mac)
		return ERR_CHECKSUM_FAILED;

	memcpy(&t->serial, &in[VER_CHARS], SERIAL_CHARS);
	t->serial[SERIAL_CHARS] = 0;

	numinput_to_bits(&in[BINENC_OFS], d, BINENC_BITS);
	memcpy(t->enc_seed, d, AES_KEY_SIZE);
	t->has_enc_seed = 1;

	t->flags = get_bits(d, 128, 16);
	t->exp_date = get_bits(d, 144, 14);
	t->dec_seed_hash = get_bits(d, 159, 15);
	t->device_id_hash = get_bits(d, 174, 15);

	return ERR_NONE;
}

static int generate_key_hash(uint8_t *key_hash, const char *pass,
	const char *devid, uint16_t *device_id_hash, int is_smartphone)
{
	uint8_t key[MAX_PASS + DEVID_CHARS + MAGIC_LEN + 1], *devid_buf;
	int pos = 0, devid_len = is_smartphone ? 40 : 32;
	const uint8_t magic[] = { 0xd8, 0xf5, 0x32, 0x53, 0x82, 0x89, 0x00 };

	memset(key, 0, sizeof(key));

	if (pass) {
		pos = strlen(pass);
		if (pos > MAX_PASS)
			return ERR_BAD_PASSWORD;
		memcpy(key, pass, pos);
	}

	devid_buf = &key[pos];
	if (devid) {
		int len = 0;

		/*
		 * For iPhone/Android ctf strings, the device ID takes up
		 * 40 bytes and consists of hex digits + zero padding.
		 *
		 * For other ctf strings (e.g. --blocks), the device ID takes
		 * up 32 bytes and consists of decimal digits + zero padding.
		 *
		 * If this seed isn't locked to a device, we'll just hash
		 * 40 (or 32) zero bytes, below.
		 */
		for (; *devid; devid++) {
			if ((is_smartphone && !isxdigit(*devid)) ||
			    (!is_smartphone && !isdigit(*devid)))
				continue;
			if (len++ > devid_len)
				return ERR_BAD_PASSWORD;
			key[pos++] = *devid;
		}
	}
	if (device_id_hash)
		*device_id_hash = securid_shortmac(devid_buf, devid_len);

	memcpy(&key[pos], magic, MAGIC_LEN);
	securid_mac(key, pos + MAGIC_LEN, key_hash);

	return ERR_NONE;
}

int securid_decrypt_seed(struct securid_token *t, const char *pass,
	const char *devid)
{
	uint8_t key_hash[AES_BLOCK_SIZE], dec_seed_hash[AES_BLOCK_SIZE];
	uint16_t computed_mac;
	uint16_t device_id_hash;
	int rc;

	if (t->flags & FL_PASSPROT && !pass)
		return ERR_MISSING_PASSWORD;
	if (t->flags & FL_SNPROT && !devid)
		return ERR_MISSING_PASSWORD;

	rc = generate_key_hash(key_hash,
			       t->flags & FL_PASSPROT ? pass : NULL,
			       t->flags & FL_SNPROT ? devid : NULL,
			       &device_id_hash, t->is_smartphone);
	if (rc)
		return rc;

	if (t->flags & FL_SNPROT && device_id_hash != t->device_id_hash)
		return ERR_BAD_DEVID;

	aes128_ecb_decrypt(key_hash, t->enc_seed, t->dec_seed);
	securid_mac(t->dec_seed, AES_KEY_SIZE, dec_seed_hash);
	computed_mac = (dec_seed_hash[0] << 7) | (dec_seed_hash[1] >> 1);

	if (computed_mac != t->dec_seed_hash)
		return ERR_DECRYPT_FAILED;
	t->has_dec_seed = 1;

	return ERR_NONE;
}

static void key_from_time(const uint8_t *bcd_time, int bcd_time_bytes,
	const uint8_t *serial, uint8_t *key)
{
	int i;

	memset(key, 0xaa, 8);
	memcpy(key, bcd_time, bcd_time_bytes);
	memset(key + 12, 0xbb, 4);

	/* write BCD-encoded partial serial number */
	key += 8;
	for (i = 4; i < 12; i += 2)
		*(key++) = ((serial[i] - '0') << 4) |
			    (serial[i + 1] - '0');
}

static void bcd_write(uint8_t *out, int val, unsigned int bytes)
{
	out += bytes - 1;
	for (; bytes; bytes--) {
		*out = val % 10;
		val /= 10;
		*(out--) |= (val % 10) << 4;
		val /= 10;
	}
}

void securid_compute_tokencode(struct securid_token *t, time_t now,
	char *code_out)
{
	uint8_t bcd_time[8];
	uint8_t key0[AES_KEY_SIZE], key1[AES_KEY_SIZE];
	int i;
	uint32_t tokencode;
	struct tm *gmt;
	int pin_len = strlen(t->pin);

	gmt = gmtime(&now);
	bcd_write(&bcd_time[0], gmt->tm_year + 1900, 2);
	bcd_write(&bcd_time[2], gmt->tm_mon + 1, 1);
	bcd_write(&bcd_time[3], gmt->tm_mday, 1);
	bcd_write(&bcd_time[4], gmt->tm_hour, 1);
	bcd_write(&bcd_time[5], gmt->tm_min & ~0x03, 1);
	bcd_time[6] = bcd_time[7] = 0;

	key_from_time(bcd_time, 2, t->serial, key0);
	aes128_ecb_encrypt(t->dec_seed, key0, key0);
	key_from_time(bcd_time, 3, t->serial, key1);
	aes128_ecb_encrypt(key0, key1, key1);
	key_from_time(bcd_time, 4, t->serial, key0);
	aes128_ecb_encrypt(key1, key0, key0);
	key_from_time(bcd_time, 5, t->serial, key1);
	aes128_ecb_encrypt(key0, key1, key1);
	key_from_time(bcd_time, 8, t->serial, key0);
	aes128_ecb_encrypt(key1, key0, key0);

	/* key0 now contains 4 consecutive token codes */
	i = (gmt->tm_min & 0x03) << 2;
	tokencode = (key0[i + 0] << 24) | (key0[i + 1] << 16) |
		    (key0[i + 2] << 8)  | (key0[i + 3] << 0);

	/* add PIN digits to tokencode, if available */
	for (i = 0; i < 8; i++) {
		uint8_t c = tokencode % 10;
		tokencode /= 10;

		if (i < pin_len)
			c += t->pin[pin_len - i - 1] - '0';
		code_out[7 - i] = c % 10 + '0';
	}
	code_out[8] = 0;
}

int securid_encode_token(const struct securid_token *t, const char *pass,
	const char *devid, char *out)
{
	struct securid_token newt = *t;
	uint8_t d[MAX_TOKEN_BITS / 8 + 2];
	uint8_t key_hash[AES_BLOCK_SIZE];
	int rc;

	/* empty password means "no password" */
	if (pass && !strlen(pass))
		pass = NULL;
	if (devid && !strlen(devid))
		devid = NULL;

	rc = generate_key_hash(key_hash, pass, devid, &newt.device_id_hash,
			       newt.is_smartphone);
	if (rc)
		return rc;

	if (pass)
		newt.flags |= FL_PASSPROT;
	else
		newt.flags &= ~FL_PASSPROT;

	if (devid)
		newt.flags |= FL_SNPROT;
	else
		newt.flags &= ~FL_SNPROT;

	memset(d, 0, sizeof(d));
	aes128_ecb_encrypt(key_hash, newt.dec_seed, newt.enc_seed);
	memcpy(d, newt.enc_seed, AES_KEY_SIZE);

	set_bits(d, 128, 16, newt.flags);
	set_bits(d, 144, 14, newt.exp_date);
	set_bits(d, 159, 15, securid_shortmac(newt.dec_seed, AES_KEY_SIZE));
	set_bits(d, 174, 15, newt.device_id_hash);

	sprintf(out, "2%s", newt.serial);
	bits_to_numoutput(d, &out[BINENC_OFS], BINENC_BITS);

	set_bits(d, 0, 15, securid_shortmac(out, CHECKSUM_OFS));
	bits_to_numoutput(d, &out[CHECKSUM_OFS], CHECKSUM_BITS);

	return ERR_NONE;
}

int securid_random_token(struct securid_token *t)
{
	time_t now = time(NULL);
	uint8_t randbytes[16], key_hash[AES_BLOCK_SIZE];
	int i;

	memset(t, 0, sizeof(*t));

	if (securid_rand(t->dec_seed, AES_KEY_SIZE) ||
	    securid_rand(randbytes, sizeof(randbytes)))
		return ERR_GENERAL;

	t->dec_seed_hash = securid_shortmac(t->dec_seed, AES_KEY_SIZE);

	generate_key_hash(key_hash, NULL, NULL, &t->device_id_hash,
			  t->is_smartphone);
	aes128_ecb_encrypt(key_hash, t->dec_seed, t->enc_seed);
	t->has_enc_seed = 1;

	t->flags = FL_FEAT5 | FLD_DIGIT_MASK | FLD_PINMODE_MASK |
		   (1 << FLD_NUMSECONDS_SHIFT) | FL_128BIT;
	t->pinmode = 3;

	for (i = 0; i < 12; i++)
		t->serial[i] = '0' + randbytes[i] % 10;

	/* set the expiration date a couple of months out */
	t->exp_date = (now - SECURID_EPOCH) / (24 * 60 * 60) + 60 +
		(randbytes[12] & 0x0f) * 30;

	return ERR_NONE;
}

void securid_token_info(const struct securid_token *t,
	void (*callback)(const char *key, const char *value))
{
	char str[256];
	unsigned int i;
	struct tm *exp_tm;
	time_t exp_unix_time;

	callback("Serial number", t->serial);

	if (t->has_dec_seed) {
		for (i = 0; i < AES_KEY_SIZE; i++)
			sprintf(&str[i * 3], "%02x ", t->dec_seed[i]);
		callback("Decrypted seed", str);
	}

	if (t->has_enc_seed) {
		for (i = 0; i < AES_KEY_SIZE; i++)
			sprintf(&str[i * 3], "%02x ", t->enc_seed[i]);
		callback("Encrypted seed", str);

		callback("Encrypted w/password",
			t->flags & FL_PASSPROT ? "yes" : "no");
		callback("Encrypted w/devid",
			t->flags & FL_SNPROT ? "yes" : "no");
	}

	exp_unix_time = SECURID_EPOCH + (t->exp_date + 1) * 60 * 60 * 24;
	exp_tm = gmtime(&exp_unix_time);
	strftime(str, 32, "%Y/%m/%d", exp_tm);
	callback("Expiration date", str);

	callback("Key length", t->flags & FL_128BIT ? "128" : "64");

	sprintf(str, "%d",
		((t->flags & FLD_DIGIT_MASK) >> FLD_DIGIT_SHIFT) + 1);
	callback("Tokencode digits", str);

	sprintf(str, "%d",
		((t->flags & FLD_PINMODE_MASK) >> FLD_PINMODE_SHIFT));
	callback("PIN mode", str);

	switch ((t->flags & FLD_NUMSECONDS_MASK) >> FLD_NUMSECONDS_SHIFT) {
	case 0x00:
		strcpy(str, "30");
		break;
	case 0x01:
		strcpy(str, "60");
		break;
	default:
		strcpy(str, "unknown");
	}
	callback("Seconds per tokencode", str);

	callback("Feature bit 3", t->flags & FL_FEAT3 ? "yes" : "no");
	callback("Feature bit 4", t->flags & FL_FEAT4 ? "yes" : "no");
	callback("Feature bit 5", t->flags & FL_FEAT5 ? "yes" : "no");
	callback("Feature bit 6", t->flags & FL_FEAT6 ? "yes" : "no");
}

int securid_check_exp(struct securid_token *t, time_t now)
{
	time_t exp_unix_time;
	const int halfday = 60 * 60 * 12, wholeday = 60 * 60 * 24;

	exp_unix_time = SECURID_EPOCH + (t->exp_date + 1) * wholeday;

	/*
	 * Other soft token implementations seem to allow ~12hrs as a grace
	 * period.  Actual results will depend on how soon the server cuts
	 * off expired tokens.
	 */
	exp_unix_time += halfday;
	exp_unix_time -= now;
	return exp_unix_time / wholeday;
}

int securid_pin_format_ok(const char *pin)
{
	int i, rc;

	rc = strlen(pin);
	if (rc < MIN_PIN || rc > MAX_PIN)
		return ERR_BAD_LEN;
	for (i = 0; i < rc; i++)
		if (!isdigit(pin[i]))
			return ERR_GENERAL;
	return ERR_NONE;
}

int securid_pin_required(const struct securid_token *t)
{
	return ((t->flags >> FLD_PINMODE_SHIFT) & FLD_PINMODE_MASK) >= 2;
}

int securid_pass_required(const struct securid_token *t)
{
	return !!(t->flags & FL_PASSPROT);
}

int securid_devid_required(const struct securid_token *t)
{
	return !!(t->flags & FL_SNPROT);
}

char *securid_encrypt_pin(const char *pin, const char *password)
{
	int i;
	uint8_t buf[AES_BLOCK_SIZE], iv[AES_BLOCK_SIZE],
		passhash[AES_BLOCK_SIZE], *ret;

	if (securid_pin_format_ok(pin) != ERR_NONE)
		return NULL;

	memset(buf, 0, sizeof(buf));
	strcpy(buf, pin);
	buf[AES_BLOCK_SIZE - 1] = strlen(pin);

	securid_mac(password, strlen(password), passhash);

	if (securid_rand(iv, AES_BLOCK_SIZE))
		return NULL;

	for (i = 0; i < AES_BLOCK_SIZE; i++)
		buf[i] ^= iv[i];
	aes128_ecb_encrypt(passhash, buf, buf);

	ret = malloc(AES_BLOCK_SIZE * 2 * 2 + 1);
	if (!ret)
		return NULL;

	for (i = 0; i < AES_BLOCK_SIZE; i++)
		sprintf(&ret[i * 2], "%02x", iv[i]);
	for (i = 0; i < AES_BLOCK_SIZE; i++)
		sprintf(&ret[(AES_BLOCK_SIZE + i) * 2], "%02x", buf[i]);

	return ret;
}

static uint8_t hex2byte(const char *in)
{
	uint8_t ret = in[1] - '0';
	const uint8_t hex_offs = 'a' - '0' - 10;

	if (ret > 9)
		ret -= hex_offs;
	if (in[0] > '9')
		ret += (in[0] - hex_offs) << 4;
	else
		ret += (in[0] - '0') << 4;
	return ret;
}

int securid_decrypt_pin(const char *enc_pin, const char *password, char *pin)
{
	int i;
	uint8_t buf[AES_BLOCK_SIZE], iv[AES_BLOCK_SIZE],
		passhash[AES_BLOCK_SIZE];

	if (strlen(enc_pin) != AES_BLOCK_SIZE * 2 * 2)
		return ERR_BAD_LEN;

	for (i = 0; i < AES_BLOCK_SIZE; i++) {
		iv[i] = hex2byte(&enc_pin[i * 2]);
		buf[i] = hex2byte(&enc_pin[(i + AES_BLOCK_SIZE) * 2]);
	}

	securid_mac(password, strlen(password), passhash);
	aes128_ecb_decrypt(passhash, buf, buf);

	for (i = 0; i < AES_BLOCK_SIZE; i++)
		buf[i] ^= iv[i];

	if (buf[AES_BLOCK_SIZE - 2] != 0 ||
	    buf[AES_BLOCK_SIZE - 1] != strlen(buf))
		return ERR_GENERAL;
	if (securid_pin_format_ok(buf) != ERR_NONE)
		return ERR_GENERAL;

	strcpy(pin, buf);
	return ERR_NONE;
}
