/* SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: 2026 Daniel Gardiner d/b/a FractalSQLabs
 *
 * fsql_udf_types.h -- the small calling convention the daemon's function bodies use.
 *
 * Every function body takes an init record, an argument record and a result buffer, and reports
 * errors into a fixed-size message buffer. The daemon builds those records itself from the
 * wire-protocol frame and calls the bodies directly, so the definitions only have to agree with
 * each other inside this one binary. They are declared here, locally, so the build needs no
 * external database headers or development packages.
 */
#ifndef FSQL_UDF_TYPES_H
#define FSQL_UDF_TYPES_H

/* Argument and result kinds. */
enum Item_result {
    STRING_RESULT  = 0,
    REAL_RESULT    = 1,
    INT_RESULT     = 2,
    ROW_RESULT     = 3,
    DECIMAL_RESULT = 4
};

/* The argument record handed to a function body. */
typedef struct st_udf_args {
    unsigned int       arg_count;          /* number of arguments */
    enum Item_result  *arg_type;           /* per-argument kind */
    char             **args;               /* per-argument value (NULL when the argument is NULL) */
    unsigned long     *lengths;            /* per-argument byte length */
    char              *maybe_null;         /* per-argument: may it be NULL */
    char             **attributes;         /* per-argument name */
    unsigned long     *attribute_lengths;  /* per-argument name length */
    void              *extension;
} UDF_ARGS;

/* The init record: what a function body declares about its own result. */
typedef struct st_udf_init {
    char          maybe_null;   /* 1 if the function can return NULL */
    unsigned int  decimals;     /* digits after the decimal point for a REAL result */
    unsigned long max_length;   /* maximum result length */
    char         *ptr;          /* scratch pointer the function owns between calls */
    char          const_item;   /* 1 if the function always returns the same value */
    void         *extension;
} UDF_INIT;

/* Size of the error-message buffer every function body writes into. */
#define FSQL_ERRMSG_SIZE 512

#endif /* FSQL_UDF_TYPES_H */
