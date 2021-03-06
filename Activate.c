/*
 * Activate.c
 * Based off Spirit jailbreak for Linux
 *
 * Activate Copyright (c) 2011 Luke Young. All Rights Reserved.
 * Spirit Jailbreak Copyright (c) 2010 Joshua Hill. All Rights Reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA 
 */

#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/time.h>
#include <openssl/sha.h>
#include <plist/plist.h>
#include <libimobiledevice/afc.h>
#include <libimobiledevice/lockdown.h>
#include <libimobiledevice/mobilebackup.h>
#include <libimobiledevice/libimobiledevice.h>

#include "afc.h"

#define BUFSIZE 0x10000

typedef enum {
	kStageFirst,
	kStageSecond,
	kStageAll
} send_file_stage;

int some_unique;
static idevice_t device;
static lockdownd_client_t lockdownd;

static void receive(mobilebackup_client_t backup) {
	char* xml = NULL;
	uint32_t size = 0;
	plist_t data = NULL;
	mobilebackup_receive(backup, &data);
	plist_free(data);
}

char* sha1_of_data(char* input, uint32_t size) {
	unsigned char* hash = (unsigned char*) malloc(20);
	SHA_CTX ctx;
	SHA1_Init(&ctx);
	SHA1_Update(&ctx, input, size);
	SHA1_Final(hash, &ctx);
	return hash;
}

char* data_to_hex(unsigned char* input, size_t size) {
	unsigned int i = 0;
	unsigned char* result = (unsigned char*) malloc((size*2)+1);
	for(i = 0; i < size*2; i+=2) {
		unsigned char byte = input[i/2];
		sprintf(&result[i], "%02x", byte);
	}
	return result;
}

char* read_file(const char *filename, uint64_t* size) {
	printf("INFO: Read %s\n", filename);
	FILE* fd = fopen(filename, "rb");
	if(fd == NULL) {
		fprintf(stderr, "ERROR: Cannot open file\n");
		return NULL;
	}

	fseek(fd, 0, SEEK_END);
	long length = ftell(fd);
	fseek(fd, 0, SEEK_SET);

	char* data = (char*) malloc(length+1);
	if(data == NULL) {
		fprintf(stderr, "ERROR: Unable to allocate data buffer\n");
		fclose(fd);
		return NULL;
	}

	size_t bytes = fread(data, 1, length, fd);
	if(bytes != length) {
		fprintf(stderr, "ERROR: Incorrect size\n");
		free(data);
		data = NULL;
	}

	*size = (uint64_t) length;
	fclose(fd);
	return data;
}

static void start_restore(mobilebackup_client_t backup, plist_t files) {
	char* uuid = NULL;
	idevice_get_uuid(device, &uuid);
	printf("DEBUG: start_restore\n");
	plist_t m1dict = plist_new_dict();
	plist_dict_insert_item(m1dict, "Version", plist_new_string("6.2"));
	plist_dict_insert_item(m1dict, "DeviceId", plist_new_string(uuid));
	plist_dict_insert_item(m1dict, "Applications", plist_new_dict());
	plist_dict_insert_item(m1dict, "Files", files);

	uint32_t m1size = 0;
	char* m1data = NULL;
	plist_to_bin(m1dict, &m1data, &m1size);

	plist_t manifest = plist_new_dict();
	plist_dict_insert_item(manifest, "Version", plist_new_string("4.0"));
	plist_dict_insert_item(manifest, "AuthSignature", plist_new_data(sha1_of_data(m1data, m1size), 20));
	plist_dict_insert_item(manifest, "IsEncrypted", plist_new_uint(0));
	plist_dict_insert_item(manifest, "Data", plist_new_data(m1data, m1size));

	plist_t mdict = plist_new_dict();
	plist_dict_insert_item(mdict, "BackupMessageTypeKey", plist_new_string("kBackupMessageRestoreRequest"));
	plist_dict_insert_item(mdict, "BackupNotifySpringBoard", plist_new_bool(1));
	plist_dict_insert_item(mdict, "BackupProtocolVersion", plist_new_string("3.0"));
	plist_dict_insert_item(mdict, "BackupPreserveCameraRoll", plist_new_bool(1));
	plist_dict_insert_item(mdict, "BackupPreserveSettings", plist_new_bool(1));
	plist_dict_insert_item(mdict, "BackupManifestKey", manifest);

	plist_t message = plist_new_array();
	plist_array_append_item(message, plist_new_string("DLMessageProcessMessage"));
	plist_array_append_item(message, mdict);
	mobilebackup_send(backup, message);
	receive(backup);
}

void* add_file(plist_t files, char* crap, uint64_t crap_size, char* domain, char* path, int uid, int gid, int mode) {
	unsigned char* pathdata = strdup(path);
	char* manifestkey = data_to_hex(sha1_of_data(pathdata, strlen(path)), 20);
	printf("DEBUG: add_file\n");
	plist_t dict = plist_new_dict();
	plist_dict_insert_item(dict, "Domain", plist_new_string(domain));
	plist_dict_insert_item(dict, "Path", plist_new_string(path));
	plist_dict_insert_item(dict, "Greylist", plist_new_bool(0));
	plist_dict_insert_item(dict, "Version", plist_new_string("4.0"));
	plist_dict_insert_item(dict, "Data", plist_new_data(crap, crap_size));
	printf("DEBUG: Data size %llu:\n%s\n", crap_size, data_to_hex(crap, crap_size));

	char* datahash = malloc(20);
	char* extra = ";(null);(null);(null);3.0";
	SHA_CTX ctx;
	SHA1_Init(&ctx);
	SHA1_Update(&ctx, crap, crap_size);
	SHA1_Update(&ctx, pathdata, strlen(pathdata));
	SHA1_Update(&ctx, extra, strlen(extra));
	SHA1_Final(datahash, &ctx);

	plist_t manifest = plist_new_dict();
	plist_dict_insert_item(manifest, "DataHash", plist_new_data(datahash, 20));
	plist_dict_insert_item(manifest, "Domain", plist_new_string(domain));
	plist_dict_insert_item(manifest, "FileLength", plist_new_uint(crap_size));
	plist_dict_insert_item(manifest, "Group ID", plist_new_uint(gid));
	plist_dict_insert_item(manifest, "User ID", plist_new_uint(uid));
	plist_dict_insert_item(manifest, "Mode", plist_new_uint(mode));
	plist_dict_insert_item(manifest, "ModificationTime", plist_new_date(2020964986UL, 0));
	plist_dict_insert_item(files, manifestkey, manifest);

	plist_t info = plist_new_dict();
	char* templateo = (char*) malloc(0x20);
	snprintf(templateo, 0x20, "/tmp/stuff.%06d", ++some_unique);
	plist_dict_insert_item(info, "DLFileDest", plist_new_string(templateo));
	plist_dict_insert_item(info, "Path", plist_new_string(path));
	plist_dict_insert_item(info, "Version", plist_new_string("3.0"));
	plist_dict_insert_item(info, "Crap", plist_new_data(crap, crap_size));

	return info;
}

void send_file(mobilebackup_client_t backup, plist_t info, send_file_stage stage) {
	char* crap = NULL;
	uint64_t crap_size = 0;
	printf("DEBUG: Sending file\n");

	char* dest = NULL;
	plist_get_string_val(plist_dict_get_item(info, "DLFileDest"), &dest);
	plist_get_data_val(plist_dict_get_item(info, "Crap"), &crap, &crap_size);
	plist_dict_remove_item(info, "Crap");

	plist_dict_insert_item(info, "DLFileAttributesKey", plist_new_dict());
	plist_dict_insert_item(info, "DLFileSource", plist_new_string(dest));
	plist_dict_insert_item(info, "DLFileIsEncrypted", plist_new_uint(0));
	plist_dict_insert_item(info, "DLFileOffsetKey", plist_new_uint(0));
	plist_dict_insert_item(info, "DLFileStatusKey", plist_new_uint(2));

	plist_t message = plist_new_array();
	plist_array_append_item(message, plist_new_string("DLSendFile"));
	plist_array_append_item(message, plist_new_data(crap, crap_size));
	plist_array_append_item(message, info);
	mobilebackup_send(backup, message);

	receive(backup);
	//if(crap) plist_free(crap);
	//if(message) plist_free(message);
}

static void restore_thread() {
	lockdownd_error_t lockdownd_error = 0;
	printf("INFO: Creating lockdownd client\n");
	lockdownd_error = lockdownd_client_new_with_handshake(device, &lockdownd, "Activate");
	if(lockdownd_error != LOCKDOWN_E_SUCCESS) {
		fprintf(stderr, "ERROR: Cannot create lockdownd client\n");
	}
	
	uint16_t port = 0;
	mobilebackup_client_t backup = NULL;
	printf("INFO: Starting MobileBackup service\n");

	lockdownd_error_t lockdown_err = lockdownd_start_service(lockdownd, "com.apple.mobilebackup", &port);
	if (lockdown_err != LOCKDOWN_E_SUCCESS) {
		fprintf(stderr, "ERROR: Cannot start MobileBackup service\n");
		return;
	}
	
	if(lockdownd) { 
		lockdownd_client_free(lockdownd);
		lockdownd = NULL;
	}

	mobilebackup_error_t backup_err = mobilebackup_client_new(device, port, &backup);
	if(backup_err != MOBILEBACKUP_E_SUCCESS) {
		fprintf(stderr, "ERROR: Cannot connect to MobileBackup service, %d\n", backup_err);
		return;
	}
	printf("INFO: Beginning restore process\n");

	uint64_t size = 0;
	plist_t files =  plist_new_dict();
	char* data = read_file("com.apple.purplebuddy.plist", &size);
	plist_t overrides = add_file(files, data, size, "HomeDomain", "Library/Preferences/com.apple.purplebuddy.plist", 0, 0, 0600);

	start_restore(backup, files);

	send_file(backup, overrides, kStageAll);

	printf("INFO: Completed restore\n");
	mobilebackup_client_free(backup);
	//plist_free(files);
}

int main(int argc, char** argv) {
	char uuid[41];
	int count = 0;
	char **list = NULL;
	idevice_error_t device_error = 0;

	printf("INFO: Retriving device list\n");
	//idevice_set_debug_level(1);
	if (idevice_get_device_list(&list, &count) < 0) {
		fprintf(stderr, "ERROR: Cannot retrieve device list\n");
		return -1;
	}

	memset(uuid, '\0', 41);
	memcpy(uuid, list[0], 40);
	idevice_device_list_free(list);

	printf("INFO: Opening device\n");
	device_error = idevice_new(&device, uuid);
	if(device_error != IDEVICE_E_SUCCESS) {
		if(device_error == IDEVICE_E_NO_DEVICE) {
			fprintf(stderr, "ERROR: No device found\n");
		} else {
			fprintf(stderr, "ERROR: Unable to open device, %d\n", device_error);
		}
		return -1;
	}

	some_unique = (int) time(NULL);
	restore_thread();

	printf("INFO: Completed successfully\n");
	if(device) idevice_free(device);
	return 0;
}
