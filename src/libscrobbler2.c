/*
 * cmusfm - libscrobbler2.c
 * Copyright (c) 2011-2014 Arkadiusz Bokowy
 *
 * This file is a part of a cmusfm.
 *
 * cmusfm is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * cmusfm is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * If you want to read full version of the GNU General Public License
 * see <http://www.gnu.org/licenses/>.
 */

#ifdef HAVE_CONFIG_H
#include "../config.h"
#endif

#include "libscrobbler2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <curl/curl.h>
#include <openssl/md5.h>

#include "debug.h"


// used as a buffer for GET/POST server response
struct sb_response_data {
	char *data;
	int len;
};

// used for quick URL and signature creation process
struct sb_getpost_data {
	char *name;
	char data_format;
	void *data;
};

char *mem2hex(const unsigned char *mem, int len, char *str);
unsigned char *hex2mem(const char *str, int len, unsigned char *mem);


// CURL write callback function.
static size_t sb_curl_write_callback(char *ptr, size_t size, size_t nmemb,
		void *data)
{
	struct sb_response_data *resp = (struct sb_response_data*)data;
	int len = size * nmemb;

	debug("read: len: %d, body: %s", len, ptr);
	resp->data = realloc(resp->data, resp->len + len + 1);
	memcpy(&resp->data[resp->len], ptr, len);
	resp->len += len;
	resp->data[resp->len] = 0;
	return len;
}

// Initialize CURL handler for internal usage.
CURL *sb_curl_init(CURLoption method, struct sb_response_data *response)
{
	CURL *curl;

	if((curl = curl_easy_init()) == NULL)
		return NULL;

#ifdef CURLOPT_PROTOCOLS
	curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP);
#endif
	curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1);

	curl_easy_setopt(curl, method, 1);

	memset(response, 0, sizeof(*response));
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sb_curl_write_callback);

	return curl;
}

// Cleanup CURL handler and free allocated response buffer.
void sb_curl_cleanup(CURL *curl, struct sb_response_data *response)
{
	curl_easy_cleanup(curl);
	free(response->data);
}

// Check scrobble API response status (and curl itself).
int sb_check_response(struct sb_response_data *response, int curl_status,
		scrobbler_session_t *sbs)
{
	char *ptr;

	debug("check: status: %d, body: %s", curl_status, response->data);
	if(curl_status != 0) {
		// network transfer failure (curl error)
		sbs->error_code = curl_status;
		return SCROBBERR_CURLPERF;
	}
	if(strstr(response->data, "<lfm status=\"ok\"") == NULL) {
		// scrobbler service failure
		ptr = strstr(response->data, "<error code=");
		if(ptr) sbs->error_code = atoi(ptr + 13);
		else
			// error code was not found in the response, so maybe we are calling
			// wrong service... set error code as value not used by the Last.fm
			sbs->error_code = 1;
		return SCROBBERR_SBERROR;
	}
	return 0;
}

// Generate MD5 scrobbler API method signature
void sb_generate_method_signature(struct sb_getpost_data *sb_data, int len,
		uint8_t secret[16], uint8_t sign[MD5_DIGEST_LENGTH])
{
	char secret_hex[16*2 + 1];
	char tmp_str[2048], format[8];
	int x, offset;

	mem2hex(secret, 16, secret_hex);

	for(x = offset = 0; x < len; x++) {
		// it means that if numerical data is zero it is also discarded
		if(sb_data[x].data == NULL) continue;

		sprintf(format, "%%s%%%c", sb_data[x].data_format);
		offset += sprintf(tmp_str + offset, format, sb_data[x].name, sb_data[x].data);
	}

	debug("signature data: %s", tmp_str);
	strcat(tmp_str, secret_hex);
	MD5((unsigned char*)tmp_str, strlen(tmp_str), sign);
}

// Make curl GET/POST string (escape data)
char *sb_make_curl_getpost_string(CURL *curl, char *str_buffer,
		struct sb_getpost_data *sb_data, int len)
{
	char *escaped_data, format[8];
	int x, offset;

	for(x = offset = 0; x < len; x++) {
		// it means that if numerical data is zero it is also discarded
		if(sb_data[x].data == NULL) continue;

		sprintf(format, "%%s=%%%c&", sb_data[x].data_format);

		if(sb_data[x].data_format == 's'){ //escape for string data
			escaped_data = curl_easy_escape(curl, sb_data[x].data, 0);
			offset += sprintf(str_buffer + offset, format,
					sb_data[x].name, escaped_data);
			curl_free(escaped_data);}
		else //non-string content -> no need for escaping
			offset += sprintf(str_buffer + offset, format,
					sb_data[x].name, sb_data[x].data);
	}

	str_buffer[offset - 1] = 0; //strip '&' at the end of string
	debug("params: %s", str_buffer);
	return str_buffer;
}

// Scrobble a track.
int scrobbler_scrobble(scrobbler_session_t *sbs, scrobbler_trackinfo_t *sbt)
{
	CURL *curl;
	int status;
	uint8_t sign[MD5_DIGEST_LENGTH];
	char api_key_hex[sizeof(sbs->api_key)*2 + 1];
	char session_key_hex[sizeof(sbs->session_key)*2 + 1];
	char sign_hex[sizeof(sign)*2 + 1];
	char post_data[2048];
	struct sb_response_data response;

	// data in alphabetical order sorted by name field (except api_sig)
	struct sb_getpost_data sb_data[] = {
		{"album", 's', sbt->album},
		{"albumArtist", 's', sbt->album_artist},
		{"api_key", 's', api_key_hex},
		{"artist", 's', sbt->artist},
		//{"context", 's', NULL},
		{"duration", 'd', (void*)(long)sbt->duration},
		{"mbid", 's', sbt->mbid},
		{"method", 's', "track.scrobble"},
		{"sk", 's', session_key_hex},
		{"timestamp", 'd', (void*)sbt->timestamp},
		{"track", 's', sbt->track},
		{"trackNumber", 'd', (void*)(long)sbt->track_number},
		//{"streamId", 's', NULL},
		{"api_sig", 's', sign_hex}};

	debug("scrobble: %ld", sbt->timestamp);
	debug("payload: %s - %s (%s) - %d. %s (%ds)",
			sbt->artist, sbt->album, sbt->album_artist,
			sbt->track_number, sbt->track, sbt->duration);

	if(sbt->artist == NULL || sbt->track == NULL || sbt->timestamp == 0)
		return SCROBBERR_TRACKINF;

	if((curl = sb_curl_init(CURLOPT_POST, &response)) == NULL)
		return SCROBBERR_CURLINIT;

	mem2hex(sbs->api_key, sizeof(sbs->api_key), api_key_hex);
	mem2hex(sbs->session_key, sizeof(sbs->session_key), session_key_hex);

	// make signature for track.updateNowPlaying API call
	sb_generate_method_signature(sb_data, 11, sbs->secret, sign);
	mem2hex(sign, sizeof(sign), sign_hex);

	// make track.updateNowPlaying POST request
	sb_make_curl_getpost_string(curl, post_data, sb_data, 12);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data);
	curl_easy_setopt(curl, CURLOPT_URL, SCROBBLER_URL);
	status = curl_easy_perform(curl);
	status = sb_check_response(&response, status, sbs);
	debug("scrobble status: %d", status);

	sb_curl_cleanup(curl, &response);
	return status;
}

// Notify Last.fm that a user has started listening to a track.
// This is engine function (without required argument check)
int sb_update_now_playing(scrobbler_session_t *sbs,
		scrobbler_trackinfo_t *sbt)
{
	CURL *curl;
	int status;
	uint8_t sign[MD5_DIGEST_LENGTH];
	char api_key_hex[sizeof(sbs->api_key)*2 + 1];
	char session_key_hex[sizeof(sbs->session_key)*2 + 1];
	char sign_hex[sizeof(sign)*2 + 1];
	char post_data[2048];
	struct sb_response_data response;

	// data in alphabetical order sorted by name field (except api_sig)
	struct sb_getpost_data sb_data[] = {
		{"album", 's', sbt->album},
		{"albumArtist", 's', sbt->album_artist},
		{"api_key", 's', api_key_hex},
		{"artist", 's', sbt->artist},
		//{"context", 's', NULL},
		{"duration", 'd', (void*)(long)sbt->duration},
		{"mbid", 's', sbt->mbid},
		{"method", 's', "track.updateNowPlaying"},
		{"sk", 's', session_key_hex},
		{"track", 's', sbt->track},
		{"trackNumber", 'd', (void*)(long)sbt->track_number},
		{"api_sig", 's', sign_hex}};

	debug("now playing: %ld", sbt->timestamp);
	debug("payload: %s - %s (%s) - %d. %s (%ds)",
			sbt->artist, sbt->album, sbt->album_artist,
			sbt->track_number, sbt->track, sbt->duration);

	if((curl = sb_curl_init(CURLOPT_POST, &response)) == NULL)
		return SCROBBERR_CURLINIT;

	mem2hex(sbs->api_key, sizeof(sbs->api_key), api_key_hex);
	mem2hex(sbs->session_key, sizeof(sbs->session_key), session_key_hex);

	// make signature for track.updateNowPlaying API call
	sb_generate_method_signature(sb_data, 10, sbs->secret, sign);
	mem2hex(sign, sizeof(sign), sign_hex);

	// make track.updateNowPlaying POST request
	sb_make_curl_getpost_string(curl, post_data, sb_data, 11);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data);
	curl_easy_setopt(curl, CURLOPT_URL, SCROBBLER_URL);
	status = curl_easy_perform(curl);
	status = sb_check_response(&response, status, sbs);
	debug("now playing status: %d", status);

	sb_curl_cleanup(curl, &response);
	return status;
}

// Update "Now playing" notification.
int scrobbler_update_now_playing(scrobbler_session_t *sbs,
		scrobbler_trackinfo_t *sbt)
{
	debug("now playing wrapper");
	if(sbt->artist == NULL || sbt->track == NULL)
		return SCROBBERR_TRACKINF;
	return sb_update_now_playing(sbs, sbt);
}

// Hard-codded method for validating session key. This approach uses the
// updateNotify method call with the wrong number of parameters as a test
// call.
int scrobbler_test_session_key(scrobbler_session_t *sbs)
{
	scrobbler_trackinfo_t sbt;
	int status;

	debug("test service connection");
	memset(&sbt, 0, sizeof(sbt));
	status = sb_update_now_playing(sbs, &sbt);

	// 'invalid parameters' is not the error in this case :)
	if(status == SCROBBERR_SBERROR && sbs->error_code == 6) return 0;
	else return status;
}

// Return session key in string hex dump. The memory block pointed by the
// *str has to be big enough to contain sizeof(session_key)*2 + 1.
char *scrobbler_get_session_key_str(scrobbler_session_t *sbs, char *str)
{
	return mem2hex(sbs->session_key, sizeof(sbs->session_key), str);
}

// Set session key by parsing the hex dump of this key.
void scrobbler_set_session_key_str(scrobbler_session_t *sbs, const char *str)
{
	hex2mem(str, sizeof(sbs->session_key), sbs->session_key);
}

// Perform scrobbler service authentication process.
int scrobbler_authentication(scrobbler_session_t *sbs,
		scrobbler_authuser_callback_t callback)
{
	CURL *curl;
	int status;
	uint8_t sign[MD5_DIGEST_LENGTH];
	char api_key_hex[sizeof(sbs->api_key)*2 + 1];
	char sign_hex[sizeof(sign)*2 + 1], token_hex[33];
	char get_url[1024], *ptr;
	struct sb_response_data response;

	// data in alphabetical order sorted by name field (except api_sig)
	struct sb_getpost_data sb_data_token[] = {
		{"api_key", 's', api_key_hex},
		{"method", 's', "auth.getToken"},
		{"api_sig", 's', sign_hex}};
	struct sb_getpost_data sb_data_session[] = {
		{"api_key", 's', api_key_hex},
		{"method", 's', "auth.getSession"},
		{"token", 's', token_hex},
		{"api_sig", 's', sign_hex}};

	if((curl = sb_curl_init(CURLOPT_HTTPGET, &response)) == NULL)
		return SCROBBERR_CURLINIT;

	mem2hex(sbs->api_key, sizeof(sbs->api_key), api_key_hex);

	// make signature for auth.getToken API call
	sb_generate_method_signature(sb_data_token, 2, sbs->secret, sign);
	mem2hex(sign, sizeof(sign), sign_hex);

	// make auth.getToken GET request
	strcpy(get_url, SCROBBLER_URL "?");
	sb_make_curl_getpost_string(curl, get_url + strlen(get_url), sb_data_token, 3);
	curl_easy_setopt(curl, CURLOPT_URL, get_url);
	status = curl_easy_perform(curl);
	status = sb_check_response(&response, status, sbs);

	if(status != 0) {
		sb_curl_cleanup(curl, &response);
		return status;
	}

	memcpy(token_hex, strstr(response.data, "<token>") + 7, 32);
	token_hex[32] = 0;

	// perform user authorization (callback function)
	sprintf(get_url, SCROBBLER_USERAUTH_URL "?api_key=%s&token=%s",
			api_key_hex, token_hex);
	if(callback(get_url) != 0) {
		sb_curl_cleanup(curl, &response);
		return SCROBBERR_CALLBACK;
	}
	
	// make signature for auth.getSession API call
	sb_generate_method_signature(sb_data_session, 3, sbs->secret, sign);
	mem2hex(sign, sizeof(sign), sign_hex);

	// reinitialize response buffer
	response.len = 0;

	// make auth.getSession GET request
	strcpy(get_url, SCROBBLER_URL "?");
	sb_make_curl_getpost_string(curl, get_url + strlen(get_url), sb_data_session, 4);
	curl_easy_setopt(curl, CURLOPT_URL, get_url);
	status = curl_easy_perform(curl);
	status = sb_check_response(&response, status, sbs);
	debug("authentication status: %d", status);

	if(status != 0) {
		sb_curl_cleanup(curl, &response);
		return status;
	}

	strncpy(sbs->user_name, strstr(response.data, "<name>") + 6,
			sizeof(sbs->user_name));
	sbs->user_name[sizeof(sbs->user_name) - 1] = 0;
	if((ptr = strchr(sbs->user_name, '<')) != NULL) *ptr = 0;
	memcpy(get_url, strstr(response.data, "<key>") + 5, 32);
	hex2mem(get_url, sizeof(sbs->session_key), sbs->session_key);

	sb_curl_cleanup(curl, &response);
	return 0;
}

// Initialize scrobbler session. On success this function returns pointer
// to allocated scrobbler_session structure, which must be freed by call
// to scrobbler_free. On error NULL is returned.
scrobbler_session_t *scrobbler_initialize(uint8_t api_key[16],
		uint8_t secret[16])
{
	scrobbler_session_t *sbs;

	// allocate space for scrobbler session structure
	if((sbs = calloc(1, sizeof(scrobbler_session_t))) == NULL)
		return NULL;

	if(curl_global_init(CURL_GLOBAL_NOTHING) != 0) {
		free(sbs);
		return NULL;
	}

	memcpy(sbs->api_key, api_key, sizeof(sbs->api_key));
	memcpy(sbs->secret, secret, sizeof(sbs->secret));

	return sbs;
}

void scrobbler_free(scrobbler_session_t *sbs)
{
	curl_global_cleanup();
	free(sbs);
}

// Dump memory into hexadecimal string format. Note that *str has to be
// big enough to contain 2*len+1.
char *mem2hex(const unsigned char *mem, int len, char *str)
{
	char hexchars[] = "0123456789abcdef", *ptr;
	int x;

	for(x = 0, ptr = str; x < len; x++){
		*(ptr++) = hexchars[(mem[x] >> 4) & 0x0f];
		*(ptr++) = hexchars[mem[x] & 0x0f];}
	*ptr = 0;

	return str;
}

// Opposite to mem2hex. Len is the number of bytes in hex representation,
// so strlen(str) should be 2*len.
unsigned char *hex2mem(const char *str, int len, unsigned char *mem)
{
	int x;

	for(x = 0; x < len; x++) {
		if(isdigit(str[x*2])) mem[x] = (str[x*2] - '0') << 4;
		else mem[x] = (tolower(str[x*2]) - 'a' + 10) << 4;

		if(isdigit(str[x*2 + 1])) mem[x] += str[x*2 + 1] - '0';
		else mem[x] += tolower(str[x*2 + 1]) - 'a' + 10;
	}

	return mem;
}
