/******************************************************************************
 *
 *  Copyright (C) 2014 Google, Inc.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

#define LOG_TAG "bt_osi_config"

#include "osi/include/config.h"

#include <base/logging.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "osi/include/allocator.h"
#include "osi/include/list.h"
#include "osi/include/log.h"
#include "osi/include/compat.h"
#include "log/log.h"
#include "bt_target.h"
#include <inttypes.h>

typedef struct {
  char* key;
  char* value;
} entry_t;

typedef struct {
  char* name;
  list_t* entries;
} section_t;

struct config_t {
  list_t* sections;
};

// Empty definition; this type is aliased to list_node_t.
struct config_section_iter_t {};

static bool config_parse(FILE* fp, config_t* config);

static section_t* section_new(const char* name);
static void section_free(void* ptr);
static section_t* section_find(const config_t* config, const char* section);

static entry_t* entry_new(const char* key, const char* value);
static void entry_free(void* ptr);
static entry_t* entry_find(const config_t* config, const char* section,
                           const char* key);

config_t* config_new_empty(void) {
  config_t* config = static_cast<config_t*>(osi_calloc(sizeof(config_t)));

  config->sections = list_new(section_free);
  if (!config->sections) {
    LOG_ERROR(LOG_TAG, "%s unable to allocate list for sections.", __func__);
    goto error;
  }

  return config;

error:;
  config_free(config);
  return NULL;
}

config_t* config_new(const char* filename) {
  CHECK(filename != NULL);

  config_t* config = config_new_empty();
  if (!config) return NULL;

  FILE* fp = fopen(filename, "rt");
  if (!fp) {
    LOG_ERROR(LOG_TAG, "%s unable to open file '%s': %s", __func__, filename,
              strerror(errno));
    config_free(config);
    return NULL;
  }

  if (!config_parse(fp, config)) {
    config_free(config);
    config = NULL;
  }

  fclose(fp);
  return config;
}

config_t* config_new_clone(const config_t* src) {
  CHECK(src != NULL);

  config_t* ret = config_new_empty();

  CHECK(ret != NULL);

  for (const list_node_t* node = list_begin(src->sections);
       node != list_end(src->sections); node = list_next(node)) {
    section_t* sec = static_cast<section_t*>(list_node(node));

    if (sec) {
      for (const list_node_t* node_entry = list_begin(sec->entries);
           node_entry != list_end(sec->entries);
           node_entry = list_next(node_entry)) {
        entry_t* entry = static_cast<entry_t*>(list_node(node_entry));

        config_set_string(ret, sec->name, entry->key, entry->value);
      }
    }
  }

  return ret;
}

void config_free(config_t* config) {
  if (!config) return;

  list_free(config->sections);
  osi_free(config);
}

bool config_has_section(const config_t* config, const char* section) {
  CHECK(config != NULL);
  CHECK(section != NULL);

  return (section_find(config, section) != NULL);
}

bool config_has_key(const config_t* config, const char* section,
                    const char* key) {
  CHECK(config != NULL);
  CHECK(section != NULL);
  CHECK(key != NULL);

  return (entry_find(config, section, key) != NULL);
}

int config_get_int(const config_t* config, const char* section, const char* key,
                   int def_value) {
  CHECK(config != NULL);
  CHECK(section != NULL);
  CHECK(key != NULL);

  entry_t* entry = entry_find(config, section, key);
  if (!entry) return def_value;

  char* endptr;
  int ret = strtol(entry->value, &endptr, 0);
  return (*endptr == '\0') ? ret : def_value;
}

unsigned short int config_get_uint16(const config_t* config, const char* section, const char* key,
                   uint16_t def_value) {
  CHECK(config != NULL);
  CHECK(section != NULL);
  CHECK(key != NULL);

  entry_t* entry = entry_find(config, section, key);
  if (!entry) return def_value;

  char* endptr;
  uint16_t ret = (uint16_t)strtoumax(entry->value, &endptr, 0);
  return (*endptr == '\0') ? ret : def_value;
}

uint64_t config_get_uint64(const config_t* config, const char* section, const char* key,
                   uint64_t def_value) {
  CHECK(config != NULL);
  CHECK(section != NULL);
  CHECK(key != NULL);

  entry_t* entry = entry_find(config, section, key);
  if (!entry) return def_value;

  char* endptr;
  uint64_t ret = (uint64_t)strtoull(entry->value, &endptr, 0);
  return (*endptr == '\0') ? ret : def_value;
}

bool config_get_bool(const config_t* config, const char* section,
                     const char* key, bool def_value) {
  CHECK(config != NULL);
  CHECK(section != NULL);
  CHECK(key != NULL);

  entry_t* entry = entry_find(config, section, key);
  if (!entry) return def_value;

  if (!strcmp(entry->value, "true")) return true;
  if (!strcmp(entry->value, "false")) return false;

  return def_value;
}

const char* config_get_string(const config_t* config, const char* section,
                              const char* key, const char* def_value) {
  CHECK(config != NULL);
  CHECK(section != NULL);
  CHECK(key != NULL);

  entry_t* entry = entry_find(config, section, key);
  if (!entry) return def_value;

  return entry->value;
}

void config_set_int(config_t* config, const char* section, const char* key,
                    int value) {
  CHECK(config != NULL);
  CHECK(section != NULL);
  CHECK(key != NULL);

  char value_str[32] = {0};
  snprintf(value_str, sizeof(value_str), "%d", value);
  config_set_string(config, section, key, value_str);
}

void config_set_uint16(config_t* config, const char* section, const char* key,
                    uint16_t value) {
  CHECK(config != NULL);
  CHECK(section != NULL);
  CHECK(key != NULL);

  char value_str[16] = {0};
  snprintf(value_str, sizeof(value_str), "%u", value);
  config_set_string(config, section, key, value_str);
}

void config_set_uint64(config_t* config, const char* section, const char* key,
                    uint64_t value) {
  CHECK(config != NULL);
  CHECK(section != NULL);
  CHECK(key != NULL);

  char value_str[64] = {0};
  snprintf(value_str, sizeof(value_str), "%" PRIu64, value);
  config_set_string(config, section, key, value_str);
}

void config_set_bool(config_t* config, const char* section, const char* key,
                     bool value) {
  CHECK(config != NULL);
  CHECK(section != NULL);
  CHECK(key != NULL);

  config_set_string(config, section, key, value ? "true" : "false");
}

void config_set_string(config_t* config, const char* section, const char* key,
                       const char* value) {
  section_t* sec = section_find(config, section);
  if (!sec) {
    sec = section_new(section);
    if (sec)
      list_append(config->sections, sec);
    else {
      LOG_ERROR(LOG_TAG,"%s: Unable to allocate memory for section", __func__);
    }
  }

  std::string value_string = value;
  std::string value_no_newline;
  size_t newline_position = value_string.find("\n");
  if (newline_position != std::string::npos) {
    android_errorWriteLog(0x534e4554, "70808273");
    value_no_newline = value_string.substr(0, newline_position);
  } else {
    value_no_newline = value_string;
  }

  if (sec) {
    for (const list_node_t* node = list_begin(sec->entries);
         node != list_end(sec->entries); node = list_next(node)) {
      entry_t* entry = static_cast<entry_t*>(list_node(node));
      if (!strcmp(entry->key, key)) {
        osi_free(entry->value);
        entry->value = osi_strdup(value_no_newline.c_str());
        return;
      }
    }

    entry_t* entry = entry_new(key, value_no_newline.c_str());
    list_append(sec->entries, entry);
  }
}

bool config_remove_section(config_t* config, const char* section) {
  CHECK(config != NULL);
  CHECK(section != NULL);

  section_t* sec = section_find(config, section);
  if (!sec) return false;

  return list_remove(config->sections, sec);
}

bool config_remove_key(config_t* config, const char* section, const char* key) {
  CHECK(config != NULL);
  CHECK(section != NULL);
  CHECK(key != NULL);

  section_t* sec = section_find(config, section);
  entry_t* entry = entry_find(config, section, key);
  if (!sec || !entry) return false;

  return list_remove(sec->entries, entry);
}

const config_section_node_t* config_section_begin(const config_t* config) {
  CHECK(config != NULL);
  return (const config_section_node_t*)list_begin(config->sections);
}

const config_section_node_t* config_section_end(const config_t* config) {
  CHECK(config != NULL);
  return (const config_section_node_t*)list_end(config->sections);
}

const config_section_node_t* config_section_next(
    const config_section_node_t* node) {
  CHECK(node != NULL);
  return (const config_section_node_t*)list_next((const list_node_t*)node);
}

const char* config_section_name(const config_section_node_t* node) {
  CHECK(node != NULL);
  const list_node_t* lnode = (const list_node_t*)node;
  const section_t* section = (const section_t*)list_node(lnode);
  return section->name;
}

#if (BT_IOT_LOGGING_ENABLED == TRUE)
void config_sections_sort_by_entry_key(config_t* config, compare_func comp) {
  CHECK(config != NULL);

  for (list_node_t* node = list_begin(config->sections);
      node != list_end(config->sections);
      node = list_next(node)) {
    section_t* sec = (section_t*)list_node(node);
    if (list_length(sec->entries) <= 1)
      continue;
    list_node_t* p = list_end(sec->entries);
    list_node_t* head_next = list_next(list_begin(sec->entries));
    bool changed = true;

    while (p != head_next && changed) {
      list_node_t* q = list_begin(sec->entries);
      changed = false;
      for (;list_next(q) && list_next(q) != p; q = list_next(q)) {
        entry_t* first = (entry_t*)list_node(q);
        entry_t* second = (entry_t*)list_node(list_next(q));
        char* tmp_key;
        char* tmp_value;
        if (comp(first->key, second->key) > 0) {
          tmp_key = first->key;
          tmp_value = first->value;
          first->key = second->key;
          first->value = second->value;
          second->key = tmp_key;
          second->value = tmp_value;
          changed = true;
        }
      }
      p = q;
    }

  }
}
#endif

bool config_save(const config_t* config, const char* filename) {
  CHECK(config != NULL);
  CHECK(filename != NULL);
  CHECK(*filename != '\0');

  // Steps to ensure content of config file gets to disk:
  //
  // 1) Open and write to temp file (e.g. bt_config.conf.new).
  // 2) Sync the temp file to disk with fsync().
  // 3) Rename temp file to actual config file (e.g. bt_config.conf).
  //    This ensures atomic update.
  // 4) Sync directory that has the conf file with fsync().
  //    This ensures directory entries are up-to-date.
  int dir_fd = -1;
  FILE* fp = NULL;

  // Build temp config file based on config file (e.g. bt_config.conf.new).
  static const char* temp_file_ext = ".new";
  const int filename_len = strlen(filename);
  const int temp_filename_len = filename_len + strlen(temp_file_ext) + 1;
  char* temp_filename = static_cast<char*>(osi_calloc(temp_filename_len));
  snprintf(temp_filename, temp_filename_len, "%s%s", filename, temp_file_ext);

  // Extract directory from file path (e.g. /data/misc/bluedroid).
  char* temp_dirname = osi_strdup(filename);
  const char* directoryname = dirname(temp_dirname);
  if (!directoryname) {
    LOG_ERROR(LOG_TAG, "%s error extracting directory from '%s': %s", __func__,
              filename, strerror(errno));
    goto error;
  }

  dir_fd = open(directoryname, O_RDONLY);
  if (dir_fd < 0) {
    LOG_ERROR(LOG_TAG, "%s unable to open dir '%s': %s", __func__,
              directoryname, strerror(errno));
    goto error;
  }

  fp = fopen(temp_filename, "wt");
  if (!fp) {
    LOG_ERROR(LOG_TAG, "%s unable to write file '%s': %s", __func__,
              temp_filename, strerror(errno));
    goto error;
  }

  for (const list_node_t* node = list_begin(config->sections);
       node != list_end(config->sections); node = list_next(node)) {
    const section_t* section = (const section_t*)list_node(node);
    if (section->name[0] == '#') {
        if (fprintf(fp, "%s", section->name) < 0) {
            LOG_ERROR(LOG_TAG, "%s unable to write to file '%s': %s", __func__, temp_filename, strerror(errno));
            goto error;
        }
    } else if (fprintf(fp, "[%s]\n", section->name) < 0) {
      LOG_ERROR(LOG_TAG, "%s unable to write to file '%s': %s", __func__, temp_filename, strerror(errno));
      goto error;
    }

    for (const list_node_t* enode = list_begin(section->entries);
         enode != list_end(section->entries); enode = list_next(enode)) {
      const entry_t* entry = (const entry_t*)list_node(enode);
      if (fprintf(fp, "%s = %s\n", entry->key, entry->value) < 0) {
        LOG_ERROR(LOG_TAG, "%s unable to write to file '%s': %s", __func__,
                  temp_filename, strerror(errno));
        goto error;
      }
    }

    // Only add a separating newline if there are more sections.
    if (list_next(node) != list_end(config->sections)) {
      if (fputc('\n', fp) == EOF) {
        LOG_ERROR(LOG_TAG, "%s unable to write to file '%s': %s", __func__,
                  temp_filename, strerror(errno));
        goto error;
      }
    }
  }

  // Sync written temp file out to disk. fsync() is blocking until data makes it
  // to disk.
  if (fsync(fileno(fp)) < 0) {
    LOG_WARN(LOG_TAG, "%s unable to fsync file '%s': %s", __func__,
             temp_filename, strerror(errno));
  }

  if (fclose(fp) == EOF) {
    LOG_ERROR(LOG_TAG, "%s unable to close file '%s': %s", __func__,
              temp_filename, strerror(errno));
    goto error;
  }
  fp = NULL;

  // Change the file's permissions to Read/Write by User and Group
  if (chmod(temp_filename, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP) == -1) {
    LOG_ERROR(LOG_TAG, "%s unable to change file permissions '%s': %s",
              __func__, filename, strerror(errno));
    goto error;
  }

  // Rename written temp file to the actual config file.
  if (rename(temp_filename, filename) == -1) {
    LOG_ERROR(LOG_TAG, "%s unable to commit file '%s': %s", __func__, filename,
              strerror(errno));
    goto error;
  }

  // This should ensure the directory is updated as well.
  if (fsync(dir_fd) < 0) {
    LOG_WARN(LOG_TAG, "%s unable to fsync dir '%s': %s", __func__,
             directoryname, strerror(errno));
  }

  if (close(dir_fd) < 0) {
    LOG_ERROR(LOG_TAG, "%s unable to close dir '%s': %s", __func__,
              directoryname, strerror(errno));
    goto error;
  }
  //sync() will ensure bt_config is saved to NVRAM and prevent file curruption
  sync();
  osi_free(temp_filename);
  osi_free(temp_dirname);
  return true;

error:
  // This indicates there is a write issue.  Unlink as partial data is not
  // acceptable.
  unlink(temp_filename);
  if (fp) fclose(fp);
  if (dir_fd != -1) close(dir_fd);
  osi_free(temp_filename);
  osi_free(temp_dirname);
  return false;
}

static char* trim(char* str) {
  while (isspace(*str)) ++str;

  if (!*str) return str;

  char* end_str = str + strlen(str) - 1;
  while (end_str > str && isspace(*end_str)) --end_str;

  end_str[1] = '\0';
  return str;
}

static bool config_parse(FILE* fp, config_t* config) {
  CHECK(fp != NULL);
  CHECK(config != NULL);

  int line_num = 0;
  char line[1024] = { '\0' };
  char section[1024] = { '\0' };
  char comment[1024] = { '\0' };
  bool skip_entries = false;
  strcpy(section, CONFIG_DEFAULT_SECTION);

  while (fgets(line, sizeof(line), fp)) {
    char* line_ptr = trim(line);
    ++line_num;

    // ignore the line if the line length is more than 1023
    if (strlen(line) == 1023){
        int ch = '\0';
        // read until next line or EOF
        while(((ch = fgetc(fp)) != EOF) && (ch != '\n'));
        continue;
    }

    // Skip blanks.
    if (*line_ptr == '\0')
      continue;

    if (*line_ptr == '#') {
        strlcpy(comment, line_ptr, 1024);

        if(!section_find(config, comment)) {
            section_t *sec = section_new(comment);
            if (sec)
                list_append(config->sections, sec);
        }
    } else if (*line_ptr == '[') {
      size_t len = strlen(line_ptr);
      if (line_ptr[len - 1] != ']') {
        LOG_DEBUG(LOG_TAG, "%s unterminated section name on line %d.", __func__, line_num);
        skip_entries = true;
        continue;
      }
      strncpy(section, line_ptr + 1, len - 2);
      section[len - 2] = '\0';
      skip_entries = false;
    } else {
      char *split = strchr(line_ptr, '=');
      if(skip_entries) {
        LOG_DEBUG(LOG_TAG, "%s skip entries due invalid section line %d.", __func__, line_num);
        continue;
      }
      if (!split) {
        LOG_DEBUG(LOG_TAG, "%s no key/value separator found on line %d.", __func__, line_num);
        continue;
      }

      *split = '\0';
      config_set_string(config, section, trim(line_ptr), trim(split + 1));
    }
  }
  return true;
}

static section_t* section_new(const char* name) {
  section_t* section = static_cast<section_t*>(osi_calloc(sizeof(section_t)));

  section->name = osi_strdup(name);
  section->entries = list_new(entry_free);
  return section;
}

static void section_free(void* ptr) {
  if (!ptr) return;

  section_t* section = static_cast<section_t*>(ptr);
  osi_free(section->name);
  list_free(section->entries);
  osi_free(section);
}

static section_t* section_find(const config_t* config, const char* section) {
  for (const list_node_t* node = list_begin(config->sections);
       node != list_end(config->sections); node = list_next(node)) {
    section_t* sec = static_cast<section_t*>(list_node(node));
    if (sec && !strcmp(sec->name, section)) return sec;
}

  return NULL;
}

static entry_t* entry_new(const char* key, const char* value) {
  entry_t* entry = static_cast<entry_t*>(osi_calloc(sizeof(entry_t)));

  entry->key = osi_strdup(key);
  entry->value = osi_strdup(value);
  return entry;
}

static void entry_free(void* ptr) {
  if (!ptr) return;

  entry_t* entry = static_cast<entry_t*>(ptr);
  osi_free(entry->key);
  osi_free(entry->value);
  osi_free(entry);
}

static entry_t* entry_find(const config_t* config, const char* section,
                           const char* key) {
  section_t* sec = section_find(config, section);
  if (!sec) return NULL;

  for (const list_node_t* node = list_begin(sec->entries);
       node != list_end(sec->entries); node = list_next(node)) {
    entry_t* entry = static_cast<entry_t*>(list_node(node));
    if (!strcmp(entry->key, key)) return entry;
  }

  return NULL;
}
