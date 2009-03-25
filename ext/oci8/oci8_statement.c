/*
   +----------------------------------------------------------------------+
   | PHP Version 5                                                        |
   +----------------------------------------------------------------------+
   | Copyright (c) 1997-2006 The PHP Group                                |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
   | Authors: Stig S�ther Bakken <ssb@php.net>                            |
   |          Thies C. Arntzen <thies@thieso.net>                         |
   |                                                                      |
   | Collection support by Andy Sautins <asautins@veripost.net>           |
   | Temporary LOB support by David Benson <dbenson@mancala.com>          |
   | ZTS per process OCIPLogon by Harald Radi <harald.radi@nme.at>        |
   |                                                                      |
   | Redesigned by: Antony Dovgal <antony@zend.com>                       |
   |                Andi Gutmans <andi@zend.com>                          |
   |                Wez Furlong <wez@omniti.com>                          |
   +----------------------------------------------------------------------+
*/

/* $Id: oci8_statement.c,v 1.7.2.8 2006/01/01 12:50:10 sniper Exp $ */


#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "ext/standard/info.h"
#include "php_ini.h"

#if HAVE_OCI8

#include "php_oci8.h"
#include "php_oci8_int.h"

/* {{{ php_oci_statement_create() 
 Create statemend handle and allocate necessary resources */
php_oci_statement *php_oci_statement_create (php_oci_connection *connection, char *query, long query_len, zend_bool cached TSRMLS_DC)
{
	php_oci_statement *statement;
	
	statement = ecalloc(1,sizeof(php_oci_statement));

#if HAVE_OCI_STMT_PREPARE2
	if (!query_len) {
		/* do not allocate stmt handle for refcursors, we'll get it from OCIStmtPrepare2() */
		PHP_OCI_CALL(OCIHandleAlloc, (connection->env, (dvoid **)&(statement->stmt), OCI_HTYPE_STMT, 0, NULL));
	}
#else
	PHP_OCI_CALL(OCIHandleAlloc, (connection->env, (dvoid **)&(statement->stmt), OCI_HTYPE_STMT, 0, NULL));
#endif
			
	PHP_OCI_CALL(OCIHandleAlloc, (connection->env, (dvoid **)&(statement->err), OCI_HTYPE_ERROR, 0, NULL));
	
	if (query_len > 0) {
#if HAVE_OCI_STMT_PREPARE2
		connection->errcode = PHP_OCI_CALL(OCIStmtPrepare2, (connection->svc, &(statement->stmt), connection->err, (text *)query, query_len, NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT));
#else
		connection->errcode = PHP_OCI_CALL(OCIStmtPrepare, (statement->stmt, connection->err, (text *)query, query_len, OCI_NTV_SYNTAX, OCI_DEFAULT));
#endif		
		if (connection->errcode != OCI_SUCCESS) {
			php_oci_error(connection->err, connection->errcode TSRMLS_CC);

#if HAVE_OCI_STMT_PREPARE2
			PHP_OCI_CALL(OCIStmtRelease, (statement->stmt, statement->err, NULL, 0, OCI_DEFAULT));
			PHP_OCI_CALL(OCIHandleFree,(statement->err, OCI_HTYPE_ERROR));
#else
			PHP_OCI_CALL(OCIHandleFree,(statement->stmt, OCI_HTYPE_STMT));
			PHP_OCI_CALL(OCIHandleFree,(statement->err, OCI_HTYPE_ERROR));
#endif
			
			efree(statement);
			PHP_OCI_HANDLE_ERROR(connection, connection->errcode);
			return NULL;
		}
	}
	
	if (query && query_len) {
		statement->last_query = estrndup(query, query_len);
		statement->last_query_len = query_len;
	}
	else {
		statement->last_query = NULL;
		statement->last_query_len = 0;
	}

	statement->connection = connection;
	statement->has_data = 0;

	if (OCI_G(default_prefetch) > 0) {
		php_oci_statement_set_prefetch(statement, OCI_G(default_prefetch) TSRMLS_CC);
	}
	
	PHP_OCI_REGISTER_RESOURCE(statement, le_statement);
	
	return statement;
}
/* }}} */

/* {{{ php_oci_statement_set_prefetch()
 Set prefetch buffer size for the statement (we're assuming that one row is ~1K sized) */
int php_oci_statement_set_prefetch(php_oci_statement *statement, ub4 size TSRMLS_DC)
{ 
	ub4 prefetch = size * 1024;

	if (size < 1) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Number of rows has to be greater than or equal to 1");
		return 1;
	}
	
	statement->errcode = PHP_OCI_CALL (OCIAttrSet, (statement->stmt, OCI_HTYPE_STMT, &prefetch, 0, OCI_ATTR_PREFETCH_MEMORY, statement->err));

	if (statement->errcode != OCI_SUCCESS) {
		php_oci_error(statement->err, statement->errcode TSRMLS_CC);
		PHP_OCI_HANDLE_ERROR(statement->connection, statement->errcode);
		return 1;
	}

	prefetch = size;
	statement->errcode = PHP_OCI_CALL (OCIAttrSet, (statement->stmt, OCI_HTYPE_STMT, &prefetch, 0, OCI_ATTR_PREFETCH_ROWS, statement->err));
	
	if (statement->errcode != OCI_SUCCESS) {
		php_oci_error(statement->err, statement->errcode TSRMLS_CC);
		PHP_OCI_HANDLE_ERROR(statement->connection, statement->errcode);
		return 1;
	}

	return 0;
}
/* }}} */

/* {{{ php_oci_statement_fetch() 
 Fetch a row from the statement */
int php_oci_statement_fetch(php_oci_statement *statement, ub4 nrows TSRMLS_DC)
{
	int i;
	php_oci_out_column *column;

	statement->errcode = PHP_OCI_CALL(OCIStmtFetch, (statement->stmt, statement->err, nrows, OCI_FETCH_NEXT, OCI_DEFAULT));

	if ( statement->errcode == OCI_NO_DATA || nrows == 0 ) {
		if (statement->last_query == NULL) {
			/* reset define-list for refcursors */
			if (statement->columns) {
				zend_hash_destroy(statement->columns);
				efree(statement->columns);
				statement->columns = NULL;
				statement->ncolumns = 0;
			}
			statement->executed = 0;
		}

		statement->errcode = 0; /* OCI_NO_DATA is NO error for us!!! */
		statement->has_data = 0;

		if (nrows == 0) {
			/* this is exactly what we requested */
			return 0;
		}
		
		return 1;
	}

	/* reset length for all piecewise columns */
	for (i = 0; i < statement->ncolumns; i++) {
		column = php_oci_statement_get_column(statement, i + 1, NULL, 0 TSRMLS_CC);
		if (column->piecewise) {
			column->retlen4 = 0;
		}
	}
	
	while (statement->errcode == OCI_NEED_DATA) {
		for (i = 0; i < statement->ncolumns; i++) {
			column = php_oci_statement_get_column(statement, i + 1, NULL, 0 TSRMLS_CC);
			if (column->piecewise)	{
				if (!column->data) {
					column->data = (text *) emalloc(PHP_OCI_PIECE_SIZE);
				} else {
					column->data = erealloc(column->data, column->retlen4 + PHP_OCI_PIECE_SIZE);
				}

				column->cb_retlen = PHP_OCI_PIECE_SIZE;

				PHP_OCI_CALL( OCIStmtSetPieceInfo, ((void *) column->oci_define, OCI_HTYPE_DEFINE, statement->err, ((char*)column->data) + column->retlen4, &(column->cb_retlen), OCI_NEXT_PIECE, &column->indicator, &column->retcode));
			}
		}

		statement->errcode = PHP_OCI_CALL( OCIStmtFetch, (statement->stmt, statement->err, nrows, OCI_FETCH_NEXT, OCI_DEFAULT));

		for (i = 0; i < statement->ncolumns; i++) {
			column = php_oci_statement_get_column(statement, i + 1, NULL, 0 TSRMLS_CC);
			if (column && column->piecewise)	{
				column->retlen4 += column->cb_retlen;
			}
		}
	}

	if (statement->errcode == OCI_SUCCESS_WITH_INFO || statement->errcode == OCI_SUCCESS) {
		statement->has_data = 1;

		/* do the stuff needed for OCIDefineByName */
		for (i = 0; i < statement->ncolumns; i++) {
			column = php_oci_statement_get_column(statement, i + 1, NULL, 0 TSRMLS_CC);
			if (column == NULL) {
				continue;
			}
			
			if (!column->define) {
				continue;
			}
			
			zval_dtor(column->define->zval);
			php_oci_column_to_zval(column, column->define->zval, 0 TSRMLS_CC);
		}

		return 0;
	}

	php_oci_error(statement->err, statement->errcode TSRMLS_CC);
	PHP_OCI_HANDLE_ERROR(statement->connection, statement->errcode);

	statement->has_data = 0;

	return 1;
}
/* }}} */

/* {{{ php_oci_statement_get_column() 
 Get column from the result set */
php_oci_out_column *php_oci_statement_get_column(php_oci_statement *statement, long column_index, char *column_name, long column_name_len TSRMLS_DC)
{
	php_oci_out_column *column = NULL;
	int i;

	if (statement->columns == NULL) { /* we release the columns at the end of a fetch */
		return NULL;
	}

	if (column_name) {
		for (i = 0; i < statement->ncolumns; i++) {
			column = php_oci_statement_get_column(statement, i + 1, NULL, 0 TSRMLS_CC);
			if (column == NULL) {
				continue;
			} else if (((int) column->name_len == column_name_len) && (!strncmp(column->name, column_name, column_name_len))) {
				return column;
			}
		}
	} else if (column_index != -1) {
		if (zend_hash_index_find(statement->columns, column_index, (void **)&column) == FAILURE) {
			return NULL;
		}
		return column;
	}

	return NULL;
}
/* }}} */

/* {{{ php_oci_statement_execute() 
 Execute statement */
int php_oci_statement_execute(php_oci_statement *statement, ub4 mode TSRMLS_DC)
{
	php_oci_out_column *outcol;
	php_oci_out_column column;
	OCIParam *param = NULL;
	text *colname;
	ub4 counter;
	ub2 define_type;
	ub4 iters;
	ub4 colcount;
	ub2 dynamic;
	int dtype;
	dvoid *buf;
	php_oci_descriptor *descr;

	switch (mode) {
		case OCI_COMMIT_ON_SUCCESS:
		case OCI_DEFAULT:
			/* only these two are allowed */
			break;
		default:
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Invalid execute mode given: %d", mode);
			return 1;
			break;
	}
	
	if (!statement->stmttype) {
		/* get statement type */
		statement->errcode = PHP_OCI_CALL(OCIAttrGet, ((dvoid *)statement->stmt, OCI_HTYPE_STMT, (ub2 *)&statement->stmttype, (ub4 *)0,	OCI_ATTR_STMT_TYPE,	statement->err));

		if (statement->errcode != OCI_SUCCESS) {
			php_oci_error(statement->err, statement->errcode TSRMLS_CC);
			PHP_OCI_HANDLE_ERROR(statement->connection, statement->errcode);
			return 1;
		}
	}

	if (statement->stmttype == OCI_STMT_SELECT) {
		iters = 0;
	} else {
		iters = 1;
	}
	
	if (statement->last_query) { 
		/* if we execute refcursors we don't have a query and 
		   we don't want to execute!!! */

		if (statement->binds) {
			zend_hash_apply(statement->binds, (apply_func_t) php_oci_bind_pre_exec TSRMLS_CC);
		}

		/* execute statement */
		statement->errcode = PHP_OCI_CALL(OCIStmtExecute, (statement->connection->svc,	statement->stmt, statement->err, iters,	0, NULL, NULL, mode));

		if (statement->errcode != OCI_SUCCESS) {
			php_oci_error(statement->err, statement->errcode TSRMLS_CC);
			PHP_OCI_HANDLE_ERROR(statement->connection, statement->errcode);
			return 1;
		}
		
		if (statement->binds) {
			zend_hash_apply(statement->binds, (apply_func_t) php_oci_bind_post_exec TSRMLS_CC);
		}

		if (mode & OCI_COMMIT_ON_SUCCESS) {
			statement->connection->needs_commit = 0;
		} else {
			statement->connection->needs_commit = 1;
		}
	}

	if (statement->stmttype == OCI_STMT_SELECT && statement->executed == 0) {
		/* we only need to do the define step is this very statement is executed the first time! */
		statement->executed = 1;
		
		ALLOC_HASHTABLE(statement->columns);
		zend_hash_init(statement->columns, 13, NULL, php_oci_column_hash_dtor, 0);
		
		counter = 1;

		/* get number of columns */
		statement->errcode = PHP_OCI_CALL(OCIAttrGet, ((dvoid *)statement->stmt, OCI_HTYPE_STMT, (dvoid *)&colcount, (ub4 *)0, OCI_ATTR_PARAM_COUNT, statement->err));
		
		if (statement->errcode != OCI_SUCCESS) {
			php_oci_error(statement->err, statement->errcode TSRMLS_CC);
			PHP_OCI_HANDLE_ERROR(statement->connection, statement->errcode);
			return 1; 
		}

		statement->ncolumns = colcount;
		
		for (counter = 1; counter <= colcount; counter++) {
			memset(&column,0,sizeof(php_oci_out_column));
			
			if (zend_hash_index_update(statement->columns, counter, &column, sizeof(php_oci_out_column), (void**) &outcol) == FAILURE) {
				efree(statement->columns);
				/* out of memory */
				return 1;
			} 
			
			outcol->statement = statement;

			/* get column */
			statement->errcode = PHP_OCI_CALL(OCIParamGet, ((dvoid *)statement->stmt, OCI_HTYPE_STMT, statement->err, (dvoid**)&param, counter));
			
			if (statement->errcode != OCI_SUCCESS) {
				php_oci_error(statement->err, statement->errcode TSRMLS_CC);
				PHP_OCI_HANDLE_ERROR(statement->connection, statement->errcode);
				return 1;
			}

			/* get column datatype */
			statement->errcode = PHP_OCI_CALL(OCIAttrGet, ((dvoid *)param, OCI_DTYPE_PARAM, (dvoid *)&outcol->data_type, (ub4 *)0, OCI_ATTR_DATA_TYPE, statement->err));

			if (statement->errcode != OCI_SUCCESS) {
				PHP_OCI_CALL(OCIDescriptorFree, (param, OCI_DTYPE_PARAM));
				php_oci_error(statement->err, statement->errcode TSRMLS_CC);
				PHP_OCI_HANDLE_ERROR(statement->connection, statement->errcode);
				return 1;
			}

			/* get size of the column */
			statement->errcode = PHP_OCI_CALL(OCIAttrGet, ((dvoid *)param, OCI_DTYPE_PARAM, (dvoid *)&outcol->data_size, (dvoid *)0, OCI_ATTR_DATA_SIZE, statement->err));
			
			if (statement->errcode != OCI_SUCCESS) {
				PHP_OCI_CALL(OCIDescriptorFree, (param, OCI_DTYPE_PARAM));
				php_oci_error(statement->err, statement->errcode TSRMLS_CC);
				PHP_OCI_HANDLE_ERROR(statement->connection, statement->errcode);
				return 1; 
			}

			outcol->storage_size4 = outcol->data_size;
			outcol->retlen = outcol->data_size;

			/* get scale of the column */
			statement->errcode = PHP_OCI_CALL(OCIAttrGet, ((dvoid *)param, OCI_DTYPE_PARAM, (dvoid *)&outcol->scale, (dvoid *)0, OCI_ATTR_SCALE, statement->err));
			
			if (statement->errcode != OCI_SUCCESS) {
				PHP_OCI_CALL(OCIDescriptorFree, (param, OCI_DTYPE_PARAM));
				php_oci_error(statement->err, statement->errcode TSRMLS_CC);
				PHP_OCI_HANDLE_ERROR(statement->connection, statement->errcode);
				return 1;
			}

			/* get precision of the column */
			statement->errcode = PHP_OCI_CALL(OCIAttrGet, ((dvoid *)param, OCI_DTYPE_PARAM, (dvoid *)&outcol->precision, (dvoid *)0, OCI_ATTR_PRECISION, statement->err));
			
			if (statement->errcode != OCI_SUCCESS) {
				PHP_OCI_CALL(OCIDescriptorFree, (param, OCI_DTYPE_PARAM));
				php_oci_error(statement->err, statement->errcode TSRMLS_CC);
				PHP_OCI_HANDLE_ERROR(statement->connection, statement->errcode);
				return 1;
			}
			
			/* get name of the column */
			statement->errcode = PHP_OCI_CALL(OCIAttrGet, ((dvoid *)param, OCI_DTYPE_PARAM, (dvoid **)&colname, (ub4 *)&outcol->name_len, (ub4)OCI_ATTR_NAME, statement->err));
			
			if (statement->errcode != OCI_SUCCESS) {
				PHP_OCI_CALL(OCIDescriptorFree, (param, OCI_DTYPE_PARAM));
				php_oci_error(statement->err, statement->errcode TSRMLS_CC);
				PHP_OCI_HANDLE_ERROR(statement->connection, statement->errcode);
				return 1;
			}
			PHP_OCI_CALL(OCIDescriptorFree, (param, OCI_DTYPE_PARAM));

			outcol->name = estrndup((char*) colname, outcol->name_len);

			/* find a user-setted define */
			if (statement->defines) {
				zend_hash_find(statement->defines,outcol->name,outcol->name_len,(void **) &outcol->define);
			}

			buf = 0;
			switch (outcol->data_type) {
				case SQLT_RSET:
					outcol->statement = php_oci_statement_create(statement->connection, NULL, 0, 0 TSRMLS_CC);
					outcol->stmtid = outcol->statement->id;

					define_type = SQLT_RSET;
					outcol->is_cursor = 1;
					outcol->storage_size4 = -1;
					outcol->retlen = -1;
					dynamic = OCI_DEFAULT;
					buf = &(outcol->statement->stmt);
					break;

			 	case SQLT_RDD:	 /* ROWID */
				case SQLT_BLOB:  /* binary LOB */
				case SQLT_CLOB:  /* character LOB */
				case SQLT_BFILE: /* binary file LOB */
					define_type = outcol->data_type;
					outcol->is_descr = 1;
					outcol->storage_size4 = -1;
					dynamic = OCI_DEFAULT;

					if (outcol->data_type == SQLT_BFILE) {
						dtype = OCI_DTYPE_FILE;
					} else if (outcol->data_type == SQLT_RDD ) {
						dtype = OCI_DTYPE_ROWID;
					} else {
						dtype = OCI_DTYPE_LOB;
					}
					
					descr = php_oci_lob_create(statement->connection, dtype TSRMLS_CC);
					if (!descr) {
						efree(outcol->name);
						return 1;
					}
					outcol->descid = descr->id;
					buf = &(descr->descriptor);
					break;

				case SQLT_LNG:
				case SQLT_LBI:
					if (outcol->data_type == SQLT_LBI) {
						define_type = SQLT_BIN;
					} else {
						define_type = SQLT_CHR;
					}
					outcol->storage_size4 = PHP_OCI_MAX_DATA_SIZE;
					outcol->piecewise = 1;
					dynamic = OCI_DYNAMIC_FETCH;
					break;

				case SQLT_BIN:
				default:
					define_type = SQLT_CHR;
					if ((outcol->data_type == SQLT_DAT) || (outcol->data_type == SQLT_NUM)
#ifdef SQLT_TIMESTAMP
						|| (outcol->data_type == SQLT_TIMESTAMP)
#endif
#ifdef SQLT_TIMESTAMP_TZ
						|| (outcol->data_type == SQLT_TIMESTAMP_TZ)
#endif
						) {
						outcol->storage_size4 = 512; /* XXX this should fit "most" NLS date-formats and Numbers */
					} else {
						outcol->storage_size4++; /* add one for string terminator */
					}
					
					outcol->storage_size4 *= 3;
					
					dynamic = OCI_DEFAULT;
					buf = outcol->data = (text *) emalloc(outcol->storage_size4);
					break;
			}

			if (dynamic == OCI_DYNAMIC_FETCH) {
				statement->errcode = PHP_OCI_CALL(
					OCIDefineByPos,
					(
						statement->stmt,                           /* IN/OUT handle to the requested SQL query */
						(OCIDefine **)&outcol->oci_define,             /* IN/OUT pointer to a pointer to a define handle */
						statement->err,                          /* IN/OUT An error handle  */
						counter,                                    /* IN     position in the select list */
						(dvoid *)NULL,                              /* IN/OUT pointer to a buffer */
						outcol->storage_size4,                      /* IN     The size of each valuep buffer in bytes */
						define_type,                                /* IN     The data type */
						(dvoid *)&outcol->indicator,                /* IN     pointer to an indicator variable or arr */
						(ub2 *)NULL,                                /* IN/OUT Pointer to array of length of data fetched */
						(ub2 *)NULL,                                /* OUT    Pointer to array of column-level return codes */
						OCI_DYNAMIC_FETCH	                        /* IN     mode (OCI_DEFAULT, OCI_DYNAMIC_FETCH) */
					)
				);

			} else {
				statement->errcode = PHP_OCI_CALL(
					OCIDefineByPos,
					(
						statement->stmt,                           /* IN/OUT handle to the requested SQL query */ 
						(OCIDefine **)&outcol->oci_define,             /* IN/OUT pointer to a pointer to a define handle */
						statement->err,                          /* IN/OUT An error handle  */
						counter,                                    /* IN     position in the select list */
						(dvoid *)buf,                               /* IN/OUT pointer to a buffer */
						outcol->storage_size4,                      /* IN     The size of each valuep buffer in bytes */
						define_type,                                /* IN     The data type */
						(dvoid *)&outcol->indicator,                /* IN     pointer to an indicator variable or arr */
						(ub2 *)&outcol->retlen,                     /* IN/OUT Pointer to array of length of data fetched */
						(ub2 *)&outcol->retcode,                    /* OUT    Pointer to array of column-level return codes */
						OCI_DEFAULT	                                /* IN     mode (OCI_DEFAULT, OCI_DYNAMIC_FETCH) */
					)
				);

			}
			
			if (statement->errcode != OCI_SUCCESS) {
				php_oci_error(statement->err, statement->errcode TSRMLS_CC);
				PHP_OCI_HANDLE_ERROR(statement->connection, statement->errcode);
				return 0;
			}
		}
	}

	return 0;
}
/* }}} */

/* {{{ php_oci_statement_cancel() 
 Cancel statement */
int php_oci_statement_cancel(php_oci_statement *statement TSRMLS_DC)
{
	
	return php_oci_statement_fetch(statement, 0 TSRMLS_CC);
		
} /* }}} */

/* {{{ php_oci_statement_free() 
 Destroy statement handle and free associated resources */
void php_oci_statement_free(php_oci_statement *statement TSRMLS_DC)
{
 	if (statement->stmt) {
#if HAVE_OCI_STMT_PREPARE2
		if (statement->last_query_len) { /* FIXME: magical */
			PHP_OCI_CALL(OCIStmtRelease, (statement->stmt, statement->err, NULL, 0, OCI_DEFAULT));
		}
		else {
			PHP_OCI_CALL(OCIHandleFree, (statement->stmt, OCI_HTYPE_STMT));
		}
#else
		PHP_OCI_CALL(OCIHandleFree, (statement->stmt, OCI_HTYPE_STMT));
#endif
		statement->stmt = 0;
	}

	if (statement->err) {
		PHP_OCI_CALL(OCIHandleFree, (statement->err, OCI_HTYPE_ERROR));
		statement->err = 0;
	}

	if (statement->last_query) {
		efree(statement->last_query);
	}

	if (statement->columns) {
		zend_hash_destroy(statement->columns);
		efree(statement->columns);
	}

	if (statement->binds) {
		zend_hash_destroy(statement->binds);
		efree(statement->binds);
	}

	if (statement->defines) {
		zend_hash_destroy(statement->defines);
		efree(statement->defines);
	}
	
	zend_list_delete(statement->connection->rsrc_id);
	efree(statement);
} /* }}} */

/* {{{ php_oci_bind_pre_exec() 
 Helper function */
int php_oci_bind_pre_exec(void *data TSRMLS_DC)
{
	php_oci_bind *bind = (php_oci_bind *) data;

	/* reset all bind stuff to a normal state..-. */

	bind->indicator = 0; 

	return 0;
}
/* }}} */

/* {{{ php_oci_bind_post_exec() 
 Helper function */
int php_oci_bind_post_exec(void *data TSRMLS_DC)
{
	php_oci_bind *bind = (php_oci_bind *) data;
	php_oci_connection *connection = bind->parent_statement->connection;

	if (bind->indicator == -1) { /* NULL */
		zval *val = bind->zval;
		if (Z_TYPE_P(val) == IS_STRING) {
			*Z_STRVAL_P(val) = '\0'; /* XXX avoid warning in debug mode */
		}
		zval_dtor(val);
		ZVAL_NULL(val);
	} else if (Z_TYPE_P(bind->zval) == IS_STRING && Z_STRLEN_P(bind->zval) > 0) {
		Z_STRVAL_P(bind->zval) = erealloc(Z_STRVAL_P(bind->zval), Z_STRLEN_P(bind->zval)+1);
		Z_STRVAL_P(bind->zval)[ Z_STRLEN_P(bind->zval) ] = '\0';
	}
	else if (Z_TYPE_P(bind->zval) == IS_ARRAY) {
		int i;
		zval **entry;
		HashTable *hash = HASH_OF(bind->zval);
	
		switch (bind->array.type) {
			case SQLT_NUM:
			case SQLT_INT:
			case SQLT_LNG:
				for (i = 0; i < bind->array.current_length; i++) {
					if ((i < bind->array.old_length) && (zend_hash_get_current_data(hash, (void **) &entry) != FAILURE)) {
						zval_dtor(*entry);
						ZVAL_LONG(*entry, ((ub4 *)(bind->array.elements))[i]);
						zend_hash_move_forward(hash);
					}
					else {
						add_next_index_long(bind->zval, ((ub4 *)(bind->array.elements))[i]);
					}
				}
				break;
			case SQLT_FLT:
				for (i = 0; i < bind->array.current_length; i++) {
					if ((i < bind->array.old_length) && (zend_hash_get_current_data(hash, (void **) &entry) != FAILURE)) {
						zval_dtor(*entry);
						ZVAL_DOUBLE(*entry, ((double *)(bind->array.elements))[i]);
						zend_hash_move_forward(hash);
					}
					else {
						add_next_index_double(bind->zval, ((double *)(bind->array.elements))[i]);
					}
				}
				break;
			case SQLT_ODT:
				for (i = 0; i < bind->array.current_length; i++) {
					char buff[1024];
					int buff_len = 1024;

					memset((void*)buff,0,sizeof(buff));
							
					if ((i < bind->array.old_length) && (zend_hash_get_current_data(hash, (void **) &entry) != FAILURE)) {
						connection->errcode = PHP_OCI_CALL(OCIDateToText, (connection->err, &(((OCIDate *)(bind->array.elements))[i]), 0, 0, 0, 0, &buff_len, buff));
						zval_dtor(*entry);

						if (connection->errcode != OCI_SUCCESS) {
							php_oci_error(connection->err, connection->errcode TSRMLS_CC);
							ZVAL_NULL(*entry);
						}
						else {
							ZVAL_STRINGL(*entry, buff, buff_len, 1);
						}
						zend_hash_move_forward(hash);
					}
					else {
						connection->errcode = PHP_OCI_CALL(OCIDateToText, (connection->err, &(((OCIDate *)(bind->array.elements))[i]), 0, 0, 0, 0, &buff_len, buff));
						if (connection->errcode != OCI_SUCCESS) {
							php_oci_error(connection->err, connection->errcode TSRMLS_CC);
							add_next_index_null(bind->zval);
						}
						else {
							add_next_index_stringl(bind->zval, buff, buff_len, 1);
						}
					}
				}
				break;
	
			case SQLT_AFC:
			case SQLT_CHR:
			case SQLT_VCS:
			case SQLT_AVC:
			case SQLT_STR:
			case SQLT_LVC:
				for (i = 0; i < bind->array.current_length; i++) {
					int curr_element_length = strlen(((text *)bind->array.elements)+i*bind->array.max_length);
					if ((i < bind->array.old_length) && (zend_hash_get_current_data(hash, (void **) &entry) != FAILURE)) {
						zval_dtor(*entry);
						ZVAL_STRINGL(*entry, ((text *)bind->array.elements)+i*bind->array.max_length, curr_element_length, 1);
						zend_hash_move_forward(hash);
					}
					else {
						add_next_index_stringl(bind->zval, ((text *)bind->array.elements)+i*bind->array.max_length, curr_element_length, 1);
					}
				}
				break;
		}
	}

	return 0;
}
/* }}} */

/* {{{ php_oci_bind_by_name() 
 Bind zval to the given placeholder */
int php_oci_bind_by_name(php_oci_statement *statement, char *name, int name_len, zval* var, long maxlength, long type TSRMLS_DC)
{
	php_oci_collection *bind_collection = NULL;
	php_oci_descriptor *bind_descriptor = NULL;
	php_oci_statement  *bind_statement  = NULL;
	dvoid *oci_desc                 = NULL;
	/* dvoid *php_oci_collection           = NULL; */
	OCIStmt *oci_stmt               = NULL;
	dvoid *bind_data                = NULL;
	php_oci_bind bind, *bindp;
	int mode = OCI_DATA_AT_EXEC;
	sb4 value_sz = -1;

	switch (type) {
#ifdef PHP_OCI8_HAVE_COLLECTIONS
		case SQLT_NTY:
		{
			zval **tmp;
			
			if (Z_TYPE_P(var) != IS_OBJECT || zend_hash_find(Z_OBJPROP_P(var), "collection", sizeof("collection"), (void **)&tmp) == FAILURE) {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "Unable to find collection property");
				return 1;
			}

			PHP_OCI_ZVAL_TO_COLLECTION_EX(*tmp, bind_collection);
			value_sz = sizeof(void*);
			mode = OCI_DEFAULT;
		
			if (!bind_collection->collection) {
				return 1;
			}
		}
			break;
#endif
		case SQLT_BFILEE:
		case SQLT_CFILEE:
		case SQLT_CLOB:
		case SQLT_BLOB:
		case SQLT_RDD:
		{
			zval **tmp;
			
			if (Z_TYPE_P(var) != IS_OBJECT || zend_hash_find(Z_OBJPROP_P(var), "descriptor", sizeof("descriptor"), (void **)&tmp) == FAILURE) {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "Unable to find descriptor property");
				return 1;
			}

			PHP_OCI_ZVAL_TO_DESCRIPTOR_EX(*tmp, bind_descriptor);

			value_sz = sizeof(void*);
			
			oci_desc = bind_descriptor->descriptor;
			
			if (!oci_desc) {
				return 1;
			}
		}
			break;
			
		case SQLT_INT:
		case SQLT_NUM:
			convert_to_long(var);
			bind_data = (ub4 *)&Z_LVAL_P(var);
			value_sz = sizeof(ub4);
			mode = OCI_DEFAULT;
			break;
			
		case SQLT_CHR:
			/* this is the default case when type was not specified */
			convert_to_string(var);
			if (maxlength == -1) {
				value_sz = Z_STRLEN_P(var);
			}
			else {
				value_sz = maxlength;
			}
			break;

		case SQLT_RSET:
			PHP_OCI_ZVAL_TO_STATEMENT_EX(var, bind_statement);
			value_sz = sizeof(void*);

			oci_stmt = bind_statement->stmt;

			if (!oci_stmt) {
				return 1;
			}
			break;

		default:
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Unknown or unsupported datatype given: %ld", type);
			return 1;
			break;
	}
	
	if (value_sz == 0) { 
		value_sz = 1;
	}
	
	if (!statement->binds) {
		ALLOC_HASHTABLE(statement->binds);
		zend_hash_init(statement->binds, 13, NULL, php_oci_bind_hash_dtor, 0);
	}

	memset((void*)&bind,0,sizeof(php_oci_bind));
	zend_hash_update(statement->binds, name, name_len + 1, &bind, sizeof(php_oci_bind), (void **)&bindp);
	
	bindp->descriptor = oci_desc;
	bindp->statement = oci_stmt;
	bindp->parent_statement = statement;
	bindp->zval = var;
	zval_add_ref(&var); 
	
	statement->errcode = PHP_OCI_CALL(
		OCIBindByName,
		(
			statement->stmt,                /* statement handle */
			(OCIBind **)&bindp->bind,       /* bind hdl (will alloc) */
			statement->err,               /* error handle */
			(text*) name,                    /* placeholder name */					  
			name_len,                        /* placeholder length */
			(dvoid *)bind_data,              /* in/out data */
			value_sz, /* PHP_OCI_MAX_DATA_SIZE, */ /* max size of input/output data */
			(ub2)type,                       /* in/out data type */
			(dvoid *)&bindp->indicator,      /* indicator (ignored) */
			(ub2 *)0,                        /* size array (ignored) */
			(ub2 *)&bindp->retcode,          /* return code (ignored) */
			(ub4)0,                          /* maxarr_len (PL/SQL only?) */
			(ub4 *)0,                        /* actual array size (PL/SQL only?) */
			mode                             /* mode */
		)
	);

	if (statement->errcode != OCI_SUCCESS) {
		php_oci_error(statement->err, statement->errcode TSRMLS_CC);
		PHP_OCI_HANDLE_ERROR(statement->connection, statement->errcode);
		return 1;
	}

	if (mode == OCI_DATA_AT_EXEC) {
		statement->errcode = PHP_OCI_CALL(OCIBindDynamic, (bindp->bind, statement->err, (dvoid *)bindp, php_oci_bind_in_callback, (dvoid *)bindp, php_oci_bind_out_callback));

		if (statement->errcode != OCI_SUCCESS) {
			php_oci_error(statement->err, statement->errcode TSRMLS_CC);
			PHP_OCI_HANDLE_ERROR(statement->connection, statement->errcode);
			return 1;
		}
	}

#ifdef PHP_OCI8_HAVE_COLLECTIONS
	if (type == SQLT_NTY) {
		/* Bind object */
		statement->errcode = PHP_OCI_CALL(OCIBindObject, (bindp->bind, statement->err, bind_collection->tdo, (dvoid **) &(bind_collection->collection), (ub4 *) 0, (dvoid **) 0, (ub4 *) 0));
		
		if (statement->errcode) {
			php_oci_error(statement->err, statement->errcode TSRMLS_CC);
			PHP_OCI_HANDLE_ERROR(statement->connection, statement->errcode);
			return 1;
		}
	}
#endif
	
	return 0;
} /* }}} */

/* {{{ php_oci_bind_in_callback() 
 Callback used when binding LOBs and VARCHARs */
sb4 php_oci_bind_in_callback(
					dvoid *ictxp,     /* context pointer */
					OCIBind *bindp,   /* bind handle */
					ub4 iter,         /* 0-based execute iteration value */
					ub4 index,        /* index of current array for PL/SQL or row index for SQL */
					dvoid **bufpp,    /* pointer to data */
					ub4 *alenp,       /* size after value/piece has been read */
					ub1 *piecep,      /* which piece */
					dvoid **indpp)    /* indicator value */
{
	php_oci_bind *phpbind;
	zval *val;
	TSRMLS_FETCH();

	if (!(phpbind=(php_oci_bind *)ictxp) || !(val = phpbind->zval)) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Invalid phpbind pointer value");
		return OCI_ERROR;
	}

	if (ZVAL_IS_NULL(val)) {
		/* we're going to insert a NULL column */
		phpbind->indicator = -1;
		*bufpp = 0;
		*alenp = -1;
		*indpp = (dvoid *)&phpbind->indicator;
	} else 	if ((phpbind->descriptor == 0) && (phpbind->statement == 0)) {
		/* "normal string bind */
		convert_to_string(val); 

		*bufpp = Z_STRVAL_P(val);
		*alenp = Z_STRLEN_P(val);
		*indpp = (dvoid *)&phpbind->indicator;
	} else if (phpbind->statement != 0) {
		/* RSET */
		*bufpp = phpbind->statement;
		*alenp = -1;		/* seems to be allright */
		*indpp = (dvoid *)&phpbind->indicator;
	} else { 
		/* descriptor bind */
		*bufpp = phpbind->descriptor;
		*alenp = -1;		/* seems to be allright */
		*indpp = (dvoid *)&phpbind->indicator;
	}

	*piecep = OCI_ONE_PIECE; /* pass all data in one go */

	return OCI_CONTINUE;
}/* }}} */

/* {{{ php_oci_bind_out_callback()
 Callback used when binding LOBs and VARCHARs */
sb4 php_oci_bind_out_callback(
					dvoid *octxp,      /* context pointer */
					OCIBind *bindp,    /* bind handle */
					ub4 iter,          /* 0-based execute iteration value */
					ub4 index,         /* index of current array for PL/SQL or row index for SQL */
					dvoid **bufpp,     /* pointer to data */
					ub4 **alenpp,      /* size after value/piece has been read */
					ub1 *piecep,       /* which piece */
					dvoid **indpp,     /* indicator value */
					ub2 **rcodepp)     /* return code */
{
	php_oci_bind *phpbind;
	zval *val;
	sb4 retval = OCI_ERROR;
	TSRMLS_FETCH();

	if (!(phpbind=(php_oci_bind *)octxp) || !(val = phpbind->zval)) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Invalid phpbind pointer value");
		return retval;
	}

	if ((Z_TYPE_P(val) == IS_OBJECT) || (Z_TYPE_P(val) == IS_RESOURCE)) {
		retval = OCI_CONTINUE;
	} else {
		convert_to_string(val);
		zval_dtor(val);
		
		Z_STRLEN_P(val) = PHP_OCI_PIECE_SIZE; /* 64K-1 is max XXX */
		Z_STRVAL_P(val) = emalloc(Z_STRLEN_P(phpbind->zval));
		
		/* XXX we assume that zend-zval len has 4 bytes */
		*alenpp = (ub4*) &Z_STRLEN_P(phpbind->zval); 
		*bufpp = Z_STRVAL_P(phpbind->zval);
		*piecep = OCI_ONE_PIECE;
		*rcodepp = &phpbind->retcode;
		*indpp = &phpbind->indicator;
		retval = OCI_CONTINUE;
	}

	return retval;
}
/* }}} */

/* {{{ php_oci_statement_get_column_helper() 
 Helper function to get column by name and index */
php_oci_out_column *php_oci_statement_get_column_helper(INTERNAL_FUNCTION_PARAMETERS)
{
	zval *z_statement, *column_index;
	php_oci_statement *statement;
	php_oci_out_column *column;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "rz", &z_statement, &column_index) == FAILURE) {
		return NULL;
	}

	statement = (php_oci_statement *) zend_fetch_resource(&z_statement TSRMLS_CC, -1, "oci8 statement", NULL, 1, le_statement);

	if (!statement) {
		return NULL;
	}
	
	if (Z_TYPE_P(column_index) == IS_STRING) {
		column = php_oci_statement_get_column(statement, -1, Z_STRVAL_P(column_index), Z_STRLEN_P(column_index) TSRMLS_CC);
		if (!column) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Invalid column name \"%s\"", Z_STRVAL_P(column_index));
			return NULL;
		}
	}
	else {
		convert_to_long(column_index);
		column = php_oci_statement_get_column(statement, Z_LVAL_P(column_index), NULL, 0 TSRMLS_CC);
		if (!column) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Invalid column index \"%ld\"", Z_LVAL_P(column_index));
			return NULL;
		}
	}
	return column;
} /* }}} */

/* {{{ php_oci_statement_get_type()
 Return type of the statement */
int php_oci_statement_get_type(php_oci_statement *statement, ub2 *type TSRMLS_DC)
{
	ub2 statement_type;
	
	*type = 0;
	
	statement->errcode = PHP_OCI_CALL(OCIAttrGet, ((dvoid *)statement->stmt, OCI_HTYPE_STMT, (ub2 *)&statement_type, (ub4 *)0, OCI_ATTR_STMT_TYPE, statement->err));

	if (statement->errcode != OCI_SUCCESS) {
		php_oci_error(statement->err, statement->errcode TSRMLS_CC);
		PHP_OCI_HANDLE_ERROR(statement->connection, statement->errcode);
		return 1;
	}

	*type = statement_type;

	return 0;
} /* }}} */

/* {{{ php_oci_statement_get_numrows() 
 Get the number of rows fetched to the clientside (NOT the number of rows in the result set) */
int php_oci_statement_get_numrows(php_oci_statement *statement, ub4 *numrows TSRMLS_DC)
{
	ub4 statement_numrows;
	
	*numrows = 0;
	
	statement->errcode = PHP_OCI_CALL(OCIAttrGet, ((dvoid *)statement->stmt, OCI_HTYPE_STMT, (ub4 *)&statement_numrows, (ub4 *)0, OCI_ATTR_ROW_COUNT, statement->err));

	if (statement->errcode != OCI_SUCCESS) {
		php_oci_error(statement->err, statement->errcode TSRMLS_CC);
		PHP_OCI_HANDLE_ERROR(statement->connection, statement->errcode);
		return 1;
	}

	*numrows = statement_numrows;

	return 0;
} /* }}} */

/* {{{ php_oci_bind_array_by_name() 
 Bind arrays to PL/SQL types */
int php_oci_bind_array_by_name(php_oci_statement *statement, char *name, int name_len, zval* var, long max_table_length, long maxlength, long type TSRMLS_DC)
{
	php_oci_bind *bind, *bindp;

	convert_to_array(var);

	if (maxlength < -1) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Invalid max length value (%ld)", maxlength);
		return 1;
	}
	
	switch(type) {
		case SQLT_NUM:
		case SQLT_INT:
		case SQLT_LNG:
			bind = php_oci_bind_array_helper_number(var, max_table_length TSRMLS_CC);
			break;

		case SQLT_FLT:
			bind = php_oci_bind_array_helper_double(var, max_table_length TSRMLS_CC);
			break;
			
		case SQLT_AFC:
		case SQLT_CHR:
		case SQLT_VCS:
		case SQLT_AVC:
		case SQLT_STR:
		case SQLT_LVC:
			if (maxlength == -1 && zend_hash_num_elements(Z_ARRVAL_P(var)) == 0) {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "You must provide max length value for empty arrays");
				return 1;
			}
			bind = php_oci_bind_array_helper_string(var, max_table_length, maxlength TSRMLS_CC);
			break;
		case SQLT_ODT:
			bind = php_oci_bind_array_helper_date(var, max_table_length, statement->connection TSRMLS_CC);
			break;
		default:
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Unknown or unsupported datatype given: %ld", type);
			return 1;
			break;
	}

	if (bind == NULL) {
		/* failed to generate bind struct */
		return 1;
	}
	
	if (!statement->binds) {
		ALLOC_HASHTABLE(statement->binds);
		zend_hash_init(statement->binds, 13, NULL, php_oci_bind_hash_dtor, 0);
	}

	zend_hash_update(statement->binds, name, name_len + 1, bind, sizeof(php_oci_bind), (void **)&bindp);

	bindp->descriptor = NULL;
	bindp->statement = NULL;
	bindp->parent_statement = statement;
	bindp->bind = NULL;
	bindp->zval = var;
	bindp->array.type = type;
	zval_add_ref(&var);

	statement->errcode = PHP_OCI_CALL(
							OCIBindByName, 
							(
								statement->stmt,
								(OCIBind **)&bindp->bind,
								statement->err,
								(text *)name,
								name_len,
								(dvoid *) bindp->array.elements, 
								(sb4) bind->array.max_length,
								type,
								(dvoid *)0, /* bindp->array.indicators, */
								(ub2 *)0, /* bindp->array.element_lengths, */
								(ub2 *)0, /* bindp->array.retcodes, */
								(ub4) max_table_length,
								(ub4 *) &(bindp->array.current_length),
								(ub4) OCI_DEFAULT
							)
						);
	
		
	if (statement->errcode != OCI_SUCCESS) {
		efree(bind);
		php_oci_error(statement->err, statement->errcode TSRMLS_CC);
		PHP_OCI_HANDLE_ERROR(statement->connection, statement->errcode);
		return 1;
	}
	efree(bind);
	return 0;
} /* }}} */

/* {{{ php_oci_bind_array_helper_string() 
 Bind arrays to PL/SQL types */
php_oci_bind *php_oci_bind_array_helper_string(zval* var, long max_table_length, long maxlength TSRMLS_DC)
{
	php_oci_bind *bind;
	ub4 i;
	HashTable *hash;
	zval **entry;

	hash = HASH_OF(var);

	if (maxlength == -1) {
		zend_hash_internal_pointer_reset(hash);
		while (zend_hash_get_current_data(hash, (void **) &entry) != FAILURE) {
			convert_to_string_ex(entry);
			if (Z_STRLEN_PP(entry) > maxlength) {
				maxlength = Z_STRLEN_PP(entry) + 1;
			}
			zend_hash_move_forward(hash);
		}
	}
	
	bind = emalloc(sizeof(php_oci_bind));
	bind->array.elements		= (text *)ecalloc(1, max_table_length * sizeof(text) * (maxlength + 1));
	bind->array.current_length	= zend_hash_num_elements(Z_ARRVAL_P(var));
	bind->array.old_length		= bind->array.current_length;
	bind->array.max_length		= maxlength;

	zend_hash_internal_pointer_reset(hash);
	for (i = 0; i < max_table_length; i++) {
		if ((i < bind->array.current_length) && (zend_hash_get_current_data(hash, (void **) &entry) != FAILURE)) {
			int element_length; 
			
			convert_to_string_ex(entry);
			element_length = (maxlength > Z_STRLEN_PP(entry)) ? Z_STRLEN_PP(entry) : maxlength;
			
			memcpy((text *)bind->array.elements + i*maxlength, Z_STRVAL_PP(entry), element_length);
			((text *)bind->array.elements)[i*maxlength + element_length] = '\0';
			
			zend_hash_move_forward(hash);
		}
		else {
			((text *)bind->array.elements)[i*maxlength] = '\0';
		}
	}
	zend_hash_internal_pointer_reset(hash);

	return bind;
} /* }}} */

/* {{{ php_oci_bind_array_helper_number() 
 Bind arrays to PL/SQL types */
php_oci_bind *php_oci_bind_array_helper_number(zval* var, long max_table_length TSRMLS_DC)
{
	php_oci_bind *bind;
	ub4 i;
	HashTable *hash;
	zval **entry;

	hash = HASH_OF(var);

	bind = emalloc(sizeof(php_oci_bind));
	bind->array.elements		= (ub4 *)emalloc(max_table_length * sizeof(ub4));
	bind->array.current_length	= zend_hash_num_elements(Z_ARRVAL_P(var));
	bind->array.old_length		= bind->array.current_length;
	bind->array.max_length		= sizeof(ub4);
	
	zend_hash_internal_pointer_reset(hash);
	for (i = 0; i < max_table_length; i++) {
		if ((i < bind->array.current_length) && (zend_hash_get_current_data(hash, (void **) &entry) != FAILURE)) {
			convert_to_long_ex(entry);
			((ub4 *)bind->array.elements)[i] = (ub4) Z_LVAL_PP(entry);
			zend_hash_move_forward(hash);
		}
		else {
			((ub4 *)bind->array.elements)[i] = 0;
		}
	}
	zend_hash_internal_pointer_reset(hash);

	return bind;
} /* }}} */

/* {{{ php_oci_bind_array_helper_double() 
 Bind arrays to PL/SQL types */
php_oci_bind *php_oci_bind_array_helper_double(zval* var, long max_table_length TSRMLS_DC)
{
	php_oci_bind *bind;
	ub4 i;
	HashTable *hash;
	zval **entry;

	hash = HASH_OF(var);

	bind = emalloc(sizeof(php_oci_bind));
	bind->array.elements		= (double *)emalloc(max_table_length * sizeof(double));
	bind->array.current_length	= zend_hash_num_elements(Z_ARRVAL_P(var));
	bind->array.old_length		= bind->array.current_length;
	bind->array.max_length		= sizeof(double);
	
	zend_hash_internal_pointer_reset(hash);
	for (i = 0; i < max_table_length; i++) {
		if ((i < bind->array.current_length) && (zend_hash_get_current_data(hash, (void **) &entry) != FAILURE)) {
			convert_to_double_ex(entry);
			((double *)bind->array.elements)[i] = (double) Z_DVAL_PP(entry);
			zend_hash_move_forward(hash);
		}
		else {
			((double *)bind->array.elements)[i] = 0;
		}
	}
	zend_hash_internal_pointer_reset(hash);

	return bind;
} /* }}} */

/* {{{ php_oci_bind_array_helper_date() 
 Bind arrays to PL/SQL types */
php_oci_bind *php_oci_bind_array_helper_date(zval* var, long max_table_length, php_oci_connection *connection TSRMLS_DC)
{
	php_oci_bind *bind;
	ub4 i;
	HashTable *hash;
	zval **entry;

	hash = HASH_OF(var);

	bind = emalloc(sizeof(php_oci_bind));
	bind->array.elements		= (OCIDate *)emalloc(max_table_length * sizeof(OCIDate));
	bind->array.current_length	= zend_hash_num_elements(Z_ARRVAL_P(var));
	bind->array.old_length		= bind->array.current_length;
	bind->array.max_length		= sizeof(OCIDate);

	zend_hash_internal_pointer_reset(hash);
	for (i = 0; i < max_table_length; i++) {
		OCIDate oci_date;
		if ((i < bind->array.current_length) && (zend_hash_get_current_data(hash, (void **) &entry) != FAILURE)) {
			
			convert_to_string_ex(entry);
			connection->errcode = PHP_OCI_CALL(OCIDateFromText, (connection->err, Z_STRVAL_PP(entry), Z_STRLEN_PP(entry), NULL, 0, NULL, 0, &oci_date));

			if (connection->errcode != OCI_SUCCESS) {
				/* failed to convert string to date */
				efree(bind->array.elements);
				efree(bind);
				php_oci_error(connection->err, connection->errcode TSRMLS_CC);
				return NULL;
			}
			
			((OCIDate *)bind->array.elements)[i] = oci_date;
			zend_hash_move_forward(hash);
		}
		else {
			connection->errcode = PHP_OCI_CALL(OCIDateFromText, (connection->err, "01-JAN-00", sizeof("01-JAN-00")-1, NULL, 0, NULL, 0, &oci_date));

			if (connection->errcode != OCI_SUCCESS) {
				/* failed to convert string to date */
				efree(bind->array.elements);
				efree(bind);
				php_oci_error(connection->err, connection->errcode TSRMLS_CC);
				return NULL;
			}
	
			((OCIDate *)bind->array.elements)[i] = oci_date;
		}
	}
	zend_hash_internal_pointer_reset(hash);

	return bind;
} /* }}} */

#endif /* HAVE_OCI8 */