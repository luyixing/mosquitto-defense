/*
Copyright (c) 2010-2018 Roger Light <roger@atchoo.org>

All rights reserved. This program and the accompanying materials
are made available under the terms of the Eclipse Public License v1.0
and Eclipse Distribution License v1.0 which accompany this distribution.
 
The Eclipse Public License is available at
   http://www.eclipse.org/legal/epl-v10.html
and the Eclipse Distribution License is available at
  http://www.eclipse.org/org/documents/edl-v10.php.
 
Contributors:
   Roger Light - initial implementation and documentation.
*/

#include "config.h"

#ifdef WITH_PERSISTENCE

#ifndef WIN32
#include <arpa/inet.h>
#endif
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "mosquitto_broker_internal.h"
#include "memory_mosq.h"
#include "persist.h"
#include "time_mosq.h"
#include "util_mosq.h"

static uint32_t db_version;


static int persist__restore_sub(struct mosquitto_db *db, const char *client_id, const char *sub, int qos);

static struct mosquitto *persist__find_or_add_context(struct mosquitto_db *db, const char *client_id, uint16_t last_mid)
{
	struct mosquitto *context;

	context = NULL;
	HASH_FIND(hh_id, db->contexts_by_id, client_id, strlen(client_id), context);
	if(!context){
		context = context__init(db, -1);
		if(!context) return NULL;
		context->id = mosquitto__strdup(client_id);
		if(!context->id){
			mosquitto__free(context);
			return NULL;
		}

		context->clean_session = false;

		HASH_ADD_KEYPTR(hh_id, db->contexts_by_id, context->id, strlen(context->id), context);
	}
	if(last_mid){
		context->last_mid = last_mid;
	}
	return context;
}

static int persist__client_messages_write(struct mosquitto_db *db, FILE *db_fptr, struct mosquitto *context, struct mosquitto_client_msg *queue)
{
	uint32_t length;
	dbid_t i64temp;
	uint16_t i16temp, slen;
	uint8_t i8temp;
	struct mosquitto_client_msg *cmsg;

	assert(db);
	assert(db_fptr);
	assert(context);

	cmsg = queue;
	while(cmsg){
		if(!strncmp(cmsg->store->topic, "$SYS", 4)
				&& cmsg->store->ref_count <= 1
				&& cmsg->store->dest_id_count == 0){

			/* This $SYS message won't have been persisted, so we can't persist
			 * this client message. */
			cmsg = cmsg->next;
			continue;
		}

		slen = strlen(context->id);

		length = htonl(sizeof(dbid_t) + sizeof(uint16_t) + sizeof(uint8_t) +
				sizeof(uint8_t) + sizeof(uint8_t) + sizeof(uint8_t) +
				sizeof(uint8_t) + 2+slen);

		i16temp = htons(DB_CHUNK_CLIENT_MSG);
		write_e(db_fptr, &i16temp, sizeof(uint16_t));
		write_e(db_fptr, &length, sizeof(uint32_t));

		i16temp = htons(slen);
		write_e(db_fptr, &i16temp, sizeof(uint16_t));
		write_e(db_fptr, context->id, slen);

		i64temp = cmsg->store->db_id;
		write_e(db_fptr, &i64temp, sizeof(dbid_t));

		i16temp = htons(cmsg->mid);
		write_e(db_fptr, &i16temp, sizeof(uint16_t));

		i8temp = (uint8_t )cmsg->qos;
		write_e(db_fptr, &i8temp, sizeof(uint8_t));

		i8temp = (uint8_t )cmsg->retain;
		write_e(db_fptr, &i8temp, sizeof(uint8_t));

		i8temp = (uint8_t )cmsg->direction;
		write_e(db_fptr, &i8temp, sizeof(uint8_t));

		i8temp = (uint8_t )cmsg->state;
		write_e(db_fptr, &i8temp, sizeof(uint8_t));

		i8temp = (uint8_t )cmsg->dup;
		write_e(db_fptr, &i8temp, sizeof(uint8_t));

		cmsg = cmsg->next;
	}

	return MOSQ_ERR_SUCCESS;
error:
	log__printf(NULL, MOSQ_LOG_ERR, "Error: %s.", strerror(errno));
	return 1;
}


static int persist__message_store_write(struct mosquitto_db *db, FILE *db_fptr)
{
	uint32_t length;
	dbid_t i64temp;
	uint32_t i32temp;
	uint16_t i16temp, slen, tlen;
	uint8_t i8temp;
	struct mosquitto_msg_store *stored;
	bool force_no_retain;

	assert(db);
	assert(db_fptr);

	stored = db->msg_store;
	while(stored){
		if(stored->topic && !strncmp(stored->topic, "$SYS", 4)){
			if(stored->ref_count <= 1 && stored->dest_id_count == 0){
				/* $SYS messages that are only retained shouldn't be persisted. */
				stored = stored->next;
				continue;
			}
			/* Don't save $SYS messages as retained otherwise they can give
			 * misleading information when reloaded. They should still be saved
			 * because a disconnected durable client may have them in their
			 * queue. */
			force_no_retain = true;
		}else{
			force_no_retain = false;
		}
		if(stored->topic){
			tlen = strlen(stored->topic);
		}else{
			tlen = 0;
		}
		length = htonl(sizeof(dbid_t) + 2+strlen(stored->source_id) +
				sizeof(uint16_t) + sizeof(uint16_t) +
				2+tlen + sizeof(uint32_t) +
				stored->payloadlen + sizeof(uint8_t) + sizeof(uint8_t));

		i16temp = htons(DB_CHUNK_MSG_STORE);
		write_e(db_fptr, &i16temp, sizeof(uint16_t));
		write_e(db_fptr, &length, sizeof(uint32_t));

		i64temp = stored->db_id;
		write_e(db_fptr, &i64temp, sizeof(dbid_t));

		slen = strlen(stored->source_id);
		i16temp = htons(slen);
		write_e(db_fptr, &i16temp, sizeof(uint16_t));
		if(slen){
			write_e(db_fptr, stored->source_id, slen);
		}

		i16temp = htons(stored->source_mid);
		write_e(db_fptr, &i16temp, sizeof(uint16_t));

		i16temp = htons(stored->mid);
		write_e(db_fptr, &i16temp, sizeof(uint16_t));

		i16temp = htons(tlen);
		write_e(db_fptr, &i16temp, sizeof(uint16_t));
		if(tlen){
			write_e(db_fptr, stored->topic, tlen);
		}

		i8temp = (uint8_t )stored->qos;
		write_e(db_fptr, &i8temp, sizeof(uint8_t));

		if(force_no_retain == false){
			i8temp = (uint8_t )stored->retain;
		}else{
			i8temp = 0;
		}
		write_e(db_fptr, &i8temp, sizeof(uint8_t));

		i32temp = htonl(stored->payloadlen);
		write_e(db_fptr, &i32temp, sizeof(uint32_t));
		if(stored->payloadlen){
			write_e(db_fptr, UHPA_ACCESS_PAYLOAD(stored), (unsigned int)stored->payloadlen);
		}
		stored = stored->next;
	}

	return MOSQ_ERR_SUCCESS;
error:
	log__printf(NULL, MOSQ_LOG_ERR, "Error: %s.", strerror(errno));
	return 1;
}

static int persist__client_write(struct mosquitto_db *db, FILE *db_fptr)
{
	struct mosquitto *context, *ctxt_tmp;
	uint16_t i16temp, slen;
	uint32_t length;
	time_t disconnect_t;

	assert(db);
	assert(db_fptr);

	HASH_ITER(hh_id, db->contexts_by_id, context, ctxt_tmp){
		if(context && context->clean_session == false){
			length = htonl(2+strlen(context->id) + sizeof(uint16_t) + sizeof(time_t));

			i16temp = htons(DB_CHUNK_CLIENT);
			write_e(db_fptr, &i16temp, sizeof(uint16_t));
			write_e(db_fptr, &length, sizeof(uint32_t));

			slen = strlen(context->id);
			i16temp = htons(slen);
			write_e(db_fptr, &i16temp, sizeof(uint16_t));
			write_e(db_fptr, context->id, slen);
			i16temp = htons(context->last_mid);
			write_e(db_fptr, &i16temp, sizeof(uint16_t));
			if(context->disconnect_t){
				disconnect_t = context->disconnect_t;
			}else{
				disconnect_t = time(NULL);
			}
			write_e(db_fptr, &disconnect_t, sizeof(time_t));

			if(persist__client_messages_write(db, db_fptr, context, context->inflight_msgs)) return 1;
			if(persist__client_messages_write(db, db_fptr, context, context->queued_msgs)) return 1;
		}
	}

	return MOSQ_ERR_SUCCESS;
error:
	log__printf(NULL, MOSQ_LOG_ERR, "Error: %s.", strerror(errno));
	return 1;
}

static int persist__subs_retain_write(struct mosquitto_db *db, FILE *db_fptr, struct mosquitto__subhier *node, const char *topic, int level)
{
	struct mosquitto__subhier *subhier, *subhier_tmp;
	struct mosquitto__subleaf *sub;
	char *thistopic;
	uint32_t length;
	uint16_t i16temp;
	uint8_t i8temp;
	dbid_t i64temp;
	size_t slen;

	slen = strlen(topic) + node->topic_len + 2;
	thistopic = mosquitto__malloc(sizeof(char)*slen);
	if(!thistopic) return MOSQ_ERR_NOMEM;
	if(level > 1 || strlen(topic)){
		snprintf(thistopic, slen, "%s/%s", topic, UHPA_ACCESS_TOPIC(node));
	}else{
		snprintf(thistopic, slen, "%s", UHPA_ACCESS_TOPIC(node));
	}

	sub = node->subs;
	while(sub){
		if(sub->context->clean_session == false){
			length = htonl(2+strlen(sub->context->id) + 2+strlen(thistopic) + sizeof(uint8_t));

			i16temp = htons(DB_CHUNK_SUB);
			write_e(db_fptr, &i16temp, sizeof(uint16_t));
			write_e(db_fptr, &length, sizeof(uint32_t));

			slen = strlen(sub->context->id);
			i16temp = htons(slen);
			write_e(db_fptr, &i16temp, sizeof(uint16_t));
			write_e(db_fptr, sub->context->id, slen);

			slen = strlen(thistopic);
			i16temp = htons(slen);
			write_e(db_fptr, &i16temp, sizeof(uint16_t));
			write_e(db_fptr, thistopic, slen);

			i8temp = (uint8_t )sub->qos;
			write_e(db_fptr, &i8temp, sizeof(uint8_t));
		}
		sub = sub->next;
	}
	if(node->retained){
		if(strncmp(node->retained->topic, "$SYS", 4)){
			/* Don't save $SYS messages. */
			length = htonl(sizeof(dbid_t));

			i16temp = htons(DB_CHUNK_RETAIN);
			write_e(db_fptr, &i16temp, sizeof(uint16_t));
			write_e(db_fptr, &length, sizeof(uint32_t));

			i64temp = node->retained->db_id;
			write_e(db_fptr, &i64temp, sizeof(dbid_t));
		}
	}

	HASH_ITER(hh, node->children, subhier, subhier_tmp){
		persist__subs_retain_write(db, db_fptr, subhier, thistopic, level+1);
	}
	mosquitto__free(thistopic);
	return MOSQ_ERR_SUCCESS;
error:
	log__printf(NULL, MOSQ_LOG_ERR, "Error: %s.", strerror(errno));
	return 1;
}

static int persist__subs_retain_write_all(struct mosquitto_db *db, FILE *db_fptr)
{
	struct mosquitto__subhier *subhier, *subhier_tmp;

	HASH_ITER(hh, db->subs, subhier, subhier_tmp){
		if(subhier->children){
			persist__subs_retain_write(db, db_fptr, subhier->children, "", 0);
		}
	}
	
	return MOSQ_ERR_SUCCESS;
}

int persist__backup(struct mosquitto_db *db, bool shutdown)
{
	int rc = 0;
	FILE *db_fptr = NULL;
	uint32_t db_version_w = htonl(MOSQ_DB_VERSION);
	uint32_t crc = htonl(0);
	dbid_t i64temp;
	uint32_t i32temp;
	uint16_t i16temp;
	uint8_t i8temp;
	char *err;
	char *outfile = NULL;
	int len;

	if(!db || !db->config || !db->config->persistence_filepath) return MOSQ_ERR_INVAL;
	log__printf(NULL, MOSQ_LOG_INFO, "Saving in-memory database to %s.", db->config->persistence_filepath);

	len = strlen(db->config->persistence_filepath)+5;
	outfile = mosquitto__malloc(len+1);
	if(!outfile){
		log__printf(NULL, MOSQ_LOG_INFO, "Error saving in-memory database, out of memory.");
		return MOSQ_ERR_NOMEM;
	}
	snprintf(outfile, len, "%s.new", db->config->persistence_filepath);
	outfile[len] = '\0';

#ifndef WIN32
	/**
 	*
	* If a system lost power during the rename operation at the
	* end of this file the filesystem could potentially be left
	* with a directory that looks like this after powerup:
	*
	* 24094 -rw-r--r--    2 root     root          4099 May 30 16:27 mosquitto.db
	* 24094 -rw-r--r--    2 root     root          4099 May 30 16:27 mosquitto.db.new
	*
	* The 24094 shows that mosquitto.db.new is hard-linked to the
	* same file as mosquitto.db.  If fopen(outfile, "wb") is naively
	* called then mosquitto.db will be truncated and the database
	* potentially corrupted.
	*
	* Any existing mosquitto.db.new file must be removed prior to
	* opening to guarantee that it is not hard-linked to
	* mosquitto.db.
	*
	*/
	rc = unlink(outfile);
	if (rc != 0) {
		if (errno != ENOENT) {
			log__printf(NULL, MOSQ_LOG_INFO, "Error saving in-memory database, unable to remove %s.", outfile);
			goto error;
		}
	}
#endif

	db_fptr = mosquitto__fopen(outfile, "wb", true);
	if(db_fptr == NULL){
		log__printf(NULL, MOSQ_LOG_INFO, "Error saving in-memory database, unable to open %s for writing.", outfile);
		goto error;
	}

	/* Header */
	write_e(db_fptr, magic, 15);
	write_e(db_fptr, &crc, sizeof(uint32_t));
	write_e(db_fptr, &db_version_w, sizeof(uint32_t));

	/* DB config */
	i16temp = htons(DB_CHUNK_CFG);
	write_e(db_fptr, &i16temp, sizeof(uint16_t));
	/* chunk length */
	i32temp = htonl(sizeof(dbid_t) + sizeof(uint8_t) + sizeof(uint8_t));
	write_e(db_fptr, &i32temp, sizeof(uint32_t));
	/* db written at broker shutdown or not */
	i8temp = shutdown;
	write_e(db_fptr, &i8temp, sizeof(uint8_t));
	i8temp = sizeof(dbid_t);
	write_e(db_fptr, &i8temp, sizeof(uint8_t));
	/* last db mid */
	i64temp = db->last_db_id;
	write_e(db_fptr, &i64temp, sizeof(dbid_t));

	if(persist__message_store_write(db, db_fptr)){
		goto error;
	}

	persist__client_write(db, db_fptr);
	persist__subs_retain_write_all(db, db_fptr);

#ifndef WIN32
	/**
	*
	* Closing a file does not guarantee that the contents are
	* written to disk.  Need to flush to send data from app to OS
	* buffers, then fsync to deliver data from OS buffers to disk
	* (as well as disk hardware permits).
	* 
	* man close (http://linux.die.net/man/2/close, 2016-06-20):
	* 
	*   "successful close does not guarantee that the data has
	*   been successfully saved to disk, as the kernel defers
	*   writes.  It is not common for a filesystem to flush
	*   the  buffers  when  the stream is closed.  If you need
	*   to be sure that the data is physically stored, use
	*   fsync(2).  (It will depend on the disk hardware at this
	*   point."
	*
	* This guarantees that the new state file will not overwrite
	* the old state file before its contents are valid.
	*
	*/

	fflush(db_fptr);
	fsync(fileno(db_fptr));
#endif
	fclose(db_fptr);

#ifdef WIN32
	if(remove(db->config->persistence_filepath) != 0){
		if(errno != ENOENT){
			goto error;
		}
	}
#endif
	if(rename(outfile, db->config->persistence_filepath) != 0){
		goto error;
	}
	mosquitto__free(outfile);
	outfile = NULL;
	return rc;
error:
	mosquitto__free(outfile);
	err = strerror(errno);
	log__printf(NULL, MOSQ_LOG_ERR, "Error: %s.", err);
	if(db_fptr) fclose(db_fptr);
	return 1;
}

static int persist__client_msg_restore(struct mosquitto_db *db, const char *client_id, uint16_t mid, uint8_t qos, uint8_t retain, uint8_t direction, uint8_t state, uint8_t dup, uint64_t store_id)
{
	struct mosquitto_client_msg *cmsg;
	struct mosquitto_client_msg **msgs, **last_msg;
	struct mosquitto_msg_store_load *load;
	struct mosquitto *context;

	cmsg = mosquitto__malloc(sizeof(struct mosquitto_client_msg));
	if(!cmsg){
		log__printf(NULL, MOSQ_LOG_ERR, "Error: Out of memory.");
		return MOSQ_ERR_NOMEM;
	}

	cmsg->next = NULL;
	cmsg->store = NULL;
	cmsg->mid = mid;
	cmsg->qos = qos;
	cmsg->retain = retain;
	cmsg->timestamp = 0;
	cmsg->direction = direction;
	cmsg->state = state;
	cmsg->dup = dup;

	HASH_FIND(hh, db->msg_store_load, &store_id, sizeof(dbid_t), load);
	if(!load){
		mosquitto__free(cmsg);
		log__printf(NULL, MOSQ_LOG_ERR, "Error restoring persistent database, message store corrupt.");
		return 1;
	}
	cmsg->store = load->store;
	cmsg->store->ref_count++;

	context = persist__find_or_add_context(db, client_id, 0);
	if(!context){
		mosquitto__free(cmsg);
		log__printf(NULL, MOSQ_LOG_ERR, "Error restoring persistent database, message store corrupt.");
		return 1;
	}

	if (state == mosq_ms_queued){
		msgs = &(context->queued_msgs);
		last_msg = &(context->last_queued_msg);
	}else{
		msgs = &(context->inflight_msgs);
		last_msg = &(context->last_inflight_msg);
	}
	if(*msgs){
		(*last_msg)->next = cmsg;
	}else{
		*msgs = cmsg;
	}
	*last_msg = cmsg;

	return MOSQ_ERR_SUCCESS;
}

static int persist__client_chunk_restore(struct mosquitto_db *db, FILE *db_fptr)
{
	uint16_t i16temp, slen, last_mid;
	char *client_id = NULL;
	int rc = 0;
	struct mosquitto *context;
	time_t disconnect_t;

	read_e(db_fptr, &i16temp, sizeof(uint16_t));
	slen = ntohs(i16temp);
	if(!slen){
		log__printf(NULL, MOSQ_LOG_ERR, "Error: Corrupt persistent database.");
		fclose(db_fptr);
		return 1;
	}
	client_id = mosquitto__malloc(slen+1);
	if(!client_id){
		fclose(db_fptr);
		log__printf(NULL, MOSQ_LOG_ERR, "Error: Out of memory.");
		return MOSQ_ERR_NOMEM;
	}
	read_e(db_fptr, client_id, slen);
	client_id[slen] = '\0';

	read_e(db_fptr, &i16temp, sizeof(uint16_t));
	last_mid = ntohs(i16temp);

	if(db_version == 2){
		disconnect_t = time(NULL);
	}else{
		read_e(db_fptr, &disconnect_t, sizeof(time_t));
	}

	context = persist__find_or_add_context(db, client_id, last_mid);
	if(context){
		context->disconnect_t = disconnect_t;
	}else{
		rc = 1;
	}

	mosquitto__free(client_id);

	return rc;
error:
	log__printf(NULL, MOSQ_LOG_ERR, "Error: %s.", strerror(errno));
	fclose(db_fptr);
	mosquitto__free(client_id);
	return 1;
}

static int persist__client_msg_chunk_restore(struct mosquitto_db *db, FILE *db_fptr)
{
	dbid_t i64temp, store_id;
	uint16_t i16temp, slen, mid;
	uint8_t qos, retain, direction, state, dup;
	char *client_id = NULL;
	int rc;
	char *err;

	read_e(db_fptr, &i16temp, sizeof(uint16_t));
	slen = ntohs(i16temp);
	if(!slen){
		log__printf(NULL, MOSQ_LOG_ERR, "Error: Corrupt persistent database.");
		fclose(db_fptr);
		return 1;
	}
	client_id = mosquitto__malloc(slen+1);
	if(!client_id){
		fclose(db_fptr);
		log__printf(NULL, MOSQ_LOG_ERR, "Error: Out of memory.");
		return MOSQ_ERR_NOMEM;
	}
	read_e(db_fptr, client_id, slen);
	client_id[slen] = '\0';

	read_e(db_fptr, &i64temp, sizeof(dbid_t));
	store_id = i64temp;

	read_e(db_fptr, &i16temp, sizeof(uint16_t));
	mid = ntohs(i16temp);

	read_e(db_fptr, &qos, sizeof(uint8_t));
	read_e(db_fptr, &retain, sizeof(uint8_t));
	read_e(db_fptr, &direction, sizeof(uint8_t));
	read_e(db_fptr, &state, sizeof(uint8_t));
	read_e(db_fptr, &dup, sizeof(uint8_t));

	rc = persist__client_msg_restore(db, client_id, mid, qos, retain, direction, state, dup, store_id);
	mosquitto__free(client_id);

	return rc;
error:
	err = strerror(errno);
	log__printf(NULL, MOSQ_LOG_ERR, "Error: %s.", err);
	fclose(db_fptr);
	mosquitto__free(client_id);
	return 1;
}

static int persist__msg_store_chunk_restore(struct mosquitto_db *db, FILE *db_fptr)
{
	dbid_t i64temp, store_id;
	uint32_t i32temp, payloadlen = 0;
	uint16_t i16temp, slen, source_mid;
	uint8_t qos, retain;
	mosquitto__payload_uhpa payload;
	char *source_id = NULL;
	char *topic = NULL;
	int rc = 0;
	struct mosquitto_msg_store *stored = NULL;
	struct mosquitto_msg_store_load *load;
	char *err;

	payload.ptr = NULL;

	load = mosquitto__malloc(sizeof(struct mosquitto_msg_store_load));
	if(!load){
		fclose(db_fptr);
		log__printf(NULL, MOSQ_LOG_ERR, "Error: Out of memory.");
		return MOSQ_ERR_NOMEM;
	}

	read_e(db_fptr, &i64temp, sizeof(dbid_t));
	store_id = i64temp;

	read_e(db_fptr, &i16temp, sizeof(uint16_t));
	slen = ntohs(i16temp);
	if(slen){
		source_id = mosquitto__malloc(slen+1);
		if(!source_id){
			mosquitto__free(load);
			fclose(db_fptr);
			log__printf(NULL, MOSQ_LOG_ERR, "Error: Out of memory.");
			return MOSQ_ERR_NOMEM;
		}
		read_e(db_fptr, source_id, slen);
		source_id[slen] = '\0';
	}
	read_e(db_fptr, &i16temp, sizeof(uint16_t));
	source_mid = ntohs(i16temp);

	/* This is the mid - don't need it */
	read_e(db_fptr, &i16temp, sizeof(uint16_t));

	read_e(db_fptr, &i16temp, sizeof(uint16_t));
	slen = ntohs(i16temp);
	if(slen){
		topic = mosquitto__malloc(slen+1);
		if(!topic){
			mosquitto__free(load);
			fclose(db_fptr);
			mosquitto__free(source_id);
			log__printf(NULL, MOSQ_LOG_ERR, "Error: Out of memory.");
			return MOSQ_ERR_NOMEM;
		}
		read_e(db_fptr, topic, slen);
		topic[slen] = '\0';
	}else{
		topic = NULL;
	}
	read_e(db_fptr, &qos, sizeof(uint8_t));
	read_e(db_fptr, &retain, sizeof(uint8_t));
	
	read_e(db_fptr, &i32temp, sizeof(uint32_t));
	payloadlen = ntohl(i32temp);

	if(payloadlen){
		if(UHPA_ALLOC(payload, payloadlen) == 0){
			mosquitto__free(load);
			fclose(db_fptr);
			mosquitto__free(source_id);
			mosquitto__free(topic);
			log__printf(NULL, MOSQ_LOG_ERR, "Error: Out of memory.");
			return MOSQ_ERR_NOMEM;
		}
		read_e(db_fptr, UHPA_ACCESS(payload, payloadlen), payloadlen);
	}

	rc = db__message_store(db, source_id, source_mid, topic, qos, payloadlen, &payload, retain, &stored, store_id, NULL);
	mosquitto__free(source_id);

	if(rc == MOSQ_ERR_SUCCESS){
		load->db_id = stored->db_id;
		load->store = stored;

		HASH_ADD(hh, db->msg_store_load, db_id, sizeof(dbid_t), load);
		return MOSQ_ERR_SUCCESS;
	}else{
		mosquitto__free(load);
		fclose(db_fptr);
		mosquitto__free(topic);
		UHPA_FREE(payload, payloadlen);
		return rc;
	}
error:
	err = strerror(errno);
	log__printf(NULL, MOSQ_LOG_ERR, "Error: %s.", err);
	fclose(db_fptr);
	mosquitto__free(source_id);
	mosquitto__free(topic);
	UHPA_FREE(payload, payloadlen);
	return 1;
}

static int persist__retain_chunk_restore(struct mosquitto_db *db, FILE *db_fptr)
{
	dbid_t i64temp, store_id;
	struct mosquitto_msg_store_load *load;
	char *err;

	if(fread(&i64temp, sizeof(dbid_t), 1, db_fptr) != 1){
		err = strerror(errno);
		log__printf(NULL, MOSQ_LOG_ERR, "Error: %s.", err);
		fclose(db_fptr);
		return 1;
	}
	store_id = i64temp;
	HASH_FIND(hh, db->msg_store_load, &store_id, sizeof(dbid_t), load);
	if(load){
		sub__messages_queue(db, NULL, load->store->topic, load->store->qos, load->store->retain, &load->store);
	}else{
		log__printf(NULL, MOSQ_LOG_ERR, "Error: Corrupt database whilst restoring a retained message.");
		return MOSQ_ERR_INVAL;
	}
	return MOSQ_ERR_SUCCESS;
}

static int persist__sub_chunk_restore(struct mosquitto_db *db, FILE *db_fptr)
{
	uint16_t i16temp, slen;
	uint8_t qos;
	char *client_id;
	char *topic;
	int rc = 0;
	char *err;

	read_e(db_fptr, &i16temp, sizeof(uint16_t));
	slen = ntohs(i16temp);
	client_id = mosquitto__malloc(slen+1);
	if(!client_id){
		fclose(db_fptr);
		log__printf(NULL, MOSQ_LOG_ERR, "Error: Out of memory.");
		return MOSQ_ERR_NOMEM;
	}
	read_e(db_fptr, client_id, slen);
	client_id[slen] = '\0';

	read_e(db_fptr, &i16temp, sizeof(uint16_t));
	slen = ntohs(i16temp);
	topic = mosquitto__malloc(slen+1);
	if(!topic){
		fclose(db_fptr);
		log__printf(NULL, MOSQ_LOG_ERR, "Error: Out of memory.");
		mosquitto__free(client_id);
		return MOSQ_ERR_NOMEM;
	}
	read_e(db_fptr, topic, slen);
	topic[slen] = '\0';

	read_e(db_fptr, &qos, sizeof(uint8_t));
	if(persist__restore_sub(db, client_id, topic, qos)){
		rc = 1;
	}
	mosquitto__free(client_id);
	mosquitto__free(topic);

	return rc;
error:
	err = strerror(errno);
	log__printf(NULL, MOSQ_LOG_ERR, "Error: %s.", err);
	fclose(db_fptr);
	return 1;
}

int persist__restore(struct mosquitto_db *db)
{
	FILE *fptr;
	char header[15];
	int rc = 0;
	uint32_t crc;
	dbid_t i64temp;
	uint32_t i32temp, length;
	uint16_t i16temp, chunk;
	uint8_t i8temp;
	ssize_t rlen;
	char *err;
	struct mosquitto_msg_store_load *load, *load_tmp;

	assert(db);
	assert(db->config);
	assert(db->config->persistence_filepath);

	db->msg_store_load = NULL;

	fptr = mosquitto__fopen(db->config->persistence_filepath, "rb", false);
	if(fptr == NULL) return MOSQ_ERR_SUCCESS;
	rlen = fread(&header, 1, 15, fptr);
	if(rlen == 0){
		fclose(fptr);
		log__printf(NULL, MOSQ_LOG_WARNING, "Warning: Persistence file is empty.");
		return 0;
	}else if(rlen != 15){
		goto error;
	}
	if(!memcmp(header, magic, 15)){
		// Restore DB as normal
		read_e(fptr, &crc, sizeof(uint32_t));
		read_e(fptr, &i32temp, sizeof(uint32_t));
		db_version = ntohl(i32temp);
		/* IMPORTANT - this is where compatibility checks are made.
		 * Is your DB change still compatible with previous versions?
		 */
		if(db_version > MOSQ_DB_VERSION && db_version != 0){
			if(db_version == 2){
				/* Addition of disconnect_t to client chunk in v3. */
			}else{
				fclose(fptr);
				log__printf(NULL, MOSQ_LOG_ERR, "Error: Unsupported persistent database format version %d (need version %d).", db_version, MOSQ_DB_VERSION);
				return 1;
			}
		}

		while(rlen = fread(&i16temp, sizeof(uint16_t), 1, fptr), rlen == 1){
			chunk = ntohs(i16temp);
			read_e(fptr, &i32temp, sizeof(uint32_t));
			length = ntohl(i32temp);
			switch(chunk){
				case DB_CHUNK_CFG:
					read_e(fptr, &i8temp, sizeof(uint8_t)); // shutdown
					read_e(fptr, &i8temp, sizeof(uint8_t)); // sizeof(dbid_t)
					if(i8temp != sizeof(dbid_t)){
						log__printf(NULL, MOSQ_LOG_ERR, "Error: Incompatible database configuration (dbid size is %d bytes, expected %lu)",
								i8temp, (unsigned long)sizeof(dbid_t));
						fclose(fptr);
						return 1;
					}
					read_e(fptr, &i64temp, sizeof(dbid_t));
					db->last_db_id = i64temp;
					break;

				case DB_CHUNK_MSG_STORE:
					if(persist__msg_store_chunk_restore(db, fptr)) return 1;
					break;

				case DB_CHUNK_CLIENT_MSG:
					if(persist__client_msg_chunk_restore(db, fptr)) return 1;
					break;

				case DB_CHUNK_RETAIN:
					if(persist__retain_chunk_restore(db, fptr)) return 1;
					break;

				case DB_CHUNK_SUB:
					if(persist__sub_chunk_restore(db, fptr)) return 1;
					break;

				case DB_CHUNK_CLIENT:
					if(persist__client_chunk_restore(db, fptr)) return 1;
					break;

				default:
					log__printf(NULL, MOSQ_LOG_WARNING, "Warning: Unsupported chunk \"%d\" in persistent database file. Ignoring.", chunk);
					fseek(fptr, length, SEEK_CUR);
					break;
			}
		}
		if(rlen < 0) goto error;
	}else{
		log__printf(NULL, MOSQ_LOG_ERR, "Error: Unable to restore persistent database. Unrecognised file format.");
		rc = 1;
	}

	fclose(fptr);

	HASH_ITER(hh, db->msg_store_load, load, load_tmp){
		HASH_DELETE(hh, db->msg_store_load, load);
		mosquitto__free(load);
	}
	return rc;
error:
	err = strerror(errno);
	log__printf(NULL, MOSQ_LOG_ERR, "Error: %s.", err);
	if(fptr) fclose(fptr);
	return 1;
}

static int persist__restore_sub(struct mosquitto_db *db, const char *client_id, const char *sub, int qos)
{
	struct mosquitto *context;

	assert(db);
	assert(client_id);
	assert(sub);

	context = persist__find_or_add_context(db, client_id, 0);
	if(!context) return 1;
	return sub__add(db, context, sub, qos, &db->subs);
}

#endif
