/*
 * Minimal cJSON-compatible interface used by CineBook.
 * Provides enough API surface for reports.cpp and related code.
 */

#ifndef CJSON_H
#define CJSON_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

/* cJSON type flags (compatible naming) */
#define cJSON_False  (1 << 0)
#define cJSON_True   (1 << 1)
#define cJSON_NULL   (1 << 2)
#define cJSON_Number (1 << 3)
#define cJSON_String (1 << 4)
#define cJSON_Array  (1 << 5)
#define cJSON_Object (1 << 6)

typedef struct cJSON {
    struct cJSON *next;
    struct cJSON *prev;
    struct cJSON *child;

    int type;

    char *valuestring;
    int valueint;
    double valuedouble;

    char *string;
} cJSON;

/* Parse + lifecycle */
cJSON *cJSON_Parse(const char *value);
void cJSON_Delete(cJSON *item);

/* Tree access */
cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON *object, const char *string);
int cJSON_GetArraySize(const cJSON *array);
cJSON *cJSON_GetArrayItem(const cJSON *array, int index);

/* Type checks */
int cJSON_IsString(const cJSON *item);
int cJSON_IsNumber(const cJSON *item);
int cJSON_IsArray(const cJSON *item);

#ifdef __cplusplus
}
#endif

#endif /* CJSON_H */