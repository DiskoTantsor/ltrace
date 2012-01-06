/*
 * This file is part of ltrace.
 * Copyright (C) 2011,2012 Petr Machata, Red Hat Inc.
 * Copyright (C) 1997-2009 Juan Cespedes
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 */

#ifndef TYPE_H
#define TYPE_H

#include <stddef.h>
#include "forward.h"
#include "vect.h"

enum arg_type {
	ARGTYPE_UNKNOWN = -1,
	ARGTYPE_VOID,
	ARGTYPE_INT,
	ARGTYPE_UINT,
	ARGTYPE_LONG,
	ARGTYPE_ULONG,
	ARGTYPE_OCTAL,
	ARGTYPE_CHAR,
	ARGTYPE_SHORT,
	ARGTYPE_USHORT,
	ARGTYPE_FLOAT,		/* float value, may require index */
	ARGTYPE_DOUBLE,		/* double value, may require index */
	ARGTYPE_ADDR,
	ARGTYPE_FILE,
	ARGTYPE_FORMAT,		/* printf-like format */
	ARGTYPE_STRING,		/* NUL-terminated string */
	ARGTYPE_STRING_N,	/* String of known maxlen */
	ARGTYPE_ARRAY,		/* Series of values in memory */
	ARGTYPE_ENUM,		/* Enumeration */
	ARGTYPE_STRUCT,		/* Structure of values */
	ARGTYPE_POINTER,	/* Pointer to some other type */
	ARGTYPE_COUNT		/* number of ARGTYPE_* values */
};

struct arg_type_info {
	enum arg_type type;
	union {
		struct vect entries;

		/* ARGTYPE_ENUM */
		struct {
			size_t entries;
			char **keys;
			int *values;
		} enum_info;

		/* ARGTYPE_ARRAY */
		struct {
			struct arg_type_info *elt_type;
			struct expr_node *length;
			int own_info:1;
			int own_length:1;
		} array_info;

		/* ARGTYPE_STRING_N */
		struct {
			struct expr_node *length;
			int own_length:1;
		} string_n_info;

		/* ARGTYPE_POINTER */
		struct {
			struct arg_type_info *info;
			int own_info:1;
		} ptr_info;

		/* ARGTYPE_FLOAT */
		struct {
			size_t float_index;
		} float_info;

		/* ARGTYPE_DOUBLE */
		struct {
			size_t float_index;
		} double_info;
	} u;
};

/* Return a type info for simple type TYPE (which shall not be array,
 * struct, enum or pointer.  Each call with the same TYPE yields the
 * same arg_type_info pointer.  */
struct arg_type_info *type_get_simple(enum arg_type type);

/* Initialize INFO so it becomes ARGTYPE_ENUM.  Returns 0 on success
 * or negative value on failure.  */
void type_init_enum(struct arg_type_info *info);

/* Push another member of the enumeration, named KEY, with given
 * VALUE.  If OWN_KEY, KEY is owned and released after the type is
 * destroyed.  KEY is typed as const char *, but note that if
 * OWN_KEY, this value will be freed.  */
int type_enum_add(struct arg_type_info *info,
		  const char *key, int own_key, int value);

/* Return number of enum elements of type INFO.  */
size_t type_enum_size(struct arg_type_info *info);

/* Look up enum key with given VALUE in INFO.  */
const char *type_enum_get(struct arg_type_info *info, int value);

/* Initialize INFO so it becomes ARGTYPE_STRUCT.  The created
 * structure contains no fields.  Use type_struct_add to populate the
 * structure.  */
void type_init_struct(struct arg_type_info *info);

/* Add a new field of type FIELD_INFO to a structure INFO.  If OWN,
 * the field type is owned and destroyed together with INFO.  */
int type_struct_add(struct arg_type_info *info,
		    struct arg_type_info *field_info, int own);

/* Get IDX-th field of structure type INFO.  */
struct arg_type_info *type_struct_get(struct arg_type_info *info, size_t idx);

/* Return number of fields of structure type INFO.  */
size_t type_struct_size(struct arg_type_info *info);

/* Initialize INFO so it becomes ARGTYPE_ARRAY.  The element type is
 * passed in ELEMENT_INFO, and array length in LENGTH_EXPR.  If,
 * respectively, OWN_INFO and OWN_LENGTH are true, the pointee and
 * length are owned and destroyed together with INFO.  */
void type_init_array(struct arg_type_info *info,
		     struct arg_type_info *element_info, int own_info,
		     struct expr_node *length, int own_length);

/* Initialize INFO so it becomes ARGTYPE_POINTER.  The pointee type is
 * passed in POINTEE_INFO.  If OWN_INFO, the pointee type is owned and
 * destroyed together with INFO.  */
void type_init_pointer(struct arg_type_info *info,
		       struct arg_type_info *pointee_info, int own_info);

/* Release any memory associated with INFO.  Doesn't free INFO
 * itself.  */
void type_destroy(struct arg_type_info *info);

/* Compute a size of given type.  Return (size_t)-1 for error.  */
size_t type_sizeof(struct Process *proc, struct arg_type_info *type);

/* Compute an alignment necessary for elements of this type.  Return
 * (size_t)-1 for error.  */
size_t type_alignof(struct Process *proc, struct arg_type_info *type);

/* Align value SZ to ALIGNMENT and return the result.  */
size_t align(size_t sz, size_t alignment);

/* Return ELT-th element of compound type TYPE.  This is useful for
 * arrays and structures.  */
struct arg_type_info *type_element(struct arg_type_info *type, size_t elt);

/* Compute an offset of EMT-th element of type TYPE.  This works for
 * arrays and structures.  Return (size_t)-1 for error.  */
size_t type_offsetof(struct Process *proc,
		     struct arg_type_info *type, size_t elt);

struct arg_type_info *lookup_prototype(enum arg_type at);

#endif /* TYPE_H */