// quickjs-dbi.c — QuickJS + libDBI SQLite bridge
#include "quickjs.h"
#include "cutils.h"

#include <stdbool.h>
#include <ctype.h>
#include <dbi.h>
#include <string.h>
#include <stdio.h>

typedef struct DBIResult DBIResult;
typedef struct { 
	dbi_conn	conn;
	DBIResult	*results;
} DBIHandle;

struct DBIResult {
	bool		valid;
	dbi_result	res;

	// Used in case of conn invalidation:
	DBIHandle	*conn;
	DBIResult	*prev;
	DBIResult	*next;
};

static JSClassID dbi_class_id;
static JSClassID dbi_result_class_id;

static DBIResult *jsdbi_alloc_result(JSContext *ctx, DBIHandle *handle, dbi_result result) {
	DBIResult *r = js_mallocz(ctx, sizeof(DBIResult));
	r->valid = true;
	r->res = result;
	r->conn = handle;
	r->prev = NULL;
	r->next = handle->results;
	if (r->next)
		r->next->prev = r;
	handle->results = r;
	return r;
}

static void jsdbi_close_connection(DBIHandle *h)
{
	DBIResult *rhnext = NULL;
	for( DBIResult *rh = h->results; rh; rh=rhnext )
	{
		rhnext = rh->next;
		if( rh->valid && rh->res )
			dbi_result_free(rh->res);
		rh->valid = false;
		rh->conn = NULL;
		rh->res = NULL;
	}
	h->results = NULL;

	if (h->conn)
		dbi_conn_close(h->conn);

	h->conn = NULL;
}

static void dbi_finalizer(JSRuntime *rt, JSValue val) {
	DBIHandle *h = JS_GetOpaque(val, dbi_class_id);
	if(!h)
		return;

	jsdbi_close_connection(h);

	js_free_rt(rt, h);
}

static void dbi_result_finalizer(JSRuntime *rt, JSValue val) {
	DBIResult *r = JS_GetOpaque(val, dbi_result_class_id);
	if(!r)
		return;

	if( r->conn ) {
		if( !r->prev ) {
			r->conn->results = r->next;
			if( r->next )
				r->next->prev = NULL;
		} else {
			r->prev->next = r->next;
			if( r->next )
				r->next->prev = r->prev;
		}
	}

	if (r->valid && r->res) dbi_result_free(r->res);
	js_free_rt(rt, r);
}

static JSValue res_next(JSContext *ctx, JSValueConst this_val,
						int argc, JSValueConst *argv) {
	DBIResult *r = JS_GetOpaque2(ctx, this_val, dbi_result_class_id);
	if (!r || !r->valid) return JS_EXCEPTION;
	int has = dbi_result_next_row(r->res);
	return JS_NewBool(ctx, has > 0);
}

static JSValue res_numfields(JSContext *ctx, JSValueConst this_val,
			  int argc, JSValueConst *argv) {
	DBIResult *r = JS_GetOpaque2(ctx, this_val, dbi_result_class_id);
	if (!r || !r->valid) return JS_EXCEPTION;
	return JS_NewInt32(ctx, dbi_result_get_numfields(r->res));
}

static JSValue res_numrows(JSContext *ctx, JSValueConst this_val,
			  int argc, JSValueConst *argv) {
	DBIResult *r = JS_GetOpaque2(ctx, this_val, dbi_result_class_id);
	if (!r || !r->valid) return JS_EXCEPTION;
	return JS_NewBigUint64(ctx, dbi_result_get_numrows(r->res));
}

static JSValue _res_get_value(JSContext *ctx, DBIResult *r, int field_index)
{
	int ftype = dbi_result_get_field_type_idx(r->res, field_index);
	unsigned int flags = dbi_result_get_field_attribs_idx(r->res, field_index);
	JSValue out = JS_UNDEFINED;

	if(dbi_result_field_is_null_idx(r->res, field_index))
		return JS_NULL;

	switch (ftype) {
	case DBI_TYPE_INTEGER:
		if( flags & DBI_INTEGER_UNSIGNED )
			out = JS_NewBigUint64(ctx, dbi_result_get_ulonglong_idx(r->res, field_index));
		else
			out = JS_NewInt64(ctx, dbi_result_get_longlong_idx(r->res, field_index));
		break;
	case DBI_TYPE_DECIMAL:
		out = JS_NewFloat64(ctx, dbi_result_get_double_idx(r->res, field_index));
		break;
	case DBI_TYPE_BINARY: {
		const unsigned char *ptr = dbi_result_get_binary_idx(r->res, field_index);
		unsigned long len = dbi_result_get_field_length_idx(r->res, field_index);
		out = JS_NewArrayBufferCopy(ctx, ptr, len);
		break;
		}
	case DBI_TYPE_DATETIME:
		out = JS_NewDate(ctx, 1000 * dbi_result_get_datetime_idx(r->res, field_index));
		break;
	default: // STRING, BOOLEAN, etc:
		out = JS_NewString(ctx, dbi_result_get_string_idx(r->res, field_index));
		break;
	}

	return out;
}

static JSValue res_get(JSContext *ctx, JSValueConst this_val,
			   int argc, JSValueConst *argv) {
	DBIResult *r = JS_GetOpaque2(ctx, this_val, dbi_result_class_id);
	if (!r || !r->valid) return JS_EXCEPTION;

	int field_index = -1;
	if( JS_IsNumber(argv[0]) ) {
		if (JS_ToInt32(ctx, &field_index, argv[0]) || field_index < 0)
			return JS_EXCEPTION;

		// DBI is 1-indexed
		field_index++;
	} else {
		const char *fname = JS_ToCString(ctx, argv[0]);
		if (!fname) return JS_EXCEPTION;

		field_index = dbi_result_get_field_idx(r->res, fname);
		JS_FreeCString(ctx, fname);
	}

	if (field_index <= 0 || field_index > dbi_result_get_numfields(r->res))
		return JS_UNDEFINED;

	if (dbi_result_field_is_null_idx(r->res, field_index))
		return JS_NULL;

	return _res_get_value(ctx, r, field_index);
}

static JSValue res_toArray(JSContext *ctx, JSValueConst this_val,
			   int argc, JSValueConst *argv) {
	DBIResult *r = JS_GetOpaque2(ctx, this_val, dbi_result_class_id);
	if (!r || !r->valid) return JS_EXCEPTION;

	JSValue arr = JS_NewArray(ctx);

	bool dict = false;
	if (argc > 0 && JS_IsBool(argv[0])) {
		dict = JS_ToBool(ctx, argv[0]);
	}

	if( !dbi_result_first_row(r->res) )
		return arr;

	unsigned int nfields = dbi_result_get_numfields(r->res);
	const char **fnames = js_malloc(ctx, nfields * sizeof(char *));
	unsigned int row = 0;

	for (unsigned int i = 0; i < nfields; i++)
		fnames[i] = dbi_result_get_field_name(r->res, i+1);

	do {
		JSValue rowval = dict ? JS_NewObject(ctx) : JS_NewArray(ctx);
		for (unsigned int i = 1; i <= nfields; i++) {
			int ret;
			JSValue val = _res_get_value(ctx, r, i);
			if (dict)
				ret = JS_SetPropertyStr(ctx, rowval, fnames[i-1], val);
			else
				ret = JS_SetPropertyUint32(ctx, rowval, i-1, val);

			if( ret < 0 ) {
				js_free(ctx, fnames);
				JS_FreeValue(ctx, arr);
				JS_FreeValue(ctx, rowval);
				return JS_EXCEPTION;
			}
		}
		JS_SetPropertyUint32(ctx, arr, row++, rowval);
	} while (dbi_result_next_row(r->res));

	js_free(ctx, fnames);

	return arr;
}

static const JSCFunctionListEntry dbi_result_proto_funcs[] = {
	JS_CFUNC_DEF("next", 0, res_next),
	JS_CFUNC_DEF("get", 1, res_get),
	JS_CFUNC_DEF("numfields", 0, res_numfields),
	JS_CFUNC_DEF("numrows", 0, res_numrows),
	JS_CFUNC_DEF("toArray", 1, res_toArray),
};

static dbi_inst global_dbi_instance = NULL;

static JSValue jsdbi_open(JSContext *ctx, JSValueConst this_val,
			  int argc, JSValueConst *argv)
{
	if (argc < 2)
		return JS_ThrowTypeError(ctx, "Usage: DBI.open(driver, options)");

	const char *driver_name = JS_ToCString(ctx, argv[0]);
	if (!driver_name)
		return JS_EXCEPTION;

	if (!JS_IsObject(argv[1])) {
		JS_FreeCString(ctx, driver_name);
		return JS_ThrowTypeError(ctx, "Second argument must be an object of connection options");
	}

	/* Initialize libDBI once per runtime */
	if (!global_dbi_instance) {
		if (dbi_initialize_r(NULL, &global_dbi_instance) < 0) {
			JS_FreeCString(ctx, driver_name);
			return JS_ThrowInternalError(ctx, "libDBI init failed");
		}
	}

	dbi_driver drv = dbi_driver_open_r(driver_name, global_dbi_instance);
	JS_FreeCString(ctx, driver_name);
	if (!drv)
		return JS_ThrowInternalError(ctx, "Unable to load DBI driver");

	dbi_conn conn = dbi_conn_open(drv);
	if (!conn) {
		return JS_ThrowInternalError(ctx, "Unable to create DBI connection");
	}

	/* Iterate over connection options */
	JSPropertyEnum *props;
	uint32_t len;
	if (JS_GetOwnPropertyNames(ctx, &props, &len, argv[1], JS_GPN_STRING_MASK) < 0) {
		dbi_conn_close(conn);
		return JS_EXCEPTION;
	}

	for (uint32_t i = 0; i < len; i++) {
		JSAtom atom = props[i].atom;
		const char *key = JS_AtomToCString(ctx, atom);
		if (!key)
			continue;

		JSValue val = JS_GetProperty(ctx, argv[1], atom);
		const char *vstr = JS_ToCString(ctx, val);
		if (vstr) {
			dbi_conn_set_option(conn, key, vstr);
			JS_FreeCString(ctx, vstr);
		}

		JS_FreeCString(ctx, key);
		JS_FreeValue(ctx, val);
	}

	js_free(ctx, props);

	/* Connect */
	if (dbi_conn_connect(conn) < 0) {
		const char *errmsg = NULL;
		dbi_conn_error(conn, &errmsg);

		const char *drvname = dbi_driver_get_name(dbi_conn_get_driver(conn));
		const char *dbname = dbi_conn_get_option(conn, "dbname");

		JSValue err = JS_ThrowInternalError(
			ctx,
			"DB connection failed (%s, %s): %s",
			drvname ? drvname : "unknown driver",
			dbname ? dbname : "unknown dbname",
			errmsg ? errmsg : "unknown error"
		);

		dbi_conn_close(conn);
		return err;
	}

	/* Allocate and bind handle */
	DBIHandle *h = js_mallocz(ctx, sizeof(DBIHandle));
	if (!h) {
		dbi_conn_close(conn);
		return JS_EXCEPTION;
	}
	h->conn = conn;

	JSValue obj = JS_NewObjectClass(ctx, dbi_class_id);
	JS_SetOpaque(obj, h);

	return obj;
}

// TODO: If/when we can "unimport" modules, do this thing.
static void jsdbi_shutdown(void)
{
	if (global_dbi_instance) {
		dbi_shutdown_r(global_dbi_instance);
		global_dbi_instance = NULL;
	}
}


static void append_value(JSContext *ctx, DynBuf *db, JSValue val) {
	if (JS_IsNull(val) || JS_IsUndefined(val)) {
		dbuf_putstr(db, "NULL");

	} else if (JS_IsBool(val)) {
		dbuf_putstr(db, JS_ToBool(ctx, val) ? "1" : "0");
	} else if (JS_IsNumber(val)) {
		double n;
		JS_ToFloat64(ctx, &n, val);
		dbuf_printf(db, "%.17g", n);
	} else if (JS_IsString(val)) {
		const char *str = JS_ToCString(ctx, val);
		if (!str) {
			dbuf_putstr(db, "NULL");
			return;
		}
		dbuf_putc(db, '\'');
		for (const char *p = str; *p; p++) {
			if (*p == '\'')
				dbuf_putc(db, '\''); // escape single quotes
			dbuf_putc(db, *p);
		}
		dbuf_putc(db, '\'');
		JS_FreeCString(ctx, str);
	} else {
		/* Try ArrayBuffer or TypedArray */
		size_t len = 0;
		uint8_t *buf = JS_GetArrayBuffer(ctx, &len, val);

		if (!buf) {
			/* Try TypedArray view */
			size_t offset = 0, byte_len = 0, bytes_per_el = 0;
			JSValue abuf = JS_GetTypedArrayBuffer(ctx, val, &offset, &byte_len, &bytes_per_el);
			if (!JS_IsException(abuf)) {
				size_t total_len = 0;
				uint8_t *base = JS_GetArrayBuffer(ctx, &total_len, abuf);
				if (base && offset + byte_len <= total_len)
					buf = base + offset, len = byte_len;
				JS_FreeValue(ctx, abuf);
			}
		}

		if (buf && len > 0) {
			dbuf_putstr(db, "X'");
			for (size_t i = 0; i < len; i++)
				dbuf_printf(db, "%02X", buf[i]);
			dbuf_putc(db, '\'');
		} else {
			dbuf_putstr(db, "NULL");  /* fallback for unsupported type */
		}
	}
}


unsigned int _array_length(JSContext *ctx, JSValue v)
{
	unsigned int len = 0;
	JSValue lv = JS_GetPropertyStr(ctx,v,"length");
	JS_ToUint32(ctx,&len,lv);
	JS_FreeValue(ctx,lv);
	return len;
}

/* Build a new SQL string with bound parameters. */
static char *build_query(JSContext *ctx, const char *sql, JSValue args) {
	DynBuf db;
	dbuf_init(&db);

	const char *p = sql;
	if (JS_IsArray(ctx, args)) {
		// Positional parameters (?)
		uint32_t len = _array_length(ctx, args);
		uint32_t idx = 0;
		for (; *p; p++) {
			if('?' == *p && idx < len) {
				JSValue val = JS_GetPropertyUint32(ctx, args, idx++);
				append_value(ctx, &db, val);
				JS_FreeValue(ctx, val);
			} else {
				dbuf_putc(&db, *p);
			}
		}
	} else if (JS_IsObject(args)) {
		// Named parameters (:name)
		for (; *p; ) {
			if( ':' == *p && isalpha((unsigned char)p[1])) {
				const char *start = ++p;
				while (isalnum((unsigned char)*p) || '_' == *p)
					p++;
				size_t keylen = p - start;
				char key[64];
				if (keylen >= sizeof(key)) keylen = sizeof(key)-1;
				memcpy(key, start, keylen);
				key[keylen] = 0;
				JSValue val = JS_GetPropertyStr(ctx, args, key);
				append_value(ctx, &db, val);
				JS_FreeValue(ctx, val);
			} else {
				dbuf_putc(&db, *p++);
			}
		}
	} else {
		dbuf_putstr(&db, sql);
	}

	dbuf_putc(&db, 0);
	return db.buf;
}

static JSValue jsdbi_query(JSContext *ctx, JSValueConst this_val,
			 int argc, JSValueConst *argv) {
	DBIHandle *h = JS_GetOpaque2(ctx, this_val, dbi_class_id);
	if (!h) return JS_EXCEPTION;

	const char *sql = JS_ToCString(ctx, argv[0]);
	if (!sql) return JS_EXCEPTION;

	JSValue args = argc > 1 ? argv[1] : JS_UNDEFINED;

	char *final = build_query(ctx, sql, args);
	JS_FreeCString(ctx, sql);

	dbi_result res = dbi_conn_query(h->conn, final);
	js_free(ctx, final);
	if (!res) return JS_NULL;

	DBIResult *r = jsdbi_alloc_result(ctx, h, res);
	JSValue obj = JS_NewObjectClass(ctx, dbi_result_class_id);
	JS_SetOpaque(obj, r);
	return obj;
}

static JSValue jsdbi_exec(JSContext *ctx, JSValueConst this_val,
			int argc, JSValueConst *argv) {
	DBIHandle *h = JS_GetOpaque2(ctx, this_val, dbi_class_id);
	if (!h) return JS_EXCEPTION;

	const char *sql = JS_ToCString(ctx, argv[0]);
	if (!sql) return JS_EXCEPTION;

	JSValue args = argc > 1 ? argv[1] : JS_UNDEFINED;

	char *final = build_query(ctx, sql, args);
	JS_FreeCString(ctx, sql);

	dbi_result res = dbi_conn_query(h->conn, final);
	js_free(ctx, final);

	if (!res) return JS_FALSE;
	dbi_result_free(res);
	return JS_TRUE;
}

static JSValue jsdbi_close(JSContext *ctx, JSValueConst this_val,
			 int argc, JSValueConst *argv) {
	DBIHandle *h = JS_GetOpaque2(ctx, this_val, dbi_class_id);
	if (!h) return JS_EXCEPTION;
	jsdbi_close_connection(h);
	return JS_UNDEFINED;
}

static const JSCFunctionListEntry dbi_proto_funcs[] = {
	JS_CFUNC_DEF("query", 2, jsdbi_query),
	JS_CFUNC_DEF("exec", 2, jsdbi_exec),
	JS_CFUNC_DEF("close", 0, jsdbi_close),
};

static int js_dbi_init(JSContext *ctx, JSModuleDef *m)
{
	JSRuntime *rt = JS_GetRuntime(ctx);

	/* ---- DBIHandle ---- */
	JS_NewClassID(&dbi_class_id);
	JSClassDef cls = { "DBI", .finalizer = dbi_finalizer };
	JS_NewClass(rt, dbi_class_id, &cls);
	JSValue proto = JS_NewObject(ctx);
	JS_SetPropertyFunctionList(ctx, proto,
		dbi_proto_funcs,
		sizeof(dbi_proto_funcs) / sizeof(JSCFunctionListEntry));
	JS_SetClassProto(ctx, dbi_class_id, proto);

	/* ---- DBIResult ---- */
	JS_NewClassID(&dbi_result_class_id);
	JSClassDef rcls = { "DBIResult", .finalizer = dbi_result_finalizer };
	JS_NewClass(rt, dbi_result_class_id, &rcls);
	JSValue rproto = JS_NewObject(ctx);
	JS_SetPropertyFunctionList(ctx, rproto,
		dbi_result_proto_funcs,
		sizeof(dbi_result_proto_funcs) / sizeof(JSCFunctionListEntry));
	JS_SetClassProto(ctx, dbi_result_class_id, rproto);

	/* ---- Exports ---- */
	JSValue open_fn = JS_NewCFunction(ctx, jsdbi_open, "open", 2);
	JS_SetModuleExport(ctx, m, "open", open_fn);

	return 0;
}

JSModuleDef *js_init_module(JSContext *ctx, const char *name)
{
	JSModuleDef *m = JS_NewCModule(ctx, name, js_dbi_init);
	if (!m)
		return NULL;

	JS_AddModuleExport(ctx, m, "open");
	return m;
}
