#include "cJSON.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static cJSON *cjson_new_item(void)
{
    cJSON *item = (cJSON *)calloc(1, sizeof(cJSON));
    return item;
}

static void cjson_delete_internal(cJSON *item)
{
    if (!item) return;

    cJSON *child = item->child;
    while (child) {
        cJSON *next = child->next;
        cjson_delete_internal(child);
        child = next;
    }

    free(item->valuestring);
    free(item->string);
    free(item);
}

void cJSON_Delete(cJSON *item)
{
    cjson_delete_internal(item);
}

static const char *skip_ws(const char *s)
{
    while (s && *s && isspace((unsigned char)*s)) s++;
    return s;
}

static char *dup_range(const char *start, const char *end)
{
    if (!start || !end || end < start) return NULL;
    size_t len = (size_t)(end - start);
    char *out = (char *)malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, start, len);
    out[len] = '\0';
    return out;
}

static char *parse_json_string_raw(const char **sp)
{
    const char *s = *sp;
    if (!s || *s != '"') return NULL;
    s++;

    size_t cap = 32;
    size_t len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) return NULL;

    while (*s && *s != '"') {
        char ch = *s++;
        if (ch == '\\') {
            char esc = *s++;
            if (!esc) {
                free(buf);
                return NULL;
            }
            switch (esc) {
                case '"': ch = '"'; break;
                case '\\': ch = '\\'; break;
                case '/': ch = '/'; break;
                case 'b': ch = '\b'; break;
                case 'f': ch = '\f'; break;
                case 'n': ch = '\n'; break;
                case 'r': ch = '\r'; break;
                case 't': ch = '\t'; break;
                case 'u':
                    /* Minimal handling: skip 4 hex chars and store '?' */
                    for (int i = 0; i < 4; i++) {
                        if (!isxdigit((unsigned char)*s)) {
                            free(buf);
                            return NULL;
                        }
                        s++;
                    }
                    ch = '?';
                    break;
                default:
                    free(buf);
                    return NULL;
            }
        }

        if (len + 2 > cap) {
            size_t ncap = cap * 2;
            char *nbuf = (char *)realloc(buf, ncap);
            if (!nbuf) {
                free(buf);
                return NULL;
            }
            buf = nbuf;
            cap = ncap;
        }
        buf[len++] = ch;
    }

    if (*s != '"') {
        free(buf);
        return NULL;
    }
    s++;

    buf[len] = '\0';
    *sp = s;
    return buf;
}

static const char *parse_value(const char *s, cJSON **out);

static const char *parse_number(const char *s, cJSON **out)
{
    const char *p = s;
    if (*p == '-') p++;
    if (!isdigit((unsigned char)*p)) return NULL;
    if (*p == '0') {
        p++;
    } else {
        while (isdigit((unsigned char)*p)) p++;
    }
    if (*p == '.') {
        p++;
        if (!isdigit((unsigned char)*p)) return NULL;
        while (isdigit((unsigned char)*p)) p++;
    }
    if (*p == 'e' || *p == 'E') {
        p++;
        if (*p == '+' || *p == '-') p++;
        if (!isdigit((unsigned char)*p)) return NULL;
        while (isdigit((unsigned char)*p)) p++;
    }

    char *num = dup_range(s, p);
    if (!num) return NULL;

    cJSON *item = cjson_new_item();
    if (!item) {
        free(num);
        return NULL;
    }

    item->type = cJSON_Number;
    item->valuedouble = strtod(num, NULL);
    item->valueint = (int)item->valuedouble;
    free(num);

    *out = item;
    return p;
}

static const char *parse_literal(const char *s, const char *lit, int type, cJSON **out)
{
    size_t n = strlen(lit);
    if (strncmp(s, lit, n) != 0) return NULL;

    cJSON *item = cjson_new_item();
    if (!item) return NULL;
    item->type = type;

    if (type == cJSON_True) {
        item->valueint = 1;
        item->valuedouble = 1.0;
    }

    *out = item;
    return s + n;
}

static void append_child(cJSON *parent, cJSON *child)
{
    if (!parent || !child) return;
    if (!parent->child) {
        parent->child = child;
        return;
    }
    cJSON *cur = parent->child;
    while (cur->next) cur = cur->next;
    cur->next = child;
    child->prev = cur;
}

static const char *parse_array(const char *s, cJSON **out)
{
    if (*s != '[') return NULL;
    s = skip_ws(s + 1);

    cJSON *arr = cjson_new_item();
    if (!arr) return NULL;
    arr->type = cJSON_Array;

    if (*s == ']') {
        *out = arr;
        return s + 1;
    }

    while (1) {
        cJSON *val = NULL;
        s = parse_value(s, &val);
        if (!s || !val) {
            cJSON_Delete(arr);
            return NULL;
        }
        append_child(arr, val);

        s = skip_ws(s);
        if (*s == ',') {
            s = skip_ws(s + 1);
            continue;
        }
        if (*s == ']') {
            *out = arr;
            return s + 1;
        }

        cJSON_Delete(arr);
        return NULL;
    }
}

static const char *parse_object(const char *s, cJSON **out)
{
    if (*s != '{') return NULL;
    s = skip_ws(s + 1);

    cJSON *obj = cjson_new_item();
    if (!obj) return NULL;
    obj->type = cJSON_Object;

    if (*s == '}') {
        *out = obj;
        return s + 1;
    }

    while (1) {
        char *key = parse_json_string_raw(&s);
        if (!key) {
            cJSON_Delete(obj);
            return NULL;
        }

        s = skip_ws(s);
        if (*s != ':') {
            free(key);
            cJSON_Delete(obj);
            return NULL;
        }

        s = skip_ws(s + 1);

        cJSON *val = NULL;
        s = parse_value(s, &val);
        if (!s || !val) {
            free(key);
            cJSON_Delete(obj);
            return NULL;
        }

        val->string = key;
        append_child(obj, val);

        s = skip_ws(s);
        if (*s == ',') {
            s = skip_ws(s + 1);
            continue;
        }
        if (*s == '}') {
            *out = obj;
            return s + 1;
        }

        cJSON_Delete(obj);
        return NULL;
    }
}

static const char *parse_string_value(const char *s, cJSON **out)
{
    char *str = parse_json_string_raw(&s);
    if (!str) return NULL;

    cJSON *item = cjson_new_item();
    if (!item) {
        free(str);
        return NULL;
    }

    item->type = cJSON_String;
    item->valuestring = str;
    *out = item;
    return s;
}

static const char *parse_value(const char *s, cJSON **out)
{
    s = skip_ws(s);
    if (!s || !*s) return NULL;

    if (*s == '"') return parse_string_value(s, out);
    if (*s == '{') return parse_object(s, out);
    if (*s == '[') return parse_array(s, out);
    if (*s == '-' || isdigit((unsigned char)*s)) return parse_number(s, out);
    if (*s == 't') return parse_literal(s, "true", cJSON_True, out);
    if (*s == 'f') return parse_literal(s, "false", cJSON_False, out);
    if (*s == 'n') return parse_literal(s, "null", cJSON_NULL, out);

    return NULL;
}

cJSON *cJSON_Parse(const char *value)
{
    if (!value) return NULL;

    cJSON *root = NULL;
    const char *end = parse_value(value, &root);
    if (!end || !root) return NULL;

    end = skip_ws(end);
    if (*end != '\0') {
        cJSON_Delete(root);
        return NULL;
    }

    return root;
}

cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON *object, const char *string)
{
    if (!object || !(object->type & cJSON_Object) || !string) return NULL;

    cJSON *child = object->child;
    while (child) {
        if (child->string && strcmp(child->string, string) == 0) return child;
        child = child->next;
    }
    return NULL;
}

int cJSON_GetArraySize(const cJSON *array)
{
    if (!array || !(array->type & cJSON_Array)) return 0;

    int count = 0;
    cJSON *child = array->child;
    while (child) {
        count++;
        child = child->next;
    }
    return count;
}

cJSON *cJSON_GetArrayItem(const cJSON *array, int index)
{
    if (!array || !(array->type & cJSON_Array) || index < 0) return NULL;

    int i = 0;
    cJSON *child = array->child;
    while (child) {
        if (i == index) return child;
        i++;
        child = child->next;
    }
    return NULL;
}

int cJSON_IsString(const cJSON *item)
{
    return (item && (item->type & cJSON_String)) ? 1 : 0;
}

int cJSON_IsNumber(const cJSON *item)
{
    return (item && (item->type & cJSON_Number)) ? 1 : 0;
}

int cJSON_IsArray(const cJSON *item)
{
    return (item && (item->type & cJSON_Array)) ? 1 : 0;
}