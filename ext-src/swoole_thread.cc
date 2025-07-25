/*
  +----------------------------------------------------------------------+
  | Swoole                                                               |
  +----------------------------------------------------------------------+
  | This source file is subject to version 2.0 of the Apache license,    |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.apache.org/licenses/LICENSE-2.0.html                      |
  | If you did not receive a copy of the Apache2.0 license and are unable|
  | to obtain it through the world-wide-web, please send a note to       |
  | license@swoole.com so we can mail you a copy immediately.            |
  +----------------------------------------------------------------------+
  | Author: Tianfeng Han  <rango@swoole.com>                             |
  +----------------------------------------------------------------------+
*/

#include "php_swoole_cxx.h"
#include "php_swoole_thread.h"

#ifdef SW_THREAD

#include <sys/ipc.h>
#include <sys/resource.h>

#include <atomic>

BEGIN_EXTERN_C()
#include "stubs/php_swoole_thread_arginfo.h"
END_EXTERN_C()

zend_class_entry *swoole_thread_ce;
static zend_object_handlers swoole_thread_handlers;

zend_class_entry *swoole_thread_error_ce;
static zend_object_handlers swoole_thread_error_handlers;

static struct {
    char *path_translated;
    zend_string *argv_serialized;
    int argc;
} request_info;

TSRMLS_CACHE_EXTERN();

using swoole::Thread;

struct PhpThread {
    std::shared_ptr<Thread> thread;

    PhpThread() : thread(std::make_shared<Thread>()) {}

    bool join() const {
        if (!thread->joinable()) {
            return false;
        }
        thread->join();
        return true;
    }
};

struct ThreadObject {
    PhpThread *pt;
    zend_object std;
};

static void thread_register_stdio_file_handles(bool no_close);

static thread_local zval thread_argv = {};
static thread_local JMP_BUF *thread_bailout = nullptr;
static std::atomic<size_t> thread_num(1);

static sw_inline ThreadObject *thread_fetch_object(zend_object *obj) {
    return reinterpret_cast<ThreadObject *>(reinterpret_cast<char *>(obj) - swoole_thread_handlers.offset);
}

static sw_inline ThreadObject *thread_fetch_object(const zval *zobj) {
    return thread_fetch_object(Z_OBJ_P(zobj));
}

static sw_inline PhpThread *thread_get_php_thread(zend_object *obj) {
    return thread_fetch_object(obj)->pt;
}

static sw_inline PhpThread *thread_get_php_thread(const zval *zobj) {
    return thread_fetch_object(zobj)->pt;
}

static void thread_free_object(zend_object *object) {
    auto pt = thread_get_php_thread(object);
    pt->join();
    delete pt;
    zend_object_std_dtor(object);
}

static zend_object *thread_create_object(zend_class_entry *ce) {
    auto to = static_cast<ThreadObject *>(zend_object_alloc(sizeof(ThreadObject), ce));
    zend_object_std_init(&to->std, ce);
    object_properties_init(&to->std, ce);
    to->pt = new PhpThread();
    to->std.handlers = &swoole_thread_handlers;
    return &to->std;
}

SW_EXTERN_C_BEGIN
static PHP_METHOD(swoole_thread, __construct);
static PHP_METHOD(swoole_thread, isAlive);
static PHP_METHOD(swoole_thread, join);
static PHP_METHOD(swoole_thread, joinable);
static PHP_METHOD(swoole_thread, getExitStatus);
static PHP_METHOD(swoole_thread, detach);
static PHP_METHOD(swoole_thread, getArguments);
static PHP_METHOD(swoole_thread, getId);
static PHP_METHOD(swoole_thread, getInfo);
static PHP_METHOD(swoole_thread, activeCount);
static PHP_METHOD(swoole_thread, yield);
static PHP_METHOD(swoole_thread, setName);
#ifdef HAVE_CPU_AFFINITY
static PHP_METHOD(swoole_thread, setAffinity);
static PHP_METHOD(swoole_thread, getAffinity);
#endif
static PHP_METHOD(swoole_thread, setPriority);
static PHP_METHOD(swoole_thread, getPriority);
static PHP_METHOD(swoole_thread, getNativeId);
SW_EXTERN_C_END

// clang-format off
static const zend_function_entry swoole_thread_methods[] = {
    PHP_ME(swoole_thread, __construct,   arginfo_class_Swoole_Thread___construct,   ZEND_ACC_PUBLIC)
    PHP_ME(swoole_thread, isAlive,       arginfo_class_Swoole_Thread_isAlive,       ZEND_ACC_PUBLIC)
    PHP_ME(swoole_thread, join,          arginfo_class_Swoole_Thread_join,          ZEND_ACC_PUBLIC)
    PHP_ME(swoole_thread, joinable,      arginfo_class_Swoole_Thread_joinable,      ZEND_ACC_PUBLIC)
    PHP_ME(swoole_thread, getExitStatus, arginfo_class_Swoole_Thread_getExitStatus, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_thread, detach,        arginfo_class_Swoole_Thread_detach,        ZEND_ACC_PUBLIC)
    PHP_ME(swoole_thread, getArguments,  arginfo_class_Swoole_Thread_getArguments,  ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(swoole_thread, getId,         arginfo_class_Swoole_Thread_getId,         ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(swoole_thread, getInfo,       arginfo_class_Swoole_Thread_getInfo,       ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(swoole_thread, activeCount,   arginfo_class_Swoole_Thread_activeCount,   ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(swoole_thread, yield,         arginfo_class_Swoole_Thread_yield,         ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(swoole_thread, setName,       arginfo_class_Swoole_Thread_setName,       ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
#ifdef HAVE_CPU_AFFINITY
    PHP_ME(swoole_thread, setAffinity,   arginfo_class_Swoole_Thread_setAffinity,   ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(swoole_thread, getAffinity,   arginfo_class_Swoole_Thread_getAffinity,   ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
#endif
    PHP_ME(swoole_thread, setPriority,   arginfo_class_Swoole_Thread_setPriority,   ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(swoole_thread, getPriority,   arginfo_class_Swoole_Thread_getPriority,   ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(swoole_thread, getNativeId,   arginfo_class_Swoole_Thread_getNativeId,   ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_FE_END
};
// clang-format on

void php_swoole_thread_minit(int module_number) {
    SW_INIT_CLASS_ENTRY(swoole_thread, "Swoole\\Thread", nullptr, swoole_thread_methods);
    swoole_thread_ce->ce_flags |= ZEND_ACC_FINAL | ZEND_ACC_NOT_SERIALIZABLE;
    SW_SET_CLASS_CLONEABLE(swoole_thread, sw_zend_class_clone_deny);
    SW_SET_CLASS_UNSET_PROPERTY_HANDLER(swoole_thread, sw_zend_class_unset_property_deny);
    SW_SET_CLASS_CUSTOM_OBJECT(swoole_thread, thread_create_object, thread_free_object, ThreadObject, std);

    zend_declare_property_long(swoole_thread_ce, ZEND_STRL("id"), 0, ZEND_ACC_PUBLIC | ZEND_ACC_READONLY);
    zend_declare_class_constant_long(
        swoole_thread_ce, ZEND_STRL("HARDWARE_CONCURRENCY"), std::thread::hardware_concurrency());
    zend_declare_class_constant_string(swoole_thread_ce, ZEND_STRL("API_NAME"), tsrm_api_name());

    zend_declare_class_constant_long(swoole_thread_ce, ZEND_STRL("SCHED_OTHER"), SCHED_OTHER);
    zend_declare_class_constant_long(swoole_thread_ce, ZEND_STRL("SCHED_FIFO"), SCHED_FIFO);
    zend_declare_class_constant_long(swoole_thread_ce, ZEND_STRL("SCHED_RR"), SCHED_RR);
#ifdef SCHED_BATCH
    zend_declare_class_constant_long(swoole_thread_ce, ZEND_STRL("SCHED_BATCH"), SCHED_BATCH);
#endif
#ifdef SCHED_ISO
    zend_declare_class_constant_long(swoole_thread_ce, ZEND_STRL("SCHED_ISO"), SCHED_ISO);
#endif
#ifdef SCHED_IDLE
    zend_declare_class_constant_long(swoole_thread_ce, ZEND_STRL("SCHED_IDLE"), SCHED_IDLE);
#endif
#ifdef SCHED_DEADLINE
    zend_declare_class_constant_long(swoole_thread_ce, ZEND_STRL("SCHED_DEADLINE"), SCHED_DEADLINE);
#endif

    SW_INIT_CLASS_ENTRY_DATA_OBJECT(swoole_thread_error, "Swoole\\Thread\\Error");
    zend_declare_property_long(swoole_thread_error_ce, ZEND_STRL("code"), 0, ZEND_ACC_PUBLIC | ZEND_ACC_READONLY);
}

static PHP_METHOD(swoole_thread, __construct) {
    char *script_file;
    size_t l_script_file;
    zval *args;
    int argc;
    ZendArray *argv = nullptr;

    ZEND_PARSE_PARAMETERS_START(1, -1)
    Z_PARAM_STRING(script_file, l_script_file)
    Z_PARAM_VARIADIC('+', args, argc)
    ZEND_PARSE_PARAMETERS_END();

    if (l_script_file < 1) {
        zend_throw_exception(swoole_exception_ce, "exec file name is empty", SW_ERROR_INVALID_PARAMS);
        return;
    }

    auto pt = thread_get_php_thread(ZEND_THIS);
    zend_string *file = zend_string_init(script_file, l_script_file, true);

    if (argc > 0) {
        argv = new ZendArray();
        for (int i = 0; i < argc; i++) {
            argv->append(&args[i]);
        }
    }

    try {
        pt->thread->start([file, argv, pt]() { php_swoole_thread_start(pt->thread, file, argv); });
    } catch (const std::exception &e) {
        zend_throw_exception(swoole_exception_ce, e.what(), SW_ERROR_SYSTEM_CALL_FAIL);
        return;
    }

    zend::object_set(ZEND_THIS, ZEND_STRL("id"), (zend_long) pt->thread->get_id());
}

static PHP_METHOD(swoole_thread, isAlive) {
    auto pt = thread_get_php_thread(ZEND_THIS);
    RETURN_BOOL(pt->thread->is_alive());
}

static PHP_METHOD(swoole_thread, join) {
    auto pt = thread_get_php_thread(ZEND_THIS);
    RETURN_BOOL(pt->join());
}

static PHP_METHOD(swoole_thread, joinable) {
    auto pt = thread_get_php_thread(ZEND_THIS);
    RETURN_BOOL(pt->thread->joinable());
}

static PHP_METHOD(swoole_thread, detach) {
    auto pt = thread_get_php_thread(ZEND_THIS);
    if (!pt->thread->joinable()) {
        RETURN_FALSE;
    }
    pt->thread->detach();
    RETURN_TRUE;
}

static PHP_METHOD(swoole_thread, getArguments) {
    if (Z_TYPE(thread_argv) == IS_ARRAY) {
        RETURN_ZVAL(&thread_argv, 1, 0);
    }
}

static PHP_METHOD(swoole_thread, getId) {
    RETURN_LONG((zend_long) pthread_self());
}

static PHP_METHOD(swoole_thread, getExitStatus) {
    auto pt = thread_get_php_thread(ZEND_THIS);
    RETURN_LONG(pt->thread->get_exit_status());
}

static PHP_METHOD(swoole_thread, setName) {
    char *name;
    size_t l_name;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_STRING(name, l_name)
    ZEND_PARSE_PARAMETERS_END();

    RETURN_BOOL(swoole_thread_set_name(name));
}

#ifdef HAVE_CPU_AFFINITY
static PHP_METHOD(swoole_thread, setAffinity) {
    zval *array;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_ARRAY(array)
    ZEND_PARSE_PARAMETERS_END();

    cpu_set_t cpu_set;
    if (!php_swoole_array_to_cpu_set(array, &cpu_set)) {
        RETURN_FALSE;
    }

    if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set), &cpu_set) < 0) {
        php_swoole_error(E_WARNING, "pthread_setaffinity_np() failed");
        RETURN_FALSE;
    }
    RETURN_TRUE;
}

static PHP_METHOD(swoole_thread, getAffinity) {
    cpu_set_t cpu_set;
    if (pthread_getaffinity_np(pthread_self(), sizeof(cpu_set), &cpu_set) < 0) {
        php_swoole_error(E_WARNING, "pthread_getaffinity_np() failed");
        RETURN_FALSE;
    }
    php_swoole_cpu_set_to_array(return_value, &cpu_set);
}
#endif

static PHP_METHOD(swoole_thread, setPriority) {
    zend_long priority, policy = -1;
    ZEND_PARSE_PARAMETERS_START(1, 2)
    Z_PARAM_LONG(priority)
    Z_PARAM_OPTIONAL
    Z_PARAM_LONG(policy)
    ZEND_PARSE_PARAMETERS_END();

    struct sched_param param;
    if (policy == -1) {
        pthread_setschedparam(pthread_self(), policy, &param);
    }

    param.sched_priority = priority;
    int retval = pthread_setschedparam(pthread_self(), policy, &param);
    if (retval == 0) {
        RETURN_TRUE;
    } else {
        php_swoole_sys_error(E_WARNING, "pthread_setschedparam() failed");
        RETURN_FALSE;
    }
}

static PHP_METHOD(swoole_thread, getPriority) {
    struct sched_param param;
    int policy;
    if (pthread_getschedparam(pthread_self(), &policy, &param) != 0) {
        php_swoole_error(E_WARNING, "pthread_getschedparam() failed");
        RETURN_FALSE;
    }

    array_init(return_value);
    add_assoc_long_ex(return_value, ZEND_STRL("policy"), policy);
    add_assoc_long_ex(return_value, ZEND_STRL("priority"), param.sched_priority);
}

static PHP_METHOD(swoole_thread, getNativeId) {
    RETURN_LONG((zend_long) swoole_thread_get_native_id());
}

void php_swoole_thread_rinit() {
    if (tsrm_is_main_thread()) {
        if (SG(request_info).path_translated) {
            request_info.path_translated = strdup(SG(request_info).path_translated);
        }
        // Return reference
        zval *global_argv = zend_hash_find_ind(&EG(symbol_table), ZSTR_KNOWN(ZEND_STR_ARGV));
        if (global_argv) {
            request_info.argv_serialized = php_swoole_serialize(global_argv);
            request_info.argc = SG(request_info).argc;
        }
    }
}

void php_swoole_thread_rshutdown() {
    zval_dtor(&thread_argv);
    if (!tsrm_is_main_thread()) {
        return;
    }
    if (sw_active_thread_count() > 1) {
        swoole_warning("Fatal Error: %zu active threads are running, cannot exit safely.", sw_active_thread_count());
        exit(200);
    }
    if (request_info.path_translated) {
        free((void *) request_info.path_translated);
        request_info.path_translated = nullptr;
    }
    if (request_info.argv_serialized) {
        zend_string_release(request_info.argv_serialized);
        request_info.argv_serialized = nullptr;
    }
}

static void thread_register_stdio_file_handles(bool no_close) {
    php_stream *s_in, *s_out, *s_err;
    php_stream_context *sc_in = nullptr, *sc_out = nullptr, *sc_err = nullptr;
    zend_constant ic, oc, ec;

    s_in = php_stream_open_wrapper_ex("php://stdin", "rb", 0, NULL, sc_in);
    s_out = php_stream_open_wrapper_ex("php://stdout", "wb", 0, NULL, sc_out);
    s_err = php_stream_open_wrapper_ex("php://stderr", "wb", 0, NULL, sc_err);

    if (s_in == nullptr || s_out == nullptr || s_err == nullptr) {
        if (s_in) php_stream_close(s_in);
        if (s_out) php_stream_close(s_out);
        if (s_err) php_stream_close(s_err);
        return;
    }

    if (no_close) {
        s_in->flags |= PHP_STREAM_FLAG_NO_CLOSE;
        s_out->flags |= PHP_STREAM_FLAG_NO_CLOSE;
        s_err->flags |= PHP_STREAM_FLAG_NO_CLOSE;
    }

    php_stream_to_zval(s_in, &ic.value);
    php_stream_to_zval(s_out, &oc.value);
    php_stream_to_zval(s_err, &ec.value);

    ZEND_CONSTANT_SET_FLAGS(&ic, CONST_CS, 0);
    ic.name = zend_string_init_interned("STDIN", sizeof("STDIN") - 1, false);
    zend_register_constant(&ic);

    ZEND_CONSTANT_SET_FLAGS(&oc, CONST_CS, 0);
    oc.name = zend_string_init_interned("STDOUT", sizeof("STDOUT") - 1, false);
    zend_register_constant(&oc);

    ZEND_CONSTANT_SET_FLAGS(&ec, CONST_CS, 0);
    ec.name = zend_string_init_interned("STDERR", sizeof("STDERR") - 1, false);
    zend_register_constant(&ec);
}

void php_swoole_thread_start(std::shared_ptr<Thread> thread, zend_string *file, ZendArray *argv) {
    thread_num.fetch_add(1);
    thread->enter();
    ts_resource(0);
#if defined(COMPILE_DL_SWOOLE) && defined(ZTS)
    ZEND_TSRMLS_CACHE_UPDATE();
#endif
    zend_file_handle file_handle{};
    zval global_argc, global_argv;

    PG(expose_php) = false;
    PG(auto_globals_jit) = true;
#if PHP_VERSION_ID >= 80100
    PG(enable_dl) = false;
#else
    PG(enable_dl) = 0;
#endif

    swoole_thread_init(false);

    if (php_request_startup() != SUCCESS) {
        EG(exit_status) = 1;
        goto _startup_error;
    }

    PG(during_request_startup) = false;
    SG(sapi_started) = false;
    SG(headers_sent) = 1;
    SG(request_info).no_headers = true;
    SG(request_info).path_translated = request_info.path_translated;
    SG(request_info).argc = request_info.argc;

    zend_stream_init_filename(&file_handle, ZSTR_VAL(file));
    file_handle.primary_script = true;

    zend_first_try {
        thread_bailout = EG(bailout);
        if (request_info.argv_serialized) {
            php_swoole_unserialize(request_info.argv_serialized, &global_argv);
            ZVAL_LONG(&global_argc, request_info.argc);
            zend_hash_update(&EG(symbol_table), ZSTR_KNOWN(ZEND_STR_ARGV), &global_argv);
            zend_hash_update(&EG(symbol_table), ZSTR_KNOWN(ZEND_STR_ARGC), &global_argc);
        }
        if (argv) {
            argv->to_array(&thread_argv);
            argv->del_ref();
        }
        thread_register_stdio_file_handles(true);
        php_execute_script(&file_handle);
    }
    zend_end_try();

    zend_destroy_file_handle(&file_handle);

    php_request_shutdown(nullptr);
    file_handle.filename = nullptr;

_startup_error:
    zend_string_release(file);
    thread->exit(EG(exit_status));
    ts_free_thread();
    swoole_thread_clean(false);
    thread_num.fetch_sub(1);
}

size_t sw_active_thread_count(void) {
    return thread_num.load();
}

void php_swoole_thread_bailout(void) {
    if (thread_bailout) {
        EG(bailout) = thread_bailout;
    }
    zend_bailout();
}

int php_swoole_thread_stream_cast(zval *zstream) {
    php_stream *stream;
    int sockfd;
    int cast_flags = PHP_STREAM_AS_FD_FOR_SELECT | PHP_STREAM_CAST_INTERNAL;
    if ((php_stream_from_zval_no_verify(stream, zstream))) {
        if (php_stream_cast(stream, cast_flags, (void **) &sockfd, 1) == SUCCESS && sockfd >= 0) {
            return dup(sockfd);
        }
    }
    return -1;
}

int php_swoole_thread_co_socket_cast(zval *zvalue, swSocketType *type) {
    swoole::coroutine::Socket *socket = php_swoole_get_socket(zvalue);
    if (!socket) {
        return -1;
    }
    int sockfd = socket->get_fd();
    if (sockfd < 0) {
        return -1;
    }
    int newfd = dup(sockfd);
    if (newfd < 0) {
        return -1;
    }
    *type = socket->get_type();
    return newfd;
}

void php_swoole_thread_stream_create(zval *return_value, zend_long sockfd) {
    std::string path = "php://fd/" + std::to_string(sockfd);
    // The file descriptor will be duplicated once here
    php_stream *stream = php_stream_open_wrapper_ex(path.c_str(), "", 0, NULL, NULL);
    if (stream) {
        php_stream_to_zval(stream, return_value);
    } else {
        object_init_ex(return_value, swoole_thread_error_ce);
        zend::object_set(return_value, ZEND_STRL("code"), errno);
    }
}

void php_swoole_thread_co_socket_create(zval *return_value, zend_long sockfd, swSocketType type) {
    int newfd = dup(sockfd);
    if (newfd < 0) {
    _error:
        object_init_ex(return_value, swoole_thread_error_ce);
        zend::object_set(return_value, ZEND_STRL("code"), errno);
        return;
    }
    zend_object *sockobj = php_swoole_create_socket_from_fd(newfd, type);
    if (sockobj) {
        ZVAL_OBJ(return_value, sockobj);
    } else {
        goto _error;
    }
}

#ifdef SWOOLE_SOCKETS_SUPPORT
void php_swoole_thread_php_socket_create(zval *return_value, zend_long sockfd) {
    int newfd = dup(sockfd);
    if (newfd < 0) {
    _error:
        object_init_ex(return_value, swoole_thread_error_ce);
        zend::object_set(return_value, ZEND_STRL("code"), errno);
        return;
    }
    object_init_ex(return_value, socket_ce);
    auto retsock = Z_SOCKET_P(return_value);
    if (!socket_import_file_descriptor(newfd, retsock)) {
        goto _error;
    }
}
#endif

static PHP_METHOD(swoole_thread, getInfo) {
    array_init(return_value);
    add_assoc_bool(return_value, "is_main_thread", tsrm_is_main_thread());
    add_assoc_bool(return_value, "is_shutdown", tsrm_is_shutdown());
    add_assoc_long(return_value, "thread_num", thread_num.load());
}

static PHP_METHOD(swoole_thread, activeCount) {
    RETURN_LONG(thread_num.load());
}

static PHP_METHOD(swoole_thread, yield) {
    std::this_thread::yield();
}

#define CAST_OBJ_TO_RESOURCE(_name, _type)                                                                             \
    else if (instanceof_function(Z_OBJCE_P(zvalue), swoole_thread_##_name##_ce)) {                                     \
        value.resource = php_swoole_thread_##_name##_cast(zvalue);                                                     \
        value.resource->add_ref();                                                                                     \
        type = _type;                                                                                                  \
        break;                                                                                                         \
    }

void ArrayItem::store(zval *zvalue) {
    type = Z_TYPE_P(zvalue);
    switch (type) {
    case IS_LONG:
        value.lval = zval_get_long(zvalue);
        break;
    case IS_DOUBLE:
        value.dval = zval_get_double(zvalue);
        break;
    case IS_STRING: {
        value.str = zend_string_init(Z_STRVAL_P(zvalue), Z_STRLEN_P(zvalue), true);
        break;
    }
    case IS_TRUE:
    case IS_FALSE:
    case IS_NULL:
        break;
    case IS_RESOURCE: {
        value.socket.fd = php_swoole_thread_stream_cast(zvalue);
        type = IS_STREAM_SOCKET;
        if (value.socket.fd == -1) {
            zend_throw_exception(swoole_exception_ce, "failed to convert to socket fd", errno);
        }
        break;
    }
    case IS_ARRAY: {
        type = zend_array_is_list(Z_ARRVAL_P(zvalue)) ? IS_ARRAYLIST : IS_MAP;
        value.resource = ZendArray::from(Z_ARRVAL_P(zvalue));
        break;
    }
    case IS_OBJECT: {
        if (sw_zval_is_co_socket(zvalue)) {
            value.socket.fd = php_swoole_thread_co_socket_cast(zvalue, &value.socket.type);
            type = IS_CO_SOCKET;
            if (value.socket.fd == -1) {
                zend_throw_exception(swoole_exception_ce, "failed to convert to socket fd", errno);
            }
            break;
        }
#ifdef SWOOLE_SOCKETS_SUPPORT
        else if (sw_zval_is_php_socket(zvalue)) {
            php_socket *php_sock = SW_Z_SOCKET_P(zvalue);
            if (php_sock->bsd_socket == -1) {
                zend_throw_exception(swoole_exception_ce, "invalid socket fd", EBADF);
                break;
            }
            value.socket.fd = dup(php_sock->bsd_socket);
            if (value.socket.fd == -1) {
                zend_throw_exception(swoole_exception_ce, "failed to dup socket fd", errno);
            }
            type = IS_PHP_SOCKET;
            break;
        }
#endif
        CAST_OBJ_TO_RESOURCE(arraylist, IS_ARRAYLIST)
        CAST_OBJ_TO_RESOURCE(map, IS_MAP)
        CAST_OBJ_TO_RESOURCE(queue, IS_QUEUE)
        CAST_OBJ_TO_RESOURCE(lock, IS_LOCK)
        CAST_OBJ_TO_RESOURCE(atomic, IS_ATOMIC)
        CAST_OBJ_TO_RESOURCE(atomic_long, IS_ATOMIC_LONG)
        CAST_OBJ_TO_RESOURCE(barrier, IS_BARRIER)
    }
    /* no break */
    default: {
        auto _serialized_object = php_swoole_serialize(zvalue);
        if (!_serialized_object) {
            type = IS_UNDEF;
            break;
        } else {
            type = IS_SERIALIZED_OBJECT;
            value.serialized_object = _serialized_object;
        }
        break;
    }
    }
}

bool ArrayItem::equals(const zval *zvalue) const {
    if (Z_TYPE_P(zvalue) != type) {
        return false;
    }
    switch (type) {
    case IS_LONG:
        return Z_LVAL_P(zvalue) == value.lval;
    case IS_DOUBLE:
        return Z_DVAL_P(zvalue) == value.dval;
    case IS_TRUE:
    case IS_FALSE:
    case IS_NULL:
        return true;
    case IS_STRING:
        return zend_string_equals(value.str, Z_STR_P(zvalue));
    default:
        return false;
    }
}

#define TYPE_PAIR(t1, t2) (((t1) << 4) | (t2))
#define ITEM_TYPE(item) (item->type)
#define ITEM_LVAL(item) (item->value.lval)
#define ITEM_DVAL(item) (item->value.dval)
#define ITEM_STR(item) (item->value.str)

static int compare_long_to_string(zend_long lval, const zend_string *str) /* {{{ */
{
    zend_long str_lval;
    double str_dval;
    zend_uchar type = is_numeric_string(ZSTR_VAL(str), ZSTR_LEN(str), &str_lval, &str_dval, false);

    if (type == IS_LONG) {
        return lval > str_lval ? 1 : lval < str_lval ? -1 : 0;
    }

    if (type == IS_DOUBLE) {
        double diff = (double) lval - str_dval;
        return ZEND_NORMALIZE_BOOL(diff);
    }

    zend_string *lval_as_str = zend_long_to_str(lval);
    int cmp_result = zend_binary_strcmp(ZSTR_VAL(lval_as_str), ZSTR_LEN(lval_as_str), ZSTR_VAL(str), ZSTR_LEN(str));
    zend_string_release(lval_as_str);
    return ZEND_NORMALIZE_BOOL(cmp_result);
}
/* }}} */

static int compare_double_to_string(double dval, const zend_string *str) /* {{{ */
{
    zend_long str_lval;
    double str_dval;
    zend_uchar type = is_numeric_string(ZSTR_VAL(str), ZSTR_LEN(str), &str_lval, &str_dval, false);

    if (type == IS_LONG) {
        double diff = dval - (double) str_lval;
        return ZEND_NORMALIZE_BOOL(diff);
    }

    if (type == IS_DOUBLE) {
        if (dval == str_dval) {
            return 0;
        }
        return ZEND_NORMALIZE_BOOL(dval - str_dval);
    }

    zend_string *dval_as_str = zend_double_to_str(dval);
    int cmp_result = zend_binary_strcmp(ZSTR_VAL(dval_as_str), ZSTR_LEN(dval_as_str), ZSTR_VAL(str), ZSTR_LEN(str));
    zend_string_release(dval_as_str);
    return ZEND_NORMALIZE_BOOL(cmp_result);
}
/* }}} */

int ArrayItem::compare(Bucket *a, Bucket *b) {
    const ArrayItem *op1 = static_cast<ArrayItem *>(Z_PTR(a->val));
    const ArrayItem *op2 = static_cast<ArrayItem *>(Z_PTR(b->val));

    switch (TYPE_PAIR(ITEM_TYPE(op1), ITEM_TYPE(op2))) {
    case TYPE_PAIR(IS_LONG, IS_LONG):
        return ITEM_LVAL(op1) > ITEM_LVAL(op2) ? 1 : (ITEM_LVAL(op1) < ITEM_LVAL(op2) ? -1 : 0);

    case TYPE_PAIR(IS_DOUBLE, IS_LONG):
        return ZEND_NORMALIZE_BOOL(ITEM_DVAL(op1) - (double) ITEM_LVAL(op2));

    case TYPE_PAIR(IS_LONG, IS_DOUBLE):
        return ZEND_NORMALIZE_BOOL((double) ITEM_LVAL(op1) - ITEM_DVAL(op2));

    case TYPE_PAIR(IS_DOUBLE, IS_DOUBLE):
        if (ITEM_DVAL(op1) == ITEM_DVAL(op2)) {
            return 0;
        } else {
            return ZEND_NORMALIZE_BOOL(ITEM_DVAL(op1) - ITEM_DVAL(op2));
        }

    case TYPE_PAIR(IS_NULL, IS_NULL):
    case TYPE_PAIR(IS_NULL, IS_FALSE):
    case TYPE_PAIR(IS_FALSE, IS_NULL):
    case TYPE_PAIR(IS_FALSE, IS_FALSE):
    case TYPE_PAIR(IS_TRUE, IS_TRUE):
        return 0;

    case TYPE_PAIR(IS_NULL, IS_TRUE):
        return -1;

    case TYPE_PAIR(IS_TRUE, IS_NULL):
        return 1;

    case TYPE_PAIR(IS_STRING, IS_STRING):
        if (ITEM_STR(op1) == ITEM_STR(op2)) {
            return 0;
        }
        return zendi_smart_strcmp(ITEM_STR(op1), ITEM_STR(op2));

    case TYPE_PAIR(IS_NULL, IS_STRING):
        return Z_STRLEN_P(op2) == 0 ? 0 : -1;

    case TYPE_PAIR(IS_STRING, IS_NULL):
        return Z_STRLEN_P(op1) == 0 ? 0 : 1;

    case TYPE_PAIR(IS_LONG, IS_STRING):
        return compare_long_to_string(ITEM_LVAL(op1), ITEM_STR(op2));

    case TYPE_PAIR(IS_STRING, IS_LONG):
        return -compare_long_to_string(ITEM_LVAL(op2), ITEM_STR(op1));

    case TYPE_PAIR(IS_DOUBLE, IS_STRING):
        if (zend_isnan(ITEM_DVAL(op1))) {
            return 1;
        }
        return compare_double_to_string(ITEM_DVAL(op1), ITEM_STR(op2));

    case TYPE_PAIR(IS_STRING, IS_DOUBLE):
        if (zend_isnan(ITEM_DVAL(op2))) {
            return 1;
        }
        return -compare_double_to_string(ITEM_DVAL(op2), ITEM_STR(op1));

    case TYPE_PAIR(IS_OBJECT, IS_NULL):
        return 1;

    case TYPE_PAIR(IS_NULL, IS_OBJECT):
        return -1;

    default:
        zend_throw_error(nullptr, "Unsupported operand types");
        return 1;
    }
}

void ArrayItem::fetch(zval *return_value) const {
    switch (type) {
    case IS_LONG:
        RETVAL_LONG(value.lval);
        break;
    case IS_DOUBLE:
        RETVAL_DOUBLE(value.dval);
        break;
    case IS_TRUE:
        RETVAL_TRUE;
        break;
    case IS_FALSE:
        RETVAL_FALSE;
        break;
    case IS_STRING:
        RETVAL_NEW_STR(zend_string_init(ZSTR_VAL(value.str), ZSTR_LEN(value.str), 0));
        break;
    case IS_ARRAYLIST:
        value.resource->add_ref();
        php_swoole_thread_arraylist_create(return_value, value.resource);
        break;
    case IS_QUEUE:
        value.resource->add_ref();
        php_swoole_thread_queue_create(return_value, value.resource);
        break;
    case IS_LOCK:
        value.resource->add_ref();
        php_swoole_thread_lock_create(return_value, value.resource);
        break;
    case IS_MAP:
        value.resource->add_ref();
        php_swoole_thread_map_create(return_value, value.resource);
        break;
    case IS_BARRIER:
        value.resource->add_ref();
        php_swoole_thread_barrier_create(return_value, value.resource);
        break;
    case IS_ATOMIC:
        value.resource->add_ref();
        php_swoole_thread_atomic_create(return_value, value.resource);
        break;
    case IS_ATOMIC_LONG:
        value.resource->add_ref();
        php_swoole_thread_atomic_long_create(return_value, value.resource);
        break;
    case IS_STREAM_SOCKET:
        php_swoole_thread_stream_create(return_value, value.socket.fd);
        break;
    case IS_CO_SOCKET:
        php_swoole_thread_co_socket_create(return_value, value.socket.fd, value.socket.type);
        break;
#ifdef SWOOLE_SOCKETS_SUPPORT
    case IS_PHP_SOCKET:
        php_swoole_thread_php_socket_create(return_value, value.socket.fd);
        break;
#endif
    case IS_SERIALIZED_OBJECT:
        php_swoole_unserialize(value.serialized_object, return_value);
        break;
    default:
        break;
    }
}

void ArrayItem::release() {
    if (type == IS_STRING) {
        zend_string_release(value.str);
        value.str = nullptr;
    } else if (type == IS_STREAM_SOCKET || type == IS_CO_SOCKET || type == IS_PHP_SOCKET) {
        close(value.socket.fd);
        value.socket.fd = -1;
    } else if (type == IS_SERIALIZED_OBJECT) {
        zend_string_release(value.serialized_object);
        value.serialized_object = nullptr;
    } else if (type >= IS_ARRAYLIST && type <= IS_ATOMIC_LONG) {
        value.resource->del_ref();
        value.resource = nullptr;
    }
}

#define INIT_DECR_VALUE(v)                                                                                             \
    zval rvalue = *v;                                                                                                  \
    if (Z_TYPE_P(v) == IS_DOUBLE) {                                                                                    \
        rvalue.value.dval = -rvalue.value.dval;                                                                        \
    } else {                                                                                                           \
        ZVAL_LONG(&rvalue, -zval_get_long(v));                                                                         \
    }

void ZendArray::incr_update(ArrayItem *item, zval *zvalue, zval *return_value) {
    if (item->type == IS_DOUBLE) {
        item->value.dval += zval_get_double(zvalue);
        RETVAL_DOUBLE(item->value.dval);
    } else {
        item->value.lval += zval_get_long(zvalue);
        RETVAL_LONG(item->value.lval);
    }
}

ArrayItem *ZendArray::incr_create(zval *zvalue, zval *return_value) {
    zval rvalue = *zvalue;
    if (Z_TYPE_P(zvalue) == IS_DOUBLE) {
        RETVAL_DOUBLE(rvalue.value.dval);
    } else {
        ZVAL_LONG(&rvalue, zval_get_long(zvalue));
        RETVAL_LONG(rvalue.value.lval);
    }
    return new ArrayItem(&rvalue);
}

void ZendArray::strkey_incr(zval *zkey, zval *zvalue, zval *return_value) {
    zend::String skey(zkey);

    lock_.lock();
    ArrayItem *item = static_cast<ArrayItem *>(zend_hash_find_ptr(&ht, skey.get()));
    if (item) {
        incr_update(item, zvalue, return_value);
    } else {
        item = incr_create(zvalue, return_value);
        item->setKey(skey);
        zend_hash_update_ptr(&ht, item->key, item);
    }
    lock_.unlock();
}

void ZendArray::intkey_incr(zend_long index, zval *zvalue, zval *return_value) {
    lock_.lock();
    auto item = static_cast<ArrayItem *>(zend_hash_index_find_ptr(&ht, index));
    if (item) {
        incr_update(item, zvalue, return_value);
    } else {
        item = incr_create(zvalue, return_value);
        item = new ArrayItem(zvalue);
        zend_hash_index_update_ptr(&ht, index, item);
    }
    lock_.unlock();
}

void ZendArray::strkey_decr(zval *zkey, zval *zvalue, zval *return_value) {
    INIT_DECR_VALUE(zvalue);
    strkey_incr(zkey, &rvalue, return_value);
}

void ZendArray::intkey_decr(zend_long index, zval *zvalue, zval *return_value) {
    INIT_DECR_VALUE(zvalue);
    intkey_incr(index, &rvalue, return_value);
}

void ZendArray::strkey_add(zval *zkey, zval *zvalue, zval *return_value) {
    zend::String skey(zkey);
    lock_.lock();
    if (strkey_exists(skey)) {
        RETVAL_FALSE;
    } else {
        add(skey, zvalue);
        RETVAL_TRUE;
    }
    lock_.unlock();
}

void ZendArray::intkey_add(zend_long index, zval *zvalue, zval *return_value) {
    lock_.lock();
    if (intkey_exists(index)) {
        RETVAL_FALSE;
    } else {
        add(index, zvalue);
        RETVAL_TRUE;
    }
    lock_.unlock();
}

void ZendArray::strkey_update(zval *zkey, zval *zvalue, zval *return_value) {
    zend::String skey(zkey);
    lock_.lock();
    if (!strkey_exists(skey)) {
        RETVAL_FALSE;
    } else {
        auto item = new ArrayItem(zvalue);
        item->setKey(skey);
        zend_hash_update_ptr(&ht, item->key, item);
        RETVAL_TRUE;
    }
    lock_.unlock();
}

void ZendArray::intkey_update(zend_long index, zval *zvalue, zval *return_value) {
    lock_.lock();
    if (!intkey_exists(index)) {
        RETVAL_FALSE;
    } else {
        auto item = new ArrayItem(zvalue);
        zend_hash_index_update_ptr(&ht, index, item);
        RETVAL_TRUE;
    }
    lock_.unlock();
}

bool ZendArray::index_offsetGet(zend_long index, zval *return_value) {
    bool out_of_range = true;
    lock_.lock_rd();
    if (index_exists(index)) {
        out_of_range = false;
        auto item = static_cast<ArrayItem *>(zend_hash_index_find_ptr(&ht, index));
        if (item) {
            item->fetch(return_value);
        }
    }
    lock_.unlock();
    return !out_of_range;
}

bool ZendArray::index_offsetSet(zend_long index, zval *zvalue) {
    auto item = new ArrayItem(zvalue);
    bool success = true;
    lock_.lock();
    if (index > zend_hash_num_elements(&ht)) {
        success = false;
        delete item;
    } else if (index == -1 || index == zend_hash_num_elements(&ht)) {
        zend_hash_next_index_insert_ptr(&ht, item);
    } else {
        zend_hash_index_update_ptr(&ht, index, item);
    }
    lock_.unlock();
    return success;
}

void ZendArray::append(zval *zvalue) {
    zend_hash_next_index_insert_ptr(&ht, new ArrayItem(zvalue));
}

bool ZendArray::index_incr(zval *zkey, zval *zvalue, zval *return_value) {
    zend_long index = ZVAL_IS_NULL(zkey) ? -1 : zval_get_long(zkey);

    bool success = true;
    lock_.lock();
    if (index > zend_hash_num_elements(&ht)) {
        success = false;
    } else if (index == -1 || index == zend_hash_num_elements(&ht)) {
        auto item = incr_create(zvalue, return_value);
        zend_hash_next_index_insert_ptr(&ht, item);
    } else {
        auto item = static_cast<ArrayItem *>(zend_hash_index_find_ptr(&ht, index));
        incr_update(item, zvalue, return_value);
    }
    lock_.unlock();
    return success;
}

void ZendArray::index_offsetExists(zend_long index, zval *return_value) {
    lock_.lock_rd();
    RETVAL_BOOL(index_exists(index));
    lock_.unlock();
}

void ZendArray::index_offsetUnset(zend_long index) {
    lock_.lock();
    zend_long i = index;
    zend_long n = zend_hash_num_elements(&ht);
    HT_FLAGS(&ht) |= HASH_FLAG_PACKED | HASH_FLAG_STATIC_KEYS;
    auto item = static_cast<ArrayItem *>(zend_hash_index_find_ptr(&ht, index));
    delete item;
    while (i < n - 1) {
#if PHP_VERSION_ID >= 80200
        Z_PTR(ht.arPacked[i]) = Z_PTR(ht.arPacked[i + 1]);
#else
        Z_PTR(ht.arData[i].val) = Z_PTR(ht.arData[i + 1].val);
#endif
        i++;
    }
    ht.nNumUsed--;
    ht.nNumOfElements--;
    ht.nNextFreeElement--;
    lock_.unlock();
}

bool ZendArray::index_decr(zval *zkey, zval *zvalue, zval *return_value) {
    INIT_DECR_VALUE(zvalue);
    return index_incr(zkey, &rvalue, return_value);
}

void ZendArray::keys(zval *return_value) {
    lock_.lock_rd();
    zend_ulong elem_count = zend_hash_num_elements(&ht);
    array_init_size(return_value, elem_count);
    zend_hash_real_init_packed(Z_ARRVAL_P(return_value));
    zend_ulong num_idx;
    zend_string *str_idx;
    zval *entry;
    ZEND_HASH_FILL_PACKED(Z_ARRVAL_P(return_value)) {
        if (HT_IS_PACKED(&ht) && HT_IS_WITHOUT_HOLES(&ht)) {
            /* Optimistic case: range(0..n-1) for vector-like packed array */
            zend_ulong lval = 0;

            for (; lval < elem_count; ++lval) {
                ZEND_HASH_FILL_SET_LONG(lval);
                ZEND_HASH_FILL_NEXT();
            }
        } else {
            /* Go through input array and add keys to the return array */
            ZEND_HASH_FOREACH_KEY_VAL(&ht, num_idx, str_idx, entry) {
                if (str_idx) {
                    ZEND_HASH_FILL_SET_STR(zend_string_init(str_idx->val, str_idx->len, 0));
                } else {
                    ZEND_HASH_FILL_SET_LONG(num_idx);
                }
                ZEND_HASH_FILL_NEXT();
            }
            ZEND_HASH_FOREACH_END();
        }
        (void) entry;
    }
    ZEND_HASH_FILL_END();
    lock_.unlock();
}

void ZendArray::values(zval *return_value) {
    lock_.lock_rd();
    zend_ulong elem_count = zend_hash_num_elements(&ht);
    array_init_size(return_value, elem_count);
    void *tmp;
    ZEND_HASH_FOREACH_PTR(&ht, tmp) {
        zval value;
        auto item = static_cast<ArrayItem *>(tmp);
        item->fetch(&value);
        zend_hash_next_index_insert_new(Z_ARR_P(return_value), &value);
    }
    ZEND_HASH_FOREACH_END();
    lock_.unlock();
}

void ZendArray::to_array(zval *return_value) {
    lock_.lock_rd();
    zend_ulong elem_count = zend_hash_num_elements(&ht);
    array_init_size(return_value, elem_count);
    zend_string *key;
    zend_ulong index;
    void *tmp;
    ZEND_HASH_FOREACH_KEY_PTR(&ht, index, key, tmp) {
        zval value;
        const auto item = static_cast<ArrayItem *>(tmp);
        item->fetch(&value);
        if (key) {
            zend_hash_str_add(Z_ARR_P(return_value), ZSTR_VAL(key), ZSTR_LEN(key), &value);
        } else {
            zend_hash_index_add(Z_ARR_P(return_value), index, &value);
        }
    }
    ZEND_HASH_FOREACH_END();
    lock_.unlock();
}

void ZendArray::find(const zval *search, zval *return_value) {
    lock_.lock_rd();
    zend_string *key;
    zend_ulong index;
    void *tmp;
    ZEND_HASH_FOREACH_KEY_PTR(&ht, index, key, tmp) {
        const auto item = static_cast<ArrayItem *>(tmp);
        if (item->equals(search)) {
            if (key) {
                RETVAL_STRINGL(ZSTR_VAL(key), ZSTR_LEN(key));
            } else {
                RETVAL_LONG(index);
            }
            break;
        }
    }
    ZEND_HASH_FOREACH_END();
    lock_.unlock();
}

void ZendArray::sort(bool renumber) {
    lock_.lock();
    zend_hash_sort(&ht, ArrayItem::compare, renumber);
    lock_.unlock();
}

ZendArray *ZendArray::from(zend_array *src) {
    zend_string *key;
    zend_ulong index;
    zval *tmp;
    ZendArray *result = new ZendArray();
    ZEND_HASH_FOREACH_KEY_VAL(src, index, key, tmp) {
        ZVAL_DEREF(tmp);
        if (key) {
            result->add(key, tmp);
        } else {
            result->add(index, tmp);
        }
    }
    ZEND_HASH_FOREACH_END();
    return result;
}

#endif
