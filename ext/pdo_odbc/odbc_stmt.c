/*
  +----------------------------------------------------------------------+
  | PHP Version 5                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 1997-2016 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.0 of the PHP license,       |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_0.txt.                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author: Wez Furlong <wez@php.net>                                    |
  +----------------------------------------------------------------------+
*/

/* $Id$ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "pdo/php_pdo.h"
#include "pdo/php_pdo_driver.h"
#include "php_pdo_odbc.h"
#include "php_pdo_odbc_int.h"

enum pdo_odbc_conv_result {
	PDO_ODBC_CONV_NOT_REQUIRED,
	PDO_ODBC_CONV_OK,
	PDO_ODBC_CONV_FAIL
};

static int pdo_odbc_sqltype_is_unicode(pdo_odbc_stmt *S, SWORD sqltype)
{
	if (!S->assume_utf8) return 0;
	switch (sqltype) {
#ifdef SQL_WCHAR
		case SQL_WCHAR:
			return 1;
#endif
#ifdef SQL_WLONGVARCHAR
		case SQL_WLONGVARCHAR:
			return 1;
#endif
#ifdef SQL_WVARCHAR
		case SQL_WVARCHAR:
			return 1;
#endif
		default:
			return 0;
	}
}

static int pdo_odbc_utf82ucs2(pdo_stmt_t *stmt, int is_unicode, const char *buf, 
	unsigned long buflen, unsigned long *outlen)
{
#ifdef PHP_WIN32
	if (is_unicode && buflen) {
		pdo_odbc_stmt *S = (pdo_odbc_stmt*)stmt->driver_data;
		DWORD ret;

		ret = MultiByteToWideChar(CP_UTF8, 0, buf, buflen, NULL, 0);
		if (ret == 0) {
			/*printf("%s:%d %d [%d] %.*s\n", __FILE__, __LINE__, GetLastError(), buflen, buflen, buf);*/
			return PDO_ODBC_CONV_FAIL;
		}

		ret *= sizeof(WCHAR);

		if (S->convbufsize <= ret) {
			S->convbufsize = ret + sizeof(WCHAR);
			S->convbuf = erealloc(S->convbuf, S->convbufsize);
		}
		
		ret = MultiByteToWideChar(CP_UTF8, 0, buf, buflen, (LPWSTR)S->convbuf, S->convbufsize / sizeof(WCHAR));
		if (ret == 0) {
			/*printf("%s:%d %d [%d] %.*s\n", __FILE__, __LINE__, GetLastError(), buflen, buflen, buf);*/
			return PDO_ODBC_CONV_FAIL;
		}

		ret *= sizeof(WCHAR);
		*outlen = ret;
		return PDO_ODBC_CONV_OK;
	}
#endif
	return PDO_ODBC_CONV_NOT_REQUIRED;
}

static int pdo_odbc_ucs22utf8(pdo_stmt_t *stmt, int is_unicode, const char *buf, 
	unsigned long buflen, unsigned long *outlen)
{
#ifdef PHP_WIN32
	if (is_unicode && buflen) {
		pdo_odbc_stmt *S = (pdo_odbc_stmt*)stmt->driver_data;
		DWORD ret;

		ret = WideCharToMultiByte(CP_UTF8, 0, (LPCWSTR)buf, buflen/sizeof(WCHAR), NULL, 0, NULL, NULL);
		if (ret == 0) {
			return PDO_ODBC_CONV_FAIL;
		}

		if (S->convbufsize <= ret) {
			S->convbufsize = ret + 1;
			S->convbuf = erealloc(S->convbuf, S->convbufsize);
		}
		
		ret = WideCharToMultiByte(CP_UTF8, 0, (LPCWSTR)buf, buflen/sizeof(WCHAR), S->convbuf, S->convbufsize, NULL, NULL);
		if (ret == 0) {
			return PDO_ODBC_CONV_FAIL;
		}

		*outlen = ret;
		S->convbuf[*outlen] = '\0';
		return PDO_ODBC_CONV_OK;
	}
#endif
	return PDO_ODBC_CONV_NOT_REQUIRED;
}

static void free_cols(pdo_stmt_t *stmt, pdo_odbc_stmt *S TSRMLS_DC)
{
	if (S->cols) {
		int i;

		for (i = 0; i < stmt->column_count; i++) {
			if (S->cols[i].data) {
				efree(S->cols[i].data);
			}
		}
		efree(S->cols);
		S->cols = NULL;
	}
}

static int odbc_stmt_dtor(pdo_stmt_t *stmt TSRMLS_DC)
{
	pdo_odbc_stmt *S = (pdo_odbc_stmt*)stmt->driver_data;

	if (S->stmt != SQL_NULL_HANDLE) {
		if (stmt->executed) {
			SQLCloseCursor(S->stmt);
		}
		SQLFreeHandle(SQL_HANDLE_STMT, S->stmt);
		S->stmt = SQL_NULL_HANDLE;
	}

	free_cols(stmt, S TSRMLS_CC);
	if (S->convbuf) {
		efree(S->convbuf);
	}
	efree(S);

	return 1;
}

static int odbc_stmt_execute(pdo_stmt_t *stmt TSRMLS_DC)
{
	RETCODE rc;
	pdo_odbc_stmt *S = (pdo_odbc_stmt*)stmt->driver_data;
	char *buf = NULL;
	SQLLEN row_count = -1;

	if (stmt->executed) {
		SQLCloseCursor(S->stmt);
	}
	
	rc = SQLExecute(S->stmt);	

	while (rc == SQL_NEED_DATA) {
		struct pdo_bound_param_data *param;

		rc = SQLParamData(S->stmt, (SQLPOINTER*)&param);
		if (rc == SQL_NEED_DATA) {
			php_stream *stm;
			int len;
			pdo_odbc_param *P;
	
			P = (pdo_odbc_param*)param->driver_data;
			if (Z_TYPE_P(param->parameter) != IS_RESOURCE) {
				/* they passed in a string */
				unsigned long ulen;
				convert_to_string(param->parameter);

				switch (pdo_odbc_utf82ucs2(stmt, P->is_unicode, 
							Z_STRVAL_P(param->parameter),
							Z_STRLEN_P(param->parameter),
							&ulen)) {
					case PDO_ODBC_CONV_NOT_REQUIRED:
						SQLPutData(S->stmt, Z_STRVAL_P(param->parameter),
							Z_STRLEN_P(param->parameter));
						break;
					case PDO_ODBC_CONV_OK:
						SQLPutData(S->stmt, S->convbuf, ulen);
						break;
					case PDO_ODBC_CONV_FAIL:
						pdo_odbc_stmt_error("error converting input string");
						SQLCloseCursor(S->stmt);
						if (buf) {
							efree(buf);
						}
						return 0;
				}
				continue;
			}

			/* we assume that LOBs are binary and don't need charset
			 * conversion */

			php_stream_from_zval_no_verify(stm, &param->parameter);
			if (!stm) {
				/* shouldn't happen either */
				pdo_odbc_stmt_error("input LOB is no longer a stream");
				SQLCloseCursor(S->stmt);
				if (buf) {
					efree(buf);
				}
				return 0;
			}

			/* now suck data from the stream and stick it into the database */
			if (buf == NULL) {
				buf = emalloc(8192);
			}

			do {
				len = php_stream_read(stm, buf, 8192);
				if (len == 0) {
					break;
				}
				SQLPutData(S->stmt, buf, len);
			} while (1);
		}
	}

	if (buf) {
		efree(buf);
	}

	switch (rc) {
		case SQL_SUCCESS:
			break;
		case SQL_NO_DATA_FOUND:
		case SQL_SUCCESS_WITH_INFO:
			pdo_odbc_stmt_error("SQLExecute");
			break;

		default:
			pdo_odbc_stmt_error("SQLExecute");
			return 0;
	}

	SQLRowCount(S->stmt, &row_count);
	stmt->row_count = row_count;

	if (!stmt->executed) {
		/* do first-time-only definition of bind/mapping stuff */
		SQLSMALLINT colcount;

		/* how many columns do we have ? */
		SQLNumResultCols(S->stmt, &colcount);

		stmt->column_count = (int)colcount;
		S->cols = ecalloc(colcount, sizeof(pdo_odbc_column));
		S->going_long = 0;
	}

	return 1;
}

static int odbc_stmt_param_hook(pdo_stmt_t *stmt, struct pdo_bound_param_data *param,
		enum pdo_param_event event_type TSRMLS_DC)
{
	pdo_odbc_stmt *S = (pdo_odbc_stmt*)stmt->driver_data;
	RETCODE rc;
	SWORD sqltype = 0, ctype = 0, scale = 0, nullable = 0;
	SQLULEN precision = 0;
	pdo_odbc_param *P;
	
	/* we're only interested in parameters for prepared SQL right now */
	if (param->is_param) {

		switch (event_type) {
			case PDO_PARAM_EVT_FETCH_PRE:
			case PDO_PARAM_EVT_FETCH_POST:
			case PDO_PARAM_EVT_NORMALIZE:
				/* Do nothing */
				break;

			case PDO_PARAM_EVT_FREE:
				P = param->driver_data;
				if (P) {
					efree(P);
				}
				break;

			case PDO_PARAM_EVT_ALLOC:
			{
				/* figure out what we're doing */
				switch (PDO_PARAM_TYPE(param->param_type)) {
					case PDO_PARAM_LOB:
						break;

					case PDO_PARAM_STMT:
						return 0;
					
					default:
						break;
				}

				rc = SQLDescribeParam(S->stmt, (SQLUSMALLINT) param->paramno+1, &sqltype, &precision, &scale, &nullable);
				if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
					/* MS Access, for instance, doesn't support SQLDescribeParam,
					 * so we need to guess */
					sqltype = PDO_PARAM_TYPE(param->param_type) == PDO_PARAM_LOB ?
									SQL_LONGVARBINARY :
									SQL_LONGVARCHAR;
					precision = 4000;
					scale = 5;
					nullable = 1;

					if (param->max_value_len > 0) {
						precision = param->max_value_len;
					}
				}
				if (sqltype == SQL_BINARY || sqltype == SQL_VARBINARY || sqltype == SQL_LONGVARBINARY) {
					ctype = SQL_C_BINARY;
				} else {
					ctype = SQL_C_CHAR;
				}

				P = emalloc(sizeof(*P));
				param->driver_data = P;

				P->len = 0; /* is re-populated each EXEC_PRE */
				P->outbuf = NULL;

				P->is_unicode = pdo_odbc_sqltype_is_unicode(S, sqltype);
				if (P->is_unicode) {
					/* avoid driver auto-translation: we'll do it ourselves */
					ctype = SQL_C_BINARY;
				}

				if ((param->param_type & PDO_PARAM_INPUT_OUTPUT) == PDO_PARAM_INPUT_OUTPUT) {
					P->paramtype = SQL_PARAM_INPUT_OUTPUT;
				} else if (param->max_value_len <= 0) {
					P->paramtype = SQL_PARAM_INPUT;
				} else {
					P->paramtype = SQL_PARAM_OUTPUT;
				}
				
				if (P->paramtype != SQL_PARAM_INPUT) {
					if (PDO_PARAM_TYPE(param->param_type) != PDO_PARAM_NULL) {
						/* need an explicit buffer to hold result */
						P->len = param->max_value_len > 0 ? param->max_value_len : precision;
						if (P->is_unicode) {
							P->len *= 2;
						}
						P->outbuf = emalloc(P->len + (P->is_unicode ? 2:1));
					}
				}
				
				if (PDO_PARAM_TYPE(param->param_type) == PDO_PARAM_LOB && P->paramtype != SQL_PARAM_INPUT) {
					pdo_odbc_stmt_error("Can't bind a lob for output");
					return 0;
				}

				rc = SQLBindParameter(S->stmt, (SQLUSMALLINT) param->paramno+1,
						P->paramtype, ctype, sqltype, precision, scale,
						P->paramtype == SQL_PARAM_INPUT ? 
							(SQLPOINTER)param :
							P->outbuf,
						P->len,
						&P->len
						);
	
				if (rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO) {
					return 1;
				}
				pdo_odbc_stmt_error("SQLBindParameter");
				return 0;
			}

			case PDO_PARAM_EVT_EXEC_PRE:
				P = param->driver_data;
				if (PDO_PARAM_TYPE(param->param_type) == PDO_PARAM_LOB) {
					if (Z_TYPE_P(param->parameter) == IS_RESOURCE) {
						php_stream *stm;
						php_stream_statbuf sb;

						php_stream_from_zval_no_verify(stm, &param->parameter);

						if (!stm) {
							return 0;
						}

						if (0 == php_stream_stat(stm, &sb)) {
							if (P->outbuf) {
								int len, amount;
								char *ptr = P->outbuf;
								char *end = P->outbuf + P->len;

								P->len = 0;
								do {
									amount = end - ptr;
									if (amount == 0) {
										break;
									}
									if (amount > 8192)
										amount = 8192;
									len = php_stream_read(stm, ptr, amount);
									if (len == 0) {
										break;
									}
									ptr += len;
									P->len += len;
								} while (1);

							} else {
								P->len = SQL_LEN_DATA_AT_EXEC(sb.sb.st_size);
							}
						} else {
							if (P->outbuf) {
								P->len = 0;
							} else {
								P->len = SQL_LEN_DATA_AT_EXEC(0);
							}
						}
					} else {
						convert_to_string(param->parameter);
						if (P->outbuf) {
							P->len = Z_STRLEN_P(param->parameter);
							memcpy(P->outbuf, Z_STRVAL_P(param->parameter), P->len);
						} else {
							P->len = SQL_LEN_DATA_AT_EXEC(Z_STRLEN_P(param->parameter));
						}
					}
				} else if (Z_TYPE_P(param->parameter) == IS_NULL || PDO_PARAM_TYPE(param->param_type) == PDO_PARAM_NULL) {
					P->len = SQL_NULL_DATA;
				} else {
					convert_to_string(param->parameter);
					if (P->outbuf) {
						unsigned long ulen;
						switch (pdo_odbc_utf82ucs2(stmt, P->is_unicode,
								Z_STRVAL_P(param->parameter),
								Z_STRLEN_P(param->parameter),
								&ulen)) {
							case PDO_ODBC_CONV_FAIL:
							case PDO_ODBC_CONV_NOT_REQUIRED:
								P->len = Z_STRLEN_P(param->parameter);
								memcpy(P->outbuf, Z_STRVAL_P(param->parameter), P->len);
								break;
							case PDO_ODBC_CONV_OK:
								P->len = ulen;
								memcpy(P->outbuf, S->convbuf, P->len);
								break;
						}
					} else {
						P->len = SQL_LEN_DATA_AT_EXEC(Z_STRLEN_P(param->parameter));
					}
				}
				return 1;
			
			case PDO_PARAM_EVT_EXEC_POST:
				P = param->driver_data;
				if (P->outbuf) {
					if (P->outbuf) {
						unsigned long ulen;
						char *srcbuf;
						unsigned long srclen = 0;

						switch (P->len) {
							case SQL_NULL_DATA:
								zval_dtor(param->parameter);
								ZVAL_NULL(param->parameter);
								break;
							default:
								convert_to_string(param->parameter);
								switch (pdo_odbc_ucs22utf8(stmt, P->is_unicode, P->outbuf, P->len, &ulen)) {
									case PDO_ODBC_CONV_FAIL:
										/* something fishy, but allow it to come back as binary */
									case PDO_ODBC_CONV_NOT_REQUIRED:
										srcbuf = P->outbuf;
										srclen = P->len;
										break;
									case PDO_ODBC_CONV_OK:
										srcbuf = S->convbuf;
										srclen = ulen;
										break;
								}
										
								Z_STRVAL_P(param->parameter) = erealloc(Z_STRVAL_P(param->parameter), srclen+1);
								memcpy(Z_STRVAL_P(param->parameter), srcbuf, srclen);
								Z_STRLEN_P(param->parameter) = srclen;
								Z_STRVAL_P(param->parameter)[srclen] = '\0';
						}
					}
				}
				return 1;
		}
	}
	return 1;
}

static int odbc_stmt_fetch(pdo_stmt_t *stmt,
	enum pdo_fetch_orientation ori, long offset TSRMLS_DC)
{
	RETCODE rc;
	SQLSMALLINT odbcori;
	pdo_odbc_stmt *S = (pdo_odbc_stmt*)stmt->driver_data;

	switch (ori) {
		case PDO_FETCH_ORI_NEXT:	odbcori = SQL_FETCH_NEXT; break;
		case PDO_FETCH_ORI_PRIOR:	odbcori = SQL_FETCH_PRIOR; break;
		case PDO_FETCH_ORI_FIRST:	odbcori = SQL_FETCH_FIRST; break;
		case PDO_FETCH_ORI_LAST:	odbcori = SQL_FETCH_LAST; break;
		case PDO_FETCH_ORI_ABS:		odbcori = SQL_FETCH_ABSOLUTE; break;
		case PDO_FETCH_ORI_REL:		odbcori = SQL_FETCH_RELATIVE; break;
		default: 
			strcpy(stmt->error_code, "HY106");
			return 0;
	}
	rc = SQLFetchScroll(S->stmt, odbcori, offset);

	if (rc == SQL_SUCCESS) {
		return 1;
	}
	if (rc == SQL_SUCCESS_WITH_INFO) {
		pdo_odbc_stmt_error("SQLFetchScroll");
		return 1;
	}

	if (rc == SQL_NO_DATA) {
		/* pdo_odbc_stmt_error("SQLFetchScroll"); */
		return 0;
	}

	pdo_odbc_stmt_error("SQLFetchScroll");

	return 0;
}

static int odbc_stmt_describe(pdo_stmt_t *stmt, int colno TSRMLS_DC)
{
	pdo_odbc_stmt *S = (pdo_odbc_stmt*)stmt->driver_data;
	struct pdo_column_data *col = &stmt->columns[colno];
	RETCODE rc;
	SWORD	colnamelen;
	SQLULEN	colsize;
	SQLLEN displaysize;

	rc = SQLDescribeCol(S->stmt, colno+1, S->cols[colno].colname,
			sizeof(S->cols[colno].colname)-1, &colnamelen,
			&S->cols[colno].coltype, &colsize, NULL, NULL);

	/* This fixes a known issue with SQL Server and (max) lengths,
	may affect others as well.  If we are SQL_VARCHAR,
	SQL_VARBINARY, or SQL_WVARCHAR (or any of the long variations)
	and zero is returned from colsize then consider it long */
	if (0 == colsize && 
		(S->cols[colno].coltype == SQL_VARCHAR ||
		 S->cols[colno].coltype == SQL_LONGVARCHAR ||
#ifdef SQL_WVARCHAR
		 S->cols[colno].coltype == SQL_WVARCHAR ||
#endif
#ifdef SQL_WLONGVARCHAR
		 S->cols[colno].coltype == SQL_WLONGVARCHAR ||
#endif
		 S->cols[colno].coltype == SQL_VARBINARY ||
		 S->cols[colno].coltype == SQL_LONGVARBINARY)) {
			 S->going_long = 1;
	}

	if (rc != SQL_SUCCESS) {
		pdo_odbc_stmt_error("SQLDescribeCol");
		if (rc != SQL_SUCCESS_WITH_INFO) {
			return 0;
		}
	}

	rc = SQLColAttribute(S->stmt, colno+1,
			SQL_DESC_DISPLAY_SIZE,
			NULL, 0, NULL, &displaysize);

	if (rc != SQL_SUCCESS) {
		pdo_odbc_stmt_error("SQLColAttribute");
		if (rc != SQL_SUCCESS_WITH_INFO) {
			return 0;
		}
	}
	colsize = displaysize;

	col->maxlen = S->cols[colno].datalen = colsize;
	col->namelen = colnamelen;
	col->name = estrdup(S->cols[colno].colname);
	S->cols[colno].is_unicode = pdo_odbc_sqltype_is_unicode(S, S->cols[colno].coltype);

	/* returning data as a string */
	col->param_type = PDO_PARAM_STR;

	/* tell ODBC to put it straight into our buffer, but only if it
	 * isn't "long" data, and only if we haven't already bound a long
	 * column. */
	if (colsize < 256 && !S->going_long) {
		S->cols[colno].data = emalloc(colsize+1);
		S->cols[colno].is_long = 0;

		rc = SQLBindCol(S->stmt, colno+1,
			S->cols[colno].is_unicode ? SQL_C_BINARY : SQL_C_CHAR,
			S->cols[colno].data,
 			S->cols[colno].datalen+1, &S->cols[colno].fetched_len);

		if (rc != SQL_SUCCESS) {
			pdo_odbc_stmt_error("SQLBindCol");
			return 0;
		}
	} else {
		/* allocate a smaller buffer to keep around for smaller
		 * "long" columns */
		S->cols[colno].data = emalloc(256);
		S->going_long = 1;
		S->cols[colno].is_long = 1;
	}

	return 1;
}

static int odbc_stmt_get_col(pdo_stmt_t *stmt, int colno, char **ptr, unsigned long *len, int *caller_frees TSRMLS_DC)
{
	pdo_odbc_stmt *S = (pdo_odbc_stmt*)stmt->driver_data;
	pdo_odbc_column *C = &S->cols[colno];
	unsigned long ulen;

	/* if it is a column containing "long" data, perform late binding now */
	if (C->is_long) {
		unsigned long used = 0;
		char *buf;
		RETCODE rc;

		/* fetch it into C->data, which is allocated with a length
		 * of 256 bytes; if there is more to be had, we then allocate
		 * bigger buffer for the caller to free */

		rc = SQLGetData(S->stmt, colno+1, C->is_unicode ? SQL_C_BINARY : SQL_C_CHAR, C->data,
 			256, &C->fetched_len);

		if (rc == SQL_SUCCESS) {
			/* all the data fit into our little buffer;
			 * jump down to the generic bound data case */
			goto in_data;
		}

		if (rc == SQL_SUCCESS_WITH_INFO) {
			/* promote up to a bigger buffer */

			if (C->fetched_len != SQL_NO_TOTAL) {
				/* use size suggested by the driver, if it knows it */
				buf = emalloc(C->fetched_len + 1);
				memcpy(buf, C->data, C->fetched_len);
				buf[C->fetched_len] = 0;
				used = C->fetched_len;
			} else {
				buf = estrndup(C->data, 256);
				used = 255; /* not 256; the driver NUL terminated the buffer */
			}

			do {
				C->fetched_len = 0;
				rc = SQLGetData(S->stmt, colno+1, SQL_C_CHAR,
					buf + used, alloced - used,
					&C->fetched_len);

				if (rc == SQL_NO_DATA) {
					/* we got the lot */
					break;
				} else if (rc != SQL_SUCCESS) {
					pdo_odbc_stmt_error("SQLGetData");
					if (rc != SQL_SUCCESS_WITH_INFO) {
						break;
					}
				}

				if (C->fetched_len == SQL_NO_TOTAL) {
					used += alloced - used;
				} else {
					used += C->fetched_len;
				}

				if (rc == SQL_SUCCESS) {
					/* this was the final fetch */
					break;
				}

				/* we need to fetch another chunk; resize the
				 * buffer */
				alloced *= 2;
				buf = erealloc(buf, alloced);
			} while (1);

			/* size down */
			if (used < alloced - 1024) {
				alloced = used+1;
				buf = erealloc(buf, used+1);
			}
			buf[used] = '\0';
			*ptr = buf;
			*caller_frees = 1;
			*len = used;
			if (C->is_unicode) {
				goto unicode_conv;
			}
			return 1;
		}

		/* something went caca */
		*ptr = NULL;
		*len = 0;
		return 1;
	}

in_data:
	/* check the indicator to ensure that the data is intact */
	if (C->fetched_len == SQL_NULL_DATA) {
		/* A NULL value */
		*ptr = NULL;
		*len = 0;
		return 1;
	} else if (C->fetched_len >= 0) {
		/* it was stored perfectly */
		*ptr = C->data;
		*len = C->fetched_len;
		if (C->is_unicode) {
			goto unicode_conv;
		}
		return 1;
	} else {
		/* no data? */
		*ptr = NULL;
		*len = 0;
		return 1;
	}

	unicode_conv:
	switch (pdo_odbc_ucs22utf8(stmt, C->is_unicode, *ptr, *len, &ulen)) {
		case PDO_ODBC_CONV_FAIL:
			/* oh well.  They can have the binary version of it */
		case PDO_ODBC_CONV_NOT_REQUIRED:
			/* shouldn't happen... */
			return 1;

		case PDO_ODBC_CONV_OK:
			if (*caller_frees) {
				efree(*ptr);
			}
			*ptr = emalloc(ulen + 1);
			*len = ulen;
			memcpy(*ptr, S->convbuf, ulen+1);
			*caller_frees = 1;
			return 1;
	}
	return 1;
}

static int odbc_stmt_set_param(pdo_stmt_t *stmt, long attr, zval *val TSRMLS_DC)
{
	SQLRETURN rc;
	pdo_odbc_stmt *S = (pdo_odbc_stmt*)stmt->driver_data;

	switch (attr) {
		case PDO_ATTR_CURSOR_NAME:
			convert_to_string(val);
			rc = SQLSetCursorName(S->stmt, Z_STRVAL_P(val), Z_STRLEN_P(val));

			if (rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO) {
				return 1;
			}
			pdo_odbc_stmt_error("SQLSetCursorName");
			return 0;

		case PDO_ODBC_ATTR_ASSUME_UTF8:
			S->assume_utf8 = zval_is_true(val);
			return 0;
		default:
			strcpy(S->einfo.last_err_msg, "Unknown Attribute");
			S->einfo.what = "setAttribute";
			strcpy(S->einfo.last_state, "IM001");
			return -1;
	}
}

static int odbc_stmt_get_attr(pdo_stmt_t *stmt, long attr, zval *val TSRMLS_DC)
{
	SQLRETURN rc;
	pdo_odbc_stmt *S = (pdo_odbc_stmt*)stmt->driver_data;

	switch (attr) {
		case PDO_ATTR_CURSOR_NAME:
		{
			char buf[256];
			SQLSMALLINT len = 0;
			rc = SQLGetCursorName(S->stmt, buf, sizeof(buf), &len);

			if (rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO) {
				ZVAL_STRINGL(val, buf, len, 1);
				return 1;
			}
			pdo_odbc_stmt_error("SQLGetCursorName");
			return 0;
		}

		case PDO_ODBC_ATTR_ASSUME_UTF8:
			ZVAL_BOOL(val, S->assume_utf8 ? 1 : 0);
			return 0;

		default:
			strcpy(S->einfo.last_err_msg, "Unknown Attribute");
			S->einfo.what = "getAttribute";
			strcpy(S->einfo.last_state, "IM001");
			return -1;
	}
}

static int odbc_stmt_next_rowset(pdo_stmt_t *stmt TSRMLS_DC)
{
	SQLRETURN rc;
	SQLSMALLINT colcount;
	pdo_odbc_stmt *S = (pdo_odbc_stmt*)stmt->driver_data;

	/* NOTE: can't guarantee that output or input/output parameters
	 * are set until this fella returns SQL_NO_DATA, according to
	 * MSDN ODBC docs */
	rc = SQLMoreResults(S->stmt);

	if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
		return 0;
	}

	free_cols(stmt, S TSRMLS_CC);
	/* how many columns do we have ? */
	SQLNumResultCols(S->stmt, &colcount);
	stmt->column_count = (int)colcount;
	S->cols = ecalloc(colcount, sizeof(pdo_odbc_column));
	S->going_long = 0;

	return 1;
}

struct pdo_stmt_methods odbc_stmt_methods = {
	odbc_stmt_dtor,
	odbc_stmt_execute,
	odbc_stmt_fetch,
	odbc_stmt_describe,
	odbc_stmt_get_col,
	odbc_stmt_param_hook,
	odbc_stmt_set_param,
	odbc_stmt_get_attr, /* get attr */
	NULL, /* get column meta */
	odbc_stmt_next_rowset
};

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
