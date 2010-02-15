/*
 * Copyright 2009 10gen, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * This file contains C implementations of some of the functions needed by the
 * bson module. If possible, these implementations should be used to speed up
 * BSON encoding and decoding.
 */

#include "ruby.h"

#if HAVE_RUBY_ST_H
#include "ruby/st.h"
#endif
#if HAVE_ST_H
#include "st.h"
#endif

#if HAVE_RUBY_REGEX_H
#include "ruby/regex.h"
#endif
#if HAVE_REGEX_H
#include "regex.h"
#endif

#include <string.h>
#include <math.h>
#include <unistd.h>
#include <time.h>

#include "version.h"
#include "buffer.h"
#include "encoding_helpers.h"

#define SAFE_WRITE(buffer, data, size)                                  \
    if (buffer_write((buffer), (data), (size)) != 0)                    \
        rb_raise(rb_eNoMemError, "failed to allocate memory in buffer.c")

#define SAFE_WRITE_AT_POS(buffer, position, data, size)                 \
    if (buffer_write_at_position((buffer), (position), (data), (size)) != 0) \
        rb_raise(rb_eRuntimeError, "invalid write at position in buffer.c")

#define MAX_HOSTNAME_LENGTH 256

static VALUE Binary;
static VALUE Time;
static VALUE ObjectID;
static VALUE DBRef;
static VALUE Code;
static VALUE MinKey;
static VALUE MaxKey;
static VALUE Regexp;
static VALUE RegexpOfHolding;
static VALUE OrderedHash;
static VALUE InvalidName;
static VALUE InvalidStringEncoding;
static VALUE InvalidDocument;
static VALUE DigestMD5;

#if HAVE_RUBY_ENCODING_H
#include "ruby/encoding.h"
#define STR_NEW(p,n) rb_enc_str_new((p), (n), rb_utf8_encoding())
/* MUST call TO_UTF8 before calling write_utf8. */
#define TO_UTF8(string) rb_str_export_to_enc((string), rb_utf8_encoding())
static void write_utf8(buffer_t buffer, VALUE string, char check_null) {
    result_t status = check_string(RSTRING_PTR(string), RSTRING_LEN(string),
                                   0, check_null);
    if (status == HAS_NULL) {
        buffer_free(buffer);
        rb_raise(InvalidDocument, "Key names / regex patterns must not contain the NULL byte");
    }
    SAFE_WRITE(buffer, RSTRING_PTR(string), RSTRING_LEN(string));
}
#else
#define STR_NEW(p,n) rb_str_new((p), (n))
/* MUST call TO_UTF8 before calling write_utf8. */
#define TO_UTF8(string) (string)
static void write_utf8(buffer_t buffer, VALUE string, char check_null) {
    result_t status = check_string(RSTRING_PTR(string), RSTRING_LEN(string),
                                   1, check_null);
    if (status == HAS_NULL) {
        buffer_free(buffer);
        rb_raise(InvalidDocument, "Key names / regex patterns must not contain the NULL byte");
    } else if (status == NOT_UTF_8) {
        buffer_free(buffer);
        rb_raise(InvalidStringEncoding, "String not valid UTF-8");
    }
    SAFE_WRITE(buffer, RSTRING_PTR(string), RSTRING_LEN(string));
}
#endif

// this sucks. but for some reason these moved around between 1.8 and 1.9
#ifdef ONIGURUMA_H
#define IGNORECASE ONIG_OPTION_IGNORECASE
#define MULTILINE ONIG_OPTION_MULTILINE
#define EXTENDED ONIG_OPTION_EXTEND
#else
#define IGNORECASE RE_OPTION_IGNORECASE
#define MULTILINE RE_OPTION_MULTILINE
#define EXTENDED RE_OPTION_EXTENDED
#endif

/* TODO we ought to check that the malloc or asprintf was successful
 * and raise an exception if not. */
/* TODO maybe we can use something more portable like vsnprintf instead
 * of this hack. And share it with the Python extension ;) */
#ifndef HAVE_ASPRINTF
#define INT2STRING(buffer, i)                   \
    {                                           \
        int vslength = _scprintf("%d", i) + 1;  \
        *buffer = malloc(vslength);             \
        _snprintf(*buffer, vslength, "%d", i);  \
    }
#else
#define INT2STRING(buffer, i) asprintf(buffer, "%d", i);
#endif

// this sucks too.
#ifndef RREGEXP_SRC
#define RREGEXP_SRC(r) rb_str_new(RREGEXP((r))->str, RREGEXP((r))->len)
#endif

static char zero = 0;
static char one = 1;

static int cmp_char(const void* a, const void* b) {
    return *(char*)a - *(char*)b;
}

static void write_doc(buffer_t buffer, VALUE hash, VALUE check_keys, VALUE move_id);
static int write_element(VALUE key, VALUE value, VALUE extra);
static VALUE elements_to_hash(const char* buffer, int max);

static VALUE pack_extra(buffer_t buffer, VALUE check_keys) {
    return rb_ary_new3(2, LL2NUM((long long)buffer), check_keys);
}

static VALUE pack_triple(buffer_t buffer, VALUE check_keys, int allow_id) {
    return rb_ary_new3(3, LL2NUM((long long)buffer), check_keys, allow_id);
}

static void write_name_and_type(buffer_t buffer, VALUE name, char type) {
    SAFE_WRITE(buffer, &type, 1);
    name = TO_UTF8(name);
    write_utf8(buffer, name, 1);
    SAFE_WRITE(buffer, &zero, 1);
}

static int write_element_allow_id(VALUE key, VALUE value, VALUE extra, int allow_id) {
    buffer_t buffer = (buffer_t)NUM2LL(rb_ary_entry(extra, 0));
    VALUE check_keys = rb_ary_entry(extra, 1);

    if (TYPE(key) == T_SYMBOL) {
        // TODO better way to do this... ?
        key = rb_str_new2(rb_id2name(SYM2ID(key)));
    }

    if (TYPE(key) != T_STRING) {
        buffer_free(buffer);
        rb_raise(rb_eTypeError, "keys must be strings or symbols");
    }

    if (!allow_id && strcmp("_id", RSTRING_PTR(key)) == 0) {
        return ST_CONTINUE;
    }

    if (check_keys == Qtrue) {
        int i;
        if (RSTRING_LEN(key) > 0 && RSTRING_PTR(key)[0] == '$') {
            buffer_free(buffer);
            rb_raise(InvalidName, "key must not start with '$'");
        }
        for (i = 0; i < RSTRING_LEN(key); i++) {
            if (RSTRING_PTR(key)[i] == '.') {
                buffer_free(buffer);
                rb_raise(InvalidName, "key must not contain '.'");
            }
        }
    }

    switch(TYPE(value)) {
    case T_BIGNUM:
    case T_FIXNUM:
        {
            if (rb_funcall(value, rb_intern(">"), 1, LL2NUM(9223372036854775807LL)) == Qtrue ||
                rb_funcall(value, rb_intern("<"), 1, LL2NUM(-9223372036854775808ULL)) == Qtrue) {
                buffer_free(buffer);
                rb_raise(rb_eRangeError, "MongoDB can only handle 8-byte ints");
            }
            if (rb_funcall(value, rb_intern(">"), 1, INT2NUM(2147483647L)) == Qtrue ||
                rb_funcall(value, rb_intern("<"), 1, INT2NUM(-2147483648L)) == Qtrue) {
                long long ll_value;
                write_name_and_type(buffer, key, 0x12);
                ll_value = NUM2LL(value);
                SAFE_WRITE(buffer, (char*)&ll_value, 8);
            } else {
                int int_value;
                write_name_and_type(buffer, key, 0x10);
                int_value = NUM2LL(value);
                SAFE_WRITE(buffer, (char*)&int_value, 4);
            }
            break;
        }
    case T_TRUE:
        {
            write_name_and_type(buffer, key, 0x08);
            SAFE_WRITE(buffer, &one, 1);
            break;
        }
    case T_FALSE:
        {
            write_name_and_type(buffer, key, 0x08);
            SAFE_WRITE(buffer, &zero, 1);
            break;
        }
    case T_FLOAT:
        {
            double d = NUM2DBL(value);
            write_name_and_type(buffer, key, 0x01);
            SAFE_WRITE(buffer, (char*)&d, 8);
            break;
        }
    case T_NIL:
        {
            write_name_and_type(buffer, key, 0x0A);
            break;
        }
    case T_HASH:
        {
            write_name_and_type(buffer, key, 0x03);
            write_doc(buffer, value, check_keys, Qfalse);
            break;
        }
    case T_ARRAY:
        {
            buffer_position length_location, start_position, obj_length;
            int items, i;
            VALUE* values;

            write_name_and_type(buffer, key, 0x04);
            start_position = buffer_get_position(buffer);

            // save space for length
            length_location = buffer_save_space(buffer, 4);
            if (length_location == -1) {
                rb_raise(rb_eNoMemError, "failed to allocate memory in buffer.c");
            }

            items = RARRAY_LEN(value);
            values = RARRAY_PTR(value);
            for(i = 0; i < items; i++) {
                char* name;
                VALUE key;
                INT2STRING(&name, i);
                key = rb_str_new2(name);
                write_element(key, values[i], pack_extra(buffer, check_keys));
                free(name);
            }

            // write null byte and fill in length
            SAFE_WRITE(buffer, &zero, 1);
            obj_length = buffer_get_position(buffer) - start_position;
            SAFE_WRITE_AT_POS(buffer, length_location, (const char*)&obj_length, 4);
            break;
        }
    case T_STRING:
        {
            if (strcmp(rb_class2name(RBASIC(value)->klass),
                  "Mongo::Code") == 0) {
                buffer_position length_location, start_position, total_length;
                int length;
                write_name_and_type(buffer, key, 0x0F);

                start_position = buffer_get_position(buffer);
                length_location = buffer_save_space(buffer, 4);
                if (length_location == -1) {
                    rb_raise(rb_eNoMemError, "failed to allocate memory in buffer.c");
                }

                length = RSTRING_LEN(value) + 1;
                SAFE_WRITE(buffer, (char*)&length, 4);
                SAFE_WRITE(buffer, RSTRING_PTR(value), length - 1);
                SAFE_WRITE(buffer, &zero, 1);
                write_doc(buffer, rb_funcall(value, rb_intern("scope"), 0), Qfalse, Qfalse);

                total_length = buffer_get_position(buffer) - start_position;
                SAFE_WRITE_AT_POS(buffer, length_location, (const char*)&total_length, 4);
                break;
            } else {
                int length;
                write_name_and_type(buffer, key, 0x02);
                value = TO_UTF8(value);
                length = RSTRING_LEN(value) + 1;
                SAFE_WRITE(buffer, (char*)&length, 4);
                write_utf8(buffer, value, 0);
                SAFE_WRITE(buffer, &zero, 1);
                break;
            }
        }
    case T_SYMBOL:
        {
            const char* str_value = rb_id2name(SYM2ID(value));
            int length = strlen(str_value) + 1;
            write_name_and_type(buffer, key, 0x0E);
            SAFE_WRITE(buffer, (char*)&length, 4);
            SAFE_WRITE(buffer, str_value, length);
            break;
        }
    case T_OBJECT:
        {
            // TODO there has to be a better way to do these checks...
            const char* cls = rb_class2name(RBASIC(value)->klass);
            if (strcmp(cls, "Mongo::Binary") == 0 ||
                strcmp(cls, "ByteBuffer") == 0) {
                const char subtype = strcmp(cls, "ByteBuffer") ?
                    (const char)FIX2INT(rb_funcall(value, rb_intern("subtype"), 0)) : 2;
                VALUE string_data = rb_funcall(value, rb_intern("to_s"), 0);
                int length = RSTRING_LEN(string_data);
                write_name_and_type(buffer, key, 0x05);
                if (subtype == 2) {
                    const int other_length = length + 4;
                    SAFE_WRITE(buffer, (const char*)&other_length, 4);
                    SAFE_WRITE(buffer, &subtype, 1);
                }
                SAFE_WRITE(buffer, (const char*)&length, 4);
                if (subtype != 2) {
                    SAFE_WRITE(buffer, &subtype, 1);
                }
                SAFE_WRITE(buffer, RSTRING_PTR(string_data), length);
                break;
            }
            if (strcmp(cls, "Mongo::ObjectID") == 0) {
                VALUE as_array = rb_funcall(value, rb_intern("to_a"), 0);
                int i;
                write_name_and_type(buffer, key, 0x07);
                for (i = 0; i < 12; i++) {
                    char byte = (char)FIX2INT(RARRAY_PTR(as_array)[i]);
                    SAFE_WRITE(buffer, &byte, 1);
                }
                break;
            }
            if (strcmp(cls, "Mongo::DBRef") == 0) {
                buffer_position length_location, start_position, obj_length;
                VALUE ns, oid;
                write_name_and_type(buffer, key, 0x03);

                start_position = buffer_get_position(buffer);

                // save space for length
                length_location = buffer_save_space(buffer, 4);
                if (length_location == -1) {
                    rb_raise(rb_eNoMemError, "failed to allocate memory in buffer.c");
                }

                ns = rb_funcall(value, rb_intern("namespace"), 0);
                write_element(rb_str_new2("$ref"), ns, pack_extra(buffer, Qfalse));
                oid = rb_funcall(value, rb_intern("object_id"), 0);
                write_element(rb_str_new2("$id"), oid, pack_extra(buffer, Qfalse));

                // write null byte and fill in length
                SAFE_WRITE(buffer, &zero, 1);
                obj_length = buffer_get_position(buffer) - start_position;
                SAFE_WRITE_AT_POS(buffer, length_location, (const char*)&obj_length, 4);
                break;
            }
            if (strcmp(cls, "Mongo::MaxKey") == 0) {
                write_name_and_type(buffer, key, 0x7f);
                break;
            }
            if (strcmp(cls, "Mongo::MinKey") == 0) {
                write_name_and_type(buffer, key, 0xff);
                break;
            }
            if (strcmp(cls, "DateTime") == 0 || strcmp(cls, "Date") == 0 || strcmp(cls, "ActiveSupport::TimeWithZone") == 0) {
                buffer_free(buffer);
                rb_raise(InvalidDocument, "%s is not currently supported; use a UTC Time instance instead.", cls);
                break;
            }
            if(strcmp(cls, "Complex") == 0 || strcmp(cls, "Rational") == 0 || strcmp(cls, "BigDecimal") == 0) {
                buffer_free(buffer);
                rb_raise(InvalidDocument, "Cannot serialize the Numeric type %s as BSON; only Bignum, Fixnum, and Float are supported.", cls);
                break;
            }
            buffer_free(buffer);
            rb_raise(InvalidDocument, "Cannot serialize an object of class %s into BSON.", cls);
            break;
        }
    case T_DATA:
        {
            const char* cls = rb_class2name(RBASIC(value)->klass);
            if (strcmp(cls, "Time") == 0) {
                double t = NUM2DBL(rb_funcall(value, rb_intern("to_f"), 0));
                long long time_since_epoch = (long long)round(t * 1000);
                write_name_and_type(buffer, key, 0x09);
                SAFE_WRITE(buffer, (const char*)&time_since_epoch, 8);
                break;
            }
            if(strcmp(cls, "BigDecimal") == 0) {
                buffer_free(buffer);
                rb_raise(InvalidDocument, "Cannot serialize the Numeric type %s as BSON; only Bignum, Fixnum, and Float are supported.", cls);
                break;
            }
            buffer_free(buffer);
            rb_raise(InvalidDocument, "Cannot serialize an object of class %s into BSON.", cls);
            break;
        }
    case T_REGEXP:
        {
            VALUE pattern = RREGEXP_SRC(value);
            long flags = RREGEXP(value)->ptr->options;
            VALUE has_extra;

            write_name_and_type(buffer, key, 0x0B);

            pattern = TO_UTF8(pattern);
            write_utf8(buffer, pattern, 1);
            SAFE_WRITE(buffer, &zero, 1);

            if (flags & IGNORECASE) {
                char ignorecase = 'i';
                SAFE_WRITE(buffer, &ignorecase, 1);
            }
            if (flags & MULTILINE) {
                char multiline = 'm';
                SAFE_WRITE(buffer, &multiline, 1);
            }
            if (flags & EXTENDED) {
                char extended = 'x';
                SAFE_WRITE(buffer, &extended, 1);
            }

            has_extra = rb_funcall(value, rb_intern("respond_to?"), 1, rb_str_new2("extra_options_str"));
            if (TYPE(has_extra) == T_TRUE) {
                VALUE extra = rb_funcall(value, rb_intern("extra_options_str"), 0);
                buffer_position old_position = buffer_get_position(buffer);
                SAFE_WRITE(buffer, RSTRING_PTR(extra), RSTRING_LEN(extra));
                qsort(buffer_get_buffer(buffer) + old_position, RSTRING_LEN(extra), sizeof(char), cmp_char);
            }
            SAFE_WRITE(buffer, &zero, 1);

            break;
        }
    default:
        {
            const char* cls = rb_class2name(RBASIC(value)->klass);
            buffer_free(buffer);
            rb_raise(InvalidDocument, "Cannot serialize an object of class %s (type %d) into BSON.", cls, TYPE(value));
            break;
        }
    }
    return ST_CONTINUE;
}

static int write_element(VALUE key, VALUE value, VALUE extra) {
    return write_element_allow_id(key, value, extra, 0);
}

static void write_doc(buffer_t buffer, VALUE hash, VALUE check_keys, VALUE move_id) {
    buffer_position start_position = buffer_get_position(buffer);
    buffer_position length_location = buffer_save_space(buffer, 4);
    buffer_position length;
    int allow_id;
    VALUE id_str = rb_str_new2("_id");
    VALUE id_sym = ID2SYM(rb_intern("_id"));

    if (length_location == -1) {
        rb_raise(rb_eNoMemError, "failed to allocate memory in buffer.c");
    }

    // write '_id' first if move_id is true
    if(move_id == Qtrue) {
        allow_id = 0;
        if (rb_funcall(hash, rb_intern("has_key?"), 1, id_str) == Qtrue) {
            VALUE id = rb_hash_aref(hash, id_str);
            write_element_allow_id(id_str, id, pack_extra(buffer, check_keys), 1);
        } else if (rb_funcall(hash, rb_intern("has_key?"), 1, id_sym) == Qtrue) {
            VALUE id = rb_hash_aref(hash, id_sym);
            write_element_allow_id(id_sym, id, pack_extra(buffer, check_keys), 1);
        }
    }
    else {
        allow_id = 1;
        if ((rb_funcall(hash, rb_intern("has_key?"), 1, id_str) == Qtrue) &&
               (rb_funcall(hash, rb_intern("has_key?"), 1, id_sym) == Qtrue)) {
                   VALUE obj = rb_hash_delete(hash, id_str);
        }
    }

    // we have to check for an OrderedHash and handle that specially
    if (strcmp(rb_class2name(RBASIC(hash)->klass), "OrderedHash") == 0) {
        VALUE keys = rb_funcall(hash, rb_intern("keys"), 0);
        int i;
        for(i = 0; i < RARRAY_LEN(keys); i++) {
            VALUE key = RARRAY_PTR(keys)[i];
            VALUE value = rb_hash_aref(hash, key);

            write_element_allow_id(key, value, pack_extra(buffer, check_keys), allow_id);
        }
    } else {
        rb_hash_foreach(hash, write_element_allow_id, pack_triple(buffer, check_keys, allow_id));
    }

    // write null byte and fill in length
    SAFE_WRITE(buffer, &zero, 1);
    length = buffer_get_position(buffer) - start_position;

    // make sure that length doesn't exceed 4MB
    if (length > 4 * 1024 * 1024) {
      buffer_free(buffer);
      rb_raise(InvalidDocument, "Document too large: BSON documents are limited to 4MB.");
      return;
    }
    SAFE_WRITE_AT_POS(buffer, length_location, (const char*)&length, 4);
}

static VALUE method_serialize(VALUE self, VALUE doc, VALUE check_keys, VALUE move_id) {
    VALUE result;
    buffer_t buffer = buffer_new();
    if (buffer == NULL) {
        rb_raise(rb_eNoMemError, "failed to allocate memory in buffer.c");
    }

    write_doc(buffer, doc, check_keys, move_id);

    result = rb_str_new(buffer_get_buffer(buffer), buffer_get_position(buffer));
    if (buffer_free(buffer) != 0) {
        rb_raise(rb_eRuntimeError, "failed to free buffer");
    }
    return result;
}

static VALUE get_value(const char* buffer, int* position, int type) {
    VALUE value;
    switch (type) {
    case -1:
        {
            value = rb_class_new_instance(0, NULL, MinKey);
            break;
        }
    case 1:
        {
            double d;
            memcpy(&d, buffer + *position, 8);
            value = rb_float_new(d);
            *position += 8;
            break;
        }
    case 2:
    case 13:
        {
            int value_length;
            value_length = *(int*)(buffer + *position) - 1;
            *position += 4;
            value = STR_NEW(buffer + *position, value_length);
            *position += value_length + 1;
            break;
        }
    case 3:
        {
            int size;
            memcpy(&size, buffer + *position, 4);
            if (strcmp(buffer + *position + 5, "$ref") == 0) { // DBRef
                int offset = *position + 10;
                VALUE argv[2];
                int collection_length = *(int*)(buffer + offset) - 1;
                char id_type;
                offset += 4;

                argv[0] = STR_NEW(buffer + offset, collection_length);
                offset += collection_length + 1;
                id_type = buffer[offset];
                offset += 5;
                argv[1] = get_value(buffer, &offset, (int)id_type);
                value = rb_class_new_instance(2, argv, DBRef);
            } else {
                value = elements_to_hash(buffer + *position + 4, size - 5);
            }
            *position += size;
            break;
        }
    case 4:
        {
            int size, end;
            memcpy(&size, buffer + *position, 4);
            end = *position + size - 1;
            *position += 4;

            value = rb_ary_new();
            while (*position < end) {
                int type = (int)buffer[(*position)++];
                int key_size = strlen(buffer + *position);
                VALUE to_append;

                *position += key_size + 1; // just skip the key, they're in order.
                to_append = get_value(buffer, position, type);
                rb_ary_push(value, to_append);
            }
            (*position)++;
            break;
        }
    case 5:
        {
            int length, subtype;
            VALUE data, st;
            VALUE argv[2];
            memcpy(&length, buffer + *position, 4);
            subtype = (unsigned char)buffer[*position + 4];
            if (subtype == 2) {
                data = rb_str_new(buffer + *position + 9, length - 4);
            } else {
                data = rb_str_new(buffer + *position + 5, length);
            }
            st = INT2FIX(subtype);
            argv[0] = data;
            argv[1] = st;
            value = rb_class_new_instance(2, argv, Binary);
            *position += length + 5;
            break;
        }
    case 6:
        {
            value = Qnil;
            break;
        }
    case 7:
        {
            VALUE str = rb_str_new(buffer + *position, 12);
            VALUE oid = rb_funcall(str, rb_intern("unpack"), 1, rb_str_new2("C*"));
            value = rb_class_new_instance(1, &oid, ObjectID);
            *position += 12;
            break;
        }
    case 8:
        {
            value = buffer[(*position)++] ? Qtrue : Qfalse;
            break;
        }
    case 9:
        {
            long long millis;
            VALUE seconds, microseconds;
            memcpy(&millis, buffer + *position, 8);
            seconds = LL2NUM(millis / 1000);
            microseconds = INT2NUM((millis % 1000) * 1000);

            value = rb_funcall(Time, rb_intern("at"), 2, seconds, microseconds);
            value = rb_funcall(value, rb_intern("utc"), 0);
            *position += 8;
            break;
        }
    case 10:
        {
            value = Qnil;
            break;
        }
    case 11:
        {
            int pattern_length = strlen(buffer + *position);
            VALUE pattern = STR_NEW(buffer + *position, pattern_length);
            int flags_length, flags = 0, i = 0;
            char extra[10];
            VALUE argv[3];
            *position += pattern_length + 1;

            flags_length = strlen(buffer + *position);
            extra[0] = 0;
            for (i = 0; i < flags_length; i++) {
                char flag = buffer[*position + i];
                if (flag == 'i') {
                    flags |= IGNORECASE;
                }
                else if (flag == 'm') {
                    flags |= MULTILINE;
                }
                else if (flag == 'x') {
                    flags |= EXTENDED;
                }
                else if (strlen(extra) < 9) {
                    strncat(extra, &flag, 1);
                }
            }
            argv[0] = pattern;
            argv[1] = INT2FIX(flags);
            if(extra[0] == 0) {
                value = rb_class_new_instance(2, argv, Regexp);
            }
            else { // Deserializing a RegexpOfHolding
                argv[2] = rb_str_new2(extra);
                value = rb_class_new_instance(3, argv, RegexpOfHolding);
            }
            *position += flags_length + 1;
            break;
        }
    case 12:
        {
            int collection_length;
            VALUE collection, str, oid, id, argv[2];
            collection_length = *(int*)(buffer + *position) - 1;
            *position += 4;
            collection = STR_NEW(buffer + *position, collection_length);
            *position += collection_length + 1;

            str = rb_str_new(buffer + *position, 12);
            oid = rb_funcall(str, rb_intern("unpack"), 1, rb_str_new2("C*"));
            id = rb_class_new_instance(1, &oid, ObjectID);
            *position += 12;

            argv[0] = collection;
            argv[1] = id;
            value = rb_class_new_instance(2, argv, DBRef);
            break;
        }
    case 14:
        {
            int value_length;
            memcpy(&value_length, buffer + *position, 4);
            value = ID2SYM(rb_intern(buffer + *position + 4));
            *position += value_length + 4;
            break;
        }
    case 15:
        {
            int code_length, scope_size;
            VALUE code, scope, argv[2];
            *position += 4;
            code_length = *(int*)(buffer + *position) - 1;
            *position += 4;
            code = STR_NEW(buffer + *position, code_length);
            *position += code_length + 1;

            memcpy(&scope_size, buffer + *position, 4);
            scope = elements_to_hash(buffer + *position + 4, scope_size - 5);
            *position += scope_size;

            argv[0] = code;
            argv[1] = scope;
            value = rb_class_new_instance(2, argv, Code);
            break;
        }
    case 16:
        {
            int i;
            memcpy(&i, buffer + *position, 4);
            value = LL2NUM(i);
            *position += 4;
            break;
        }
    case 17:
        {
            int i;
            int j;
            memcpy(&i, buffer + *position, 4);
            memcpy(&j, buffer + *position + 4, 4);
            value = rb_ary_new3(2, LL2NUM(i), LL2NUM(j));
            *position += 8;
            break;
        }
    case 18:
        {
            long long ll;
            memcpy(&ll, buffer + *position, 8);
            value = LL2NUM(ll);
            *position += 8;
            break;
        }
    case 127:
        {
            value = rb_class_new_instance(0, NULL, MaxKey);
            break;
        }
    default:
        {
            rb_raise(rb_eTypeError, "no c decoder for this type yet (%d)", type);
            break;
        }
    }
    return value;
}

static VALUE elements_to_hash(const char* buffer, int max) {
    VALUE hash = rb_class_new_instance(0, NULL, OrderedHash);
    int position = 0;
    while (position < max) {
        int type = (int)buffer[position++];
        int name_length = strlen(buffer + position);
        VALUE name = STR_NEW(buffer + position, name_length);
        VALUE value;
        position += name_length + 1;
        value = get_value(buffer, &position, type);
        rb_funcall(hash, rb_intern("[]="), 2, name, value);
    }
    return hash;
}

static VALUE method_deserialize(VALUE self, VALUE bson) {
    const char* buffer = RSTRING_PTR(bson);
    int remaining = RSTRING_LEN(bson);

    // NOTE we just swallow the size and end byte here
    buffer += 4;
    remaining -= 5;

    return elements_to_hash(buffer, remaining);
}


static VALUE fast_pack(VALUE self)
{
    VALUE res;
    long i;
    char c;

    res = rb_str_buf_new(0);

    for (i = 0; i < RARRAY_LEN(self); i++) {
        c = FIX2LONG(RARRAY_PTR(self)[i]);
        rb_str_buf_cat(res, &c, sizeof(char));
    }

    return res;
}


static VALUE objectid_generate(VALUE self)
{
    VALUE oid, digest;
    char hostname[MAX_HOSTNAME_LENGTH];
    unsigned char oid_bytes[12];
    unsigned long t, inc;
    unsigned short pid;
    int i;

    t = htonl(time(NULL));
    MEMCPY(&oid_bytes, &t, unsigned char, 4);

    if (gethostname(hostname, MAX_HOSTNAME_LENGTH) != 0) {
        rb_raise(rb_eRuntimeError, "failed to get hostname");
    }
    digest = rb_funcall(DigestMD5, rb_intern("digest"), 1, rb_str_new2(hostname));
    MEMCPY(&oid_bytes[4], RSTRING_PTR(digest), unsigned char, 3);

    pid = htons(getpid());
    MEMCPY(&oid_bytes[7], &pid, unsigned char, 2);

    inc = htonl(FIX2ULONG(rb_funcall(self, rb_intern("get_inc"), 0)));
    MEMCPY(&oid_bytes[9], ((unsigned char*)&inc + 1), unsigned char, 3);

    oid = rb_ary_new2(12);
    for(i = 0; i < 12; i++) {
        rb_ary_store(oid, i, INT2FIX((unsigned int)oid_bytes[i]));
    }
    return oid;
}


void Init_cbson() {
    VALUE mongo, CBson, Digest, ext_version;
    Time = rb_const_get(rb_cObject, rb_intern("Time"));

    mongo = rb_const_get(rb_cObject, rb_intern("Mongo"));
    rb_require("mongo/types/binary");
    Binary = rb_const_get(mongo, rb_intern("Binary"));
    rb_require("mongo/types/objectid");
    ObjectID = rb_const_get(mongo, rb_intern("ObjectID"));
    rb_require("mongo/types/dbref");
    DBRef = rb_const_get(mongo, rb_intern("DBRef"));
    rb_require("mongo/types/code");
    Code = rb_const_get(mongo, rb_intern("Code"));
    rb_require("mongo/types/min_max_keys");
    MinKey = rb_const_get(mongo, rb_intern("MinKey"));
    MaxKey = rb_const_get(mongo, rb_intern("MaxKey"));
    rb_require("mongo/types/regexp_of_holding");
    Regexp = rb_const_get(rb_cObject, rb_intern("Regexp"));
    RegexpOfHolding = rb_const_get(mongo, rb_intern("RegexpOfHolding"));
    rb_require("mongo/exceptions");
    InvalidName = rb_const_get(mongo, rb_intern("InvalidName"));
    InvalidStringEncoding = rb_const_get(mongo, rb_intern("InvalidStringEncoding"));
    InvalidDocument = rb_const_get(mongo, rb_intern("InvalidDocument"));
    rb_require("mongo/util/ordered_hash");
    OrderedHash = rb_const_get(rb_cObject, rb_intern("OrderedHash"));

    CBson = rb_define_module("CBson");
    ext_version = rb_str_new2(VERSION);
    rb_define_const(CBson, "VERSION", ext_version);
    rb_define_module_function(CBson, "serialize", method_serialize, 3);
    rb_define_module_function(CBson, "deserialize", method_deserialize, 1);

    rb_require("digest/md5");
    Digest = rb_const_get(rb_cObject, rb_intern("Digest"));
    DigestMD5 = rb_const_get(Digest, rb_intern("MD5"));

    rb_define_method(ObjectID, "generate", objectid_generate, 0);

    rb_define_method(rb_cArray, "fast_pack", fast_pack, 0);
}
